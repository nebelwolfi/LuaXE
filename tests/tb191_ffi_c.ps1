# TB-191: a DECLARED ffi.C symbol must close cleanly, not fault at lua_close.
#
#   powershell -File tests\tb191_ffi_c.ps1 [-Lxe bin\lxe.exe] [-Runs 3] [-WorkDir <dir>]
#
# LuaJIT's SusGetProcAddress (inside lua51.dll) resolves every ffi.C.<name> by
# walking six default modules, the first of which is lxe.exe itself. lxe.exe
# used to carry NO export directory (RVA=0,size=0), so the walk read DOS-header
# garbage as NumberOfNames/AddressOfNames and the first name read faulted
# (0xC0000005 inside lua_close, when the looked-up cdata was freed). lxe now
# exports one anchor (luaxe_export_anchor, PRIVATE): the dllexport on the
# anchor in src/main.cpp emits the entry (CMake/Ninja passes no .def for an
# executable target); src/lxe_exports.def pins the PRIVATE attribute and
# documents the contract.
#
# Measured:
#   unfixed lxe (D:\LuaXE\bin\lxe.exe, 2026-10-05): every run 0xC0000005
#   fixed clone (this branch): every run exits 0 with TB191-FFI-C-OK
#
# Exits 0 when every run exits 0 within its deadline, prints the marker, shows
# no crash-report signature, and the export table holds exactly the anchor;
# 1 otherwise.
param(
    [string]$Lxe = "",
    [int]$Runs = 3,
    [int]$TimeoutSec = 30,
    [string]$WorkDir = ""
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'tb399_common.ps1')
$repo = Split-Path -Parent $PSScriptRoot
if ($Lxe -eq "") { $Lxe = Join-Path $repo 'bin\lxe.exe' }
$Lxe = (Resolve-Path $Lxe).Path

$S = New-Tb399Dir $WorkDir 'tb191-ffi-c'
try {
    Set-Content -Encoding ascii "$S\probe.lua" @'
io.stdout:setvbuf("no")
-- A lookup alone arms it (step2/step4), but the card's shape is a CALL.
local ffi = require("ffi")
ffi.cdef[[ unsigned long long __stdcall GetTickCount64(void); ]]
local v = ffi.C.GetTickCount64()
assert(tonumber(v) ~= nil, "expected a tick count back")
-- An undeclared symbol must stay a clean error, not a crash.
local ok, err = pcall(function() return ffi.C.Tb191NoSuchSymbol() end)
assert(not ok and err:find("missing declaration"), "expected a missing-declaration error")
print("TB191-FFI-C-OK")
'@

    $failures = 0
    # A crashed child prints lxe's own crash report ("An uncaught exception
    # occurred." / "Exception Code: 0x..."): fail on that signature too, so the
    # verdict does not depend on WHICH fault finally kills the process.
    $crashSig = 'An uncaught exception occurred\.|Exception Code: 0x'
    for ($i = 1; $i -le $Runs; $i++) {
        $r = Invoke-Tb399Child $Lxe @('run', "$S\probe.lua") $S $TimeoutSec
        $ok = $r.Outcome -eq 'exit' -and $r.Code -eq 0 -and ($r.Output -match 'TB191-FFI-C-OK') -and ($r.Output -notmatch $crashSig)
        if ($ok) {
            Write-Host "ok   run $i : declared ffi.C call closes cleanly"
        } else {
            $failures++
            Write-Host "FAIL run $i : $(Format-Tb399Outcome $r)"
            ($r.Output -split "`n" | Select-Object -First 12) | ForEach-Object { Write-Host "       $_" }
        }
    }

    # A second guard: the export table must hold EXACTLY the anchor. A future
    # change exporting a lua_* stub (e.g. compiling with LUA_BUILD_AS_DLL+LUA_CORE)
    # would leave a valid directory - no crash, the probe above would pass - while
    # every ffi.C.lua_* lookup silently bound to lxe's stub instead of lua51.dll.
    # The table is read from the built exe with dumpbin when present, else parsed
    # from the PE headers directly; either way no third-party module is needed.
    $exportsError = ""
    $exportNames = @()
    $dumpbin = Get-Command dumpbin.exe -EA SilentlyContinue
    if ($dumpbin) {
        $out = & dumpbin.exe /exports $Lxe 2>&1
        if ($LASTEXITCODE -ne 0) { $exportsError = "dumpbin /exports failed" }
        else {
            $inList = $false
            foreach ($line in ($out -split "`n")) {
                if ($line -match '^\s+ordinal\s+hint\s+RVA\s+name') { $inList = $true; continue }
                if ($inList -and ($line -match '^\s*\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)')) { $exportNames += $Matches[1] }
            }
        }
    } else {
        $bytes = [IO.File]::ReadAllBytes($Lxe)
        $getU16 = { param($o) $bytes[$o] + $bytes[$o + 1] * 256 }
        $getU32 = { param($o) $bytes[$o] + $bytes[$o + 1] * 256 + $bytes[$o + 2] * 65536 + $bytes[$o + 3] * 16777216 }
        $e_lfanew = &$getU32 0x3C
        $nsec = &$getU16 ($e_lfanew + 6); $optsize = &$getU16 ($e_lfanew + 20)
        $exRva = &$getU32 ($e_lfanew + 24 + 112); $exSize = &$getU32 ($e_lfanew + 24 + 116)
        if ($exRva -eq 0 -or $exSize -eq 0) { $exportsError = "no export directory (RVA=0/size=0): the TB-191 crash is back" }
        else {
            $secOff = $e_lfanew + 24 + $optsize
            $rva2off = {
                param($rva)
                for ($i = 0; $i -lt $nsec; $i++) {
                    $o = $secOff + $i * 40
                    $vaddr = &$getU32 ($o + 12); $vsize = &$getU32 ($o + 8); $rawsz = &$getU32 ($o + 16); $rawptr = &$getU32 ($o + 20)
                    $span = [Math]::Max($vsize, $rawsz)
                    if ($rva -ge $vaddr -and $rva -lt $vaddr + $span) { return $rawptr + ($rva - $vaddr) }
                }
                return $null
            }
            $eo = &$rva2off $exRva
            if ($null -eq $eo) { $exportsError = "export directory outside any section" }
            else {
                $nNames = &$getU32 ($eo + 24); $aon = &$getU32 ($eo + 32)
                $aonOff = &$rva2off $aon
                for ($i = 0; $i -lt $nNames; $i++) {
                    $nameRva = &$getU32 ($aonOff + $i * 4)
                    $nameOff = &$rva2off $nameRva
                    $e = $nameOff; while ($bytes[$e] -ne 0) { $e++ }
                    $exportNames += [Text.Encoding]::ASCII.GetString($bytes, $nameOff, $e - $nameOff)
                }
            }
        }
    }
    if ($exportsError -eq "" -and !($exportNames.Count -eq 1 -and $exportNames[0] -eq 'luaxe_export_anchor')) {
        $exportsError = "expected exactly [luaxe_export_anchor], got [$(($exportNames -join ', '))]"
    }
    if ($exportsError -ne "") {
        $failures++
        Write-Host "FAIL export table : $exportsError"
    }
} finally {
    Remove-Item -Recurse -Force $S -EA SilentlyContinue
}
if ($failures -gt 0) { Write-Host "$failures of $Runs run(s) failed"; exit 1 }
Write-Host "all $Runs runs clean"
exit 0
