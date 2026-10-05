# TB-399 item 1: LefFile::loaded must survive readers on other threads while
# the main state restarts.
#
#   powershell -File tests\tb399_lef_loaded.ps1 [-Lxe bin\lxe.exe] [-ThreadDll <thread.dll>]
#                                               [-Runs 4] [-Restarts 40] [-WorkDir <dir>]
#
# The payload (a .lef with 300 chunks, so the loaded list is long) starts 4
# workers that `require` names it does not carry - every such require walks the
# payload loader, i.e. LefFile::loaded - and then restarts the main state
# (env.reload) 40 times. thread's __gc DETACHES a still-running worker instead
# of joining it, so readers from closed states keep walking while every restart
# clears and refills the list.
#
# Measured (scratch builds, this script, 6 runs each):
#   main 5020bdd: 1/6 clean - 4x 0xC0000005 and 1x 0xC0000374 (heap corruption),
#                 each after only 7-14 of 40 states
#   TB-399:       6/6 runs reach 40 states and exit 0
# Bisect on main: the same payload with readers but no restarts, or restarts and
# no readers, is clean - the fault needs both sides.
#
# Exits 0 when every run exits 0 within its deadline, 1 otherwise.
param(
    [string]$Lxe = "",
    [string]$ThreadDll = "",
    [int]$Runs = 4,
    [int]$Restarts = 40,
    [int]$TimeoutSec = 60,
    [string]$WorkDir = ""
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'tb399_common.ps1')
$repo = Split-Path -Parent $PSScriptRoot
if ($Lxe -eq "") { $Lxe = Join-Path $repo 'bin\lxe.exe' }
$Lxe = (Resolve-Path $Lxe).Path
$thread = Find-ThreadDll $ThreadDll

$S = New-Tb399Dir $WorkDir 'tb399-lef-loaded'
New-Item -ItemType Directory -Force "$S\app", "$S\src", "$S\home\modules\thread\1.0.0" | Out-Null
Copy-Item $Lxe "$S\app\lxe.exe"
Copy-Item (Join-Path (Split-Path -Parent $Lxe) 'lua51.dll') "$S\app\"
# A .lef run is a COMPILED run, so import("thread") skips flat modules\ folders
# and asks the store: give it a store of its own (LXE_HOME) holding thread, so
# the test reads nothing of the user's ~\.lxe and never reaches the network.
Copy-Item $thread "$S\home\modules\thread\1.0.0\thread.dll"
# try/finally: with -ErrorAction Stop any throw below must still restore the
# developer's LXE_HOME (unset stays unset) and remove the temp dir.
$savedHome = $env:LXE_HOME
$hadHome = Test-Path Env:\LXE_HOME
$env:LXE_HOME = "$S\home"
try {
1..300 | ForEach-Object { Set-Content "$S\src\f$_.lua" "return $_" }
Set-Content "$S\src\main.lua" @'
-- Only arguments cross into a worker: thread:load dumps the function, and a
-- dumped chunk keeps NO upvalues.
local new_thread = import("thread")
assert(type(new_thread) == "function", "import(\"thread\") must be its constructor")
local counter_file, limit, done_file = arg[1], tonumber(arg[2]), arg[3]

local n = 0
local f = io.open(counter_file, "r")
if f then n = tonumber(f:read("*a")) or 0; f:close() end
n = n + 1
f = assert(io.open(counter_file, "w")); f:write(tostring(n)); f:close()

-- Positive control, every state: a find_chunk that always misses would still
-- pass the stress verdict, so prove the loader serves real payload chunks.
-- (f1..f300 each return their number; the modules. prefix path is NOT covered
-- here - compile embeds them bare, and import() covers the packaged layout.)
assert(require("f123") == 123, "payload loader must serve bundled chunks")

for _ = 1, 4 do
  local t = new_thread()
  t:load(function()
    local k = 0
    while true do
      k = k + 1
      -- not in the payload: walks every loader, lxe's walks LefFile::loaded
      pcall(require, "tb399_no_such_module_" .. k)
    end
  end)
  t:run()
end

if n >= limit then
  -- A FILE, not stdout: lua51's stdout is fully buffered on a pipe, and output
  -- from a restarted state is not reliably flushed, so it proves nothing.
  local d = assert(io.open(done_file, "w")); d:write("done after ", n, " states"); d:close()
  return
end
sleep(20) -- milliseconds: let the readers get going before the list is rewritten
env.reload()
'@
Push-Location "$S\src"
try {
    & "$S\app\lxe.exe" compile . -m main.lua -o "$S\app\probe.lef" | Out-Null
} finally { Pop-Location }
if (-not (Test-Path "$S\app\probe.lef")) { Write-Host "FAIL could not compile the payload"; exit 1 }

$failures = 0
for ($i = 1; $i -le $Runs; $i++) {
    # Per run: a run that inherited another run's counter would finish at once.
    $counter = "$S\counter-$i.txt"
    $done = "$S\done-$i.txt"
    $r = Invoke-Tb399Child "$S\app\lxe.exe" @('run', "$S\app\probe.lef", $counter, "$Restarts", $done) "$S\app" $TimeoutSec
    $marker = if (Test-Path $done) { Get-Content $done -Raw } else { "" }
    $ok = $r.Outcome -eq 'exit' -and $r.Code -eq 0 -and $marker -eq "done after $Restarts states"
    if ($ok) {
        Write-Host "ok   run $i : $Restarts states under 4 live payload readers"
    } else {
        $failures++
        $reached = if (Test-Path $counter) { (Get-Content $counter -Raw) } else { "0" }
        Write-Host "FAIL run $i : $(Format-Tb399Outcome $r) after $reached of $Restarts states"
        ($r.Output -split "`n" | Select-Object -First 12) | ForEach-Object { Write-Host "       $_" }
    }
}
} finally {
    if ($hadHome) { $env:LXE_HOME = $savedHome } else { Remove-Item Env:\LXE_HOME -EA SilentlyContinue }
    Remove-Item -Recurse -Force $S -EA SilentlyContinue
}
if ($failures -gt 0) { Write-Host "$failures of $Runs run(s) failed"; exit 1 }
Write-Host "all $Runs runs clean"
exit 0