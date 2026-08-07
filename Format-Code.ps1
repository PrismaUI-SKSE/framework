param(
    [string]$ClangFormat = "clang-format",
    [switch]$Check
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path

function Get-RelativePath {
    param(
        [string]$BasePath,
        [string]$Path
    )

    $baseUri = New-Object System.Uri(($BasePath.TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar))
    $pathUri = New-Object System.Uri($Path)
    return [System.Uri]::UnescapeDataString($baseUri.MakeRelativeUri($pathUri).ToString()).Replace('/', [System.IO.Path]::DirectorySeparatorChar)
}

$clangFormatCommand = Get-Command $ClangFormat -ErrorAction SilentlyContinue
if ($null -eq $clangFormatCommand) {
    Write-Error "clang-format was not found. Install clang-format or pass -ClangFormat <path>."
    exit 1
}

$excludedDirectories = @(
    ".git",
    "build",
    "dist",
    "external",
    "shell/dist",
    "shell/app/node_modules",
    "test_plugin/build",
    "test_plugin/dist"
)

$excludedRoots = $excludedDirectories | ForEach-Object {
    [System.IO.Path]::GetFullPath((Join-Path $repoRoot $_)).TrimEnd([System.IO.Path]::DirectorySeparatorChar, [System.IO.Path]::AltDirectorySeparatorChar)
}

$sourceFiles = @(Get-ChildItem -Path $repoRoot -Recurse -File -Include *.h, *.cpp | Where-Object {
        $fullName = [System.IO.Path]::GetFullPath($_.FullName)
        foreach ($excludedRoot in $excludedRoots) {
            if ($fullName.StartsWith($excludedRoot + [System.IO.Path]::DirectorySeparatorChar, [System.StringComparison]::OrdinalIgnoreCase) -or
                $fullName.StartsWith($excludedRoot + [System.IO.Path]::AltDirectorySeparatorChar, [System.StringComparison]::OrdinalIgnoreCase)) {
                return $false
            }
        }

        return $true
    } | Sort-Object FullName)

if ($sourceFiles.Count -eq 0) {
    Write-Host "No .h or .cpp files found."
    exit 0
}

Write-Host "Found $($sourceFiles.Count) .h/.cpp files."

$failed = $false
foreach ($sourceFile in $sourceFiles) {
    $relativePath = Get-RelativePath -BasePath $repoRoot -Path $sourceFile.FullName

    Write-Host $relativePath
    if ($Check) {
        & $clangFormatCommand.Source --style=file --dry-run --Werror $sourceFile.FullName
        if ($LASTEXITCODE -ne 0) {
            Write-Host "Needs formatting: $relativePath"
            $failed = $true
        }

        continue
    }

    & $clangFormatCommand.Source --style=file -i $sourceFile.FullName
    if ($LASTEXITCODE -ne 0) {
        Write-Error "clang-format failed for $relativePath"
        exit 1
    }
}

if ($failed) {
    Write-Error "clang-format check failed. Run .\Format-Code.ps1 to update formatting."
    exit 1
}

if ($Check) {
    Write-Host "clang-format check passed."
}
