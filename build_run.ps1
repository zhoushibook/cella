# build_run.ps1 -- run build.ps1 in a sanitized child PowerShell process.
#
# Why: in some managed/AI sandbox environments the process environment holds
# BOTH lower-case and upper-case variants of the same variable name
# (e.g. "http_proxy" AND "HTTP_PROXY", "path" AND "Path"). The VS DevShell
# entry script builds a dictionary keyed case-insensitively, so it throws
#   "Item has already been added. Key in dictionary: 'http_proxy'"
# and Enter-VsDevShell aborts -> cmake is never on PATH -> configure fails.
#
# This wrapper strips the duplicate keys from the child process environment,
# then dot-sources build.ps1 so the caller's arguments flow through unchanged.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File build_run.ps1 -Target cella_storage

param(
    [string]$SourceDir = "",
    [string]$BuildDir  = "",
    [string]$Target    = "",
    [switch]$Reconfigure
)

$ErrorActionPreference = "Continue"

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Definition
if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $BuildDir = Join-Path $scriptRoot "build"
}
$outFile = Join-Path $BuildDir "build_last.txt"

# ---- sanitize the process environment before the VS DevShell is entered ----
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

# ---- hand off to the real build script in this (now clean) process ----
$args2 = @{}
if (-not [string]::IsNullOrWhiteSpace($SourceDir)) { $args2['SourceDir'] = $SourceDir }
if (-not [string]::IsNullOrWhiteSpace($Target))    { $args2['Target']    = $Target }
if ($Reconfigure)                                 { $args2['Reconfigure'] = $true }

$scriptPath = Join-Path $scriptRoot "build.ps1"
& $scriptPath @args2 *>&1 | Out-String | ForEach-Object {
    [IO.File]::WriteAllText($outFile, $_, [Text.Encoding]::UTF8)
}
exit $LASTEXITCODE
