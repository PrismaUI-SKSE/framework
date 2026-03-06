# Download and extract external dependencies if they don't exist

$ErrorActionPreference = "Stop"

# Define dependencies
$dependencies = @(  

)

# Ensure external directory exists
$externalDir = Join-Path -Path $PSScriptRoot -ChildPath "external"
if (-not (Test-Path $externalDir)) {
    Write-Host "Creating external directory..." -ForegroundColor Yellow
    New-Item -ItemType Directory -Path $externalDir | Out-Null
}

# Process each dependency
foreach ($dep in $dependencies) {
    $targetPath = Join-Path $externalDir $dep.Name
    
    if (Test-Path $targetPath) {
        Write-Host " $($dep.Name) already exists, skipping..." -ForegroundColor Green
        continue
    }
    
    Write-Host "Downloading $($dep.Name)..." -ForegroundColor Cyan
    $archiveName = Split-Path $dep.Url -Leaf
    $archivePath = Join-Path $externalDir $archiveName
    
    try {
        # Download the file
        Invoke-WebRequest -Uri $dep.Url -OutFile $archivePath -UseBasicParsing
        Write-Host "  Downloaded to $archivePath" -ForegroundColor Gray
        
        # Extract based on archive type
        Write-Host "Extracting $archiveName..." -ForegroundColor Cyan
        
        if ($dep.ArchiveType -eq "zip") {
            # Extract ZIP file
            Expand-Archive -Path $archivePath -DestinationPath $externalDir -Force
            Write-Host "  Extracted $($dep.Name)" -ForegroundColor Green
        }
        elseif ($dep.ArchiveType -eq "tar.bz2") {
            # Extract tar.bz2 file using tar (available in Windows 10+)
            $originalLocation = Get-Location
            Set-Location $externalDir
            tar -xjf $archivePath
            Set-Location $originalLocation
            Write-Host "  Extracted $($dep.Name)" -ForegroundColor Green
        }
        
        # Clean up archive file
        Remove-Item $archivePath -Force
        Write-Host "  Cleaned up archive file" -ForegroundColor Gray
        
        }
        catch {
        Write-Host " Error processing $($dep.Name): $_" -ForegroundColor Red
        if (Test-Path $archivePath) {
            Remove-Item $archivePath -Force -ErrorAction SilentlyContinue
        }
        throw
    }
}

# =============================================================================
# Git Submodule Setup
# =============================================================================
# Handles first-time submodule init after pull, standalone clones that need
# converting, and stale .git/modules cache entries.

Write-Host "`nChecking git submodules..." -ForegroundColor Cyan

$gitmodulesPath = Join-Path $PSScriptRoot ".gitmodules"
if (Test-Path $gitmodulesPath) {
    $gitmodulesContent = Get-Content $gitmodulesPath -Raw

    # Parse [submodule] blocks for path and name
    $submoduleBlocks = [regex]::Matches($gitmodulesContent,
        '(?ms)\[submodule\s+"([^"]+)"\].*?path\s*=\s*(\S+)')

    foreach ($block in $submoduleBlocks) {
        $subName = $block.Groups[1].Value
        $subPath = $block.Groups[2].Value
        $fullPath = Join-Path $PSScriptRoot $subPath
        $gitEntry = Join-Path $fullPath ".git"

        Write-Host "  Submodule: $subPath" -ForegroundColor Gray

        if (-not (Test-Path $fullPath)) {
            Write-Host "    Not yet created, will initialize." -ForegroundColor Yellow
        }
        elseif (Test-Path $gitEntry -PathType Leaf) {
            # .git is a file pointing to the parent repo's modules dir -- correct
            Write-Host "    OK (proper submodule reference)" -ForegroundColor Green
            # Clean untracked files and reset modifications so checkout can succeed
            Write-Host "    Cleaning working tree..." -ForegroundColor Gray
            $savedEAP = $ErrorActionPreference
            $ErrorActionPreference = 'SilentlyContinue'
            git -C $fullPath clean -ffd
            git -C $fullPath checkout .
            $ErrorActionPreference = $savedEAP
        }
        elseif (Test-Path $gitEntry -PathType Container) {
            # .git is a directory -- this is a standalone clone, not a submodule
            Write-Host "    Standalone git clone detected at submodule path." -ForegroundColor Yellow
            Write-Host "    Removing so it can be re-initialized as a submodule..." -ForegroundColor Yellow
            Remove-Item -Recurse -Force $fullPath
        }
        elseif ((Get-ChildItem $fullPath -Force | Measure-Object).Count -gt 0) {
            # Directory has content but no .git at all
            Write-Host "    Non-empty directory blocking submodule init." -ForegroundColor Yellow
            Write-Host "    Removing so it can be re-initialized as a submodule..." -ForegroundColor Yellow
            Remove-Item -Recurse -Force $fullPath
        }
        else {
            Write-Host "    Empty directory, will be populated during init." -ForegroundColor Yellow
        }

        # Check for stale .git/modules cache that can block re-init
        $moduleCachePath = Join-Path -Path $PSScriptRoot -ChildPath ".git\modules\$subName"
        if ((Test-Path $moduleCachePath) -and -not (Test-Path $gitEntry -PathType Leaf)) {
            Write-Host "    Clearing stale .git/modules/$subName cache..." -ForegroundColor Yellow
            Remove-Item -Recurse -Force $moduleCachePath
        }
    }
}

try {
    Write-Host "`nInitializing and updating submodules..." -ForegroundColor Cyan
    git submodule update --init --recursive
    if ($LASTEXITCODE -ne 0) {
        throw "git submodule update --init --recursive failed (exit code $LASTEXITCODE)"
    }
    Write-Host " Git submodules updated successfully." -ForegroundColor Green

    # Verify final state
    $subStatus = git submodule status
    foreach ($line in $subStatus) {
        $line = $line.Trim()
        if ($line -match '^\-') {
            Write-Host "  WARN  $line" -ForegroundColor Yellow
        }
        elseif ($line -match '^\+') {
            Write-Host "  NOTE  $line  (commit differs from index)" -ForegroundColor Yellow
        }
        else {
            Write-Host "  OK    $line" -ForegroundColor Green
        }
    }
}
catch {
    Write-Host " Error updating git submodules: $_" -ForegroundColor Red
    throw
}

Write-Host "`nSubmodules and external dependencies are ready!" -ForegroundColor Green

$ultraLightArchive = "ultralight-free-sdk-1.4.1-dev-win-x64.7z"
if (-not (Test-Path (Join-Path $externalDir $ultraLightArchive))) {
    Write-Host "`n $ultraLightArchive not found in $externalDir. Download it from https://ultralig.ht/download" -ForegroundColor Yellow
}

