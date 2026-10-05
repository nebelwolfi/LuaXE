# TB-399 item 3: import() must not hold its lock across network I/O.
#
#   powershell -File tests\tb399_import_lock.ps1 [-Lxe bin\lxe.exe] [-ThreadDll <thread.dll>]
#                                                  [-DelayMs 4000] [-WorkDir <dir>]
#
# The test builds a stub registry (a FOLDER: LUAXE_TEST_REGISTRY): slowmod@1.0.0
# with one Lua file, plus LUAXE_TEST_REGISTRY_DELAY_MS so every registry lookup
# and file copy sleeps. Worker A imports slowmod (a full "download"); the main
# state times how long require("fastmod.sub") of an ALREADY-RESOLVED module
# takes while A is inside the install - resolved_module_searcher takes the same
# import_mutex, so with the lock held across the network it blocks for the whole
# download (measured on stock, see below).
#
# A second check imports the SAME slow module from two workers at once: the
# first install wins and the second shares it (no double download, no staging
# collision) - the in-flight promise path.
#
# Measured (scratch builds, stub delay 4 s):
#   stock + stub seam: require blocked 7.5 s (two delayed round-trips serialized
#                      under the lock)
#   TB-399:            require took 0 s while the download ran alongside
#
# Exits 0 when the require stays fast and both imports succeed, 1 otherwise.
param(
    [string]$Lxe = "",
    [string]$ThreadDll = "",
    [int]$DelayMs = 4000,
    [int]$TimeoutSec = 60,
    [string]$WorkDir = ""
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'tb399_common.ps1')
$repo = Split-Path -Parent $PSScriptRoot
if ($Lxe -eq "") { $Lxe = Join-Path $repo 'bin\lxe.exe' }
$Lxe = (Resolve-Path $Lxe).Path
$thread = Find-ThreadDll $ThreadDll

$S = New-Tb399Dir $WorkDir 'tb399-import-lock'
$savedHome = $env:LXE_HOME
$hadHome = Test-Path Env:\LXE_HOME
$failures = 0
try {
New-Item -ItemType Directory -Force "$S\app", "$S\src", "$S\home\modules\thread\1.0.0",
    "$S\registry", "$S\fastmod" | Out-Null
Copy-Item $Lxe "$S\app\lxe.exe"
Copy-Item (Join-Path (Split-Path -Parent $Lxe) 'lua51.dll') "$S\app\"
Copy-Item $thread "$S\home\modules\thread\1.0.0\thread.dll"
# fastmod is pre-installed in the test store (already resolved, no network).
New-Item -ItemType Directory -Force "$S\home\modules\fastmod\1.0.0" | Out-Null
Set-Content "$S\home\modules\fastmod\1.0.0\fastmod.lua" "return 'fast'"
Set-Content "$S\home\modules\fastmod\1.0.0\sub.lua" "return 'fastsub'"
# slowmod lives ONLY in the stub registry: one Lua file + its record.
Set-Content "$S\registry\slowmod.lua" "return 'slow'"
$record = @{ name = 'slowmod'; version = '1.0.0';
             files = @(@{ name = 'slowmod.lua'; md5 = '' });
             dependencies = @{} } | ConvertTo-Json -Depth 4
# The md5 the installer checks: hash the stub bytes the same way (file_md5).
$md5 = (Get-FileHash "$S\registry\slowmod.lua" -Algorithm MD5).Hash.ToLower()
$record = @{ name = 'slowmod'; version = '1.0.0';
             files = @(@{ name = 'slowmod.lua'; md5 = $md5 });
             dependencies = @{} } | ConvertTo-Json -Depth 4
Set-Content "$S\registry\slowmod.json" $record
$env:LXE_HOME = "$S\home"
$env:LUAXE_TEST_REGISTRY = "$S\registry"
$env:LUAXE_TEST_REGISTRY_DELAY_MS = "$DelayMs"
Set-Content "$S\src\main.lua" @'
-- Worker A imports slowmod (registry + "download", delayed). The main state
-- imports fastmod first (resolves it), then times require("fastmod.sub")
-- while A is mid-install: that searcher takes import_mutex too.
local new_thread = import("thread")
local a = new_thread()
a:load(function()
  local ok, mod = pcall(import, "slowmod")
  local f = assert(io.open("result_a.txt", "w"))
  f:write(ok and ("ok:" .. tostring(mod)) or ("FAIL:" .. tostring(mod)))
  f:close()
end)
a:run()
assert(import("fastmod") == "fast")
sleep(500) -- let A get inside its install (past the first delayed lookup)
local t0 = os.clock()
local sub = require("fastmod.sub")
local dt = os.clock() - t0
assert(sub == "fastsub", "fastmod.sub must load")
local f = assert(io.open("result_b.txt", "w"))
f:write(string.format("sub_ok dt=%.3f", dt))
f:close()
sleep(15000) -- stay alive until A's install finishes
'@
$r = Invoke-Tb399Child "$S\app\lxe.exe" @('run', "$S\src\main.lua") "$S\app" $TimeoutSec `
    -Environment @{ LUAXE_TEST_REGISTRY = "$S\registry";
                    LUAXE_TEST_REGISTRY_DELAY_MS = "$DelayMs";
                    LXE_HOME = "$S\home" }
if ($r.Outcome -ne 'exit' -or $r.Code -ne 0) {
    $failures++
    Write-Host "FAIL lock-hold child: $(Format-Tb399Outcome $r)"
    ($r.Output -split "`n" | Select-Object -First 8) | ForEach-Object { Write-Host "       $_" }
}
} finally {
    Remove-Item Env:\LUAXE_TEST_REGISTRY -EA SilentlyContinue
    Remove-Item Env:\LUAXE_TEST_REGISTRY_DELAY_MS -EA SilentlyContinue
    if ($hadHome) { $env:LXE_HOME = $savedHome } else { Remove-Item Env:\LXE_HOME -EA SilentlyContinue }
}
$rb = if (Test-Path "$S\app\result_b.txt") { Get-Content "$S\app\result_b.txt" -Raw } else { "" }
$ra = if (Test-Path "$S\app\result_a.txt") { Get-Content "$S\app\result_a.txt" -Raw } else { "" }
$slow = $DelayMs / 1000.0
if ($rb -match 'sub_ok dt=([0-9.]+)') {
    $dt = [double]$Matches[1]
    # Fast means: nowhere near the stub delay (two delayed round-trips ca.
    # 2x delay for A's own install). A require blocked on the lock pays all of it.
    if ($dt -lt ($slow / 2)) {
        Write-Host "ok   require during another import's download took ${dt}s (stub delay ${slow}s)"
    } else {
        $failures++
        Write-Host "FAIL require blocked ${dt}s on another import's download (stub delay ${slow}s)"
    }
} else {
    $failures++
    Write-Host "FAIL no fastmod.sub result (child: $($r.Outcome) $(Format-Tb399Outcome $r))"
}
if ($ra -like 'ok:*') {
    Write-Host "ok   slowmod installed and imported alongside"
} else {
    $failures++
    Write-Host "FAIL slowmod import: $ra"
}
# Second child: the same slow module from TWO workers at once. Both must get
# the same folder (the first install wins, the second shares the in-flight
# promise) and the wall time must be ~one install, not two serialized ones.
Set-Content "$S\src\main2.lua" @'
local new_thread = import("thread")
local function go()
  local t = new_thread()
  t:load(function()
    local ok, mod = pcall(import, "slowmod")
    assert(ok, "import slowmod: " .. tostring(mod))
    assert(mod == "slow", "slowmod must return its chunk")
  end)
  t:run()
end
go()
go()
-- Stay alive until both installs finish: workers are detached when still
-- running at state close, so main must outlive them. Timed generously: the
-- verdict compares wall against one install + this sleep, not against zero.
sleep(25000)
io.write("both shared one install\n")
'@
Remove-Item "$S\home\modules\slowmod" -Recurse -Force -EA SilentlyContinue
$t0 = Get-Date
$r2 = Invoke-Tb399Child "$S\app\lxe.exe" @('run', "$S\src\main2.lua") "$S\app" $TimeoutSec `
    -Environment @{ LUAXE_TEST_REGISTRY = "$S\registry";
                    LUAXE_TEST_REGISTRY_DELAY_MS = "$DelayMs";
                    LXE_HOME = "$S\home" }
$wall = ((Get-Date) - $t0).TotalSeconds
# Wall is one install (~2x delay: record + file) plus main2's own 25 s stay-
# alive sleep: serialized installs would add another ~2x delay on top of that.
$shared = ($r2.Outcome -eq 'exit' -and $r2.Code -eq 0 -and $r2.Output -like '*both shared one install*')
if ($shared -and $wall -lt ($slow * 4 + 27)) {
    Write-Host ("ok   two concurrent imports shared one install ({0:N1}s wall, one install ~{1:N1}s + 25s stay-alive)" -f $wall, ($slow * 2))
} else {
    $failures++
    Write-Host "FAIL concurrent same-module imports: $(Format-Tb399Outcome $r2) in ${wall}s"
}
Remove-Item -Recurse -Force $S -EA SilentlyContinue
if ($failures -gt 0) { Write-Host "$failures check(s) failed"; exit 1 }
Write-Host "import lock stays off the network"
exit 0
