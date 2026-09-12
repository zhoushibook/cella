# build.ps1 -- unified build for the three cella modules (Windows / MSVC + Ninja)
# NOTE: keep this file ASCII-only; Windows PowerShell 5.1 reads .ps1 as ANSI when
#       there is no BOM, which corrupts non-ASCII comments and breaks parsing.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File build.ps1                    # configure (if needed) + build all
#   powershell -ExecutionPolicy Bypass -File build.ps1 -Target cella_db   # build one target
#   powershell -ExecutionPolicy Bypass -File build.ps1 -Reconfigure       # force re-configure
#
# cmake / ninja / cl are not on PATH on this machine: MSVC comes from the VS
# DevShell, Ninja is taken straight from the VS install directory.
param(
    [string]$SourceDir = "",
    [string]$BuildDir  = "",
    [string]$Target    = "",
    [switch]$Reconfigure
)

# NOTE: "Continue" on purpose -- CMake writes progress to stderr, and with
#       "Stop" PS 5.1 turns that into a terminating NativeCommandError.
$ErrorActionPreference = "Continue"

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Definition
if ([string]::IsNullOrWhiteSpace($SourceDir)) {
    $SourceDir = $scriptRoot
} elseif (-not [IO.Path]::IsPathRooted($SourceDir)) {
    $SourceDir = Join-Path (Get-Location) $SourceDir
}
if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $BuildDir = Join-Path $SourceDir "build"
} elseif (-not [IO.Path]::IsPathRooted($BuildDir)) {
    $BuildDir = Join-Path (Get-Location) $BuildDir
}

$SourceDir = [IO.Path]::GetFullPath($SourceDir)
$BuildDir = [IO.Path]::GetFullPath($BuildDir)

$vsRoot   = "C:\Program Files\Microsoft Visual Studio\2022\Community"
$devShell = Join-Path $vsRoot "Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
$cmake    = Join-Path $vsRoot "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninjaDir = Join-Path $vsRoot "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"

if (-not (Test-Path $BuildDir)) { New-Item -ItemType Directory -Path $BuildDir | Out-Null }

# Normalize PATH env-var casing: some managed environments inject a lowercase
# "path", which makes Enter-VsDevShell throw "duplicate key Path/PATH" and
# Start-Process fail ("An item with the same key has already been added").
# Removing and re-adding with canonical casing fixes both.
$procPath = [Environment]::GetEnvironmentVariable('path', 'Process')
if ($null -ne $procPath) {
    [Environment]::SetEnvironmentVariable('path', $null, 'Process')
    [Environment]::SetEnvironmentVariable('Path', $procPath, 'Process')
}

Import-Module $devShell
Enter-VsDevShell -VsInstallPath $vsRoot -SkipAutomaticLocation -DevCmdArguments "-arch=x64" | Out-Null
$env:PATH = "$ninjaDir;$env:PATH"

$log = Join-Path $BuildDir "build.log"
$lines = @()
$lines += "=== cella build $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') ==="

if ($Reconfigure -or -not (Test-Path (Join-Path $BuildDir "CMakeCache.txt"))) {
    $lines += "--- configure ---"
    $lines += (& $cmake -S $SourceDir -B $BuildDir -G Ninja `
                -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) {
        $lines += "CONFIGURE FAILED (exit $LASTEXITCODE)"
        [IO.File]::WriteAllText($log, ($lines -join "`r`n"), [Text.Encoding]::UTF8)
        Write-Output ($lines -join "`r`n")
        exit 1
    }
}

$lines += "--- build ---"
if ($Target -ne "") {
    $lines += (& $cmake --build $BuildDir --target $Target 2>&1 | Out-String)
} else {
    $lines += (& $cmake --build $BuildDir 2>&1 | Out-String)
}
$code = $LASTEXITCODE
$lines += "BUILD EXIT CODE = $code"

[IO.File]::WriteAllText($log, ($lines -join "`r`n"), [Text.Encoding]::UTF8)
Write-Output ($lines -join "`r`n")
exit $code
