# Acceptance test for env.relaunch and the launcher's relaunch handshake
# (src/relaunch_protocol.h, src/lua/relaunch.cpp, launcher/launcher.c).
#
#   powershell -File tests\relaunch.ps1      (after building bin\lxe.exe and bin\lxe-launcher.exe)
#
# Exits 0 when every case holds, 1 otherwise. Works in a temp folder only.
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$S = Join-Path ([IO.Path]::GetTempPath()) ("lxe-relaunch-test-" + [guid]::NewGuid())
New-Item -ItemType Directory -Force "$S\app","$S\proj\src","$S\ws" | Out-Null
Copy-Item "$repo\bin\lxe.exe","$repo\bin\lua51.dll" "$S\app\"
Copy-Item "$repo\bin\lxe-launcher.exe" "$S\app\app.exe"
$count = Join-Path $S 'count.txt'
Set-Content "$S\proj\src\main.lua" @"
local count_file = [[$count]]
"@
Add-Content "$S\proj\src\main.lua" @'
local mode = arg[1] or ""
local n = 0
local f = io.open(count_file, "r"); if f then n = tonumber(f:read("*a")) or 0; f:close() end
n = n + 1
f = io.open(count_file, "w"); f:write(tostring(n)); f:close()
local parts = {}
for i = 1, #arg do parts[#parts + 1] = "[" .. arg[i] .. "]" end
local pipe = io.popen('cmd /c "set LXE_RELAUNCH 2>nul"')
local leaked = pipe:read("*a"); pipe:close()
print(string.format("RUN %d mode=%s relaunch_mode=%s args=%s child_env=%s", n, mode,
  tostring(env.relaunch_mode), table.concat(parts, " "), (leaked ~= "" and "LEAKED" or "clean")))
if mode == "chain" and n < 4 then
  env.relaunch({ "chain", "n=" .. n, "two words", 'quo"te' })
  return
end
if mode == "magic" then env.exit(19544) end
if mode == "chain" then env.exit(7) end
'@
Push-Location "$S\proj"; & "$S\app\lxe.exe" compile src -m main.lua -o "$S\app\app.lef" -s | Out-Null; Pop-Location

$failures = @()
function Check($name, $condition) {
    if ($condition) { Write-Host "ok   $name" } else { Write-Host "FAIL $name"; $script:failures += $name }
}
$ErrorActionPreference = 'Continue'
Push-Location "$S\ws"
try {
    Remove-Item $count -EA SilentlyContinue
    $before = @(Get-ChildItem ([IO.Path]::GetTempPath()) -Filter 'lxe-relaunch-*.txt' -EA SilentlyContinue).Count
    $out = & "$S\app\app.exe" chain 2>&1 | % { "$_" }
    Check 'launcher: 3 relaunches, then the exit code' ($LASTEXITCODE -eq 7 -and @($out | ? { $_ -like 'RUN *' }).Count -eq 4)
    Check 'launcher: arguments survive spaces and quotes' (($out -join "`n").Contains('[n=3] [two words] [quo"te]'))
    Check 'launcher: the handshake never reaches a grandchild' (-not (($out -join "`n") -like '*LEAKED*'))
    Check 'launcher: relaunch_mode is "launcher"' (($out -join "`n") -like '*relaunch_mode=launcher*')
    $after = @(Get-ChildItem ([IO.Path]::GetTempPath()) -Filter 'lxe-relaunch-*.txt' -EA SilentlyContinue).Count
    Check 'launcher: no handshake file left behind' ($after -le $before)

    Remove-Item $count -EA SilentlyContinue
    & "$S\app\app.exe" magic 2>&1 | Out-Null
    Check 'an unrelated exit 0x4C58 is passed through' ($LASTEXITCODE -eq 19544)

    Remove-Item $count -EA SilentlyContinue
    $p = Start-Process "$S\app\lxe.exe" -ArgumentList "`"$S\app\app.lef`" chain" -NoNewWindow -PassThru `
        -RedirectStandardOutput "$S\nested.out" -RedirectStandardError "$S\nested.err"
    $null = $p.Handle; $p.WaitForExit()
    $nested = Get-Content "$S\nested.out"
    Check 'nested: one waiting parent, 4 runs, exit code passed through' ($p.ExitCode -eq 7 -and @($nested | ? { $_ -like 'RUN *' }).Count -eq 4)
    Check 'nested: the first run says "nested"' (($nested | Select-Object -First 1) -like '*relaunch_mode=nested*')

    Remove-Item $count -EA SilentlyContinue
    Move-Item "$S\app\lxe.exe" "$S\app\lxe.aside"; Move-Item "$S\app\app.lef" "$S\app\app.lef.aside"
    $job = Start-Job { param($d) Start-Sleep -Milliseconds 600; Move-Item "$d\lxe.aside" "$d\lxe.exe"; Move-Item "$d\app.lef.aside" "$d\app.lef" } -ArgumentList "$S\app"
    $saved = $env:USERPROFILE, $env:LXE_HOME, $env:PATH
    $env:USERPROFILE = "$S\ws"; $env:LXE_HOME = "$S\ws"; $env:PATH = "$env:SystemRoot\System32"
    $out = & "$S\app\app.exe" plain 2>&1 | % { "$_" }
    $code = $LASTEXITCODE
    $env:USERPROFILE, $env:LXE_HOME, $env:PATH = $saved
    Wait-Job $job | Out-Null; Remove-Job $job
    Check 'launcher: waits out an update''s swap window' ($code -eq 0 -and (($out -join "`n") -like '*RUN 1 mode=plain*'))

    $size = (Get-Item "$repo\bin\lxe-launcher.exe").Length
    Check "launcher stays under 50 KB ($size bytes)" ($size -lt 50KB)
} finally {
    Pop-Location
    Remove-Item -Recurse -Force $S -EA SilentlyContinue
}
if ($failures.Count) { Write-Host "$($failures.Count) failed"; exit 1 }
Write-Host 'all passed'
exit 0
