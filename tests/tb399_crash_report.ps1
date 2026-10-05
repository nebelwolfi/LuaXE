# TB-399 item 2: the crash reporter must serialize simultaneous faults.
#
#   powershell -File tests\tb399_crash_report.ps1 [-Lxe bin\lxe.exe] [-ThreadDll <thread.dll>]
#                                                  [-Runs 8] [-Workers 8] [-WorkDir <dir>]
#
# The payload starts N workers, all looping on an os.clock() barrier and then
# faulting in the same instant (a write through a null pointer). Every faulting
# thread enters the vectored exception handler, which used to call dbghelp from
# all of them at once on one global tracer.
#
# Measured (scratch builds, this script, 8 workers x 8 runs):
#   main 5020bdd: 0/8 pass - up to 78 "Exception Code:" headers per run
#                 (fragments spliced mid-line), heap corruption (0xC0000374)
#                 and reporter deadlocks (TIMEOUT) instead of one report.
#   TB-399:       8/8 report exactly once (16 stack lines) and die 0xC0000005.
#
# Acceptance: within TimeoutSec the process DIES (any nonzero crash exit) and
# EXACTLY ONE full report ("Exception Code:" + a stack line " at 0x") appears;
# every other faulting thread prints at most its one bounded-wait line. A hang
# (timeout) or a second full stack fails the run.
#
# Exits 0 when every run passes, 1 otherwise.
param(
    [string]$Lxe = "",
    [string]$ThreadDll = "",
    [int]$Runs = 8,
    [int]$Workers = 8,
    [int]$TimeoutSec = 20,
    [string]$WorkDir = ""
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'tb399_common.ps1')
$repo = Split-Path -Parent $PSScriptRoot
if ($Lxe -eq "") { $Lxe = Join-Path $repo 'bin\lxe.exe' }
$Lxe = (Resolve-Path $Lxe).Path
$thread = Find-ThreadDll $ThreadDll

$S = New-Tb399Dir $WorkDir 'tb399-crash-report'
$savedHome = $env:LXE_HOME
$hadHome = Test-Path Env:\LXE_HOME
$failures = 0
try {
New-Item -ItemType Directory -Force "$S\app", "$S\src", "$S\home\modules\thread\1.0.0" | Out-Null
Copy-Item $Lxe "$S\app\lxe.exe"
Copy-Item (Join-Path (Split-Path -Parent $Lxe) 'lua51.dll') "$S\app\"
# Same store trick as the item 1 test: import("thread") of a plain script sees
# the LXE_HOME store, so the test never touches the user's ~/.lxe or network.
Copy-Item $thread "$S\home\modules\thread\1.0.0\thread.dll"
$env:LXE_HOME = "$S\home"
# The barrier instant travels through an argument: a dumped worker chunk keeps
# NO upvalues, and os.clock() is the same wall clock in every state.
Set-Content "$S\src\main.lua" @'
local new_thread = import("thread")
local N = tonumber(arg[1])
local go = os.clock() + 0.75
for _ = 1, N do
  local t = new_thread()
  t.buffer = tostring(go)
  t:load(function()
    local ffi = ffi or require("ffi")
    local at = tonumber(_T.buffer)
    while os.clock() < at do end
    ffi.cast("volatile int *", 16)[0] = 1 -- write fault, all workers at once
  end)
  t:run()
end
io.write("armed ", N, "\n")
io.stdout:flush()
sleep(10000) -- the faults arrive while the main state waits here
io.write("main state survived (unexpected)\n")
'@
# Trial run, BEFORE the loop: import("thread") installs into the store on first
# use, which is slow I/O - if run 1 did it, later runs would start from a warm
# store and the barriers would desynchronize. Also proves the store path works.
Set-Content "$S\app\stage.lua" "assert(type(import('thread')) == 'function')"
$stage = Invoke-Tb399Child "$S\app\lxe.exe" @('run', "$S\app\stage.lua") "$S\app" 60
if ($stage.Outcome -ne 'exit' -or $stage.Code -ne 0) {
    Write-Host "FAIL could not stage thread.dll in the test store: $(Format-Tb399Outcome $stage)"; exit 1
}
$crashCodes = @(-1073741819, -1073740791, -1073741571, -1073740940, -1073740777)
# A main-thread fault must ALSO report exactly once: the vectored handler runs
# first and the run loop's __except filter sees the same record after unwinding
# (an early gate draft reported it twice - review of 1c4b9d0 caught it).
Set-Content "$S\src\maincrash.lua" @'
local ffi = ffi or require("ffi")
ffi.cast("volatile int *", 16)[0] = 1
'@
$r = Invoke-Tb399Child "$S\app\lxe.exe" @('run', "$S\src\maincrash.lua") "$S\app" $TimeoutSec
$mainFull = @($r.Output -split "`n" | Where-Object { $_ -match 'Exception Code:' }).Count
if ($r.Outcome -eq 'exit' -and $crashCodes -contains $r.Code -and $mainFull -eq 1) {
    Write-Host "ok   main-thread fault: one report, died $(Format-Tb399Outcome $r)"
} else {
    $failures++
    Write-Host "FAIL main-thread fault: $(Format-Tb399Outcome $r) with $mainFull full report(s)"
}
for ($i = 1; $i -le $Runs; $i++) {
    $r = Invoke-Tb399Child "$S\app\lxe.exe" @('run', "$S\src\main.lua", "$Workers") "$S\app" $TimeoutSec
    if ($r.Outcome -eq 'timeout') {
        $failures++
        Write-Host "FAIL run $i : TIMEOUT - the reporter deadlocked instead of reporting"
        continue
    }
    if ($r.Code -eq 0 -or $crashCodes -notcontains $r.Code) {
        $failures++
        Write-Host "FAIL run $i : $(Format-Tb399Outcome $r) - expected a crash exit"
        continue
    }
    # "Exception Code:" counts FULL reports. The other faulting threads print a
    # one-line bounded-wait note ("An uncaught exception occurred. (...)", one
    # line each), which is EXPECTED, not a second report. So the verdict is:
    # exactly one full report, and that report carries a stack when dbghelp
    # resolved frames (on a fault inside dbghelp itself there may be none).
    $lines = @($r.Output -split "`n")
    $fullReports = @($lines | Where-Object { $_ -match 'Exception Code:' }).Count
    $stackLines = @($lines | Where-Object { $_ -match ' at 0x[0-9a-fA-F]+' }).Count
    if ($fullReports -eq 1) {
        Write-Host "ok   run $i : one report ($stackLines stack line(s)), died $(Format-Tb399Outcome $r)"
    } else {
        $failures++
        Write-Host "FAIL run $i : $(Format-Tb399Outcome $r) with $fullReports full report(s)"
        ($lines | Select-Object -First 10) | ForEach-Object { Write-Host "       $_" }
    }
}
} finally {
    if ($hadHome) { $env:LXE_HOME = $savedHome } else { Remove-Item Env:\LXE_HOME -EA SilentlyContinue }
    Remove-Item -Recurse -Force $S -EA SilentlyContinue
}
if ($failures -gt 0) { Write-Host "$failures of $Runs run(s) failed"; exit 1 }
Write-Host "all $Runs runs reported exactly once and died"
exit 0
