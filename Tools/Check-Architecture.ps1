<#
.SYNOPSIS
    Mechanical checks for the rules CLAUDE.md states in prose. Exits non-zero on any hit.

.DESCRIPTION
    Every rule here is one that was broken in the September 2026 review series and went
    unnoticed for weeks because nothing measured it. Prose rules are read once; this runs
    on every Run-AirsideTests.ps1 invocation, which is the pre-commit path.

    Rules:
      1. Include direction. Model/ and Solve/ never include Build|Tool|Present|Entities|
         Content; Tool/ never includes Present|Content; Build/ never includes Present|Tool;
         Solve/ includes only CoreMinimal.h and Solve/. (Issue #31: Model<->Entities and
         Tool<->Present cycles shipped and stayed; issue #104: AirportOps' FuelService.cpp
         resolved a content default itself instead of taking it from Present/; issue #181:
         FPlotPlaceTool reached Content/ directly for the depot kit table, the #78 pattern
         in a new tool, and this rule did not catch it because it only forbade Present/.)
      2. One log category per name across each unity-build module: Airside, AirportOps,
         AirsideEditor, AirsideTests and AirportOpsTests. It is a unity build, so two
         DEFINE_LOG_CATEGORY_STATIC of one name in different .cpp files collide at compile
         time - but only once the two land in the same Module.*.cpp blob, which is why it
         passed locally and failed later. (Issue #35 / PR #39; issue #104 widened this past
         Airside/AirportOps to the editor and test modules, none of which were checked.)
      3. No stacked doc comments in headers: a */ followed by /** with no declaration between
         means one of them lost its subject. (Issue #34.)
      4. The Piper fallback is called from exactly one production site. Any other call of a
         PiperMeridian*() function outside AircraftType.*, UAirsideSettings and, in the test
         modules, AirsideTestFixtures.cpp (TestAirframes::Piper(), #100) is a second source of
         truth for one aeroplane. (Issue #30.)
      5. No hand-built slot-map handle outside RoadSlotMap.h. A handle is {index, generation},
         and RoadSlot::HandleAt is the one function allowed to assemble one - it performs the
         bAlive check a hand-built handle skips. Issue #79 closed this shape three times over;
         issue #173 found it back within a month, in five more places, because nothing linted
         it. Test modules are exempt: their fixtures build handles for slots the production
         accessors have no reason to expose (a deliberately dead one, say), the way rule 4
         exempts AirsideTestFixtures.cpp for the same reason.
      6. FRoadAgent's WaitingOn, BlockedStep, StalledSeconds, RunwayHeld, bAwaitingStand and
         GoalNode are written only through their own mutators (Refuse, ClearArbitration,
         AccrueStall/ResetStall, HoldRunway/ReleaseRunway, SetAwaitingStand/ClearAwaitingStand,
         SetGoal/SetGoalFrom) outside RoadAgent.cpp. Issue #174: 27 direct writes at five call
         sites had each grown its own copy of an invariant (bAwaitingStand implies GoalNode
         set; WaitingOn != 0 implies BlockedStep >= 0), and one of them - GroundTrafficRebuild.
         cpp's replan handover - was a hand-typed re-write of the exact triple ClearArbitration
         was added for in issue #82. Test modules are exempt, the way rules 4 and 5 exempt
         them: a scripted scenario sets up state, it does not enforce production discipline on
         itself.
      7. No FColor/FLinearColor/FSlateColor/FSlateBrush token in Tool/ (Public or Private). Tool/
         describes intent to IToolPreviewSink by MEANING, never colour - see CLAUDE.md's
         Architecture section - and issue #191 found PreviewPalette's colour table, ring radii
         and line weights sitting in Tool/ anyway, despite both its consumers (the game HUD and
         the editor viewport tool) already living in or including Present/. Moved to Present/;
         this rule is what stops the next shared-look table from landing back in Tool/ by habit.
         Comment lines are excluded the way rule 5 excludes them - a WHY comment naming a colour
         to explain why Tool/ does not hold one is not the thing this rule exists to catch.

    Not checked here, deliberately: uninitialised FVector2D locals (issue #46). The idiom
    `FVector2D X; if (!Fill(X)) ...` is legitimate and appears ~60 times as out-params; the
    bug is ignoring the return value, which a regex cannot see. The rule lives in CLAUDE.md.

.PARAMETER Root
    Project root (the directory holding AirportMgr.uproject). Defaults to this script's parent.
#>
[CmdletBinding()]
param(
    [string] $Root = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = 'Stop'
$failures = New-Object System.Collections.Generic.List[string]

$plugin      = Join-Path $Root 'Plugins\Airside\Source\Airside'
$ops         = Join-Path $Root 'Plugins\AirportOps\Source\AirportOps'
$editor      = Join-Path $Root 'Plugins\Airside\Source\AirsideEditor'
$airsideTests = Join-Path $Root 'Plugins\Airside\Source\AirsideTests'
$opsTests    = Join-Path $Root 'Plugins\AirportOps\Source\AirportOpsTests'
$modules = @($plugin, $ops)
$trees   = @((Join-Path $Root 'Plugins\Airside\Source'), (Join-Path $Root 'Plugins\AirportOps\Source'), (Join-Path $Root 'Source\AirportMgr'))

# Rule 2's own module list, wider than $modules above: AirsideEditor, and the two test
# modules, are not subject to the Model/Tool/Build include-direction rule (a composition
# test freely includes Present/), but a DEFINE_LOG_CATEGORY_STATIC in any of them collides
# under the SAME unity-build failure mode as one in Airside or AirportOps - #104: 8 of them
# in the two test modules were unique only because nobody had yet picked the same word twice.
$logCategoryModules = @($plugin, $ops, $editor, $airsideTests, $opsTests)

function Get-Sources([string] $Dir, [string[]] $Ext) {
    if (-not (Test-Path $Dir)) { return @() }
    Get-ChildItem -Path $Dir -Recurse -File | Where-Object { $Ext -contains $_.Extension }
}

# --- 1. Include direction -----------------------------------------------------------------
# Layer -> regex of forbidden include prefixes, applied inside EACH module. Solve/ is
# handled separately as an allow-list.
#
# Build's own entry names Content/AirsideSettings specifically, NOT the whole Content/
# folder (issue #191, the last "Wrong layer" item). AnchorLink.cpp and RoadGuidelineBuilder.cpp
# used to call UAirsideSettings::ResolveLargestServiceVehicle() themselves, from inside a
# per-ordered-arm-pair and a per-link loop respectively - the #78 shape, Build/ resolving a
# content default instead of taking it from whoever already resolved it once. Both are
# parameterised now (issue #190) and neither includes AirsideSettings.h any more. DepotKit.cpp
# still includes Content/AirsideContent.h, and that stays legal: DepotKitSpecs takes a
# `const UAirsideContent*` PARAMETER (PR #212) and dereferences its DepotKits map, which is
# the correct shape this rule wants everywhere else - forbidding the whole Content/ folder
# would flag a file that never resolves anything itself.
$forbidden = @{
    'Model' = 'Build/|Tool/|Present/|Entities/|Content/'
    'Tool'  = 'Present/|Content/'
    'Build' = 'Present/|Tool/|Content/AirsideSettings'
}
foreach ($module in $modules) {
    foreach ($layer in $forbidden.Keys) {
        foreach ($half in 'Public', 'Private') {
            $dir = Join-Path $module (Join-Path $half $layer)
            foreach ($file in Get-Sources $dir @('.h', '.cpp')) {
                $hits = Select-String -Path $file.FullName -Pattern ('#include\s+"(' + $forbidden[$layer] + ')')
                foreach ($h in $hits) {
                    $failures.Add("include-direction: $($file.FullName):$($h.LineNumber) $layer/ must not include $($h.Line.Trim())")
                }
            }
        }
    }
}

# --- 1b. Cross-plugin direction: Airside never includes AirportOps ------------------------
# AirportOps -> Airside is the only legal direction. A header from the ops plugin inside
# Airside would make movement depend on money, which is the boundary the plugin split exists
# to hold. Matched on CODE references - an include path, the API macro, or a UOps* class
# name (a forward declaration is the same leak with no #include to catch) - and NOT on the
# bare word, because a WHY comment that names the consumer ("AirportOps binds this") is
# exactly the kind of comment this codebase wants more of.
foreach ($file in Get-Sources (Join-Path $Root 'Plugins\Airside\Source') @('.h', '.cpp')) {
    $hits = Select-String -Path $file.FullName -Pattern '#include\s+"[^"]*AirportOps|AIRPORTOPS_API|\bUOps[A-Z]\w*'
    foreach ($h in $hits) {
        $failures.Add("cross-plugin: $($file.FullName):$($h.LineNumber) Airside must not reference AirportOps: $($h.Line.Trim())")
    }
}

foreach ($half in 'Public', 'Private') {
    $dir = Join-Path $plugin (Join-Path $half 'Solve')
    foreach ($file in Get-Sources $dir @('.h', '.cpp')) {
        $hits = Select-String -Path $file.FullName -Pattern '#include\s+"([^"]+)"' |
            Where-Object { $_.Matches[0].Groups[1].Value -notmatch '^(CoreMinimal\.h|Solve/)' }
        foreach ($h in $hits) {
            $failures.Add("solve-purity: $($file.FullName):$($h.LineNumber) Solve/ may include only CoreMinimal.h and Solve/: $($h.Line.Trim())")
        }
    }
}

# --- 2. Log category names unique within each module ------------------------------------
foreach ($module in $logCategoryModules) {
    $categories = @{}
    foreach ($file in Get-Sources $module @('.cpp', '.h')) {
        $hits = Select-String -Path $file.FullName -Pattern 'DEFINE_LOG_CATEGORY(_STATIC)?\(\s*(\w+)'
        foreach ($h in $hits) {
            $name = $h.Matches[0].Groups[2].Value
            if ($categories.ContainsKey($name)) {
                $failures.Add("log-category: $name defined in both $($categories[$name]) and $($file.FullName):$($h.LineNumber) - unity build collision; declare it once in the module's Public/*Log.h")
            }
            else {
                $categories[$name] = "$($file.FullName):$($h.LineNumber)"
            }
        }
    }
}

# --- 3. Stacked doc comments in headers -------------------------------------------------
foreach ($tree in $trees) {
    foreach ($file in Get-Sources $tree @('.h')) {
        $text = Get-Content -Raw -Path $file.FullName
        $m = [regex]::Matches($text, '\*/\s*\r?\n\s*/\*\*')
        foreach ($x in $m) {
            $line = ($text.Substring(0, $x.Index) -split "`n").Count
            $failures.Add("orphan-doc: $($file.FullName):$line a doc comment is followed by another doc comment; one of them lost its declaration")
        }
    }
}

# --- 4. Piper fallback has one production caller, one test-fixture caller ---------------
# Production code: only AircraftType.* and AirsideSettings.cpp. The two test modules are a
# unity build and once exempted this rule entirely (#100's evidence: 51 call sites across 14
# files) - now they get the same "one source of truth" rule, with AirsideTestFixtures.cpp
# (TestAirframes::Piper()) as their one allowed caller, exactly as AircraftType.cpp is
# production's.
$allowed = @('AircraftType.h', 'AircraftType.cpp', 'AirsideSettings.cpp', 'AirsideTestFixtures.cpp')
foreach ($tree in $trees) {
    foreach ($file in Get-Sources $tree @('.h', '.cpp')) {
        if ($allowed -contains $file.Name) { continue }
        # Word-boundary before "PiperMeridian" so BuildPiperMeridian() - a different concern,
        # constructing the content asset rather than reading a fallback figure - is not
        # mistaken for one of the accessor fallbacks (Ground/Climb/Approach/Engine/Wingspan/
        # Requirements) this rule exists to keep to one source of truth.
        $hits = Select-String -Path $file.FullName -Pattern '(?<![A-Za-z])PiperMeridian\w*\s*\('
        foreach ($h in $hits) {
            $reason = if ($file.FullName -match '\\(AirsideTests|AirportOpsTests)\\') {
                'go through TestAirframes::Piper() (AirsideTestFixtures.h)'
            } else {
                'go through UAirsideSettings::ResolveDefaultAirframe'
            }
            $failures.Add("content-default: $($file.FullName):$($h.LineNumber) calls a PiperMeridian*() fallback; $reason")
        }
    }
}

# --- 5. No hand-built slot-map handle outside RoadSlotMap.h -----------------------------
# Three shapes a hand-built {index, generation} takes at a call site (#79, #173):
#   {Index, Item.Generation}         - brace-init from a loop index and the item's field
#   X.Generation = Item.Generation   - the generation half, wherever the Index half came from
#   X.Index = Index; ... X.Generation = ...   - split across up to two statements
# RoadSlotMap.h is the one legal definer (RoadSlot::Add and RoadSlot::HandleAt themselves).
# Test modules are exempt (see the doc comment above) the same way rule 4 exempts them.
$handlePatterns = @(
    '\{\s*\w+,\s*\w+(\[\w+\])?\.Generation\s*\}',
    '\.Generation\s*=\s*\w+(\[\w+\])?\.Generation'
)
# "Within 2 lines": the Index line, then at most one line between it and the Generation line.
$handleSplitPattern = '\.Index\s*=\s*\w+;[^\n]*\n(?:[^\n]*\n){0,1}[^\n]*\.Generation\s*='
foreach ($tree in $trees) {
    foreach ($file in Get-Sources $tree @('.h', '.cpp')) {
        if ($file.Name -eq 'RoadSlotMap.h') { continue }
        if ($file.FullName -match '\\(AirsideTests|AirportOpsTests)\\') { continue }

        foreach ($pattern in $handlePatterns) {
            $hits = Select-String -Path $file.FullName -Pattern $pattern
            foreach ($h in $hits) {
                # CODE, not a comment naming the shape to avoid - same reasoning as the
                # cross-plugin rule above: a WHY comment that cites the banned pattern (as
                # this rule's own fix does, at RunwayQuery.cpp and RouteSearch.cpp) is exactly
                # the kind of comment this codebase wants more of, not a thing to fail on.
                if ($h.Line.Trim().StartsWith('//')) { continue }
                $failures.Add("hand-built-handle: $($file.FullName):$($h.LineNumber) builds a handle by hand; use the Network.*IdAt accessor: $($h.Line.Trim())")
            }
        }

        $text = Get-Content -Raw -Path $file.FullName
        foreach ($m in [regex]::Matches($text, $handleSplitPattern)) {
            $line = ($text.Substring(0, $m.Index) -split "`n").Count
            $failures.Add("hand-built-handle: $($file.FullName):$line splits a hand-built handle across .Index and .Generation assignments; use the Network.*IdAt accessor")
        }
    }
}

# --- 6. FRoadAgent invariant fields written only through their own mutators --------------
# Issue #174. `[^=]` after the `=` excludes `==`/`!=`/`>=`/`<=` comparisons, which this
# pattern would otherwise also match (an assignment's `=` is a leading substring of all
# four). RoadAgent.cpp itself is exempt - it is the one file allowed to write these fields,
# being where Refuse/ClearArbitration/HoldRunway/etc. are defined.
$agentFieldPattern = 'Agent\.(WaitingOn|BlockedStep|StalledSeconds|RunwayHeld|bAwaitingStand|GoalNode)\s*=[^=]'
foreach ($tree in $trees) {
    foreach ($file in Get-Sources $tree @('.cpp')) {
        if ($file.Name -eq 'RoadAgent.cpp') { continue }
        if ($file.FullName -match '\\(AirsideTests|AirportOpsTests)\\') { continue }
        $hits = Select-String -Path $file.FullName -Pattern $agentFieldPattern
        foreach ($h in $hits) {
            # A WHY comment naming the banned shape (this rule's own fix does, at
            # GroundTrafficRebuild.cpp) is not the shape itself - same exemption as rule 5.
            if ($h.Line.Trim().StartsWith('//')) { continue }
            $failures.Add("agent-field-write: $($file.FullName):$($h.LineNumber) writes an FRoadAgent invariant field by hand; use its mutator (Refuse/ClearArbitration/HoldRunway/ReleaseRunway/AccrueStall/ResetStall/SetAwaitingStand/ClearAwaitingStand/SetGoal): $($h.Line.Trim())")
        }
    }
}

# --- 7. No colour tokens in Tool/ ---------------------------------------------------------
# Issue #191. Tool/ names a MEANING (EPreviewStyle) to IToolPreviewSink, never a colour - the
# HUD and the editor viewport are the two sinks that decide what a meaning looks like, and
# PreviewPalette's colour table, ring radii and line weights had drifted into Tool/ despite
# both consumers already living in or including Present/. Applied to the production tree only
# (Public/Tool and Private/Tool), the way rule 1 scopes to $modules rather than the test trees.
$colourPattern = '\b(FColor|FLinearColor|FSlateColor|FSlateBrush)\b'
foreach ($module in $modules) {
    foreach ($half in 'Public', 'Private') {
        $dir = Join-Path $module (Join-Path $half 'Tool')
        foreach ($file in Get-Sources $dir @('.h', '.cpp')) {
            $hits = Select-String -Path $file.FullName -Pattern $colourPattern
            foreach ($h in $hits) {
                # A WHY comment naming the banned token (to explain why Tool/ does not hold
                # one) is not the thing itself - same exemption as rule 5.
                if ($h.Line.Trim().StartsWith('//')) { continue }
                $failures.Add("tool-colour: $($file.FullName):$($h.LineNumber) Tool/ must not name a colour - describe a MEANING to IToolPreviewSink instead: $($h.Line.Trim())")
            }
        }
    }
}

# --- Verdict -------------------------------------------------------------------------------
if ($failures.Count -eq 0) {
    Write-Host 'Check-Architecture: PASS (include direction, cross-plugin, log categories, doc comments, content default, hand-built handles, agent field writes, tool colour)' -ForegroundColor Green
    exit 0
}

foreach ($f in $failures) { Write-Host "  FAIL  $f" -ForegroundColor Red }
Write-Host ''
Write-Host "Check-Architecture: $($failures.Count) failure(s)." -ForegroundColor Red
exit 1
