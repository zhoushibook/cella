# build_agent.ps1 -- build driver that works in environments with duplicate
# case-variant env vars (http_proxy / HTTP_PROXY), which break Enter-VsDevShell.
#
# Unlike build.ps1 (which relies on the VS DevShell to put cmake/ninja/cl on
# PATH), this script:
#   1. dedupes the process environment by canonical (uppercase) name,
#   2. enters the VS DevShell (imports INCLUDE / LIB / PATH for MSVC),
#   3. adds the SDK include/lib dirs explicitly (DevShell -SkipAutomaticLocation
#      can omit them),
#   4. prepends the VS-bundled ninja,
#   5. runs configure (optional) + build via the VS-bundled cmake.
#
# All external-process output is written to build\agent_build.log so the caller
# can read it without relying on stdout capture behaviour of the host shell.

param(
    [string]$Target = "",
    [switch]$Reconfigure
)

$ErrorActionPreference = "Continue"

$root     = Split-Path -Parent $MyInvocation.MyCommand.Definition
$buildDir = Join-Path $root "build"
if (-not (Test-Path $buildDir)) { New-Item -ItemType Directory -Path $buildDir | Out-Null }
$logPath = Join-Path $buildDir "agent_build.log"

$vsRoot   = "C:\Program Files\Microsoft Visual Studio\2022\Community"
$devShell = Join-Path $vsRoot "Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
$cmake    = Join-Path $vsRoot "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninjaDir = Join-Path $vsRoot "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"

$log = New-Object System.Collections.Generic.List[string]

function Log([string]$s) {
    $log.Add($s) | Out-Null
    Write-Output $s
}

# ---- 1. dedupe environment ----
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

# ---- 2. enter VS DevShell ----
Log "=== cella agent build $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') ==="
try {
    Import-Module $devShell -ErrorAction Stop
    Enter-VsDevShell -VsInstallPath $vsRoot -SkipAutomaticLocation -DevCmdArguments "-arch=x64" -ErrorAction Stop | Out-Null
    Log "DevShell: OK"
} catch {
    Log "DevShell FAILED: $($_.Exception.Message)"
}

# ---- 3. make sure SDK include/lib are present ----
$sdkRoots = @()
foreach ($base in @("C:\Program Files (x86)\Windows Kits\10", "C:\Program Files\Windows Kits\10")) {
    if (Test-Path $base) { $sdkRoots += $base }
}
$sdkVer = ""
foreach ($base in $sdkRoots) {
    $inc = Join-Path $base "Include"
    if (Test-Path $inc) {
        $vers = Get-ChildItem $inc -Directory | Sort-Object Name -Descending
        if ($vers.Count -gt 0) { $sdkVer = $vers[0].Name; $sdkRoots = @($base); break }
    }
}
if ($sdkVer -ne "") {
    $sdkBase = $sdkRoots[0]
    $sdkInc  = Join-Path $sdkBase "Include\$sdkVer"
    $sdkLib  = Join-Path $sdkBase "Lib\$sdkVer"
    $incAdd = @(
        (Join-Path $sdkInc "ucrt"),
        (Join-Path $sdkInc "um"),
        (Join-Path $sdkInc "shared"),
        (Join-Path $sdkInc "winrt"),
        (Join-Path $sdkInc "cppwinrt")
    ) | Where-Object { Test-Path $_ }
    $libAdd = @(
        (Join-Path $sdkLib "ucrt\x64"),
        (Join-Path $sdkLib "um\x64")
    ) | Where-Object { Test-Path $_ }

    if ($incAdd.Count -gt 0) {
        $env:INCLUDE = (($incAdd + @($env:INCLUDE)) | Where-Object { $_ -ne "" }) -join ";"
        Log "SDK include: $sdkVer"
    }
    if ($libAdd.Count -gt 0) {
        $env:LIB = (($libAdd + @($env:LIB)) | Where-Object { $_ -ne "" }) -join ";"
        Log "SDK lib: $sdkVer"
    }
} else {
    Log "WARNING: no Windows SDK include dir found"
}

# ---- 4. ninja on PATH ----
if (Test-Path $ninjaDir) { $env:PATH = "$ninjaDir;$env:PATH" }

# ---- 5. configure + build ----
# Run an external command with output redirected to a file at the OS level.
# Collecting native stdout/stderr through PowerShell's pipeline has proven
# unreliable in this host (empty output, null $LASTEXITCODE), so we let the
# OS write the file and then read it back.
function Invoke-Logged([string]$exe, [string[]]$argv, [string]$outFile) {
    if (Test-Path $outFile) { Remove-Item $outFile -Force }
    $quoted = ($argv | ForEach-Object {
        if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ }
    }) -join ' '
    $cmdLine = '"' + $exe + '" ' + $quoted + ' > "' + $outFile + '" 2>&1'
    $comspec = $env:ComSpec
    if ([string]::IsNullOrWhiteSpace($comspec) -or -not (Test-Path $comspec)) {
        $comspec = Join-Path $env:SystemRoot "System32\cmd.exe"
    }
    if ([string]::IsNullOrWhiteSpace($comspec) -or -not (Test-Path $comspec)) {
        $comspec = "C:\Windows\System32\cmd.exe"
    }
    $p = Start-Process -FilePath $comspec -ArgumentList '/c', $cmdLine -NoNewWindow -Wait -PassThru
    return $p.ExitCode
}

if ($Reconfigure -or -not (Test-Path (Join-Path $buildDir "CMakeCache.txt"))) {
    Log "--- configure ---"
    $cfgLog = Join-Path $buildDir "agent_configure.log"
    $rc = Invoke-Logged $cmake @("-S", $root, "-B", $buildDir, "-G", "Ninja",
                                 "-DCMAKE_BUILD_TYPE=Release",
                                 "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON") $cfgLog
    if (Test-Path $cfgLog) { $log.AddRange([string[]](Get-Content $cfgLog)) }
    if ($rc -ne 0) {
        Log "CONFIGURE FAILED (exit $rc)"
        [IO.File]::WriteAllText($logPath, ($log -join "`r`n"), [Text.Encoding]::UTF8)
        exit 1
    }
    Log "configure OK"
}

Log "--- build ---"
$bLog = Join-Path $buildDir "agent_build_cmake.log"
$bArgs = @("--build", $buildDir)
if ($Target -ne "") { $bArgs += @("--target", $Target) }
$code = Invoke-Logged $cmake $bArgs $bLog
if (Test-Path $bLog) { $log.AddRange([string[]](Get-Content $bLog)) }
Log "BUILD EXIT CODE = $code"

[IO.File]::WriteAllText($logPath, ($log -join "`r`n"), [Text.Encoding]::UTF8)
exit $code
