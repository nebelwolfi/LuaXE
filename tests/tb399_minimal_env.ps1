# TB-399 item 9: child lxe with a SystemRoot+SystemDrive-only env block must not
# die at teardown when it loaded a native module.
#
#   powershell -File tests\tb399_minimal_env.ps1 [-Lxe bin\lxe.exe] [-WorkDir <dir>]
#
# Matrix: env {minimal (SystemRoot+SystemDrive), +COMSPEC, +PATH, full} x
# module {none, thread, win32}. The child loadlibs the module and exits; the
# verdict is a CLEAN exit 0 with BOTH markers ("loaded <module>" proves the
# module actually loaded - a staging failure must not read as a crash - and
# "teardown clean" proves lua_close survived it).
#
# Background (TB-263 shape; TB-245's comment was not readable from here):
# a child `lxe run` with a non-inheriting env of only SystemRoot+SystemDrive,
# loading win32.dll, died at teardown 0xC0000005 in ucrtbase (wcsnset <-
# ungetwch_nolock <- mbctolower, via luaopen_io/lua_close). Adding COMSPEC or
# PATH made it go away; LuaHarness mitigates with COMSPEC in MINIMAL_INHERIT.
#
# VERDICT (2026-10-05, this box): NOT REPRODUCED. The full matrix (4 envs x
# 3 modules = 12 cells) passes on BOTH the fixed build (tb-399-runtime tip at
# the time) and the unmodified baseline (stock 5020bdd build): 12/12 cells
# clean either way, including minimal env + win32 + io exercise. DLLs:
# win32.dll sha256 adda9942... (printed per run), thread.dll sha256 e2619946....
# So this test is kept as the bisect probe, not as proof of a fix: no code
# change, no workaround, and the COMSPEC mitigation stays exactly as it is.
#
# Exits 0 when every cell exits 0, 1 otherwise.
param(
    [string]$Lxe = "",
    [string]$Win32Dll = "",
    [int]$TimeoutSec = 60,
    [string]$WorkDir = ""
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'tb399_common.ps1')
$repo = Split-Path -Parent $PSScriptRoot
if ($Lxe -eq "") { $Lxe = Join-Path $repo 'bin\lxe.exe' }
$Lxe = (Resolve-Path $Lxe).Path

$S = New-Tb399Dir $WorkDir 'tb399-minimal-env'
$failures = 0
try {
New-Item -ItemType Directory -Force "$S\app" | Out-Null
Copy-Item $Lxe "$S\app\lxe.exe"
Copy-Item (Join-Path (Split-Path -Parent $Lxe) 'lua51.dll') "$S\app\"
# The module under test: win32.dll is the card's repro (TB-263: teardown
# 0xC0000005 in ucrtbase via lua_close after loadlib under SystemRoot-only
# env); thread.dll is the control (a /MT module of ours that must stay clean).
# Fail fast when win32.dll is missing: staging thread.dll AS win32.dll would
# fail every win32 cell with a loadlib assert, misread as the crash itself.
# (Review of 791a7c2: the first draft silently substituted thread.dll.)
$defaultWin32 = "D:\LuaXE\modules\Win32\cmake-build-release\win32.dll"
if ($Win32Dll -eq "") { $Win32Dll = $defaultWin32 }
if (-not (Test-Path $Win32Dll)) { throw "no win32.dll: pass -Win32Dll <path> (the Win32 module's build)" }
$win32 = (Resolve-Path $Win32Dll).Path
$win32Hash = (Get-FileHash $win32 -Algorithm SHA256).Hash.ToLower()
$thread = Find-ThreadDll ""
Write-Host "win32.dll: $win32"
Write-Host "win32.dll sha256: $win32Hash"
Copy-Item $win32 "$S\app\win32.dll"
Copy-Item $thread "$S\app\thread.dll"
Set-Content "$S\app\probe.lua" @'
local which = arg[1]
if which ~= "none" then
  local lib = assert(package.loadlib(which .. ".dll", "luaopen_" .. which))
  lib()
  io.write("loaded ", which, "\n")
else
  io.write("no module\n")
end
-- The TB-263 stack died in ucrtbase's mbc tables via luaopen_io/lua_close:
-- exercise io (and the module, if it has test entry points) before teardown.
-- os.tmpname() without TEMP falls back to C:\Windows (denied): use the cwd.
local tmp = "tb399_io.tmp"
local f = assert(io.open(tmp, "w"))
f:write("hello \228\184\173\230\150\135\n")
f:close()
local g = assert(io.open(tmp, "r"))
assert(g:read("*a") == "hello \228\184\173\230\150\135\n")
g:close()
os.remove(tmp)
collectgarbage("collect")
io.write("teardown clean\n")
'@
$sysRoot = [System.Environment]::GetEnvironmentVariable('SystemRoot')
$sysDrive = [System.Environment]::GetEnvironmentVariable('SystemDrive')
$comspec = [System.Environment]::GetEnvironmentVariable('COMSPEC')
$path = [System.Environment]::GetEnvironmentVariable('PATH')
$envSets = @(
    @{ Name = 'minimal';           Vars = @{ SystemRoot = $sysRoot; SystemDrive = $sysDrive } },
    @{ Name = 'minimal+COMSPEC';   Vars = @{ SystemRoot = $sysRoot; SystemDrive = $sysDrive; COMSPEC = $comspec } },
    @{ Name = 'minimal+PATH';      Vars = @{ SystemRoot = $sysRoot; SystemDrive = $sysDrive; PATH = $path } },
    @{ Name = 'full';              Vars = $null }
)
foreach ($module in @('none', 'thread', 'win32')) {
    foreach ($set in $envSets) {
        $r = Invoke-Tb399Child "$S\app\lxe.exe" @('run', "$S\app\probe.lua", $module) "$S\app" $TimeoutSec `
            -Environment $set.Vars
        $envName = $set.Name
        $loaded = if ($module -eq 'none') { 'no module' } else { "loaded $module" }
        if ($r.Outcome -eq 'exit' -and $r.Code -eq 0 -and $r.Output -like "*$loaded*" `
            -and $r.Output -like '*teardown clean*') {
            Write-Host "ok   env=$envName module=$module : clean exit"
        } else {
            $failures++
            Write-Host "FAIL env=$envName module=$module : $(Format-Tb399Outcome $r)"
            ($r.Output -split "`n" | Select-Object -First 8) | ForEach-Object { Write-Host "       $_" }
        }
    }
}
} finally {
    Remove-Item -Recurse -Force $S -EA SilentlyContinue
}
if ($failures -gt 0) { Write-Host "$failures cell(s) failed"; exit 1 }
Write-Host "every env x module cell exits clean"
exit 0
