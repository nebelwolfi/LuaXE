# Shared helpers for the TB-399 regression tests (tests\tb399_*.ps1).
#
# Every child runs as its OWN System.Diagnostics.Process with a HARD deadline;
# on a timeout it is killed by PID (taskkill /PID <pid> /T /F) - never by name,
# so a test can never touch another lxe on the machine. Works in Windows
# PowerShell 5.1 and in pwsh 7.

function New-Tb399Dir([string]$WorkDir, [string]$Name) {
    if ($WorkDir -eq "") { $WorkDir = [IO.Path]::GetTempPath() }
    $dir = Join-Path $WorkDir ("lxe-$Name-" + [guid]::NewGuid())
    New-Item -ItemType Directory -Force $dir | Out-Null
    return $dir
}

# A thread.dll to stage for the tests that need workers: -ThreadDll, else the
# highest version in the lxe store (read only).
function Find-ThreadDll([string]$Given) {
    if ($Given -ne "" -and (Test-Path $Given)) { return (Resolve-Path $Given).Path }
    $store = Join-Path $env:USERPROFILE ".lxe\modules\thread"
    $found = Get-ChildItem $store -Recurse -Filter thread.dll -EA SilentlyContinue |
             Sort-Object { [version]($_.Directory.Name -replace '[^0-9.]', '') } -Descending |
             Select-Object -First 1
    if ($found) { return $found.FullName }
    throw "no thread.dll: pass -ThreadDll <path> (the Thread module's build)"
}

# Runs <exe> <arguments> in <cwd> with a hard deadline. Returns
# @{ Outcome = 'exit' | 'timeout'; Code = <int or $null>; Output = <text> }.
# $Environment: $null = inherit; otherwise a hashtable that is the WHOLE block.
function Invoke-Tb399Child([string]$Exe, [string[]]$Arguments, [string]$Cwd,
                           [int]$TimeoutSec, $Environment = $null) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.Arguments = ($Arguments | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $psi.WorkingDirectory = $Cwd
    $psi.UseShellExecute = $false
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    if ($null -ne $Environment) {
        $psi.EnvironmentVariables.Clear()
        foreach ($k in $Environment.Keys) { $psi.EnvironmentVariables[$k] = [string]$Environment[$k] }
    }
    $proc = New-Object System.Diagnostics.Process
    $proc.StartInfo = $psi
    $buffer = New-Object System.Text.StringBuilder
    $onData = { if ($null -ne $EventArgs.Data) { [void]$Event.MessageData.AppendLine($EventArgs.Data) } }
    $subs = @(
        Register-ObjectEvent -InputObject $proc -EventName OutputDataReceived -Action $onData -MessageData $buffer
        Register-ObjectEvent -InputObject $proc -EventName ErrorDataReceived  -Action $onData -MessageData $buffer
    )
    [void]$proc.Start()
    $proc.BeginOutputReadLine(); $proc.BeginErrorReadLine()
    $proc.StandardInput.Close()
    $result = @{ Outcome = 'exit'; Code = $null; Output = '' }
    if ($proc.WaitForExit($TimeoutSec * 1000)) {
        $proc.WaitForExit() # drains the async readers
        $result.Code = $proc.ExitCode
    } else {
        & taskkill /PID $proc.Id /T /F 2>&1 | Out-Null
        try { [void]$proc.WaitForExit(5000) } catch {}
        $result.Outcome = 'timeout'
    }
    Start-Sleep -Milliseconds 100
    foreach ($s in $subs) { Unregister-Event -SourceIdentifier $s.Name -EA SilentlyContinue }
    $result.Output = $buffer.ToString()
    return $result
}

function Format-Tb399Outcome($r) {
    if ($r.Outcome -eq 'timeout') { return 'TIMEOUT' }
    switch ($r.Code) {
        0           { 'exit 0' }
        -1073741819 { 'ACCESS_VIOLATION (0xC0000005)' }
        -1073740940 { 'HEAP_CORRUPTION (0xC0000374)' }
        -1073740791 { 'STACK_BUFFER_OVERRUN (0xC0000409)' }
        default     { "exit $($r.Code)" }
    }
}