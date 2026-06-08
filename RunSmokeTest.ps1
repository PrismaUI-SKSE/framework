param(
    [string]$ModOrganizerDir,
    [string]$Preset = "debug",
    [int]$Threads = 8,
    [int]$SmokeTimeoutSeconds = 30,
    [int]$ExitTimeoutSeconds = 15,
    [switch]$SkipBuild,
    [switch]$NoLaunch,
    [switch]$ForceExitOnTimeout
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$vsDevShellPath = "C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/Tools/Launch-VsDevShell.ps1"
$smokeTestModOrganizerDir = $null
$smokePassMarker = "PRISMAUI_SMOKE_TEST_PASS"

$configPath = Join-Path $repoRoot "Build_Config_Local.ps1"
if (Test-Path -LiteralPath $configPath) {
    . $configPath
}

if ([string]::IsNullOrWhiteSpace($ModOrganizerDir)) {
    $ModOrganizerDir = $env:PRISMAUI_MO_DIR
}

if ([string]::IsNullOrWhiteSpace($ModOrganizerDir)) {
    $ModOrganizerDir = $smokeTestModOrganizerDir
}

if ([string]::IsNullOrWhiteSpace($ModOrganizerDir)) {
    throw "Set -ModOrganizerDir, PRISMAUI_MO_DIR, or `$smokeTestModOrganizerDir in Build_Config_Local.ps1."
}

$ModOrganizerDir = [System.IO.Path]::GetFullPath($ModOrganizerDir)
$modOrganizerExe = Join-Path $ModOrganizerDir "ModOrganizer.exe"

if (-not (Test-Path -LiteralPath $modOrganizerExe)) {
    throw "ModOrganizer.exe was not found at '$modOrganizerExe'."
}

function Invoke-CheckedCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,
        [Parameter(ValueFromRemainingArguments = $true)]
        [string[]]$Arguments
    )

    Write-Host "> $FilePath $($Arguments -join ' ')"
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $FilePath $($Arguments -join ' ')"
    }
}

function Build-Project {
    param(
        [Parameter(Mandatory = $true)]
        [string]$SourceDir
    )

    Push-Location $SourceDir
    try {
        Invoke-CheckedCommand cmake -S . "--preset=$Preset" "-DCMAKE_COMPILE_JOBS=$Threads" "-DCMAKE_LINK_JOBS=2" -Wno-dev
        Invoke-CheckedCommand cmake --build "--preset=$Preset" --parallel $Threads
    } finally {
        Pop-Location
    }
}

function Read-LogText {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        return ""
    }

    $stream = $null
    $reader = $null
    try {
        $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
        $reader = [System.IO.StreamReader]::new($stream)
        $stream = $null
        return $reader.ReadToEnd()
    } finally {
        if ($reader) {
            $reader.Dispose()
        }
        if ($stream) {
            $stream.Dispose()
        }
    }
}

function Wait-ForLogText {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [string]$Needle,
        [Parameter(Mandatory = $true)]
        [int]$TimeoutSeconds,
        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        if ((Read-LogText $Path).Contains($Needle)) {
            Write-Host "Observed $Description."
            return $true
        }
        Start-Sleep -Seconds 1
    }

    return $false
}

function Get-SkyrimProcesses {
    $names = @("SkyrimSE", "skse64_loader")
    foreach ($name in $names) {
        Get-Process -Name $name -ErrorAction SilentlyContinue
    }
}

function Stop-SkyrimProcesses {
    $processes = @(Get-SkyrimProcesses)
    if ($processes.Count -eq 0) {
        return
    }

    Write-Warning "Force-stopping Skyrim smoke-test processes: $($processes.ProcessName -join ', ')"
    $processes | Stop-Process -Force
}

function Wait-ForSkyrimExit {
    param(
        [Parameter(Mandatory = $true)]
        [int]$TimeoutSeconds
    )

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        if (@(Get-SkyrimProcesses).Count -eq 0) {
            return $true
        }
        Start-Sleep -Seconds 1
    }

    return @(Get-SkyrimProcesses).Count -eq 0
}

function Assert-NoLogSeverity {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,
        [Parameter(Mandatory = $true)]
        [string]$Text
    )

    if ($Text.Contains("[error]") -or $Text.Contains("[critical]")) {
        throw "$Name contains error or critical log entries."
    }
}

function Assert-SmokeLogs {
    param(
        [Parameter(Mandatory = $true)]
        [string]$PrismaLog,
        [Parameter(Mandatory = $true)]
        [string]$TestLog,
        [Parameter(Mandatory = $true)]
        [string]$Marker
    )

    $testText = Read-LogText $TestLog
    $prismaText = Read-LogText $PrismaLog

    if (-not $testText.Contains($Marker)) {
        throw "PrismaUITest log does not contain '$Marker'."
    }

    if (-not $prismaText.Contains("CefInitialize succeeded.")) {
        throw "PrismaUI log does not contain 'CefInitialize succeeded.'."
    }

    if (-not $prismaText.Contains("CEF OSR browser [")) {
        throw "PrismaUI log does not contain the CEF OSR browser creation marker."
    }

    Assert-NoLogSeverity "PrismaUITest log" $testText
    Assert-NoLogSeverity "PrismaUI log" $prismaText
}

if (-not $SkipBuild) {
    if (Test-Path -LiteralPath $vsDevShellPath) {
        $currentDirectory = $PWD.Path
        & $vsDevShellPath -Arch amd64
        Set-Location -Path $currentDirectory
    } elseif (-not $env:VSCMD_ARG_TGT_ARCH) {
        throw "VS Developer Shell was not loaded and '$vsDevShellPath' does not exist. Override `$vsDevShellPath in Build_Config_Local.ps1 or run from a VS 2022 Developer PowerShell."
    }

    Write-Host "Building PrismaUI ($Preset)..."
    Build-Project $repoRoot

    Write-Host "Building PrismaUITest ($Preset)..."
    Build-Project (Join-Path $repoRoot "test_plugin")
}

$prismaDistRoot = Join-Path $repoRoot "dist"
$prismaDist = Get-ChildItem -LiteralPath $prismaDistRoot -Directory -Filter "PrismaUI_*" |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1

if (-not $prismaDist) {
    throw "PrismaUI distribution was not found under '$prismaDistRoot'."
}

$testDist = Join-Path $repoRoot "test_plugin/dist/PrismaUITest"
if (-not (Test-Path -LiteralPath $testDist)) {
    throw "PrismaUITest distribution was not found at '$testDist'."
}

$modsDir = Join-Path $ModOrganizerDir "mods"
$prismaModDir = Join-Path $modsDir "PrismaUI"
$testModDir = Join-Path $modsDir "test_plugin"
$cefLog = Join-Path $ModOrganizerDir "overwrite\PrismaUI\logs\cef.log"
$skseLogDir = Join-Path $env:USERPROFILE "Documents\My Games\Skyrim Special Edition\SKSE"
$prismaLog = Join-Path $skseLogDir "PrismaUI.log"
$testLog = Join-Path $skseLogDir "PrismaUITest.log"

New-Item -ItemType Directory -Force -Path $modsDir | Out-Null
Remove-Item -LiteralPath $prismaModDir, $testModDir -Recurse -Force -ErrorAction SilentlyContinue

Write-Host "Copying '$($prismaDist.FullName)' to '$prismaModDir'..."
Copy-Item -LiteralPath $prismaDist.FullName -Destination $prismaModDir -Recurse -Force

Write-Host "Copying '$testDist' to '$testModDir'..."
Copy-Item -LiteralPath $testDist -Destination $testModDir -Recurse -Force

Write-Host "CEF log: $cefLog"
Write-Host "PrismaUI log: $prismaLog"
Write-Host "PrismaUITest log: $testLog"

if ($NoLaunch) {
    return
}

Remove-Item -LiteralPath $cefLog, $prismaLog, $testLog -Force -ErrorAction SilentlyContinue

$oldSmokeAutoExit = [System.Environment]::GetEnvironmentVariable("PRISMAUI_SMOKE_AUTO_EXIT", "Process")
[System.Environment]::SetEnvironmentVariable("PRISMAUI_SMOKE_AUTO_EXIT", "1", "Process")

Push-Location $ModOrganizerDir
try {
    Write-Host "> .\ModOrganizer.exe `".\skse64_loader.exe`""
    Start-Process -FilePath $modOrganizerExe -ArgumentList ".\skse64_loader.exe" -WorkingDirectory $ModOrganizerDir |
        Out-Null
} finally {
    Pop-Location
    [System.Environment]::SetEnvironmentVariable("PRISMAUI_SMOKE_AUTO_EXIT", $oldSmokeAutoExit, "Process")
}

try {
    if (-not (Wait-ForLogText $testLog $smokePassMarker $SmokeTimeoutSeconds "PrismaUITest smoke pass marker")) {
        if ($ForceExitOnTimeout) {
            Stop-SkyrimProcesses
        }
        throw "Timed out after $SmokeTimeoutSeconds seconds waiting for '$smokePassMarker' in '$testLog'."
    }

    if (-not (Wait-ForSkyrimExit $ExitTimeoutSeconds)) {
        if ($ForceExitOnTimeout) {
            Stop-SkyrimProcesses
        }
        throw "Smoke test passed, but Skyrim did not exit within $ExitTimeoutSeconds seconds."
    }

    Assert-SmokeLogs $prismaLog $testLog $smokePassMarker

    if (-not (Test-Path -LiteralPath $cefLog)) {
        Write-Warning "CEF log was not created at '$cefLog'."
    }

    Write-Host "Smoke test passed, Skyrim exited, and PrismaUI/PrismaUITest logs passed checks."
} catch {
    Write-Host "Smoke test failed. Inspect logs:"
    Write-Host "  CEF: $cefLog"
    Write-Host "  PrismaUI: $prismaLog"
    Write-Host "  PrismaUITest: $testLog"
    throw
}
