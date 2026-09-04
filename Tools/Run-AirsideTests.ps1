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
#>
[CmdletBinding()]
param(
    [string] $Filter  = 'Airside',
    [string] $Project = 'C:\repos\AirportMgr2\AirportMgr.uproject',
    [string] $Engine  = 'D:\Epic\UE_5.8'
)

$ErrorActionPreference = 'Stop'

$projectDir = Split-Path -Parent $Project
$logPath    = Join-Path $projectDir 'Saved\Logs\AirsideTests.log'

# A stale log would let a crashed run masquerade as the previous green one.
if (Test-Path $logPath) { Remove-Item $logPath -Force }

$editor  = Join-Path $Engine 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$execArg = "-ExecCmds=Automation RunTests $Filter"

& $editor $Project $execArg `
    -unattended -nopause -nosplash -nullrhi `
    -testexit='Automation Test Queue Empty' `
    -abslog="$logPath" | Out-Null

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

exit 0
