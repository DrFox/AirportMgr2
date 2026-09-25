<#
.SYNOPSIS
    Runs the Airside automation tests headless and exits non-zero if any failed.

.DESCRIPTION
    Unreal's automation runner exits 0 whether tests pass or fail when -testexit
    is used, so the process exit code cannot be trusted. This script parses the
    run's log for "Test Completed. Result={...}" lines and derives a real verdict.

.EXAMPLE
    ./Tools/Run-AirsideTests.ps1
    ./Tools/Run-AirsideTests.ps1 -Filter Airside.Solve
    ./Tools/Run-AirsideTests.ps1 -Filter AirportOps.Model
    ./Tools/Run-AirsideTests.ps1 -Filter AirportMgr.Actions
#>
[CmdletBinding()]
param(
    # Both plugins and the game module by default. '+' is the automation runner's own list separator
    # (AutomationCommandline.cpp splits RunTests arguments on it), so one cold editor
    # start covers both suites.
    [string] $Filter  = 'Airside+AirportOps+AirportMgr',
    # Derived from the script's own location, not the hardcoded main checkout: this script
    # runs from git worktrees too, each with its own .uproject beside its own Tools/, and a
    # fixed C:\repos\AirportMgr2 default silently tested the WRONG checkout's DLLs whenever
    # it was reached from one of those.
    [string] $Project = (Join-Path (Split-Path -Parent $PSScriptRoot) 'AirportMgr.uproject'),
    [string] $Engine  = 'D:\Epic\UE_5.8',
    # Issue #291: a hung editor (no crash, no exit, nothing left for the automation runner's
    # own -testexit to catch) used to block this script forever with no diagnostic at all - a
    # hang is a crash with no callstack, and 30 minutes is generously past this suite's normal
    # runtime as of 2026-09-25.
    [int] $TimeoutSeconds = 1800
)

$ErrorActionPreference = 'Stop'

$projectDir = Split-Path -Parent $Project

# The architecture rules first, because they take a second and the editor takes minutes:
# an include cycle or a duplicated log category is a verdict on the commit whatever the
# tests say, and finding it before a cold editor start is the whole point of a lint.
$lintScript = Join-Path $PSScriptRoot 'Check-Architecture.ps1'

# Issue #291: a script that fails to PARSE (a bad merge conflict resolve, a stray brace) used
# to leave $LASTEXITCODE at whatever the PREVIOUS invocation set it to - `& $lintScript; if
# ($LASTEXITCODE -ne 0)` then read the WRONG run's verdict and let a no-op lint pass the gate,
# exactly the incident this issue reports (a ParseException printed where the verdict goes, no
# tests ran, exit 0). ParseFile catches the break before the lint is even invoked.
$lintParseErrors = $null
[System.Management.Automation.Language.Parser]::ParseFile($lintScript, [ref]$null, [ref]$lintParseErrors) | Out-Null
if ($lintParseErrors.Count -gt 0) {
    Write-Host 'FAIL: Check-Architecture.ps1 does not parse; the gate cannot run:' -ForegroundColor Red
    foreach ($e in $lintParseErrors) { Write-Host "  $e" -ForegroundColor Red }
    exit 1
}

# Belt-and-suspenders past the parse check above: a script that PARSES but throws mid-run (an
# unhandled exception, a path that no longer exists) can ALSO leave $LASTEXITCODE stale with no
# verdict printed - the exit code alone cannot tell "ran clean" from "never got to the end".
# Capture the lint's own output (Write-Host lines land on the Information stream, stream 6;
# *>&1 folds every stream into the one this variable assignment can see) into a log file so it
# is both visible on the console (Tee-Object) and greppable for a real verdict line afterward.
$lintLogPath = Join-Path $projectDir 'Saved\Logs\CheckArchitecture.log'
# A checkout the editor has never opened (a fresh worktree, say) has no Saved\Logs\ yet -
# Tee-Object does not create parent directories and would fail before the lint even ran.
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $lintLogPath) | Out-Null
& $lintScript -Root $projectDir *>&1 | Tee-Object -FilePath $lintLogPath
$lintExitCode = $LASTEXITCODE
$lintOutput = if (Test-Path $lintLogPath) { Get-Content -Path $lintLogPath } else { @() }
$lintVerdict = $lintOutput | Where-Object { $_ -match '^Check-Architecture: (PASS|\d+ failure)' }
if (-not $lintVerdict) {
    Write-Host 'FAIL: Check-Architecture.ps1 printed no verdict line - a silent stop is a failure, not a pass (issue #291).' -ForegroundColor Red
    exit 1
}
if ($lintExitCode -ne 0) {
    Write-Host 'FAIL: Check-Architecture.ps1 found violations; tests not run.' -ForegroundColor Red
    exit 1
}
$logPath     = Join-Path $projectDir 'Saved\Logs\AirsideTests.log'
$prevLogPath = Join-Path $projectDir 'Saved\Logs\AirsideTests-prev.log'

# A stale log would let a crashed run masquerade as the previous green one - but deleting it
# outright loses the ONE thing a flaky, order-dependent failure needs: the raw log from the
# run that actually failed. Issue #216's third sighting had nothing but the automation
# report's single error line to go on, because this script had already overwritten the log
# by the time anyone went looking. One generation is enough - this script is never run twice
# unread - so rename (not copy) the previous run out of the way before starting a new one.
if (Test-Path $logPath) {
    if (Test-Path $prevLogPath) { Remove-Item $prevLogPath -Force }
    Move-Item -Path $logPath -Destination $prevLogPath -Force
}

$editor  = Join-Path $Engine 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'

# THE QUOTE GOES RIGHT AFTER `=`, AROUND ONLY THE VALUE - verified 2026-09-25 by diffing this
# script's own working run's `LogInit: Command Line:` against a broken one: the old call-operator
# form `-testexit='Automation Test Queue Empty'` reached UnrealEditor-Cmd.exe as literally
# `-testexit="Automation Test Queue Empty"` (PowerShell's legacy native-argument quoting rewrites
# a single-quoted literal into a double-quoted one wrapping just the value, because that value
# came from a quoted string token). A first draft of this fix dropped the quotes entirely (they
# "looked like" PowerShell syntax sugar) and FParse::Value(-testexit=) then read only up to the
# first space - `Automation` alone - and the editor exited before any test ran. Built here
# explicitly rather than left to `&`'s own quoting so Start-Process/System.Diagnostics.Process
# reproduce it exactly, byte for byte.
$execCmdsArg  = "-ExecCmds=`"Automation RunTests $Filter`""
$testExitArg  = "-testexit=`"Automation Test Queue Empty`""
$abslogArg    = "-abslog=`"$logPath`""

# System.Diagnostics.Process directly (not Start-Process -ArgumentList): -ArgumentList treats
# each array element as its OWN token and re-escapes it per Windows argv rules, which would
# re-quote the embedded `"..."` above a second time and corrupt it. Setting .Arguments to one
# raw string is used VERBATIM - the same contract the old `&` form relied on - and still gives a
# process handle to WaitForExit against, so a hang can be detected and killed instead of
# blocking this script - and by extension whatever CI or agent invoked it - forever.
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $editor
$psi.Arguments = "`"$Project`" $execCmdsArg -unattended -nopause -nosplash -nullrhi $testExitArg $abslogArg"
$psi.UseShellExecute = $false
$psi.CreateNoWindow = $true
$editorProcess = [System.Diagnostics.Process]::Start($psi)
if (-not $editorProcess.WaitForExit($TimeoutSeconds * 1000)) {
    Write-Host "FAIL: the editor did not exit within ${TimeoutSeconds}s - a hang is a crash with no callstack." -ForegroundColor Magenta
    Stop-Process -Id $editorProcess.Id -Force -ErrorAction SilentlyContinue
    Write-Host "Log: $logPath"
    exit 1
}

if (-not (Test-Path $logPath)) {
    Write-Host 'FAIL: the engine produced no log; the run did not start.' -ForegroundColor Red
    exit 1
}

$completed = Select-String -Path $logPath -Pattern 'Test Completed\. Result=\{(\w+)\}\s+Name=\{([^}]*)\}'

if ($completed.Count -eq 0) {
    Write-Host "FAIL: no tests matched filter '$Filter'. A suite that runs nothing is not a suite that passes." -ForegroundColor Red
    Write-Host "Log: $logPath"
    exit 1
}

# A test that STARTS and never COMPLETES has crashed. The completed-list alone cannot see
# that: the run just comes back one test shorter and every remaining test green, which is
# indistinguishable from a suite that legitimately shrank. Seen for real on 2026-08-31,
# where a crashing StandPlace reported as "28 test(s) run, 0 failed".
$started = Select-String -Path $logPath -Pattern 'Test Started\. Name=\{([^}]*)\}' |
    ForEach-Object { $_.Matches[0].Groups[1].Value } | Select-Object -Unique
$finished = $completed | ForEach-Object { $_.Matches[0].Groups[2].Value } | Select-Object -Unique
$crashed = @($started | Where-Object { $finished -notcontains $_ })

$failed = @()
foreach ($line in $completed) {
    $result = $line.Matches[0].Groups[1].Value
    $name   = $line.Matches[0].Groups[2].Value
    if ($result -eq 'Success') {
        Write-Host "  PASS  $name" -ForegroundColor Green
    }
    else {
        Write-Host "  $result  $name" -ForegroundColor Red
        $failed += $name
    }
}

foreach ($name in $crashed) {
    Write-Host "  CRASH $name  (started, never completed)" -ForegroundColor Magenta
}

Write-Host ''
Write-Host "$($completed.Count) test(s) run, $($failed.Count) failed, $($crashed.Count) crashed."

if ($crashed.Count -gt 0) {
    Write-Host ''
    Write-Host 'Crash detail:' -ForegroundColor Magenta
    Select-String -Path $logPath -Pattern '\[Callstack\]' |
        Select-Object -First 15 | ForEach-Object { Write-Host "  $($_.Line.Trim())" }
    Write-Host ''
    Write-Host "Log: $logPath"
    exit 1
}

if ($failed.Count -gt 0) {
    Write-Host ''
    Write-Host 'Failure detail:' -ForegroundColor Red
    Select-String -Path $logPath -Pattern 'LogAutomationController: Error:' |
        ForEach-Object { Write-Host "  $($_.Line.Trim())" }
    Write-Host ''
    Write-Host "Log: $logPath"
    exit 1
}

# Issue #291: a crash AFTER the last "Test Completed" line - during teardown, say - leaves
# every test green and $crashed empty, because the started/finished diff above only sees a
# crash that happens mid-suite. Grep the raw log for the engine's own crash markers even on
# this exit-0 path, so a green run that ends in a callstack is not reported as a pass.
$teardownCrash = Select-String -Path $logPath -Pattern '\[Callstack\]|Fatal error' -ErrorAction SilentlyContinue
if ($teardownCrash.Count -gt 0) {
    Write-Host ''
    Write-Host 'FAIL: the log contains a callstack/fatal error after every test reported green - a crash during teardown.' -ForegroundColor Magenta
    $teardownCrash | Select-Object -First 15 | ForEach-Object { Write-Host "  $($_.Line.Trim())" }
    Write-Host ''
    Write-Host "Log: $logPath"
    exit 1
}

exit 0
