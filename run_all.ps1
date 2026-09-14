# run_all.ps1 -- one-shot verification pipeline for the whole cella system.
# NOTE: this file contains Chinese literals (to match program output), so it MUST be
#       saved as UTF-8 **with BOM**; Windows PowerShell 5.1 reads BOM-less .ps1 as ANSI
#       and would fail to parse. If you edit it, keep the BOM.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File run_all.ps1
#
# Steps:
#   1. build all four modules (Ninja + MSVC via VS DevShell)
#   2. compiler regression      (cella_sql/tests/run_tests.ps1)
#   3. storage unit tests       (storage_tests.exe)
#   4. integration tests        (cella_db_tests.exe)
#   4.4 access control smoke    (cella_db.exe --auth：认证 + 授权)
#   4.5 client layer tests      (cella_client_tests.exe)
#   5. end-to-end demo scripts  (cella_db.exe sql/*.sql)
#   6. example programs         (api_quickstart.exe, concurrency_demo.exe)
#
# 判定依据一律是「进程退出码 + 程序自己落盘的产物」，不依赖捕获子进程 stdout
# （部分受限环境下子进程 stdout 无法被父进程捕获，会让结果看起来像没输出）。
param(
    [string]$Root     = "",
    [string]$BuildDir = "",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Continue"

try { [Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false) } catch { }

if ($Root -eq "")     { $Root = Split-Path -Parent $MyInvocation.MyCommand.Path }
if ($BuildDir -eq "") { $BuildDir = Join-Path $Root "build" }

$results = @()

function Add-Result([string]$name, [int]$code, [string]$detail) {
    $script:results += [pscustomobject]@{ Name = $name; Exit = $code; Detail = $detail }
}

function ReadUtf8([string]$path) {
    try {
        if (-not (Test-Path $path)) { return [string]"" }
        return [string][IO.File]::ReadAllText($path, [Text.Encoding]::UTF8)
    } catch {
        return [string]""
    }
}

# 取第一个匹配的捕获组；无匹配或非字符串输入都返回空串
function Match1([string]$text, [string]$pattern) {
    if ([string]::IsNullOrEmpty($text)) { return [string]"" }
    if ($text -match $pattern) { return [string]$Matches[1] }
    return [string]""
}

function LastLine([string]$text, [string]$pattern) {
    if ([string]::IsNullOrEmpty($text)) { return [string]"" }
    $hit = ($text -split "`r?`n") | Where-Object { $_ -match $pattern } | Select-Object -Last 1
    if ($null -eq $hit) { return [string]"" }
    return ([string]$hit).Trim()
}

# 从 "… 通过 29 / 失败 0 …" 这类（可能因编码而乱码的）行里抽出两个数字，
# 只依赖 ASCII 的 "/" 与数字，因此与中文编码无关。
function PassFail([string]$line) {
    $n1 = Match1 $line "(\d+) /"
    $n2 = Match1 $line "/ [^0-9]*(\d+)"
    return ("pass=$n1 fail=$n2")
}

Write-Output "=== cella run_all: $Root ==="

# ---------------------------------------------------------------- 1. build
if (-not $SkipBuild) {
    $o = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Root "build.ps1") 2>&1 | Out-String
    $code = $LASTEXITCODE
    $tail = (($o -split "`n") | Where-Object { $_ -match "error|warning|BUILD EXIT CODE" } | Out-String).Trim()
    Add-Result "build" $code $tail
    Write-Output "--- build: exit $code ---"
    if ($code -ne 0) {
        Write-Output $tail
        Write-Output "build failed, aborting"
        exit 1
    }
}

# ------------------------------------------- 2. compiler regression (原编译器)
# run_tests.ps1 优先用 cella_sql/build 下的 exe（独立构建的默认位置），聚合构建的产物在
# build/cella_sql/ 下。先把新产物同步过去：否则本地残留的旧 exe 会遮蔽刚构建的结果，
# 表现为莫名其妙的一批用例失败（曾因此误判掉 4 个新增的索引用例）。
$sqlExe = Join-Path $BuildDir "cella_sql\cella_sql.exe"
$sqlExeLocal = Join-Path $Root "cella_sql\build\cella_sql.exe"
if (Test-Path $sqlExe) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $sqlExeLocal) | Out-Null
    Copy-Item $sqlExe $sqlExeLocal -Force
}
$sqlOut = [string](& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Root "cella_sql\tests\run_tests.ps1") 2>&1 | Out-String)
$code = $LASTEXITCODE
$sum = PassFail (LastLine $sqlOut "==========")
Add-Result "compiler regression" $code $sum
Write-Output "--- compiler regression: exit $code $sum ---"

# ------------------------------------------------------- 3. storage unit tests
$storageDir = Join-Path $BuildDir "cella_storage"
$storageOut = Join-Path $BuildDir "storage_test_report.log"
Push-Location $storageDir
$p = Start-Process -FilePath (Join-Path $storageDir "storage_tests.exe") -NoNewWindow -Wait -PassThru `
     -RedirectStandardOutput $storageOut -RedirectStandardError ($storageOut + ".err")
Pop-Location
$sum = LastLine (ReadUtf8 $storageOut) "cases="
if ($sum -eq "") {
    Push-Location $storageDir
    & (Join-Path $storageDir "storage_tests.exe") | Out-Null
    Pop-Location
    $sum = LastLine (ReadUtf8 $storageOut) "cases="
}
Add-Result "storage tests" $p.ExitCode $sum
Write-Output "--- storage tests: exit $($p.ExitCode) $sum ---"

# -------------------------------------------------- 4. integration unit tests
$dbDir = Join-Path $BuildDir "cella_db"
$dbOut = Join-Path $BuildDir "test_report.log"
Push-Location $dbDir
$p = Start-Process -FilePath (Join-Path $dbDir "cella_db_tests.exe") -ArgumentList "--log", $dbOut -NoNewWindow -Wait -PassThru `
     -RedirectStandardOutput (Join-Path $BuildDir "cella_db_stdout.log") -RedirectStandardError (Join-Path $BuildDir "cella_db_stderr.log")
Pop-Location
$sum = PassFail (LastLine (ReadUtf8 $dbOut) "==========")
if ($sum -eq "pass= fail=") {
    Push-Location $dbDir
    & (Join-Path $dbDir "cella_db_tests.exe") --log $dbOut | Out-Null
    Pop-Location
    $sum = PassFail (LastLine (ReadUtf8 $dbOut) "==========")
}
Add-Result "integration tests" $p.ExitCode $sum
Write-Output "--- integration tests: exit $($p.ExitCode) $sum ---"

# ------------------------------------------------- 4.4 access control smoke test
# 认证启用后：建表 / 建用户 / 列用户 / 授权 / 查看授权 / 提管理员 / 改口令应全部成功（退出码 0 即全绿）。
# 登录用 stdin 喂「用户名 + 口令」——root 初始口令为空，避免空命令行参数被吞。
$authDir  = Join-Path $BuildDir "auth_smoke"
if (Test-Path $authDir) { Remove-Item -Recurse -Force $authDir }
$authOut  = Join-Path $BuildDir "auth_smoke_out.txt"
$authFeed = "root`n`nCREATE TABLE st(id INT);`nCREATE USER smoke IDENTIFIED BY 'p1';`nSHOW USERS;`nGRANT get, insert ON main.st TO smoke;`nSHOW GRANTS FOR smoke;`nGRANT admin TO smoke;`nSET PASSWORD = 'r2';`n\q`n"
$authText = [string]($authFeed | & (Join-Path $dbDir "cella_db.exe") --data $authDir --auth --log off 2>&1 | Out-String)
$authCode = $LASTEXITCODE
[IO.File]::WriteAllText($authOut, $authText, [Text.Encoding]::UTF8)
$authOk  = Match1 $authText "成功 (\d+) 条"
$authBad = Match1 $authText "失败 (\d+) 条"
Add-Result "access control smoke" $authCode "ok=$authOk fail=$authBad"
Write-Output "--- access control smoke: exit $authCode ok=$authOk fail=$authBad ---"

# ------------------------------------------------------- 4.5 client layer tests
$clientDir = Join-Path $BuildDir "cella_client"
$clientOut = Join-Path $BuildDir "client_test_report.log"
if (Test-Path (Join-Path $clientDir "cella_client_tests.exe")) {
    Push-Location $clientDir
    $p = Start-Process -FilePath (Join-Path $clientDir "cella_client_tests.exe") -ArgumentList "--log", $clientOut -NoNewWindow -Wait -PassThru `
         -RedirectStandardOutput (Join-Path $BuildDir "client_stdout.log") -RedirectStandardError (Join-Path $BuildDir "client_stderr.log")
    Pop-Location
    $sum = PassFail (LastLine (ReadUtf8 $clientOut) "==========")
    Add-Result "client tests" $p.ExitCode $sum
    Write-Output "--- client tests: exit $($p.ExitCode) $sum ---"
}

# ------------------------------------------------------ 5. end-to-end scripts
$demoOk = 0
$demoExpectedFail = 0
foreach ($script in @("demo_basic.sql", "demo_query.sql", "demo_txn.sql")) {
    $stem    = [IO.Path]::GetFileNameWithoutExtension($script)
    $dataDir = Join-Path $BuildDir ("e2e_" + $stem)
    if (Test-Path $dataDir) { Remove-Item -Recurse -Force $dataDir }
    $outFile = Join-Path $BuildDir ($stem + "_out.txt")
    $sqlPath = Join-Path $Root ("cella_db\sql\" + $script)

    $text = [string](& (Join-Path $dbDir "cella_db.exe") --data $dataDir --log off -f $sqlPath 2>&1 | Out-String)
    [IO.File]::WriteAllText($outFile, $text, [Text.Encoding]::UTF8)

    # 脚本产出的日志文件是兜底判据（统计口径与 stdout 一致）
    $line = LastLine $text "共执行成功"
    if ($line -eq "") { $line = LastLine $text "\d+ " }
    $ok  = Match1 $line "成功 (\d+) 条"
    $bad = Match1 $line "失败 (\d+) 条"
    if ($ok  -eq "") { $ok  = Match1 $text "成功 (\d+) 条" }
    if ($bad -eq "") { $bad = Match1 $text "失败 (\d+) 条" }
    if ($ok  -ne "") { $demoOk += [int]$ok }
    if ($bad -ne "") { $demoExpectedFail += [int]$bad }
    Write-Output ("--- {0}: ok={1} expected_failures={2} log={3} ---" -f $script, $ok, $bad, (Test-Path $dataDir))
}
Add-Result "e2e demo scripts" 0 "ok=$demoOk expected_failures=$demoExpectedFail"
Write-Output "--- e2e demo scripts: ok=$demoOk expected_failures=$demoExpectedFail ---"

# ------------------------------------------------------------ 6. examples
$quickOut = Join-Path $BuildDir "api_quickstart_out.txt"
$concOut  = Join-Path $BuildDir "concurrency_demo_out.txt"
Push-Location $dbDir
$quickText = [string](& (Join-Path $dbDir "api_quickstart.exe") 2>&1 | Out-String)
$quickCode = $LASTEXITCODE
$concText = [string](& (Join-Path $dbDir "concurrency_demo.exe") 2>&1 | Out-String)
$concCode = $LASTEXITCODE
Pop-Location
if ($quickText.Length -gt 0) { [IO.File]::WriteAllText($quickOut, $quickText, [Text.Encoding]::UTF8) }
if ($concText.Length  -gt 0) { [IO.File]::WriteAllText($concOut, $concText, [Text.Encoding]::UTF8) }

# 产物判据：示例程序自己写下的日志/审计文件（不依赖 stdout 捕获）
$qlog = Test-Path (Join-Path $dbDir "quickstart_data\cella-db.log")
$clog = Test-Path (Join-Path $dbDir "concurrency_data\journal.log")
Add-Result "api_quickstart" $quickCode "artifacts: engine_log=$qlog"
Add-Result "concurrency_demo" $concCode "artifacts: journal=$clog"
Write-Output "--- api_quickstart: exit $quickCode (engine_log=$qlog) ---"
Write-Output "--- concurrency_demo: exit $concCode (journal=$clog) ---"

# ------------------------------------------------------------- summary
Write-Output ""
Write-Output "==================== SUMMARY ===================="
foreach ($r in $results) {
    $mark = if ($r.Exit -eq 0) { "PASS" } else { "FAIL" }
    Write-Output ("[{0}] {1,-22} {2}" -f $mark, $r.Name, $r.Detail)
}
Write-Output "================================================="
$bad = ($results | Where-Object { $_.Exit -ne 0 }).Count
exit $bad
