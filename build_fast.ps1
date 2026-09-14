# build_fast.ps1 -- incremental build, immune to the managed-shell pitfalls.
# NOTE: keep this file ASCII-only (PowerShell 5.1 reads BOM-less .ps1 as ANSI).
#
# Two problems this script works around:
#   1. The process env carries BOTH "http_proxy" and "HTTP_PROXY" (and "path"/
#      "Path" ...). VS DevShell's env dictionary is case-insensitive, so
#      Enter-VsDevShell dies with "Item has already been added". We de-dupe by
#      canonical (uppercase) name BEFORE importing the DevShell module.
#   2. build.ps1 captures cmake output through a PowerShell pipeline
#      ("& $cmake ... | Out-String"), which some shells reject with
#      "Cannot run a document in the middle of a pipeline". We redirect the
#      native command straight to a file instead.
#
# Usage: powershell -ExecutionPolicy Bypass -File build_fast.ps1 [-Target name]

param([string]$Target = "")

$ErrorActionPreference = "Continue"

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Definition
$buildDir   = Join-Path $scriptRoot "build"
$logFile    = Join-Path $buildDir "fast_build.log"

# ---- de-dupe env vars by canonical uppercase name (must happen first) ----
$canonical = [ordered]@{}
foreach ($e in [Environment]::GetEnvironmentVariables('Process').GetEnumerator()) {
    $canon = $e.Key.ToUpperInvariant()
    if (-not $canonical.Contains($canon)) { $canonical[$canon] = $e.Value }
}
foreach ($n in @([Environment]::GetEnvironmentVariables('Process').Keys)) {
    [Environment]::SetEnvironmentVariable($n, $null, 'Process')
}
foreach ($c in $canonical.Keys) {
    [Environment]::SetEnvironmentVariable($c, $canonical[$c], 'Process')
}

$vsRoot   = "C:\Program Files\Microsoft Visual Studio\2022\Community"
$devShell = Join-Path $vsRoot "Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
$cmake    = Join-Path $vsRoot "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

Import-Module $devShell
Enter-VsDevShell -VsInstallPath $vsRoot -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null

# Run cmake as a child process with file redirection: avoids the
# "Cannot run a document in the middle of a pipeline" failure this shell
# raises when a native exe is placed inside a PowerShell pipeline.
$cmakeArgs = @("--build", $buildDir)
if (-not [string]::IsNullOrWhiteSpace($Target)) {
    $cmakeArgs += @("--target", $Target)
}
$p = Start-Process -FilePath $cmake -ArgumentList $cmakeArgs -NoNewWindow -Wait -PassThru `
                   -RedirectStandardOutput $logFile -RedirectStandardError ($logFile + ".err")
Write-Output ("EXIT " + $p.ExitCode)
