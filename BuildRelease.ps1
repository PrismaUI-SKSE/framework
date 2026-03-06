param(
    [string]$preset = "release",
    [int]$threads = 8
)

$vsDevShellPath = "C:/Program Files/Microsoft Visual Studio/2022/Professional/Common7/Tools/Launch-VsDevShell.ps1"

# Load in local variable overrides
# local developer copy of Build_Config_Template.ps1 to override $vsDevShellPath or other variables if needed
if (Test-Path .\Build_Config_Local.ps1) {
    . .\Build_Config_Local.ps1
}


# Save current directory, launch VS dev shell, and return to original directory
$currentDirectory = $PWD.Path
& $vsDevShellPath -Arch amd64; Set-Location -Path "${currentDirectory}"

Write-Host "Running preset $preset"

# Build cmake configure arguments
$cmakeArgs = @("-S", ".", "--preset=$preset", "-DCMAKE_COMPILE_JOBS=$threads", "-Wno-dev")

& cmake $cmakeArgs
if ($LASTEXITCODE -ne 0) { exit 1 }

& cmake --build --preset=$preset --parallel $threads
if ($LASTEXITCODE -ne 0) { exit 1 }