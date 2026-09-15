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
#   4.6 frontend selftests      (cella_client/run_web_tests.sh：无头浏览器跑 web/_selftest/*.html)
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

# --------------------------------------------------------------- 0. env hygiene
# 这个受管环境会同时注入同一变量的「大小写两份」：http_proxy / HTTP_PROXY、
# Path / PATH ……（正常 Windows 进程块大小写不敏感、本不该并存，这里却并存）。
# 后果很阴险：Start-Process 在重建环境字典时会抛
#   "Item has already been added. Key in dictionary: 'http_proxy' ... 'HTTP_PROXY'"
# —— 子进程根本没起来，$p 变成 $null，$p.ExitCode 取到空值，于是被判成 PASS。
# 也就是说「测试压根没跑」被伪装成「测试通过」，比直接失败更危险（曾因此让
# storage / integration / client 三个用例集体假绿）。
# 处理办法与 build.ps1 一致：按规范名（大写）去重，只保留一份。
#   注意 Path/PATH 两份内容并不相同（PATH 比 Path 多一个沙箱 shim 目录），
#   所以同名多份时取「更长」的一份，避免把目录吃掉。
$canonEnv = [ordered]@{}
foreach ($e in [Environment]::GetEnvironmentVariables('Process').GetEnumerator()) {
    $canon = $e.Key.ToUpperInvariant()
    $val   = [string]$e.Value
    if (-not $canonEnv.Contains($canon)) {
        $canonEnv[$canon] = $val
    } elseif ($val.Length -gt ([string]$canonEnv[$canon]).Length) {
        $canonEnv[$canon] = $val
    }
}
foreach ($n in @([Environment]::GetEnvironmentVariables('Process').Keys)) {
    [Environment]::SetEnvironmentVariable($n, $null, 'Process')
}
foreach ($canon in $canonEnv.Keys) {
    [Environment]::SetEnvironmentVariable($canon, [string]$canonEnv[$canon], 'Process')
}

if ($Root -eq "")     { $Root = Split-Path -Parent $MyInvocation.MyCommand.Path }
if ($BuildDir -eq "") { $BuildDir = Join-Path $Root "build" }

$results = @()

# 拿不到退出码 = 子进程压根没起来 = 失败。绝不能静默算 0，
# 否则「没跑」会伪装成「通过」（见上面 env hygiene 的说明）。
function Add-Result([string]$name, $code, [string]$detail) {
    $c = 1
    if ($null -ne $code -and "$code" -ne "") { $c = [int]$code }
    $script:results += [pscustomobject]@{ Name = $name; Exit = $c; Detail = $detail }
}

# 跑之前先把上一次的产物删掉：判据是「程序自己落盘的报告」，若报告是上一轮的残留，
# 本轮就算没跑也会读出一份漂亮的 pass=N fail=0（同样属于假绿）。
function Reset-Report([string]$path) {
    foreach ($p in @($path, ($path + ".err"))) {
        if (Test-Path $p) { Remove-Item $p -Force -ErrorAction SilentlyContinue }
    }
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
# 先删掉上一轮的报告，避免「本轮没跑、却读到上轮残留的漂亮数字」这种假绿。
$storageDir = Join-Path $BuildDir "cella_storage"
$storageOut = Join-Path $BuildDir "storage_test_report.log"
Reset-Report $storageOut
$storageCode = $null
Push-Location $storageDir
$p = Start-Process -FilePath (Join-Path $storageDir "storage_tests.exe") -NoNewWindow -Wait -PassThru `
     -RedirectStandardOutput $storageOut -RedirectStandardError ($storageOut + ".err")
if ($null -ne $p) { $storageCode = $p.ExitCode }
Pop-Location
$sum = LastLine (ReadUtf8 $storageOut) "cases="
if ($sum -eq "") {
    # 退路：Start-Process 起不来时改用调用运算符重跑。stdout 必须同样落盘 ——
    # 旧版退路漏了重定向，报告文件始终是空的，$sum 永远抽不到（等于白跑）。
    Push-Location $storageDir
    & (Join-Path $storageDir "storage_tests.exe") > $storageOut 2> ($storageOut + ".err")
    $storageCode = $LASTEXITCODE
    Pop-Location
    $sum = LastLine (ReadUtf8 $storageOut) "cases="
}
Add-Result "storage tests" $storageCode $sum
Write-Output "--- storage tests: exit $storageCode $sum ---"

# -------------------------------------------------- 4. integration unit tests
$dbDir = Join-Path $BuildDir "cella_db"
$dbOut = Join-Path $BuildDir "test_report.log"
Reset-Report $dbOut
$dbCode = $null
Push-Location $dbDir
$p = Start-Process -FilePath (Join-Path $dbDir "cella_db_tests.exe") -ArgumentList "--log", $dbOut -NoNewWindow -Wait -PassThru `
     -RedirectStandardOutput (Join-Path $BuildDir "cella_db_stdout.log") -RedirectStandardError (Join-Path $BuildDir "cella_db_stderr.log")
if ($null -ne $p) { $dbCode = $p.ExitCode }
Pop-Location
$sum = PassFail (LastLine (ReadUtf8 $dbOut) "==========")
if ($sum -eq "pass= fail=") {
    Push-Location $dbDir
    & (Join-Path $dbDir "cella_db_tests.exe") --log $dbOut | Out-Null
    $dbCode = $LASTEXITCODE
    Pop-Location
    $sum = PassFail (LastLine (ReadUtf8 $dbOut) "==========")
}
Add-Result "integration tests" $dbCode $sum
Write-Output "--- integration tests: exit $dbCode $sum ---"

# ------------------------------------------------- 4.4 access control smoke test
# 认证启用后：建表 / 建用户 / 列用户 / 授权 / 查看授权 / 提管理员 / 改口令应全部成功（退出码 0 即全绿）。
# 通过显式参数登录；root 初始口令为空，避免 PowerShell 管道对空行的处理差异。
$authDir  = Join-Path $BuildDir "auth_smoke"
if (Test-Path $authDir) { Remove-Item -Recurse -Force $authDir }
$authOut  = Join-Path $BuildDir "auth_smoke_out.txt"
$authFeed = "CREATE TABLE st(id INT);`nCREATE USER smoke IDENTIFIED BY 'p1';`nSHOW USERS;`nGRANT get, insert ON main.st TO smoke;`nSHOW GRANTS FOR smoke;`nGRANT admin TO smoke;`nSET PASSWORD = 'r2';`n\q`n"
$authText = [string]($authFeed | & (Join-Path $dbDir "cella_db.exe") --data $authDir --auth --user root --password= --log off 2>&1 | Out-String)
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
    Reset-Report $clientOut
    $clientCode = $null
    Push-Location $clientDir
    $p = Start-Process -FilePath (Join-Path $clientDir "cella_client_tests.exe") -ArgumentList "--log", $clientOut -NoNewWindow -Wait -PassThru `
         -RedirectStandardOutput (Join-Path $BuildDir "client_stdout.log") -RedirectStandardError (Join-Path $BuildDir "client_stderr.log")
    if ($null -ne $p) { $clientCode = $p.ExitCode }
    Pop-Location
    $sum = PassFail (LastLine (ReadUtf8 $clientOut) "==========")
    if ($sum -eq "pass= fail=") {
        Push-Location $clientDir
        & (Join-Path $clientDir "cella_client_tests.exe") --log $clientOut | Out-Null
        $clientCode = $LASTEXITCODE
        Pop-Location
        $sum = PassFail (LastLine (ReadUtf8 $clientOut) "==========")
    }
    Add-Result "client tests" $clientCode $sum
    Write-Output "--- client tests: exit $clientCode $sum ---"
}

# ------------------------------------------------------ 4.6 frontend selftests
# 浏览器里跑 web/_selftest/*.html（零依赖：不需要 npm，也不装 agent-browser）。
# 判据同样不是捕获 stdout，而是**无头浏览器 --dump-dom 出来的页面 DOM**：
# 断言结果写在 <pre id="out"> 里，逐行解析 PASS/FAIL。
# 跑不动就跳过（记为 PASS，不阻塞其余步骤）—— 前端改动不归它守。
#
# 这一步要调 bash 脚本：PATH 里那份 bash 很可能是 WSL 的启动器（见上面 Test-UsableBash），
# 所以不能直接 Get-Command bash 拿来就用 —— 要找、要验证、还要带 -l。详见 Find-Bash。
$clientDir = Join-Path $BuildDir "cella_client"
$webScript = Join-Path $Root "cella_client\run_web_tests.sh"

# ---- 找一个「真能跑」的 bash -----------------------------------------------
# 踩过的坑（两个，都会伪装成和代码无关的假失败）：
#   ① C:\Windows\System32\bash.exe 是 **WSL 的启动器**，不是 Git Bash。它也挂在 PATH 上，
#      所以 Get-Command bash 会先命中它；用它跑 .sh 只会得到
#      "dirname: command not found / cd: null directory" 然后退出 2。
#      → 必须显式把它筛掉，并且**验证**一下（能定位到 dirname 才算数）。
#   ② 非登录 bash 不读 /etc/profile，PATH 只来自 Windows 环境 —— 而这份 PATH 里
#      没有 msys 的 /usr/bin，于是 dirname/sed/grep/cygpath/seq 全都找不到。
#      → 调用方一律带 -l（登录模式），由 /etc/profile 把 /usr/bin、/mingw64/bin 补进 PATH。
#        已验证：非登录时 dirname 为空，加 -l 后是 /usr/bin/dirname。
function Test-UsableBash([string]$path) {
    if ([string]::IsNullOrWhiteSpace($path)) { return $false }
    if (-not (Test-Path $path)) { return $false }
    $full = (Resolve-Path $path -ErrorAction SilentlyContinue).Path
    if (-not $full) { return $false }
    foreach ($bad in @(
        (Join-Path $env:WINDIR "System32\bash.exe"),
        (Join-Path $env:LOCALAPPDATA "Microsoft\WindowsApps\bash.exe")
    )) {
        if ($full -ieq $bad) { return $false }
    }
    try {
        $probe = (& $full -lc "command -v dirname" 2>$null | Out-String).Trim()
        if ($probe -ne "") { return $true }
    } catch { }
    return $false
}

function Find-Bash {
    $cands = @()
    # ① 顺着 PATH 上的 git.exe 反推它旁边的 bash —— 最可靠（本机 git 在 D:\Git\cmd）。
    $git = Get-Command git -ErrorAction SilentlyContinue
    if ($git) {
        $gitRoot = Split-Path -Parent (Split-Path -Parent $git.Source)   # <root>\cmd\git.exe -> <root>
        $cands += (Join-Path $gitRoot "bin\bash.exe")
        $cands += (Join-Path $gitRoot "usr\bin\bash.exe")
    }
    # ② 常见安装位置
    $cands += @(
        "$env:ProgramFiles\Git\bin\bash.exe",
        "${env:ProgramFiles(x86)}\Git\bin\bash.exe",
        "$env:LOCALAPPDATA\Programs\Git\bin\bash.exe"
    )
    # ③ PATH 里冒出来的 bash（很可能就是 WSL 那个，交给 Test-UsableBash 筛）
    $cmd = Get-Command bash -ErrorAction SilentlyContinue
    if ($cmd) { $cands += $cmd.Source }
    # ④ 内置 PortableGit 兜底
    $pg = Join-Path $env:USERPROFILE ".workbuddy\binaries\PortableGit\versions"
    if (Test-Path $pg) {
        $cands += @(Get-ChildItem $pg -Filter bash.exe -Recurse -ErrorAction SilentlyContinue |
                    Select-Object -ExpandProperty FullName)
    }
    foreach ($c in $cands) {
        if (Test-UsableBash $c) { return (Resolve-Path $c).Path }
    }
    return $null
}

$bashExe = Find-Bash
$browser = @(
    "$env:ProgramFiles\Google\Chrome\Application\chrome.exe",
    "${env:ProgramFiles(x86)}\Google\Chrome\Application\chrome.exe",
    "${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe",
    "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1

if (-not (Test-Path $webScript)) {
    Add-Result "frontend selftests" 0 "skipped: no run_web_tests.sh"
    Write-Output "--- frontend selftests: skipped (no script) ---"
} elseif ($null -eq $bashExe) {
    Add-Result "frontend selftests" 0 "skipped: no usable git-bash"
    Write-Output "--- frontend selftests: skipped (no usable Git Bash) ---"
} elseif ($null -eq $browser) {
    Add-Result "frontend selftests" 0 "skipped: no browser"
    Write-Output "--- frontend selftests: skipped (no Chrome/Edge) ---"
} else {
    # 脚本自己起/停服务、自己选端口，避免和上面几个用例抢同一份数据目录。
    # 必须带 -l：非登录 bash 的 PATH 里没有 msys 的 /usr/bin，脚本第 26 行就会
    # 死在 `dirname: command not found`。
    $env:CHROME = $browser
    $webOut = [string](& $bashExe -l $webScript 8137 2>&1 | Out-String)
    $webCode = $LASTEXITCODE
    [IO.File]::WriteAllText((Join-Path $BuildDir "web_tests_out.txt"), $webOut, [Text.Encoding]::UTF8)
    $sum = LastLine $webOut "前端自测"
    Add-Result "frontend selftests" $webCode $sum
    Write-Output "--- frontend selftests: exit $webCode $sum ---"
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

# 产物判据：示例程序自己写下的日志/数据文件（不依赖 stdout 捕获）。
# 两个示例用的都是**相对** data_dir（db_engine/api_quickstart 里写死 ./xxx_data），
# 而运行前 Push-Location 到了 $dbDir，所以产物落在 $dbDir 下。
# 注意：旧版纯文本 <data_dir>/journal.log 在 P2 已退役（审计并入 WAL），
# 所以并发示例的判据看 main.wal —— 别再指向早已不存在的 journal.log（会永远 False）。
$qlog = Test-Path (Join-Path $dbDir "quickstart_data\cella-db.log")
$cwal = Test-Path (Join-Path $dbDir "concurrency_data\main.wal")
Add-Result "api_quickstart" $quickCode "artifacts: engine_log=$qlog"
Add-Result "concurrency_demo" $concCode "artifacts: wal=$cwal"
Write-Output "--- api_quickstart: exit $quickCode (engine_log=$qlog) ---"
Write-Output "--- concurrency_demo: exit $concCode (wal=$cwal) ---"

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
