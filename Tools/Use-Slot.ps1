<#
.SYNOPSIS
    Puts a new branch in a WARM worktree instead of a fresh one, so its first build is incremental.

.DESCRIPTION
    A fresh `git worktree add` has an empty Intermediate/ and its first build compiles every module
    and PCH (~4 min, inferred 2026-09-27). Every build on this machine also queues on ONE UBT mutex,
    keyed on UnrealBuildTool's own path, not the project's (UnrealBuildTool.cs:446 in 5.8), so
    parallel sessions' cold builds run one after another. A worktree that has built before only
    recompiles what `git switch` touched: warm UBT logs on 2026-09-30 were 5.7 s and 10.6 s.

    Why not copy Intermediate/ from the main checkout into a new worktree: UBT's .dep.json files
    and makefile hold ABSOLUTE paths, so a copy would miss changes to the worktree's own headers.

    The pool is every C:\repos\airportmgr2-* worktree. Directory names no longer match the branch
    (the 2026-09-23 airportmgr2-<branch> convention is superseded). A slot is FREE when all hold:
      - its working tree is clean (dirty = another session is mid-change),
      - no UnrealEditor process has a path inside it (a test run or open editor),
      - its branch has no open PR (review fixes still land there) and nothing unpushed,
      - no claim younger than $ClaimHours (the window between a switch and the first edit,
        when a slot looks clean but is taken).
    The claim lives in the worktree's own git admin dir (.git/worktrees/<name>), so it never
    makes the tree dirty, and it is created with CreateNew so two sessions racing for one slot
    cannot both win.

.EXAMPLE
    ./Tools/Use-Slot.ps1 -Branch feature/foo            # prints the slot path on the last line
    ./Tools/Use-Slot.ps1 -List                          # show every slot and why it is busy
    ./Tools/Use-Slot.ps1 -Branch feature/foo -Slot C:\repos\airportmgr2-slot2
#>
[CmdletBinding(DefaultParameterSetName = 'Take')]
param(
    [Parameter(ParameterSetName = 'Take', Mandatory)] [string] $Branch,
    [Parameter(ParameterSetName = 'Take')] [string] $Slot,
    [Parameter(ParameterSetName = 'Take')] [string] $Base = 'origin/main',
    [Parameter(ParameterSetName = 'List', Mandatory)] [switch] $List,
    [string] $PoolRoot = 'C:\repos',
    [string] $Prefix = 'airportmgr2-',
    [double] $ClaimHours = 2
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

function Get-Pool {
    $lines = git -C $repo worktree list --porcelain
    $paths = $lines | Where-Object { $_ -like 'worktree *' } | ForEach-Object { ($_ -replace '^worktree ', '') -replace '/', '\' }
    $paths | Where-Object { (Split-Path -Leaf $_) -like "$Prefix*" -and (Split-Path -Parent $_) -eq $PoolRoot }
}

function Get-ClaimPath([string] $path) {
    $gitDir = git -C $path rev-parse --absolute-git-dir
    Join-Path $gitDir 'slot-claim'
}

# Returns $null when free, else the reason it is busy.
function Get-BusyReason([string] $path, $editors) {
    if (git -C $path status --porcelain) { return 'dirty working tree' }
    if ($editors | Where-Object { $_ -and $_.IndexOf($path, [StringComparison]::OrdinalIgnoreCase) -ge 0 }) {
        return 'an UnrealEditor process is running on it'
    }
    $claim = Get-ClaimPath $path
    if ((Test-Path $claim) -and ((Get-Date) - (Get-Item $claim).LastWriteTime).TotalHours -lt $ClaimHours) {
        return "claimed: $(Get-Content $claim -Raw)".Trim()
    }
    $branch = git -C $path branch --show-current
    if ($branch) {
        $open = gh pr list --head $branch --state open --json number -q '.[].number' 2>$null
        if ($open) { return "branch $branch has open PR #$open" }
        # Unpushed work: commits on the branch that neither its upstream nor origin/main has.
        $upstream = git -C $path rev-parse --abbrev-ref '@{u}' 2>$null
        $against = if ($LASTEXITCODE -eq 0 -and $upstream) { @($upstream, 'origin/main') } else { @('origin/main') }
        $ahead = git -C $path rev-list --count HEAD --not @against
        if ([int] $ahead -gt 0) {
            # A squash-merged branch is never an ancestor of main, so "ahead" alone would pin every
            # merged slot forever; a merged PR for this branch says the commits are safe.
            $merged = gh pr list --head $branch --state merged --json number -q '.[].number' 2>$null
            if (-not $merged) { return "branch $branch has $ahead commit(s) not on origin and no merged PR" }
        }
    }
    $null
}

$editors = @(Get-CimInstance Win32_Process -Filter "Name LIKE 'UnrealEditor%'" | ForEach-Object { $_.CommandLine })
git -C $repo fetch -q origin

if ($List) {
    foreach ($p in Get-Pool) {
        $why = Get-BusyReason $p $editors
        $branch = git -C $p branch --show-current
        '{0,-45} {1,-35} {2}' -f $p, ($branch ? $branch : '(detached)'), ($why ? "BUSY: $why" : 'free')
    }
    return
}

if (git -C $repo branch --list $Branch) {
    throw "Branch $Branch already exists locally; switch to it where it lives rather than taking a slot."
}

$chosen = $null
$candidates = if ($Slot) { @($Slot) } else { @(Get-Pool) }
foreach ($p in $candidates) {
    $why = Get-BusyReason $p $editors
    if ($why) { Write-Host "skip $p - $why"; continue }
    # CreateNew is the atomic step: of two sessions that both saw this slot free, one throws here.
    $claim = Get-ClaimPath $p
    try {
        if (Test-Path $claim) { Remove-Item $claim -Force }   # stale: older than $ClaimHours
        $fs = [IO.File]::Open($claim, [IO.FileMode]::CreateNew)
        $w = [IO.StreamWriter]::new($fs); $w.Write("$Branch at $(Get-Date -Format s)"); $w.Dispose()
    } catch { Write-Host "skip $p - lost the claim race"; continue }
    $chosen = $p; break
}

if (-not $chosen) {
    if ($Slot) { throw "Slot $Slot is busy." }
    # Pool exhausted: grow it. This one pays the cold build once and is warm ever after.
    $n = 1
    while (Test-Path (Join-Path $PoolRoot "${Prefix}slot$n")) { $n++ }
    $chosen = Join-Path $PoolRoot "${Prefix}slot$n"
    Write-Host "No free slot; creating $chosen (first build is cold)."
    git -C $repo worktree add -q $chosen -b $Branch $Base
    if ($LASTEXITCODE -ne 0) { throw "git worktree add failed" }
    Set-Content -Path (Get-ClaimPath $chosen) -Value "$Branch at $(Get-Date -Format s)" -NoNewline
} else {
    # Switching from detached avoids "branch is checked out elsewhere" for the old branch; the new
    # branch starts exactly at $Base. Only files that differ get a new mtime, which is what keeps
    # the next UBT run incremental.
    $old = git -C $chosen branch --show-current
    git -C $chosen switch -q -c $Branch $Base
    if ($LASTEXITCODE -ne 0) { Remove-Item (Get-ClaimPath $chosen) -Force; throw "git switch failed in $chosen" }
    # The old branch is merged (or had no work): drop it so `git branch` does not grow forever.
    if ($old) { git -C $repo branch -D $old 2>$null | Out-Null }
}

Write-Host "Build with: Build.bat AirportMgrEditor Win64 Development -Project=`"$chosen\AirportMgr.uproject`" -WaitMutex -NoHotReloadFromIDE"
$chosen
