# dump_vcenv.ps1 -- dump the MSVC build environment to build\vcenv.json.
#
# The host blocks Start-Process / cmd.exe, so cmake cannot be driven from
# PowerShell here. Instead we capture the environment that the VS DevShell
# establishes and replay it from bash, where cmake/ninja/cl run fine.
#
# Prerequisite: the process environment must be deduped first (duplicate
# "http_proxy"/"HTTP_PROXY" keys make Enter-VsDevShell throw).

$ErrorActionPreference = "Continue"

$root     = Split-Path -Parent $MyInvocation.MyCommand.Definition
$buildDir = Join-Path $root "build"
if (-not (Test-Path $buildDir)) { New-Item -ItemType Directory -Path $buildDir | Out-Null }
$outPath  = Join-Path $buildDir "vcenv.json"

# ---- dedupe environment by canonical (uppercase) name ----
$canon = [ordered]@{}
foreach ($e in [Environment]::GetEnvironmentVariables('Process').GetEnumerator()) {
    $c = $e.Key.ToUpperInvariant()
    if (-not $canon.Contains($c)) { $canon[$c] = $e.Value }
}
foreach ($n in @([Environment]::GetEnvironmentVariables('Process').Keys)) {
    [Environment]::SetEnvironmentVariable($n, $null, 'Process')
}
foreach ($c in $canon.Keys) {
    [Environment]::SetEnvironmentVariable($c, $canon[$c], 'Process')
}

# ---- enter VS DevShell ----
$vsRoot   = "C:\Program Files\Microsoft Visual Studio\2022\Community"
$devShell = Join-Path $vsRoot "Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
$ok = $false
try {
    Import-Module $devShell -ErrorAction Stop
    Enter-VsDevShell -VsInstallPath $vsRoot -SkipAutomaticLocation -DevCmdArguments "-arch=x64" -ErrorAction Stop | Out-Null
    $ok = $true
} catch { }

# ---- capture PATH / INCLUDE / LIB / LIBPATH from the CURRENT process env ----
$snap = [ordered]@{
    devshell_ok = $ok
    PATH        = $env:PATH
    INCLUDE     = $env:INCLUDE
    LIB         = $env:LIB
    LIBPATH     = $env:LIBPATH
    WindowsSdkDir = $env:WindowsSdkDir
}
[IO.File]::WriteAllText($outPath, ($snap | ConvertTo-Json -Depth 3), [Text.Encoding]::UTF8)
Write-Output "devshell_ok=$ok"
Write-Output "written: $outPath"
