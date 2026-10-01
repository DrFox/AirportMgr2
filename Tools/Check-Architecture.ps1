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
      6. EVERY FRoadAgent field is written only through its own mutator outside RoadAgent.cpp -
         generalised (issue #295) from #174's enumerated list (WaitingOn, BlockedStep,
         StalledSeconds, RunwayHeld, bAwaitingStand, GoalNode) to ANY `Agent.<field> =` /
         `Agents[i].<field> =`. The enumerated form only ever failed on a name someone
         remembered to add to it, and #295 found nine MORE fields the #174 sweep missed
         (ReverseSpeed, EngineRPM, ShutdownPause, LastResolveAttempt, DepartureRunway,
         LastOverlaps, Follower.Travelled, LastMotion.Position) written by hand across four
         files - the "lists that must agree are one list" rule CLAUDE.md names, applied to
         itself. Class is left public (set once at birth, read at dozens of sites a getter
         would not make safer) and is the one deliberate remainder: its two birth sites
         (DispatchArrival, AdmitDispatched, both in GroundTraffic.cpp) are allow-listed by file
         AND field name, not by exempting the file outright, so a future direct write of any
         OTHER field in GroundTraffic.cpp still fails. Test modules are exempt, the way rules
         4 and 5 exempt them: a scripted scenario sets up state, it does not enforce production
         discipline on itself.
      7. No FColor/FLinearColor/FSlateColor/FSlateBrush token in Tool/ (Public or Private). Tool/
         describes intent to IToolPreviewSink by MEANING, never colour - see CLAUDE.md's
         Architecture section - and issue #191 found PreviewPalette's colour table, ring radii
         and line weights sitting in Tool/ anyway, despite both its consumers (the game HUD and
         the editor viewport tool) already living in or including Present/. Moved to Present/;
         this rule is what stops the next shared-look table from landing back in Tool/ by habit.
         Comment lines are excluded the way rule 5 excludes them - a WHY comment naming a colour
         to explain why Tool/ does not hold one is not the thing this rule exists to catch.
      11. Declared but not consumed (issue #255). Three shapes, each reconstructed from a
          shipped bug and proved to fire before being removed again (see the PR):
            a. A DECLARE_*DELEGATE* member (file-scoped: the macro and its instance sit in
               the same header, however far apart) with zero .Add*/.Bind* binders anywhere,
               or zero .Broadcast/.Execute callers anywhere - issue #169's UFlightBoard::
               OnChanged, documented as a signal nothing subscribed to.
            b. A UI_COMMAND with no MapAction anywhere - the shape issue #184 left behind:
               a command that exists and is even mapped once, but only the LAST MapAction
               of a given command survives (UICommandList::MapAction is a TMap::Add), so a
               command an later binding never touches again is functionally unconsumed.
            c. An IBuildTool hook (RoadBuildTool.h) with no caller anywhere outside its own
               declaration - issue #185's OnCommit/BuildReadout, reachable from PIE and from
               nowhere in the editor mode until each was wired in by hand.
          Production tree only for what gets CHECKED (test fixtures build scratch scenarios
          on purpose, the way rules 4-6 already exempt them); the search for a binder/caller
          spans the whole tree, since a real one may legitimately sit in a test spy.
      12. Comment-only facts (issue #255): a WARNING, not a failure - see the script's own
          Verdict section. Counts comment lines that assert a fact about other code ("the
          only caller", "never happens", "no edit needed", "nothing else reads") with no
          `// ENFORCED BY:` marker within three lines naming what actually holds the line
          true. Starts as a count so the backlog is visible; promotes to a failure once it
          reaches zero (see CLAUDE.md's "Conventions" for the marker itself).
      14. A run's width is FKitSpec::RunWidthUu, never WidthUu times a run length by hand.
          Five sites did the product until the shed's end caps arrived (2026-09-22) and had
          to be added to each; PlotYard.h is the one legal home.
      15. Ground-only consumers never name FAirframe (2026-09-23). The follower, the reverse,
          the speed profile and the road builders take FChassis; an FAirframe parameter
          coming back is a truck being handed a wingspan again. A listed file that no longer
          exists FAILS rather than passing, so a rename cannot switch the rule off.
      17. IsPlotted() is not "is a depot". A stand can be plotted too (2026-09-23's "drawn
          stands" work), so a site that read Outline.Num() >= 3, or called IsPlotted(), to
          mean "this is a depot" fenced, priced or labelled a drawn stand exactly like a
          depot's yard (Airside.Present.PlotPresenter.StandOutlineIsNotADepot pins this at
          PlotPresenter.cpp's RebuildFrom). RoadEntity.h, RoadSurfacePresenter.cpp (pad
          paving - a stand wants a pad too), RoadEditFacadeSurfaces.cpp (FindEntityAt - a
          ground pick is kind-neutral), SelectTool.cpp (the selection highlight - a
          plotted stand deserves the same polygon a plotted depot gets) and StagedPlotTool.cpp
          (issue #302: the Remove preview's IsMine(Entity) && Entity.IsPlotted(), one body
          shared by the depot and stand tools, where IsMine IS the kind check - just made
          through a virtual hook rather than spelled IsDepot()/IsStand() on the line itself)
          are allow-listed whole, each correctly kind-neutral by design. Everywhere else a
          bare IsPlotted()
          is the shape this rule exists to catch; a line that also names IsDepot() or
          IsStand() states the kind explicitly and is exempt (Controller ruling, 2026-09-23:
          later tasks write `IsStand() && IsPlotted()` outside these files and that must
          pass too).
      18. No Acos in the Airside/AirportOps production modules (2026-09-24). acos(dot) is
          1.5e-8 rad wrong one ulp from +/-1, which refused a guided T junction as "too short
          to hold the corner"; RoadGeom::AngleBetween (atan2 of cross and dot) is the idiom.
      19. The runway/taxiway profile triples are TestProfiles::Runway/NarrowRunway/Taxiway's
          own numbers, nowhere else (issue #310). New sites kept typing
          URoadProfile::MakeTransient(4500.0, 1500.0, 450.0) (or 1800/180, or 2300/230) after
          TestProfiles existed to say the same thing - 28 of them across 12 files, some newer
          than the helper itself. AirsideTestFixtures.cpp is the one legal definer; scoped to
          AirsideTests only, because AirportOpsTests cannot reach it (it is a Private header -
          see the header's own top comment) and has the SAME duplication under its own
          literals, a separate finding (#310's PR body) that this rule does not cover.
      20. No new *ForTest( forwarder on ARoadNetworkActor outside RoadNetworkActor.h's own
          explicit allow-list (issue #298).
      21. AirsideVehicleCodes:: is never compared with ==/!= (issue #308's TypeCode ladder,
          deleted, must not come back wearing a comparison).
      22. FRouteQuery is hand-built (`FRouteQuery Var;`, no initialiser) only in
          RouteSearch.cpp (issue #312). FRouteQuery::For is the one writer of AvoidRunways off
          the resolved FRoutePolicy - the hot loop reads AvoidRunways, never Policy - so a
          caller that fills a query by hand and skips For() gets the permissive
          ERunwayAvoidance::None for every errand, silently: six test helpers and 48 call
          sites did this before #312. Test modules are explicitly IN SCOPE, unlike rules 5/6 -
          a test's query reaches the same RunSearch a production one does. Four sites are
          allow-listed by (File, Var), each checked by hand (FuelService.cpp's two went to
          FRouteQuery::For in the JobBoard move, 2026-09-28; ArrivalPlanner.cpp's went to
          ArrivalPlanner::TaxiInQuery, For() with no goal, in #429's review):
          RoadEditFacadeSurfaces.cpp already sets AvoidRunways correctly
          by hand (FindToGoals takes a Goals ARRAY); RoutePolicyTest.cpp deliberately builds a no-errand query to test its
          own refusal; RouteSearchTest.cpp has the same FindToGoals shape as ArrivalPlanner.cpp;
          RouteStepDistanceTest.cpp's FRouteRunwayAvoidanceTest sweeps AvoidRunways BY HAND,
          decoupled from Policy, to test ERunwayAvoidance itself. The allow-list also fails if
          an entry stops matching anything - the same "list drifted silently" failure rule 15
          guards against.
      31. The ops event bus is subscribed to only in OpsRuntime.cpp (UOpsRuntime::WireBus) - see the
          rule's own comment.
      32. A facade commit's affordability refusal is announced (AffordOrRefuse) - see the rule.
      38. A service vehicle's State, AgentId and CurrentJob are written only by FServiceVehicleLifecycle
          (issue #428) - see the rule.
      42. No registered automation test name is a dotted prefix of another (2026-09-30): UE's automation tree
          drops the bare name's own RunTest silently - see the rule.
      57. A flight's phase is written only by UFlightBoard::TransitionTo (#442): thirteen sites wrote it, each choosing
          its own subset of the change's side effects - see the rule.
      58. EFlightPhase is grouped only in Flight.h (#442): `Accepted || Inbound`, `>= Turnaround` and their kin were
          re-spelled at a dozen sites, each a place a new phase would be missed - see the rule.

    Rule 4 above is now a data table (issue #255) rather than one hard-coded Piper check,
    so "the only caller of X is Y" claims live as ROWS an author can add to, instead of prose
    nobody re-greps.

    Not checked here, deliberately: uninitialised FVector2D locals (issue #46). The idiom
    `FVector2D X; if (!Fill(X)) ...` is legitimate and appears ~60 times as out-params; the
    bug is ignoring the return value, which a regex cannot see. The rule lives in CLAUDE.md.

    MUTATION CHECK (issue #291, run by hand - this script has no automation harness of its
    own to assert these in): copy the tree to a scratch dir, then
      (a) rename `struct AIRSIDE_API IBuildTool` in the copy's RoadBuildTool.h to anything
          else and confirm rule 11c FAILS (`unconsumed-tool-verb: ... could not find`) instead
          of silently passing every hook - this is the exact shape issue #291 reported: a
          renamed or dropped struct made 11c a no-op with no failure.
      (b) break the copy's own syntax (drop one closing brace anywhere) and confirm
          Run-AirsideTests.ps1, pointed at the scratch copy via -Root, prints
          "FAIL: Check-Architecture.ps1 does not parse" and STOPS before any editor starts -
          not a silent zero exit code with no verdict line.
    Both were exercised on 2026-09-25 against a scratch copy of this tree; see the PR body
    for the pasted output. Neither is wired into CI because both require mutating a copy of
    this very script, which the script cannot safely do to itself mid-run.

.PARAMETER Root
    Project root (the directory holding AirportMgr.uproject). Defaults to this script's parent.
#>
[CmdletBinding()]
param(
    [string] $Root = (Split-Path -Parent $PSScriptRoot),
    # Rule 42's registered-test list, one `S|<name>` (simple) or `C|<name>` (complex) per line, for
    # Run-AirsideTests.ps1 to diff against what the runner actually ran. Written by the ONE parser of the
    # IMPLEMENT_*_AUTOMATION_TEST macros (this script) so the run script does not grow a second one that can
    # disagree with it. Empty = do not write.
    [string] $TestNamesOut = ''
)

$ErrorActionPreference = 'Stop'
$failures = New-Object System.Collections.Generic.List[string]
# Issue #291: the PASS banner used to be a hand-typed list of rule names, which can drift from
# the rules that actually ran (solve-purity had silently fallen off it before anyone noticed -
# fixed as a side effect of this list existing). Each rule section below appends its own name
# right after it runs, so the banner is built from what ran, not from what someone remembered
# to type when they last touched the banner.
$ranRules = New-Object System.Collections.Generic.List[string]

# Issue #291, rule 4's path-suffix fix: `-contains $file.Name` allow-lists by BARE FILE NAME
# across the whole tree, so a same-named file dropped anywhere else (a second RoadNetworkActor.cpp
# in a stray folder, say) is exempted along with the real one. A suffix match against the file's
# full path, using an entry that names enough of the path to be unambiguous (e.g.
# 'Private\Present\RoadNetworkActor.cpp', not just 'RoadNetworkActor.cpp'), closes that: a decoy
# in a different directory no longer matches the suffix. Table entries below were widened to the
# real current path of each file (confirmed by grep, one hit each on today's tree), so this is a
# stricter check on the SAME files, not a behaviour change on today's tree.
function Test-AllowedPathSuffix([System.IO.FileInfo] $File, [string] $Suffix) {
    $full = $File.FullName.Replace('/', '\')
    $want = '\' + $Suffix.Replace('/', '\')
    return $full.EndsWith($want, [System.StringComparison]::OrdinalIgnoreCase)
}

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

# COMMENTS STRIPPED, STRING LITERALS KEPT (Strip-ArchComments) - what rule 4 decides "is this line a comment" with, and rule 60(b), whose pass names are
# strings (Strip-ArchCode, rule 34's, blanks every string literal, so a pattern that names a string - `%02d:%02d`, TEXT("FleetSeed") - needs this one).
# Character-wise, so a `//` inside a string is not a comment and a `"` inside a comment does not open one; InBlock carries an open /* ... */ across lines.
# A `'"'` char literal would confuse it - none in the files the rules read today, and a rule that reads a stripped file fails rather than passes if the
# thing it looks for is gone. Defined HERE, before rule 4, not beside rule 60 where it first lived: a PowerShell function exists only once its
# definition has run, and rule 4 is the earlier reader.
function Strip-ArchComments([string] $Line, [ref] $InBlock) {
    $out = New-Object System.Text.StringBuilder
    $inString = $false
    $i = 0
    while ($i -lt $Line.Length) {
        if ($InBlock.Value) {
            $end = $Line.IndexOf('*/', $i)
            if ($end -lt 0) { return $out.ToString() }
            $i = $end + 2
            $InBlock.Value = $false
            continue
        }
        $c = $Line[$i]
        if ($inString) {
            [void]$out.Append($c)
            if ($c -eq '\' -and $i + 1 -lt $Line.Length) { $i++; [void]$out.Append($Line[$i]) }
            elseif ($c -eq '"') { $inString = $false }
            $i++
            continue
        }
        if ($c -eq '"') { $inString = $true; [void]$out.Append($c); $i++; continue }
        if ($c -eq '/' -and $i + 1 -lt $Line.Length) {
            $next = $Line[$i + 1]
            if ($next -eq '/') { return $out.ToString() }
            if ($next -eq '*') { $InBlock.Value = $true; $i += 2; continue }
        }
        [void]$out.Append($c)
        $i++
    }
    return $out.ToString()
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
$ranRules.Add('include direction')

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
$ranRules.Add('cross-plugin')

# --- 1c. Runtime-vs-editor direction: Airside never includes or names AirsideEditor -------
# Issue #191. The SAME shape as 1b above, mirrored: AirsideEditor -> Airside is the only legal
# direction (AirsideEditor.Build.cs's own comment says so), because the runtime plugin must
# ship without an editor module. Matched on CODE references, the same three ways as 1b - an
# include path, the module's API macro, or a name from its one class family (RoadBuildEd*
# covers URoadBuildEdMode, FRoadBuildEdModeCommands, URoadBuildEditorTool and
# URoadBuildEditorToolBuilder, the whole of AirsideEditor's public surface) - so a bare
# `class URoadBuildEdMode;` forward declaration is caught with no #include required to catch
# it. Scoped to the Airside MODULE only (not AirsideTests, which composes both sides on
# purpose for the composition tests CLAUDE.md's "Architecture" section describes).
#
# COMMENT LINES EXCLUDED, unlike 1b above (which does not exempt them but happens never to
# meet one): RoadBuildEditorTool is discussed BY NAME throughout Airside's own WHY comments -
# the two-drivers design this whole plugin is built around - and a doc comment explaining
# that split is exactly the comment CLAUDE.md wants more of, not a layering violation. Doc
# blocks here are `/** ... * ... */`, not `//`, so the check is against a leading `*` or `/*`
# as well - rule 5/6/7's `StartsWith('//')` alone would have missed every one of these.
foreach ($file in Get-Sources $plugin @('.h', '.cpp')) {
    # RoadBuildEd, not \bRoadBuildEd\w* - every real name (URoadBuildEdMode,
    # FRoadBuildEdModeCommands, ...) has a U/F prefix immediately before "RoadBuildEd" with
    # no word boundary between them, so a \b there would never fire on the actual class names.
    $hits = Select-String -Path $file.FullName -Pattern '#include\s+"[^"]*AirsideEditor|AIRSIDEEDITOR_API|RoadBuildEd'
    foreach ($h in $hits) {
        if ($h.Line.Trim() -match '^(//|/\*|\*)') { continue }
        $failures.Add("editor-direction: $($file.FullName):$($h.LineNumber) Airside must not reference AirsideEditor: $($h.Line.Trim())")
    }
}
$ranRules.Add('editor direction')

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
# Issue #291's own PASS-banner audit found THIS rule missing from the hand-typed list below -
# it had silently fallen off at some earlier edit and nothing noticed, which is exactly the
# drift $ranRules exists to stop happening again.
$ranRules.Add('solve-purity')

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
$ranRules.Add('log categories')

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
$ranRules.Add('doc comments')

# --- 4. Allowed-callers table: each row's symbol resolves from ONE known set of files ---
# Issue #255 generalises the single hard-coded Piper check (issue #30) into a table, because
# this week's review found the SAME "the only caller of X is Y" shape asserted in a comment
# for four other symbols, with nothing checking any of them mechanically. A row names a
# symbol's production allow-list; test callers either get their OWN tight allow-list (Piper's
# row, unchanged from before this table existed) or a blanket TestExempt (rules 5-8's existing
# exemption: a scripted scenario sets up state, it does not enforce production discipline on
# itself). Comment lines are excluded (the same exemption rules 5-8 give a WHY comment that
# NAMES the banned shape rather than being it) - RoadEditTarget.h, RunwayTool.h, PlotPresenter.h
# and RoadNetworkActor.h all discuss these symbols by name in exactly that way. "Comment" means removed by Strip-ArchComments, not "starts with `*`" (#446's review:
# that test threw away a code line starting with a dereference, `*Context.Selection = X;`, as if it were a doc-comment continuation).
# Issue #291: every entry below names enough of its PATH to be unambiguous (checked with
# Test-AllowedPathSuffix, not bare-name equality) - 'Private\Entities\AircraftType.cpp', not
# 'AircraftType.cpp'. Confirmed by grep that each is still the file's real current location
# (one hit apiece on today's tree), so this widens what the check can catch without moving
# what it accepts today.
$AllowedCallers = @(
    @{
        # NOT AirsideSettings.cpp SINCE #449: the content-less default airframe was a hand copy of UAircraftType::
        # Airframe() assembled from these figures, and had drifted from it (8 of 20 fields, the wrong steer law). It is
        # BuildPiperMeridian -> Airframe() now, which this pattern does not match - a PiperMeridian*() call there again
        # would be the hand copy coming back.
        Name        = 'PiperMeridian fallback'
        Pattern     = '(?<![A-Za-z])PiperMeridian\w*\s*\('
        ProdAllowed = @('Public\Entities\AircraftType.h', 'Private\Entities\AircraftType.cpp')
        TestAllowed = @('Private\AirsideTestFixtures.cpp')
        ProdReason  = 'go through UAirsideSettings::ResolveDefaultAirframe'
        TestReason  = 'go through TestAirframes::Piper() (AirsideTestFixtures.h)'
    },
    @{
        # CONFIRMED BY GREP FOR #255: production callers today are AirsideSettings.cpp
        # (itself) and OpsRuntime.cpp - RoadBuildController.cpp's went with its board-less Land
        # fallback (#431), and a new one there would be that fallback returning. Unlike Piper, tests call this
        # directly from a couple dozen files on purpose - it IS the canonical "give me a
        # plane" accessor test fixtures are supposed to use - so tests are blanket-exempt
        # rather than narrowed to one fixture file.
        Name        = 'UAirsideSettings::ResolveDefaultAirframe'
        Pattern     = 'UAirsideSettings::ResolveDefaultAirframe\s*\('
        ProdAllowed = @('Public\Content\AirsideSettings.h', 'Private\Content\AirsideSettings.cpp', 'Private\Present\OpsRuntime.cpp')
        TestExempt  = $true
        ProdReason  = 'a new production caller resolves the default airframe a second way instead of taking it from context - route it through one of the rows above or extend this row and say why'
    },
    @{
        # THE CONTENT-LESS BRANCH ALONE (#479). ResolveDefaultAirframe is the door: it prefers the content set's
        # DefaultAircraft and falls back to this. A production caller of THIS directly skips the content default - the
        # day #30 authors DA_PiperMeridian it would still fly the hand-built Meridian. Public for tests, which compare
        # TestAirframes::Piper() and the fallback against it (AirsideContentTest.cpp).
        Name        = 'UAirsideSettings::ContentlessDefaultAirframe'
        Pattern     = 'UAirsideSettings::ContentlessDefaultAirframe\s*\('
        ProdAllowed = @('Public\Content\AirsideSettings.h', 'Private\Content\AirsideSettings.cpp')
        TestExempt  = $true
        ProdReason  = 'go through UAirsideSettings::ResolveDefaultAirframe, which prefers the content set''s DefaultAircraft and falls back to this'
    },
    @{
        # FRoadAgent's bundles are private since 2026-09-23 so the unused one of the two cannot
        # be read; this is the one writable door, and it exists for test fixtures only.
        Name        = 'FRoadAgent::EditAirframeForTest'
        Pattern     = '\bEditAirframeForTest\s*\('
        ProdAllowed = @('Public\Model\RoadAgent.h')
        TestExempt  = $true
        ProdReason  = 'start the agent through StartTaxi/StartArrival/StartPushback/StartDrive, which keep its body and bundle consistent'
    },
    @{
        # NO TOW HOOK IN FindToGoals (review of 9441ccf1): the whole-route tow check lives in
        # RouteSearch::Find only, so a multi-goal search is sound only for callers with no tow -
        # today the aircraft's stand choice. A vehicle caller must add the hook first.
        Name        = 'RouteSearch::FindToGoals'
        Pattern     = '\bFindToGoals\s*\('
        ProdAllowed = @('Public\Model\RouteSearch.h', 'Private\Model\RouteSearch.cpp', 'Private\Model\ArrivalPlanner.cpp')
        TestExempt  = $true
        ProdReason  = 'FindToGoals has no whole-route tow check (RouteSearch.cpp, Find); route a vehicle with Find, or add the hook'
    },
    @{
        # ONE TOW STEPPER (2026-09-25, the whole-route tow check). The router's verdict and the
        # driven agent's fold are one fact only while both step the chain through
        # VehicleFit::StepTow; a second production caller of StepChain is a second evaluator,
        # the shape that let a balloon admit a rig that then jack-knifed. Trace (one curve) is
        # the other sanctioned caller, in the same Solve file. TowReverse (2026-09-26) is the
        # third: the ONE reverse stepper, whose samples FTowReverseRun plays and
        # VehicleFit::JudgePlan judges - the same one-evaluator promise, going backwards.
        Name        = 'VehicleSweep::StepChain'
        Pattern     = '\bStepChain\s*\('
        ProdAllowed = @('Public\Solve\VehicleSweep.h', 'Private\Solve\VehicleSweep.cpp', 'Private\Model\VehicleFit.cpp', 'Private\Solve\TowReverse.cpp')
        TestExempt  = $true
        ProdReason  = 'step a tow through VehicleFit::StepTow, the one step the router (JudgePlan) and the agent (FollowAndTow) share'
    },
    @{
        Name        = 'DepotKitSpecs'
        Pattern     = 'DepotKitSpecs\s*\('
        ProdAllowed = @('Public\Build\DepotKit.h', 'Private\Build\DepotKit.cpp', 'Private\Present\RoadNetworkActor.cpp')
        TestExempt  = $true
        ProdReason  = 'go through ARoadNetworkActor::ResolveDepotKits (issue #181) - PlotPlaceTool.cpp and PlotPresenter.cpp both deliberately stopped calling this themselves'
    },
    @{
        # ONE SOLVE PER DEPOT YARD (facility-upgrades spec, 2026-09-29; narrowed by #450): the presenter's lit/ghosted bays,
        # the purchase rules' free slot, the tool's preview and the facade's commit all read DepotKit::SolveYard, through
        # ReservationOf for a placed plot. A new production caller of PlotLayoutFor is a second solve that can disagree - the
        # Buy shed that lights nothing. The tool and the facade were allow-listed here until #450, each typing its own
        # FPlotSite and its own layout fallback beside the third copy in DepotKit.cpp; they no longer reach this at all.
        Name        = 'PlotLayoutFor'
        Pattern     = '\bPlotLayoutFor\s*\('
        ProdAllowed = @('Public\Build\PlotLayoutStrategy.h', 'Private\Build\PlotLayoutStrategy.cpp', 'Private\Build\DepotKit.cpp')
        TestExempt  = $true
        ProdReason  = 'solve a depot yard through DepotKit::SolveYard (a placed plot: DepotKit::ReservationOf) - the tool, the facade and the presenter must read one answer (#450)'
    },
    @{
        # THE ONE FPlotSite OF A DEPOT YARD (#450). "Layout = definition's or Scatter; gate = frontage midpoint; seed =
        # DepotYardSeed(gate)" was typed by hand at the tool's preview, the facade's commit and the built yard, and agreed only by
        # comment; the layout fallback drifted once already (a banded ghost, a scattered depot). DepotKit.cpp builds the site once, in
        # SolveYard. The strategies read one (PlotLayoutStrategy.h/.cpp); everyone else asks SolveYard. A site built by hand anywhere
        # else is a fourth copy. Pinned from the value side by Airside.Tool.PlotPlace.ToolFacadeAndBuiltDepotSolveOneYard.
        Name        = 'FPlotSite'
        Pattern     = '\bFPlotSite\b'
        ProdAllowed = @('Public\Build\PlotLayoutStrategy.h', 'Private\Build\PlotLayoutStrategy.cpp', 'Private\Build\DepotKit.cpp')
        TestExempt  = $true
        ProdReason  = 'ask DepotKit::SolveYard, which builds the yard''s FPlotSite once (#450) - a hand-built site is a second source of truth for layout, gate and seed'
    },
    @{
        # THE COMMIT'S OWN JUDGEMENT (#450, PR #491 review). URoadEditFacade::ReserveForPlot is public so the one-yard pin can compare it
        # with the tool's preview and the built yard, but it is the facade's commit path's evaluator: a production caller elsewhere is a
        # second route to a yard verdict the tool and the presenter do not share. RoadEditFacade.h's comment says so; this holds it.
        Name        = 'URoadEditFacade::ReserveForPlot'
        Pattern     = '\bReserveForPlot\s*\('
        ProdAllowed = @('Public\Present\RoadEditFacade.h', 'Private\Present\RoadEditFacadeSurfaces.cpp')
        TestExempt  = $true
        ProdReason  = 'the commit path (PlaceEntityInPlot) is the one production caller of the facade''s yard verdict; ask DepotKit::SolveYard for a yard (#450)'
    },
    @{
        # THE SEED HAS ONE CALLER (#450, PR #491 review): DepotKit::SolveYard seeds every yard off the frontage midpoint. A second caller
        # is a second arithmetic of "which yard is this depot's", the preview-versus-built split the seed exists to prevent.
        Name        = 'DepotYardSeed'
        Pattern     = '\bDepotYardSeed\s*\('
        ProdAllowed = @('Public\Build\DepotKit.h', 'Private\Build\DepotKit.cpp')
        TestExempt  = $true
        ProdReason  = 'DepotKit::SolveYard seeds every depot yard; a second caller would roll a yard the others do not (#450)'
    },
    @{
        # THE LAYOUT FALLBACK IS TYPED ONCE (#450, PR #491 review): "a definition there is none of draws the scatter" was a ternary in the
        # tool, another in the facade and a third in ReservationOf, and the tool's drifted. DepotKit::LayoutOf is the one.
        Name        = 'EPlotLayout fallback ternary'
        Pattern     = '->\s*Layout\s*:\s*EPlotLayout::Scatter'
        ProdAllowed = @('Private\Build\DepotKit.cpp')
        TestExempt  = $true
        ProdReason  = 'ask DepotKit::LayoutOf - the scatter fallback for a missing definition is typed once (#450)'
    },
    @{
        # THE POLYGON GESTURE HAS ONE CLIENT (#450, PR #491 review). IOutlineTarget and FOutlineDrawTool lost their second client when
        # FPlotDrawTool was deleted; ApronDrawTool.h and OutlineDrawTool.h say FApronDrawTool is the only one. A second implementor is
        # the day to decide whether the abstraction earned its keep, not a thing to add quietly.
        Name        = 'IOutlineTarget / FOutlineDrawTool implementors'
        Pattern     = '(?<!:)[:,]\s*(?:public\s+)?(?:IOutlineTarget|FOutlineDrawTool)\b(?!\s*\()'
        ProdAllowed = @('Public\Tool\ApronDrawTool.h')
        TestExempt  = $true
        ProdReason  = 'FApronDrawTool is the one client of the closing-polygon gesture (#450); a second one is the moment to fold or re-justify it'
    },
    @{
        # THE FRONTAGE EDGE HAS A FEW WRITERS AND NO MORE (#450, PR #491 review; a stand's entrance too since #450's leftover): the facade's
        # PlaceEntityInPlot and PlaceStandInPlot (the edge each was GIVEN, looked up in the outline it stores), URoadNetwork::PlaceEntity (copying the
        # placement's onto the instance, and giving a stand that was given none the one StandBox::EntranceEdgeOf answer) and the load migrations
        # EnsureDepotFrontages / EnsureStandFrontages; RoadEntity.h holds the two defaults. A writer anywhere else is a second opinion on where a
        # plot faces - the heuristics this field replaced. Tests set Placement.FrontageEdge to say which edge a fixture means, and are exempt.
        Name        = 'FEntityInstance::FrontageEdge write'
        Pattern     = '\bFrontageEdge\s*=(?!=)'
        ProdAllowed = @('Public\Model\RoadEntity.h', 'Private\Model\RoadNetwork.cpp', 'Private\Present\RoadEditFacadeSurfaces.cpp')
        TestExempt  = $true
        ProdReason  = 'FrontageEdge is written by PlaceEntityInPlot / PlaceStandInPlot (the edge each was given), PlaceEntity (the copy, and the default for a stand given none) and EnsureDepotFrontages / EnsureStandFrontages (the load migrations) only (#450)'
    },
    @{
        # RULE 79 (#490): A DEPOT IS NAMED BY ITS NUMBER, NOT ITS ENTITY INDEX. RoadSlot recycles a freed slot, so the index in "Fuel depot 1" or "to
        # depot 1" named a different depot after a bulldoze. FEntityInstance::DepotNumber is issued by PlaceEntity from NextDepotNumber and
        # backfilled by EnsureStandNumbers - and by no one else: a second writer is a second numbering that the card and the vehicle lines would not agree with.
        Name        = 'FEntityInstance::DepotNumber write (rule 79)'
        Pattern     = '\b(?:Next)?DepotNumber\s*=(?!=)|\bNextDepotNumber\s*\+\+|\+\+\s*NextDepotNumber'
        ProdAllowed = @('Public\Model\RoadEntity.h', 'Public\Model\RoadNetwork.h', 'Private\Model\RoadNetwork.cpp')
        TestExempt  = $true
        ProdReason  = 'a depot is numbered by URoadNetwork::PlaceEntity (and the EnsureStandNumbers backfill) only - a second writer is a second numbering (#490)'
    },
    @{
        # RULE 79 (#490), THE READ SIDE: the three job-board sentences that say where a service vehicle is ("to depot / refilling at depot / at depot")
        # and the depot card's title once printed the entity index. OpsNames::DepotLabel is the one wording; the shape to keep out is a depot sentence
        # built from an index. (A log line may still print Home.Index - a diagnostic names the slot - and is not matched: the sentences are the whole
        # string literal, and the card's title is the Format call over a one-field index.)
        Name        = 'depot named by its entity index in a player-facing line (rule 79)'
        Pattern     = 'TEXT\("(?:to |refilling at |at )depot %d"\)|depot \{0\}"\)\.ToString\(\)\s*,\s*\{\s*\w+\.Index\b'
        ProdAllowed = @()
        TestExempt  = $true
        ProdReason  = 'name the depot with OpsNames::DepotLabel (its DepotNumber) - the entity index is recycled by a delete, so it names a different depot afterwards (#490)'
    },
    @{
        # RULE 80 (#450's leftover): THE STAND READERS READ THE STORED ENTRANCE, THEY DO NOT SEARCH. StandBox::EntranceEdgeOf is the old pose
        # reader's rearmost-midpoint search, moved to the one writer that still needs it (URoadNetwork: PlaceEntity for a stand given no edge, and
        # EnsureStandFrontages for one saved before the field). UStandDefinitionCache::PoseFromOutline and FStandMarkingBuilder::FrameFor read
        # FEntityInstance::GetFrontage now; a reader that asks EntranceEdgeOf is searching again, and "the stand cache and the paint agree" is a
        # comment again. BEST EFFORT, and it says so: a reader that re-typed the loop inline is not seen here - Airside.Model.StandFrontage.
        # ReadersReadTheStoredEdge is the half that measures the read.
        Name        = 'StandBox::EntranceEdgeOf callers (rule 80)'
        Pattern     = '\bEntranceEdgeOf\s*\('
        ProdAllowed = @('Public\Solve\StandBox.h', 'Private\Solve\StandBox.cpp', 'Private\Model\RoadNetwork.cpp')
        TestExempt  = $true
        ProdReason  = 'read FEntityInstance::GetFrontage, the entrance edge placement or the load migration stored; only URoadNetwork writes it, with this search (#450)'
    },
    @{
        # HOW A VEHICLE CAME IS WRITTEN WHERE IT IS MADE (#450/#487, PR #491 review): FServiceFleet::Create sets Origin for Add (Bought or
        # Seeded) and AddForTest alike. A write anywhere else is a second opinion on whether the vehicle pays. BEST EFFORT, and it says so:
        # `Origin` is a common member name (rays, anchors), so the pattern names the receivers a vehicle goes by - `Vehicle.Origin =` and an
        # indexed `Vehicles[i].Origin =` - and a differently named local would not be seen.
        Name        = 'FServiceVehicle::Origin write'
        Pattern     = '\bVehicles?\s*(?:\[[^\]]*\])?\s*(?:\.|->)\s*Origin\s*=(?!=)'
        ProdAllowed = @('Private\Model\ServiceFleet.cpp')
        TestExempt  = $true
        ProdReason  = 'a vehicle''s Origin is set where it is made, FServiceFleet::Create - a second writer would decide whether it pays (#487)'
    },
    @{
        # THE FRONTAGE IS STORED, NOT RECOVERED (#450). DepotKit::RecoverFrontage guessed it from the edge whose midpoint was nearest
        # Position; FEntityInstance::FrontageEdge holds the edge the facade was GIVEN. Nothing may bring the guess back under its old
        # name, tests included - a fixture says which edge it means (Placement.FrontageEdge) rather than leaving the solve to search.
        # (The stand readers - StandDefinitionCache's rearmost midpoint, StandMarkingBuilder's rearmost corner - read the stored entrance
        # now, which rule 80's row 'StandBox::EntranceEdgeOf callers' keeps from searching again; this row covers the depot's name.)
        Name        = 'RecoverFrontage'
        Pattern     = '\bRecoverFrontage\s*\('
        ProdAllowed = @()
        TestExempt  = $false
        ProdReason  = 'read FEntityInstance::GetFrontage, the edge the facade stored; a recovered frontage is a second opinion on where the plot faces (#450)'
    },
    @{
        # THE GHOST MATERIAL HAS ONE HOME (#450). ARoadNetworkActor::ResolveGhostMaterial is GetContent() plus a LoadSynchronous; the
        # actor reads it once per dirty mark into its resolved-content cache (#190, #298), and AAirsideBuildingsActor::Rebuild - which
        # runs on every Topology and Facts change - called it fresh around that cache. Everything else reads GetResolvedGhostMaterial.
        Name        = 'ARoadNetworkActor::ResolveGhostMaterial'
        Pattern     = '\bResolveGhostMaterial\s*\('
        ProdAllowed = @('Public\Present\RoadNetworkActor.h', 'Private\Present\RoadNetworkActor.cpp')
        TestExempt  = $true
        ProdReason  = 'read ARoadNetworkActor::GetResolvedGhostMaterial, the resolved-content cache - a fresh resolve is a GetContent plus a LoadSynchronous per call (#450)'
    },
    @{
        Name        = 'RoadHeal::PlanNodeDeletion'
        Pattern     = 'RoadHeal::PlanNodeDeletion\s*\('
        ProdAllowed = @('Public\Tool\RoadHeal.h', 'Private\Tool\RoadHeal.cpp', 'Private\Present\RoadEditFacade.cpp')
        TestExempt  = $true
        ProdReason  = "go through IRoadEditTarget::PlanNodeDeletion - URoadEditFacade is production's one wrapper"
    },
    @{
        # Issue #295's PR review: Id stays PUBLIC on FRoadAgent (a great many test fixtures set
        # it by hand), so unlike the private-field mutators the generalised rule 6 already
        # catches, nothing stopped a SECOND production site from writing Agent.Id directly - the
        # "one production door" RoadAgent.h claims for AssignId was a comment, not a check, until
        # this row. Confirmed by grep: GroundTraffic.cpp's Admit is the only call today, besides
        # AssignId's own definition in RoadAgent.h.
        Name        = 'FRoadAgent::AssignId'
        Pattern     = '\bAssignId\s*\('
        ProdAllowed = @('Public\Model\RoadAgent.h', 'Private\Model\GroundTraffic.cpp')
        TestExempt  = $true
        ProdReason  = "assign a new agent's id through UGroundTraffic::Admit, the one place NextAgentId is handed out"
    },
    @{
        # THE CLAIM ("outside Content/ and the actor") WENT FALSE ALREADY, same as issue #181's
        # RoadEditTarget.h comment on the runway-profile seam: #255's own grep for this row
        # found Private/Debug/RoadJunctionGallery.cpp:158 calling GetContent() directly, the
        # #78/#181 shape, undocumented before this table existed. It is listed here NOT as
        # sanctioned but as a REPORTED FINDING (see #255's PR) - fixing it is out of this
        # issue's scope (it is a lint issue, not a content-resolution one), and silently
        # allow-listing it away without saying so would be exactly the false comment this
        # issue exists to stop shipping. Follow-up: route it through ARoadNetworkActor.
        Name        = 'UAirsideSettings::GetContent (outside Content/ and the actor)'
        Pattern     = 'UAirsideSettings::GetContent\s*\(\s*\)'
        # #311: Private\Testing\AirsideTestGraph.cpp is TestProfiles::ServiceTiers - a TEST
        # fixture (the whole file behind WITH_DEV_AUTOMATION_TESTS, in the Public/Private
        # Testing/ split #189 established) that happens to live inside the Airside module
        # rather than AirsideTests, so $inTests's own '*Test.cpp' name match misses it - named
        # here because the file itself is not, the same exemption every test caller gets.
        ProdAllowed = @('Public\Content\AirsideSettings.h', 'Private\Content\AirsideSettings.cpp', 'Public\Present\RoadNetworkActor.h', 'Private\Present\RoadNetworkActor.cpp', 'Private\Debug\RoadJunctionGallery.cpp', 'Private\Testing\AirsideTestGraph.cpp')
        TestExempt  = $true
        ProdReason  = "go through Content/ or ARoadNetworkActor - see this row's own comment for the one pre-existing exception"
    },
    @{
        # THE ROW ABOVE, ONE LEVEL DOWN (#449): the settings CDO read raw is the same bypass as GetContent() - the apron's
        # build and upkeep rates were read that way in two plugins, outside every Resolve* door. Content/ reads it; the
        # rest ask a resolver (ResolveApronRates for the rates).
        Name        = 'GetDefault<UAirsideSettings> outside Content/'
        Pattern     = 'GetDefault\s*<\s*UAirsideSettings\s*>'
        ProdAllowed = @('Public\Content\AirsideSettings.h', 'Private\Content\AirsideSettings.cpp')
        TestExempt  = $true
        ProdReason  = 'ask a UAirsideSettings::Resolve* function (ResolveApronRates for the apron rates) - a raw CDO read is a second door (#449)'
    },
    @{
        # RigUtilityLookContentFields (#308, review of PR #335). ResolveRigVehicle's and
        # ResolveUtilityTowVehicle's own comments each claimed "the ONE place those four
        # fields are read" with no marker checking it - a comment stating a fact about other
        # code with nothing enforcing it, the shape #255 exists to close. These eight fields
        # name the rig's and the utility tow's look ONLY until #287 gives them their own
        # UVehicleType asset (see ResolveRigVehicle's/ResolveUtilityTowVehicle's own comments
        # for the retirement date); a second reader before then is a second source of truth
        # for what those two vehicles wear, same shape as the Piper fallback row above.
        Name        = 'RigUtilityLookContentFields'
        Pattern     = '(->|\.)\s*(RigCabMesh|RigTrailerMesh|RigCabAnimClass|RigTrailerAnimClass|UtilityMesh|UtilityTrailerMesh|UtilityAnimClass|UtilityTrailerAnimClass)\b'
        ProdAllowed = @('Private\Content\AirsideSettings.cpp')
        TestExempt  = $true
        ProdReason  = 'read these off UAirsideContent only from ResolveRigVehicle/ResolveUtilityTowVehicle (AirsideSettings.cpp) - a second reader is a second source of truth for the rig/utility look until #287 retires the fields'
    },
    @{
        # ONE FACTOR SITE (2026-09-27, shared pavement): the pavement factor meets a rate in
        # FBuildLine::Amount and in BuildCost's upkeep, nowhere else - a factor per buildable
        # kind is how roads came to be cheaper on grass while runways were not.
        Name        = 'Pavement::RateFactor'
        Pattern     = '\bRateFactor\s*\('
        ProdAllowed = @('Public\Model\Pavement.h', 'Private\Model\Pavement.cpp', 'Private\Model\BuildPurse.cpp', 'Private\Build\BuildCost.cpp')
        TestExempt  = $true
        ProdReason  = 'price through a FBuildLine (BuildCost::For*), which applies the factor once'
    },
    @{
        # ONE STAND ADMISSION (2026-09-27): size alone admitted an A380 to a grass F stand.
        Name        = 'IcaoCode::StandAdmits'
        Pattern     = '\bStandAdmits\s*\('
        ProdAllowed = @('Public\Solve\IcaoCode.h', 'Private\Solve\IcaoCode.cpp', 'Private\Model\StandAdmission.cpp')
        TestExempt  = $true
        ProdReason  = 'admit a stand through StandAdmission::Judge, which also checks its pavement and services'
    },
    @{
        # ONE SERVICE-ON-PAVEMENT RULE (2026-09-27, shared-pavement final review): Judge is the
        # only consumer, so restricting a role on grass is one body to change - a second caller
        # would be a second place the restriction has to reach.
        Name        = 'StandAdmission::PavementAdmitsRole'
        Pattern     = '\bPavementAdmitsRole\s*\('
        ProdAllowed = @('Public\Model\StandAdmission.h', 'Private\Model\StandAdmission.cpp')
        TestExempt  = $true
        ProdReason  = 'ask StandAdmission::Judge, which consumes it with the stand''s surface and size'
    },
    @{
        # ONE SIZE GATE (2026-09-27): the smallest stand letter's floor is read by
        # WhyStandRefused's size gate only; a second reader is a second opinion on 'too small'.
        Name        = 'IcaoCode::SmallestStandLetter'
        Pattern     = '\bSmallestStandLetter\s*\('
        ProdAllowed = @('Public\Solve\IcaoCode.h', 'Private\Solve\IcaoCode.cpp', 'Private\Present\RoadEditFacadeSurfaces.cpp')
        TestExempt  = $true
        ProdReason  = 'refuse a too-small stand through URoadEditFacade::WhyStandRefused, the one size gate'
    },
    @{
        # ONE REPAIR FUNCTION FOR BOTH LOADS (#426). The save-game load ran two of the four load-time repairs a level
        # gets, under a comment claiming it ran them all; ARoadNetworkActor::RepairLoadedNetwork is the one list now,
        # called by PostRegisterAllComponents and by URoadEditFacade::RestoreInPlace. A repair called from a second
        # production site is a second list that will drift from the first. EnsureStand* and EnsureDepotFrontages are also
        # URoadNetwork::PostLoad's own list (RoadNetwork.cpp, the definition file), which a level's asset load runs first.
        # (The two frontage migrations joined the row with #450's leftover: they are repairs of the same two loads.)
        Name        = 'load-time repairs'
        Pattern     = '\b(RefreshResolvedAnchors|EnsureStandOutlines|EnsureStandNumbers|EnsureDepotFrontages|EnsureStandFrontages|RepointTransientDefaultProfile)\s*\('
        ProdAllowed = @('Public\Entities\EntityDefinition.h', 'Private\Entities\EntityDefinition.cpp', 'Public\Model\RoadNetwork.h', 'Private\Model\RoadNetwork.cpp', 'Private\Present\RoadNetworkActor.cpp')
        TestExempt  = $true
        ProdReason  = 'a load-time repair runs from ARoadNetworkActor::RepairLoadedNetwork only - add it there, not beside a load'
    },
    @{
        # The definition rebind is the fourth repair; its own row because it forwards through a second class
        # (UStandDefinitionCache), whose files are allowed for the forward and nothing else.
        Name        = 'RebindStandDefinitions'
        Pattern     = '\bRebindStandDefinitions\s*\('
        ProdAllowed = @('Public\Present\StandDefinitionCache.h', 'Private\Present\StandDefinitionCache.cpp', 'Public\Present\RoadNetworkActor.h', 'Private\Present\RoadNetworkActor.cpp')
        TestExempt  = $true
        ProdReason  = 'rebind stand definitions from ARoadNetworkActor::RepairLoadedNetwork only (#426)'
    },
    @{
        # THE FLIGHT HALF OF A LOAD IN ITS ONE ORDER (#426): UFlightBoard::RestoreAfterLoad. Steps 1 and 2 are public
        # for the tests that pin one step's rule; a production caller outside the board is the order re-typed again.
        Name        = 'UFlightBoard load steps'
        Pattern     = '\b(DemoteRestoredMidFlight|CancelUnarrivedAtLoad)\s*\('
        ProdAllowed = @('Public\Model\FlightBoard.h', 'Private\Model\FlightBoard.cpp')
        TestExempt  = $true
        ProdReason  = 'call UFlightBoard::RestoreAfterLoad, which owns the order these steps are each correct only in'
    },
    @{
        # ONE DOOR FOR A LOAD (#426): OpsSave::Restore writes INTO the live network, and only the facade's
        # RestoreInPlace announces that, repairs it and adopts it. UOpsRuntime::LoadFromSlot's lambda is the one
        # production restore; a second one is a load no driver hears about.
        Name        = 'OpsSave::Restore'
        Pattern     = 'OpsSave::Restore\s*\('
        ProdAllowed = @('Public\Model\OpsSave.h', 'Private\Model\OpsSave.cpp', 'Private\Present\OpsRuntime.cpp')
        TestExempt  = $true
        ProdReason  = 'load through UOpsRuntime::LoadFromSlot, which restores inside URoadEditFacade::RestoreInPlace'
    },
    @{
        # THE NETWORK-REPLACING DOORS THAT ANNOUNCE ON URoadEditFacade::OnReplaced (#426), by their production callers.
        # URoadBuildEdMode does NOT listen to OnReplaced because none of these reaches an editor world (its own
        # header says so); a caller added under AirsideEditor would replace the network under the editor mode's
        # tool with nobody listening - bind the mode to OnReplaced first, then add the file here.
        Name        = 'network replacement doors'
        Pattern     = '\b(ClearNetwork|RestoreInPlace)\s*\('
        ProdAllowed = @('Public\Present\RoadEditFacade.h', 'Private\Present\RoadEditFacade.cpp', 'Private\Present\RoadEditFacadeSurfaces.cpp', 'Public\Present\RoadNetworkActor.h', 'Private\Present\RoadNetworkActor.cpp', 'Source\AirportMgr\RoadBuildController.cpp', 'Private\Present\OpsRuntime.cpp')
        TestExempt  = $true
        ProdReason  = 'a new caller of a network-replacing door must be a driver that listens to URoadEditFacade::OnReplaced (the editor mode does not) - see RoadBuildEdMode.h'
    },
    @{
        # AN EDIT SCOPE IS OPENED BY THE FACADE'S MUTATORS ONLY (#437, #460's review). Rule 40 checks that a
        # refusal inside one rolls back, but it reads RoadEditFacade*.cpp alone: a scope opened anywhere else is a
        # write to the network that skips CommitAndNotify's commit-and-notify pairing AND rule 40, and would
        # pass both unseen. Pattern is a DECLARATION (a type, a name, then an open paren or brace), so a
        # reference parameter or the ctor's own definition does not match. RoadEditHistory.* is where it is defined.
        # The open paren or brace is a LOOKAHEAD, the same token as rule 40's, so the two cannot disagree about
        # what a declaration is (rule 40's consumed the brace - see its own comment).
        Name        = 'FRoadEditScope (declared outside the facade)'
        Pattern     = '\bFRoadEditScope\s+\w+\s*(?=[({])'
        ProdAllowed = @('Private\Present\RoadEditFacade.cpp', 'Private\Present\RoadEditFacadeSurfaces.cpp', 'Private\Present\RoadEditHistory.cpp', 'Public\Present\RoadEditHistory.h')
        TestExempt  = $true
        ProdReason  = 'edit through a URoadEditFacade mutator (CommitAndNotify / CommitPurchase / CommitDisposal), so the edit is committed, notified and - on a refusal - rolled back; extend this row and rule 40 if a new file must open a scope'
    },
    @{
        # A SEGMENT'S PROFILE IS READ THROUGH URoadNetwork::ProfileFor (#459). A null FRoadSegment::Profile is legal and
        # means the network's default (a level load has always produced one; a save game's load does on purpose since
        # #459). Five readers dereferenced or null-tested the raw field: one crashed on the null a new-process load
        # left (StandTurnOffMarkingBuilder), four answered as if the road had no profile. The shape is `.Profile->` or
        # `.Profile == / != nullptr`, `.Get()`, a ternary condition or arm, a comparison, an argument, a copy - EVERY use
        # of `.Profile` / `->Profile` that is not an assignment TO it (#465 review: the first pattern listed the shapes
        # it had seen, and `Seg.Profile ? Seg.Profile.Get() : Net.DefaultProfile.Get()` walked past it). Grep on
        # 2026-09-30 found that on FRoadSegment alone, bar two other types excluded by name - URoadSurfacePresenter's
        # FSurfaceSettings::Profile and a follower's FSpeedProfile (Follower.Profile). It also found a SIXTH raw reader
        # the issue's list missed (EditTool's guide half-widths). ProfileFor itself reads the field and the graph surgery copies it
        # (RoadNetwork.cpp); the rebuild census counts own-profile against fallback on purpose (RoadRebuildCensus.cpp);
        # RoadHeal copies an arm's raw profile into the heal plan, where null still means the default
        # (FRoadDeletionPlan::HealProfile) - a copy, not a read of a width.
        Name        = 'FRoadSegment::Profile read raw'
        Pattern     = '(?<!Settings)(?<!Follower)(\.|->)Profile\b(?!\s*=(?!=))'
        ProdAllowed = @('Private\Model\RoadNetwork.cpp', 'Private\Debug\RoadRebuildCensus.cpp', 'Private\Tool\RoadHeal.cpp')
        TestExempt  = $true
        ProdReason  = "read a segment's profile through URoadNetwork::ProfileFor - a null one is legal and means the network's default (#459)"
    },
    @{
        # A CLASS'S GAP IS READ THROUGH FTrafficRules::GapFor, NEVER RAW (#455). GapFor floors it at half the footprint
        # (a refused vehicle must stop outside the zone where its own claim turns occupied), so a raw read of
        # VehicleGap/AircraftGap is the authored figure and not the one in force - the yardstick two tests were
        # measuring with, 36 uu short, until this row. The pattern is a READ: an assignment (`.VehicleGap = 777.0`, the
        # header's own default) is not, and a comment line is skipped by the loop. TrafficForwardersTest is the one test
        # allowed to read it - it asserts the knob REACHES the model, which is a statement about the field itself -
        # and the GapFor floor's own unit test lives there for the same reason.
        Name        = 'FTrafficRules Vehicle/AircraftGap (raw read)'
        Pattern     = '\b(Vehicle|Aircraft)Gap\b(?!\s*=[^=])'
        ProdAllowed = @('Public\Model\TrafficRules.h', 'Private\Model\TrafficRules.cpp')
        TestAllowed = @('Private\TrafficForwardersTest.cpp')
        ProdReason  = 'ask Rules.GapFor(Class), which floors the gap at half the footprint (#455) - the raw field is the authored figure, not the one in force'
        TestReason  = 'ask Rules.GapFor(Class) - the raw field is the authored figure, not the one in force (#455); only TrafficForwardersTest reads it, to assert the knob reaches the model'
    },
    @{
        # A DEPOT'S MODULES LEAVE THROUGH ONE DOOR (#266): the unplaced-module repair, whose removal and refund are one act.
        # TWO ROWS, one per layer, so neither can be skipped: URoadNetwork::RemoveEntityModules is the data write, and only
        # the facade's door calls it (a caller beside the door skips the rebuild and the undo checkpoint);
        # URoadEditFacade::RemoveUnseatedModules is the door, and only UOpsRuntime's ApplyModuleRemoval hook calls it - so a
        # module removed is always a module UFacilityPurchases::RemoveUnseated refunded. A second caller of either would take
        # modules the player paid for with no refund, no Warning and no toast.
        Name        = 'module removal (network write)'
        Pattern     = '\bRemoveEntityModules\s*\('
        ProdAllowed = @('Public\Model\RoadNetwork.h', 'Private\Model\RoadNetwork.cpp', 'Private\Present\RoadEditFacade.cpp')
        TestExempt  = $true
        ProdReason  = 'remove a module through URoadEditFacade::RemoveUnseatedModules, the door that rebuilds and checkpoints undo - and that only through the repair, which refunds it'
    },
    @{
        # The door's own row - see the network write's row above for why there are two.
        Name        = 'module removal (facade door)'
        Pattern     = '\bRemoveUnseatedModules\s*\('
        ProdAllowed = @('Public\Present\RoadEditFacade.h', 'Private\Present\RoadEditFacade.cpp', 'Private\Present\OpsRuntime.cpp')
        TestExempt  = $true
        ProdReason  = 'remove a module only through UFacilityPurchases::RemoveUnseated (the ApplyModuleRemoval hook UOpsRuntime wires to the facade), which refunds it, logs it and toasts it'
    },
    @{
        # ONE PREDICATE FOR "THE AIRPORT TAKES ARRIVALS" (#431): UAirport::AdmitsArrivals. `Status() == Open` was spelled
        # at nine sites across the ops model, the runtime and the game module, and reached the model three ways. A
        # comparison against Open outside the airport is that list growing a tenth entry.
        Name        = 'EAirportStatus::Open compared'
        Pattern     = '(==|!=)\s*EAirportStatus::Open\b|\bEAirportStatus::Open\s*(==|!=)'
        ProdAllowed = @('Public\Model\Airport.h', 'Private\Model\Airport.cpp')
        TestExempt  = $true
        ProdReason  = 'ask UAirport::AdmitsArrivals (or its static form for a status you hold) - the one predicate (#431)'
    },
    @{
        # ONE UAircraftType SCAN (#432): UAirsideSettings::EveryAircraftType. The Land panel, the letter envelope, the
        # test helper and the anim bench each walked the registry with their own rule, and only one waited for its
        # startup discovery - a scan too early under-counts, silently.
        Name        = 'UAircraftType registry scan'
        Pattern     = 'GetAssetsByClass\s*\(\s*UAircraftType::StaticClass'
        ProdAllowed = @('Private\Content\AirsideSettings.cpp')
        TestExempt  = $true
        ProdReason  = 'enumerate aircraft types through UAirsideSettings::EveryAircraftType(bMeshedOnly), the one synchronous scan (#432)'
    },
    @{
        # AN ACTOR'S FALLBACK IS MARKED IN ONE PLACE (#459, #465 review): ARoadNetworkActor::ResolveProfile, where it is
        # made. OpsSave writes every marked profile as none, so a second writer would make a real profile vanish from a
        # save. RoadProfile.h declares the field (its default).
        Name        = 'URoadProfile::bActorFallback written'
        Pattern     = '\bbActorFallback\s*=(?!=)'
        ProdAllowed = @('Public\Profiles\RoadProfile.h', 'Private\Present\RoadNetworkActor.cpp')
        TestExempt  = $true
        ProdReason  = 'only ARoadNetworkActor::ResolveProfile marks its fallback - see URoadProfile::bActorFallback'
    },
    # ONE DERIVATION (#438). The routing graph's passes - the solve, the restriction, the guideline builder, the anchor
    # links - and the Derived stamp are called from AirsideDerivation::Derive and nowhere else in production, so a pass
    # added there reaches every door at once: the presenter, TestGraph, the upgrade tool's what-if, the ops tests. Before
    # these rows URoadSurfacePresenter::RebuildInternal owned the sequence (behind its render-component check) and three
    # other sites re-typed it, each drifting - closed #101 and #311 were both a test-side copy production never ran.
    # Private\Testing\AirsideTestGraph.cpp is deliberately NOT listed, unlike the GetContent row: TestGraph goes through
    # Derive too - that is the point. Tests are exempt: a unit test of one pass (the linker's own reach, the builder's
    # own output) calls that pass alone on purpose. Each definition's own file is listed for its definition line.
    @{
        Name        = 'FRoadGuidelineBuilder::Build (outside the derivation)'
        Pattern     = 'FRoadGuidelineBuilder::Build\s*\('
        ProdAllowed = @('Private\Build\RoadGuidelineBuilder.cpp', 'Private\Build\AirsideDerivation.cpp')
        TestExempt  = $true
        ProdReason  = 'derive through AirsideDerivation::Derive (#438) - a second production sequence is how tests came to pass on a graph production never built'
    },
    @{
        Name        = 'FAnchorLink::Build (outside the derivation)'
        Pattern     = 'FAnchorLink::Build\s*\('
        ProdAllowed = @('Private\Build\AnchorLink.cpp', 'Private\Build\AirsideDerivation.cpp')
        TestExempt  = $true
        ProdReason  = 'link through AirsideDerivation::Derive (#438; its Links scope over a graph as it stands) - a second production sequence is how tests came to pass on a graph production never built'
    },
    @{
        # The upgrade hover's what-if used to call this by name on its ghost network; it asks the Facts scope now.
        Name        = 'TaxiwayRestriction::Apply (outside the derivation)'
        Pattern     = 'TaxiwayRestriction::Apply\s*\('
        ProdAllowed = @('Private\Model\TaxiwayRestriction.cpp', 'Private\Build\AirsideDerivation.cpp')
        TestExempt  = $true
        ProdReason  = 'restrict through AirsideDerivation::Derive (#438; the Facts scope for a what-if on a copy) - a second production sequence is how tests came to pass on a graph production never built'
    },
    @{
        # THE STAMP SAYS THE WHOLE DERIVATION RAN (#438): it moved from the end of FRoadGuidelineBuilder::Build, one pass,
        # to the end of Derive. A second production writer could stamp a graph whose links never ran as current.
        Name        = 'URoadNetwork::MarkGuidelinesDerived (outside the derivation)'
        Pattern     = '\bMarkGuidelinesDerived\s*\('
        ProdAllowed = @('Public\Model\RoadNetwork.h', 'Private\Build\AirsideDerivation.cpp')
        TestExempt  = $true
        ProdReason  = 'stamp through AirsideDerivation::Derive (#438) - the stamp says every pass of the derivation ran, and URoadNetwork::AreGuidelinesBehindRoad gates the planners on it'
    },
    @{
        # THE SOLVE THE MESH DRAWS IS THE SOLVE THE GRAPH WAS DERIVED FROM (#438): the presenter meshes from Derive's
        # returned result, so a second production SolveAll beside it would be a second evaluator of the same road.
        # RoadJunctionGallery (Private\Debug) solves a SCRATCH network of its own that it meshes and never routes on -
        # no graph to derive, which is why it is listed rather than routed through Derive.
        Name        = 'FRoadNetworkSolver::SolveAll (outside the derivation)'
        Pattern     = 'FRoadNetworkSolver::SolveAll\s*\('
        ProdAllowed = @('Private\Build\RoadNetworkSolver.cpp', 'Private\Build\AirsideDerivation.cpp', 'Private\Debug\RoadJunctionGallery.cpp')
        TestExempt  = $true
        ProdReason  = 'solve through AirsideDerivation::Derive (#438) and mesh from the result it returns, as URoadSurfacePresenter does'
    },
    @{
        # AN AGENT'S TRANSITION IS BROADCAST FROM ONE DOOR (#436). UGroundTraffic broadcast OnAgentPhaseChanged from eight
        # sites, one of them a Before/After diff, each passing (Id, From, To) and dropping WHY - so ops re-derived the
        # cause from the live agent a drain late. Now every site builds an FAgentTransition and hands it to
        # UGroundTraffic::Announce; rule 48 holds GroundTraffic.cpp to the one call inside it. The other file is a
        # RELAY of a delegate of the same name: UAirsideTraffic passes the model's transition on whole. (UOpsEvents was a third, the
        # ops bus's Blueprint face, until #445 cut its phase delegate - nothing listened.) Tests are exempt: OpsRuntimeBusTest stages
        # a transition on the relay directly.
        Name        = 'OnAgentPhaseChanged.Broadcast'
        Pattern     = '\bOnAgentPhaseChanged\.Broadcast\s*\('
        ProdAllowed = @('Private\Model\GroundTraffic.cpp', 'Private\Present\AirsideTraffic.cpp')
        TestExempt  = $true
        ProdReason  = 'build an FAgentTransition where the change is made and hand it to UGroundTraffic::Announce, the one broadcast (#436)'
    },
    @{
        # WHO LISTENS, IN PRODUCTION (#436 review): UAirsideTraffic (views, and the relay) and UOpsRuntime, which only
        # PUBLISHES into the ops bus - so no production listener re-enters UGroundTraffic inside the broadcast, which is
        # what GroundTraffic.cpp's re-entrancy comment now says. A third binder is a listener that may act mid-Advance
        # again (UJobBoard used to); it must publish, or the comment and the contract it describes must change with it.
        Name        = 'OnAgentPhaseChanged bound'
        Pattern     = '\bOnAgentPhaseChanged\.Add(UObject|Lambda|WeakLambda|Raw|SP)\s*\('
        ProdAllowed = @('Private\Present\AirsideTraffic.cpp', 'Private\Present\OpsRuntime.cpp')
        TestExempt  = $true
        ProdReason  = 'bind through UOpsRuntime (it publishes into the ops bus; handlers run a drain later) - a production listener that acts inside the broadcast re-enters UGroundTraffic mid-Advance (#436, #193)'
    },
    @{
        # THE CATCH-ALL TOAST IS RETIRED (#445 item 7). FNotificationEvent{FString} carried "Saved 'X'", "Save to 'X' failed" and
        # "No save 'X'" as English lines from UOpsRuntime, so the toast widget - the one place that claims to decide what the
        # player is told - could not tell a failed save from a good one, and showed both as Info. A save or a load is an
        # FSaveSlotEvent now (an EOpsSaveOutcome and the slot), a purchase an FOpsPurchase, and the widget words both. Nothing
        # brings the catch-all back, a test included: the bus's own tests publish FSaveSlotEvent instead.
        Name        = 'FNotificationEvent (retired)'
        Pattern     = '\bFNotificationEvent\b'
        ProdAllowed = @()
        ProdReason  = 'publish a typed event (FSaveSlotEvent, or a new one carrying the facts) and word it in UToastStackWidget (#445)'
        TestReason  = 'the catch-all is gone - publish a typed event (#445)'
    },
    @{
        # NO ONE-STRING FACE (#445 item 7): a dynamic delegate whose one argument is an FString is a sentence its publisher
        # decided - UOpsEvents::OnNotification and OnWarning were, and every save, load and purchase toast they carried was
        # worded in UOpsRuntime. A face carries facts (an enum, a USTRUCT, the nouns their owners name) and the toast widget
        # composes the sentence. FOpsLandRefused and FOpsArrivalRefused (Airside's refusal sentence BESIDE its reason - #456
        # review, and #471 for a dispatch's) and FOpsBuildRefused (three nouns) are not one-string faces. FString OR FText, by reference OR by value (#499 review): an FText sentence is
        # the same catch-all, localised.
        Name        = 'one-string dynamic delegate (a sentence face)'
        Pattern     = 'DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam\s*\(\s*\w+\s*,\s*(?:const\s+)?(?:FString|FText)\s*&?\s*,'
        ProdAllowed = @()
        TestExempt  = $true
        ProdReason  = 'carry the facts (an enum, a USTRUCT) and let the toast widget word them (#445)'
    },
    @{
        # A ONE-SUBSTEP EDGE IS READ ON THE SUBSTEP (#446). FLandingRun::bTouchedDown is true for the one Advance that put
        # the wheels down; UAirsideTraffic read it once a FRAME, after up to 32 substeps, and lost the smoke of three
        # landings in four at x8. FRoadAgent::Advance reads it on the call that set it and reports EAgentEvent::TouchedDown,
        # which UGroundTraffic collects per Advance - anything else hangs off that, not off the flag.
        Name        = 'FLandingRun::bTouchedDown read'
        Pattern     = '\bbTouchedDown\b'
        ProdAllowed = @('Public\Model\LandingRun.h', 'Private\Model\LandingRun.cpp', 'Private\Model\RoadAgent.cpp')
        TestExempt  = $true
        ProdReason  = 'hang it off EAgentEvent::TouchedDown (UGroundTraffic::GetMomentsThisAdvance) - the flag is an edge one substep long, and a per-frame read loses it at x4 and up (#446)'
    },
    @{
        # ONE SPELLING OF EACH VEHICLE KIND'S CODE (#430). The code is the join between a scenario's figures (AirportOps)
        # and Content's chassis (Airside), and it was typed in both modules - "FUEL" in AirsideSettings.cpp and
        # OpsDefinition.h, "UTILITY" in both - so a row that matched no chassis became a zero-size vehicle. Model/VehicleCodes.h
        # holds the three constants; everything else names AirsideVehicleCodes::. CASE-SENSITIVE ((?-i): Select-String is
        # not) and the whole quoted token, so "Fuel: ..." log text and "FUEL #7" in a doc comment are not codes.
        Name        = 'vehicle kind codes'
        Pattern     = '(?-i)"(?:FUEL|UTILITY|RIG)"'
        ProdAllowed = @('Public\Model\VehicleCodes.h')
        TestExempt  = $true
        ProdReason  = 'name the kind through AirsideVehicleCodes (Model/VehicleCodes.h) - the code is the join between a scenario row and its chassis, typed once'
    },
    @{
        # ONE CAPACITY RULE (#430, the #443 A13 note). A job's TankLitres was written from the row's raw Capacity at the bid
        # and the re-bid, and from FFuelRolePolicy::CapacityOf (floored at a litre) at the stand, so a zero-tank kind's card
        # read 0 L until the truck arrived. The row's Capacity is WRITTEN at the join (ServiceFleet.cpp) and READ by
        # CapacityOf alone (ServiceRolePolicy.cpp); everything else asks CapacityOf. CASE-SENSITIVE, a member access only:
        # FAirframe::FuelCapacityLitres and the quote's CapacityLitres are other names.
        Name        = 'vehicle row capacity'
        Pattern     = '(?-i)(?:\.|->)\s*Capacity\b'
        ProdAllowed = @('Private\Model\ServiceRolePolicy.cpp', 'Private\Model\ServiceFleet.cpp')
        TestExempt  = $true
        ProdReason  = "read a kind's tank through FFuelRolePolicy::CapacityOf, the one rule (floored at a litre) the bid, the stand and the shop's label share"
    },
    @{
        # ONE NAME FALLBACK (#430, and #461 before it). "The row's DisplayName, else its code" was typed three times, and
        # the depot card, the vehicle card and the stranded alert skipped it and printed the raw code - "FUEL #7" beside
        # the "Bowser" the shop sold. FServiceFleet::NameOf is the one resolver. BROAD ON PURPOSE: no production file
        # has another DisplayName.IsEmpty() today; a second KIND of named thing that needs one is a new row here, said.
        Name        = 'vehicle display-name fallback'
        Pattern     = 'DisplayName\s*\.\s*IsEmpty\s*\('
        ProdAllowed = @('Private\Model\ServiceFleet.cpp')
        TestExempt  = $true
        ProdReason  = "name a vehicle kind through FServiceFleet::NameOf, the one place its DisplayName falls back to its code"
    },
    @{
        # THE CATALOGUE'S TEST DOOR (#430, the #475 review). UJobBoard::CatalogueRowForTest hands out a mutable row so a
        # fixture can widen a body or empty a tank; a production caller would be a second writer of the catalogue beside
        # FServiceFleet::ResolveCatalogue, past rule 43's container patterns (it writes through a reference).
        Name        = 'UJobBoard::CatalogueRowForTest'
        Pattern     = '\bCatalogueRowForTest\s*\('
        ProdAllowed = @('Public\Model\JobBoard.h')
        TestExempt  = $true
        ProdReason  = "a catalogue row is written by FServiceFleet::ResolveCatalogue alone; read one through UJobBoard::TypeFor or GetCatalogue"
    },
    @{
        # THE FIXTURES' PER-FRAME DRIVER (#443, renamed from Tick). It runs the seeding and then Step - the two passes of a drain
        # in the order UOpsRuntime::WireBus registers them (rule 60(b) holds THAT order) - so a world-free fixture finds its
        # starter fleet as the game does. A production caller would be a second driver of the job board beside the bus's passes,
        # which is what gating Step on dirty passes removed.
        # THE PATTERN NAMES THE RECEIVER: UUiWindowHost has a TickForTest of its own (a production header), so a bare
        # `TickForTest(` would fail it. A call on anything named for a board, a service or the jobs - `Board->`, `Service.`,
        # `GetJobBoard()->` - is this one; the declaration in JobBoard.h is allowed below. A receiver named otherwise is a GAP,
        # said: the compiler still sees it, and a production caller of a *ForTest driver is what this row is for.
        Name        = 'UJobBoard::TickForTest'
        Pattern     = '\b\w*(?:Board|Service|Jobs)\w*\s*(?:\(\s*\))?\s*(?:\.|->)\s*TickForTest\s*\('
        ProdAllowed = @('Public\Model\JobBoard.h')
        TestExempt  = $true
        ProdReason  = "the job board is driven by the ops bus's FleetSeed and JobBoard passes; TickForTest is the world-free fixtures' mirror of them"
    },
    @{
        # ONE HOME PER OPS DESIGN DEFAULT (#449): the refill rate was typed 500 in three files, the inbox cap 8 in two,
        # and the fallback fuel load 0.7 beside the offer's 0.5-0.9 draw. They live in OpsDesignDefaults.h; a literal
        # assigned to one of these names, or the draw typed out, is the next copy. A test sets them freely.
        Name        = 'ops design default typed twice'
        Pattern     = '\b(RefillLitresPerMinutePerPump|DepotRefillLitresPerMinutePerPump|MaxPendingOffers)\s*=\s*[0-9]|FRandRange\s*\(\s*0\.5\s*,\s*0\.9|FuelCapacityLitres\s*,\s*0\.0\s*\)\s*\*\s*0\.'
        ProdAllowed = @('Public\Model\OpsDesignDefaults.h')
        TestExempt  = $true
        ProdReason  = 'use the constant in Model/OpsDesignDefaults.h - the scenario asset overrides it per game through UOpsRuntime::ApplyScenarioFigures (#449)'
    },
    @{
        # THE RULES REACH A ROUTE QUERY THROUGH ONE CALL (#449): FRouteQuery::WithRules copies RunwayPenalty AND
        # CongestionWeight off FTrafficRules. Six sites typed them across by hand, and the rejoin forgot the penalty. An
        # assignment to either field, or the explicit-weight WithCongestion, outside the query and the rules is that
        # copy again. A test states its own figures, so tests are exempt.
        Name        = 'route query cost figure copied'
        Pattern     = '(\.|->)(RunwayPenalty|CongestionWeight)\s*=(?!=)|\bWithCongestion\s*\('
        ProdAllowed = @('Public\Model\RouteSearch.h', 'Private\Model\RouteSearch.cpp', 'Public\Model\TrafficRules.h')
        TestExempt  = $true
        ProdReason  = 'build the query with FRouteQuery::WithRules(Rules, Occupancy, Agent) - both figures, from the rules in force (#449)'
    },
    @{
        # A LIVE AGENT'S ROUTE CHANGES IN ONE PLACE (#429). Nine operations each took a new plan with their own subset
        # of the aftermath - reservations, guideline claims, arbitration, stall, engine, pose, goal - and the ops plugin
        # and the game module composed two more. FRoadAgent::ApplyRouteChange is the one door to the follower's
        # Replace, RestartTaxi and RejoinTaxi; RestartTaxi and RejoinTaxi are private as well (the dispatches in
        # RoadAgent.cpp call RestartTaxi from cold, which is a birth, not a change of route), so these two rows are
        # the lint's half of what C++ access already refuses, and the Replace row is the whole of it - Follower is a
        # public member. VehicleFit's own local FRouteFollower (named Follower) calls Start and Advance, never Replace.
        Name        = 'Follower.Replace (a live route change)'
        Pattern     = '\bFollower\.Replace\s*\('
        ProdAllowed = @('Private\Model\RoadAgent.cpp')
        TestExempt  = $true
        ProdReason  = 'change a live agent''s route through UGroundTraffic::ChangeRoute (or FRoadAgent::ApplyRouteChange where there is no traffic) - a caller that replaces the follower by hand picks its own aftermath (#429)'
    },
    @{
        Name        = 'FRoadAgent::RestartTaxi'
        Pattern     = '\bRestartTaxi\s*\('
        ProdAllowed = @('Public\Model\RoadAgent.h', 'Private\Model\RoadAgent.cpp')
        TestExempt  = $true
        ProdReason  = 'restart through a Restart route change (FRouteChange::Restart via UGroundTraffic::ChangeRoute), which carries the engine and ends the wait (#429)'
    },
    @{
        Name        = 'FRoadAgent::RejoinTaxi'
        Pattern     = '\bRejoinTaxi\s*\('
        ProdAllowed = @('Public\Model\RoadAgent.h', 'Private\Model\RoadAgent.cpp')
        TestExempt  = $true
        ProdReason  = 'rejoin through a Rejoin route change (FRouteChange::Rejoin via UGroundTraffic::ChangeRoute or FRoadAgent::ApplyRouteChange), which lets go of the old claims and moves the goal (#429)'
    },
    @{
        # THE AGENT'S HALF OF THE SEAM (#429) has two production callers: UGroundTraffic::ChangeRoute (GroundTraffic.cpp),
        # which brackets it with the goal's claims, and FPlanReResolver (GroundTrafficRebuild.cpp), which has no
        # UGroundTraffic and only keeps or re-points a goal. A third caller is a route change that skips the traffic's
        # goal bracket - one that cannot MOVE the goal, since ERouteGoal has no Move (#429 review), but can still skip
        # the ChangeRoute its operation should have been.
        Name        = 'FRoadAgent::ApplyRouteChange'
        Pattern     = '\bApplyRouteChange\s*\('
        ProdAllowed = @('Public\Model\RoadAgent.h', 'Private\Model\RoadAgent.cpp', 'Private\Model\GroundTraffic.cpp', 'Private\Model\GroundTrafficRebuild.cpp')
        TestExempt  = $true
        ProdReason  = 'change a route through UGroundTraffic::ChangeRoute, which moves the goal with its claim (#429)'
    },
    @{
        # ONE TOW SEED BUILDER (#429, #313): FRoadAgent::LiveTowSeed / LiveTowSeedAtRest. Five sites in three modules built
        # the seed by hand and had drifted (one heading from the follower where the rest read LastMotion; two with no
        # fold check). VehicleFit's own NextSeed is the section-to-section seed INSIDE JudgePlan - the judge carrying
        # its chain across a reverse, not a caller seeding it. Declarations and pointer/reference parameters
        # (`const FTowSeed* Seed`) do not match: only a local constructed, a temporary - braced or `FTowSeed()` (#429
        # review) - or an Emplace.
        Name        = 'FTowSeed built'
        Pattern     = '\bFTowSeed\s+\w+\s*[;={(]|\bFTowSeed\s*[({]|\bTowSeed\.Emplace\s*\('
        ProdAllowed = @('Private\Model\RoadAgent.cpp', 'Private\Model\VehicleFit.cpp')
        TestExempt  = $true
        ProdReason  = 'seed a tow''s judgement from FRoadAgent::LiveTowSeed (or LiveTowSeedAtRest for a parked tow) - a second builder is how the five copies drifted (#429, #313)'
    },
    @{
        # A QUERY'S SEED IS SHAPED BY ITS BUILDER, NOT AFTER IT (#429 review). RejoinNearby took the live seed and wrote
        # `TowSeed->Speed = 0.0` over it - "from rest (RestartTaxi below)" - which was true of one caller of three and
        # then of none, while the agent it judged rejoined at speed. Where along a new plan the cab starts and how fast
        # it is going are LiveTowSeedAtRest's and LiveTowSeedJoining's to say; a field written into a query's seed by
        # hand is a sixth builder. (ExtendRoute's `Seed->Travelled -= Dropped` on its own local re-bases the seed to
        # the TRIMMED plan it judges - a plan offset, not a guess at the pose - and is not a query's seed.)
        Name        = 'FTowSeed shaped by hand'
        Pattern     = '\bTowSeed\s*->\s*\w+\s*[-+*/]?=(?!=)|\bTowSeed\.GetValue\(\)\.\w+\s*[-+*/]?=(?!=)'
        ProdAllowed = @('Private\Model\RoadAgent.cpp')
        TestExempt  = $true
        ProdReason  = 'take the seed whole from FRoadAgent::LiveTowSeed / LiveTowSeedAtRest / LiveTowSeedJoining - a field written into it afterwards is a second opinion on where and how fast the tow is (#429)'
    },
    @{
        # THE UI PRESENTS THE MODEL'S ANSWER; IT DOES NOT RE-DECIDE IT (#447). "Overdrawn" was `Balance() < 0.0` in the bar, the ledger panel's view model
        # and the alert: the lock on paid placements and the red on the balance could have parted. ULedger::IsOverdrawn is the one definition.
        Name        = 'overdrawn is the ledger''s'
        Pattern     = '\bBalance\s*\(\s*\)\s*<\s*0'
        ProdAllowed = @('Public\Model\Ledger.h')
        TestExempt  = $true
        ProdReason  = 'ask ULedger::IsOverdrawn - a second `Balance() < 0` is a second definition of overdrawn, which a change to the lock would leave behind (#447)'
    },
    @{
        # The contract's left/late was `AirborneBy() - Now` in the arrivals row, the aircraft card's turnaround line and its gate, and #398 changes what
        # "late" means. UFlight::ContractSecondsLeft / IsLate is the one subtraction. (FlightBoard scores a flight at its AirborneAt - `AirborneAt -
        # AirborneBy()` - a different instant, not matched here.)
        Name        = 'contract left is the flight''s'
        Pattern     = '\bAirborneBy\s*\(\s*\)\s*-'
        ProdAllowed = @('Public\Model\Flight.h')
        TestExempt  = $true
        ProdReason  = 'ask UFlight::ContractSecondsLeft(Now) or IsLate(Now) - a second `AirborneBy() -` is a second rule for what late means (#447, #398)'
    },
    @{
        # MOVEMENT SECONDS -> GAME SECONDS is USimClock::GameSecondsOfMovement (#447): a public static on a widget and the job board's bid each wrote
        # `x GameSecondsPerRealSecond(TimeOfDay())`. The band's rate itself is the clock's own business, so the clock's files only.
        Name        = 'movement seconds convert on the clock'
        Pattern     = '\bGameSecondsPerRealSecond\s*\('
        ProdAllowed = @('Public\Model\SimClock.h', 'Private\Model\SimClock.cpp')
        TestExempt  = $true
        ProdReason  = 'convert movement seconds with USimClock::GameSecondsOfMovement - the conversion between the two time bases lives on the clock (#447)'
    },
    @{
        # GAME TIME IS FORMATTED IN ONE FILE (#447): "Day %d  %02d:%02d" was the bar's, the ledger's and a log line's, each deriving the day itself, and a
        # duration was "+95 min" on the depot card beside "1 h 35 min" on the aircraft card.
        Name        = 'game-clock text'
        Pattern     = '%02d:%02d'
        ProdAllowed = @('Private\Model\GameTimeText.cpp')
        TestExempt  = $true
        ProdReason  = 'format a game time through GameTimeText::TimeOfDay / Stamp (and a span through Duration) - the clock face is written in GameTimeText.cpp alone (#447)'
    },
    @{
        # WHETHER A WINDOW IS OPEN IS THE HOST'S (#447): four panels kept a bShowing each, a Toggle, an IsShowing and an OnWindowClosedByPlayer resync beside
        # the host's bWanted/bUserClosed and the base's bShownRequested. UUiWindowHost::Toggle and UAirportMgrPanelWidget::IsShown are the one state and the
        # one answer; FUiWindowSpec::bToggled makes the close the toggle; OnShownChanged is what a panel did on its own Toggle. Rule 66 is the list that must agree.
        Name        = 'panel shown-state is the host''s'
        Pattern     = '\bbool\s+bShowing\s*[=;]'
        ProdAllowed = @()
        TestExempt  = $true
        ProdReason  = 'ask IsShown() and let the host own the state (UUiWindowHost::Toggle, FUiWindowSpec::bToggled, OnShownChanged) - a panel keeping its own flag is the copy that parts from the window (#447)'
    },
    @{
        # ONE DEFINITION OF STUCK (#429): FRoadAgent::IsStoppedAndWaiting feeds the stall clock, HasStalledFor is a
        # stalled waiter past a bound, IsStuck adds Stranded - all three in RoadAgent.h. "Stuck" was spelled three ways
        # in two modules (the accrual rule, the resolver's waiter test, the Unstick button's `stall >= Seconds`), and the
        # copies disagreed on the bound and on whether a blocker had to be named. The stall clock COMPARED to a bound
        # anywhere else is a fourth spelling, and so is InspectFacts' copy of it (Hold.StalledSeconds) compared: reading
        # either as a value, to show it, is not. Either operand order, and any root - `Agents[I].` included (#429 review).
        Name        = 'stall clock compared'
        Pattern     = 'GetStalledSeconds\s*\(\s*\)\s*[<>]|(?<![-<>])[<>]=?\s*[\w.\[\]>-]*GetStalledSeconds\s*\(|\bHold\.StalledSeconds\s*[<>]|(?<![-<>])[<>]=?\s*[\w.\[\]>-]*Hold\.StalledSeconds\b'
        ProdAllowed = @('Public\Model\RoadAgent.h')
        TestExempt  = $true
        ProdReason  = 'ask FRoadAgent::IsStuck (or HasStalledFor for a wait-for edge) - the one definition of stuck, whose bound and blocker test the copies had drifted from (#429)'
    },
    @{
        # ONE TAXI-IN QUERY (#429 review): ArrivalPlanner::ChooseStand chooses a stand by it and UGroundTraffic::
        # ReofferStand drives the waiter there by a route SendAgentTo searches with it. They were built twice, by hand
        # and by For(); if the two drift, a stand is chosen that the drive's search cannot reach, and the waiter is
        # offered it and refused every pass. ArrivalPlanner::TaxiInQuery is the one builder - the errand named anywhere
        # else in production is a second one. The policy table (RoutePolicy.cpp) names it to say what it means.
        Name        = 'taxi-in query built'
        Pattern     = '\bERouteErrand::ArrivalTaxiIn\b'
        ProdAllowed = @('Private\Model\ArrivalPlanner.cpp', 'Private\Model\RoutePolicy.cpp')
        TestExempt  = $true
        ProdReason  = 'take the taxi-in query from ArrivalPlanner::TaxiInQuery, the one the stand choice searches with (#429 review)'
    },
    @{
        # RULE 74 (#446): THE UNSTICK MENU IS OPENED BY A CALL. The selection.unstick row's Execute bumped a counter on the controller
        # (UnstickMenuRequests) that the inspector compared with the last count it had seen, every tick, beside two panels the same controller reached by
        # a direct call. The row calls UBuildHudLayer::OpenUnstickMenu now. The names are banned outright, and so is the SHAPE - a `Seen...Request(s)`
        # member is a count somebody diffs on a tick. Rule 74's block is the half that stops this checking nothing.
        Name        = 'unstick request counter (rule 74)'
        Pattern     = '\b(?:UnstickMenuRequests|RequestUnstickMenu|GetUnstickMenuRequests)\b|\bSeen\w*Requests?\b'
        ProdAllowed = @()
        TestExempt  = $true
        ProdReason  = 'open the popup with a call: the row runs UBuildHudLayer::OpenUnstickMenu, which calls the inspector - a request count the inspector diffs on a tick is the polling #446 removed'
    },
    @{
        # RULE 75 (#446): A SELECTION CHANGE IS ANNOUNCED, NOT DIFFED. The inspector kept `LastSelection` and compared (Kind, Id) with it every tick to
        # reopen, close its menus and disarm a sale. FBuildSession::OnSelectionChanged says it once per real change; the panel's HandleSelectionChanged
        # is what it forwards to.
        Name        = 'selection diffed on a tick (rule 75)'
        Pattern     = '\b(?:Last|Prev|Previous)Selection\b'
        ProdAllowed = @()
        TestExempt  = $true
        ProdReason  = 'do not remember the last selection to compare it: subscribe to FBuildSession::OnSelectionChanged (the controller forwards it to UInspectorWidget::HandleSelectionChanged) (#446)'
    },
    @{
        # RULE 75 (#446): EVERY WRITE OF THE SESSION'S SELECTION GOES THROUGH THE DOOR. `Selection->Kind = X` from a tool, `*Context.Selection = S`, or
        # `Selection.Clear()` changes the selection and announces nothing - the silence the inspector answered with a per-tick diff. A tool writes through
        # FToolContext::SetSelection/ClearSelection, code through FBuildSession::Select; both land in SelectionDoor::Write, which compares, stores and
        # announces. (A local FSelection being BUILT - `Out.Kind = ...` in FSelectTool::MakeSelection - is not the session's and is not matched.)
        Name        = 'selection written outside the door (rule 75)'
        Pattern     = '\b(?:Selection|Sel)\s*(?:\.|->)\s*(?:Kind|Id|Generation)\s*=(?!=)|(?<![\w>\]])\*\s*(?:\w+\s*(?:\.|->)\s*)*Selection\s*=(?!=)|\b(?:Selection|Sel)\s*(?:\.|->)\s*Clear\s*\('
        ProdAllowed = @()
        TestExempt  = $true
        ProdReason  = 'write the selection through FToolContext::SetSelection / ClearSelection (a tool) or FBuildSession::Select (code) - the door announces the change once (#446)'
    },
    @{
        Name        = 'SelectionDoor::Write callers (rule 75)'
        Pattern     = '\bSelectionDoor::Write\s*\('
        ProdAllowed = @('Public\Tool\Selection.h', 'Public\Tool\RoadBuildTool.h', 'Public\Tool\BuildSession.h')
        TestExempt  = $true
        ProdReason  = 'the door has two callers - FToolContext::SetSelection and FBuildSession::WriteSelection; a third is a second place that decides what a change is (#446)'
    },
    @{
        # RULE 76 (#446): THE BAR DESCRIBES THE SELECTED RUNWAY ONCE A FRAME. BuildActions' SelectedRunway ran InspectFacts::DescribeRunway from the
        # IsEnabled and the DynamicLabel of the flip and the mode rows - four times a tick with a runway selected. The controller's
        # SelectedRunwayFactsThisFrame answers all of them from one; the inspector's runway card has its own key-gated call.
        Name        = 'InspectFacts::DescribeRunway callers (rule 76)'
        Pattern     = '(?<![\w.>])(?:InspectFacts::)?DescribeRunway\s*\('
        ProdAllowed = @('Public\Model\InspectFacts.h', 'Private\Model\InspectFacts.cpp', 'Source\AirportMgr\InspectorNetworkCards.cpp', 'Source\AirportMgr\RoadBuildController.cpp')
        TestExempt  = $true
        ProdReason  = "ask ARoadBuildController::SelectedRunwayFactsThisFrame - one describe a frame for every bar row that needs the selected runway's facts (#446); the runway card's own call is InspectorNetworkCards.cpp's, gated by its key"
    },
    @{
        # THE BUS POINTERS ARE POINTED AND TAKEN BACK IN ONE LOOP EACH (#445): UOpsRuntime::Publishers() is the list, Attach writes every
        # slot and Detach clears every slot. A `*Publisher.Slot =` anywhere else is a second writer - a publisher pointed at a bus the runtime
        # does not know it gave it to, and never taken back. (Rule 71 counts the list against the classes that declare a Bus field.)
        Name        = 'UOpsRuntime publisher slot written'
        Pattern     = '\*\s*\w+\.Slot\s*=(?!=)'
        ProdAllowed = @('Private\Present\OpsRuntime.cpp')
        TestExempt  = $true
        ProdReason  = 'point a publisher at the bus by adding it to UOpsRuntime::Publishers(); Attach and Detach are the only writers of its slots (#445)'
    },
    @{
        # RULE 82 (#444): AN AGENT'S WAIT IS WRITTEN THROUGH ITS DOOR. EAgentWait and its payload (bWaitSaid, WaitRefusedAt) replaced
        # bAwaitingStand and a flag triple that was reset by hand at three sites, one of them a bare `bTaxiOutStale = true` past the
        # mutator. The fields are private, but UGroundTraffic and FClaimPass are FRoadAgent's friends and compile a direct write - so
        # this, not access, is what keeps them to FRoadAgent::WaitFor / EndWait (which leave a re-armed wait's payload as it was).
        # FRoadAgent's own two files are the door; rule 6's `Agent.<field> =` shape misses a write through any other name.
        Name        = 'agent wait written outside its door (rule 82)'
        Pattern     = '\b(?:Wait|bWaitSaid|WaitRefusedAt)\s*=(?!=)|\bWaitRefusedAt\s*\.\s*(?:Reset|Emplace)\s*\('
        ProdAllowed = @('Public\Model\RoadAgent.h', 'Private\Model\RoadAgent.cpp')
        TestExempt  = $true
        ProdReason  = 'arm or end a wait through FRoadAgent::WaitFor / EndWait, and its payload through MarkWaitSaid / MarkWaitRefusedAt / ForgetWaitRefusal - the door keeps the wait and its payload together (#444)'
    },
    @{
        # RULE 83 (#444): A POINT ON A RUNWAY IS FRunwayEnd::PointAt. "Threshold + Direction * x" was spelled at five sites after #88
        # bundled the end (the landing and take-off poses, the taxi's runway-entry guard, two probes just inside an end) - the
        # #88 shape, back. DOES NOT SEE the sum through a local copy of either field.
        Name        = 'runway point spelled by hand (rule 83)'
        Pattern     = '\bThreshold\s*\+\s*[\w\.\->]*Direction\s*\*'
        ProdAllowed = @('Public\Model\RunwayFacts.h')
        TestExempt  = $true
        ProdReason  = 'ask FRunwayEnd::PointAt(Along) - the one spelling of a point along the strip (#444)'
    }
)
# EACH FILE'S COMMENT-STRIPPED LINES, made once and only for a file some row's raw pattern hits (Strip-ArchComments, with the helpers at the top).
$arch4Code = @{}
foreach ($row in $AllowedCallers) {
    foreach ($tree in $trees) {
        foreach ($file in Get-Sources $tree @('.h', '.cpp')) {
            # A *Test.cpp file in a MIXED module (AirportMgr, AirsideEditor - rule 10's own
            # scoping) is a test caller too, not just one under a dedicated AirsideTests/
            # AirportOpsTests folder - InspectorWidgetTest.cpp calling ResolveDefaultAirframe
            # directly is exactly this and is not the second-source-of-truth this row exists
            # to catch.
            $inTests = ($file.FullName -match '\\(AirsideTests|AirportOpsTests)\\') -or ($file.Name -like '*Test.cpp')
            # Issue #291: PATH SUFFIX, not bare-name equality - see Test-AllowedPathSuffix and
            # the table comment above. A stray same-named file elsewhere in the tree no longer
            # rides along with the real one just because Get-ChildItem happened to find it too.
            if (($row.ProdAllowed | Where-Object { Test-AllowedPathSuffix $file $_ }).Count -gt 0) { continue }
            if ($inTests -and $row.TestAllowed -and (($row.TestAllowed | Where-Object { Test-AllowedPathSuffix $file $_ }).Count -gt 0)) { continue }
            if ($inTests -and $row.TestExempt) { continue }
            $hits = Select-String -Path $file.FullName -Pattern $row.Pattern
            foreach ($h in $hits) {
                # A COMMENT IS DECIDED FROM COMMENT-STRIPPED CODE, NOT FROM A LEADING `*` (#446's review). This used to skip every line whose trimmed text starts
                # with `//`, `/*` or `*` - and a dereference WRITE, `*Context.Selection = InSelection;`, starts with `*`, so a row aimed at it could never fire (nor
                # could #492's `*Slot = ...` row). The raw hit is now confirmed against the same line with comments removed (block comments tracked across
                # lines, string literals kept): a match inside a comment, leading or trailing, is dropped; a code line that merely starts with `*` is judged.
                if (-not $arch4Code.ContainsKey($file.FullName)) {
                    $arch4InBlock = $false
                    $arch4Code[$file.FullName] = @(Get-Content -LiteralPath $file.FullName | ForEach-Object { Strip-ArchComments $_ ([ref]$arch4InBlock) })
                }
                $arch4Lines = $arch4Code[$file.FullName]
                if ($h.LineNumber -gt $arch4Lines.Count -or $arch4Lines[$h.LineNumber - 1] -notmatch $row.Pattern) { continue }
                $reason = if ($inTests -and $row.TestReason) { $row.TestReason } else { $row.ProdReason }
                $failures.Add("allowed-callers: $($file.FullName):$($h.LineNumber) $($row.Name) - ${reason}: $($h.Line.Trim())")
            }
        }
    }
}
$ranRules.Add('allowed callers')

# --- 5. No hand-built slot-map handle outside RoadSlotMap.h -----------------------------
# Three shapes a hand-built {index, generation} takes at a call site (#79, #173):
#   {Index, Item.Generation}         - brace-init from a loop index and the item's field
#   X.Generation = Item.Generation   - the generation half, wherever the Index half came from
#   X.Index = Index; ... X.Generation = ...   - split across up to two statements
# RoadSlotMap.h is the one legal definer (RoadSlot::Add and RoadSlot::HandleAt themselves).
# Test modules are exempt (see the doc comment above) the same way rule 4 exempts them.
#
# EVERY \w+ SUBJECT BELOW IS \w+(\.\w+)*, not a bare \w+ (issue #191's review comment). PR
# #220's first draft built {Segment.Index, Segment.Generation} - a DOTTED field on both
# sides of the brace - and the bare \w+ this rule used to have could not match "Segment.Index"
# at all, so the whole hit went unseen until a human caught it in review. All three regexes
# get the same fix, since all three take a hand-built field as their subject.
$handlePatterns = @(
    '\{\s*\w+(\.\w+)*,\s*\w+(\.\w+)*(\[\w+\])?\.Generation\s*\}',
    '\.Generation\s*=\s*\w+(\.\w+)*(\[\w+\])?\.Generation'
)
# "Within 2 lines": the Index line, then at most one line between it and the Generation line.
$handleSplitPattern = '\.Index\s*=\s*\w+(\.\w+)*;[^\n]*\n(?:[^\n]*\n){0,1}[^\n]*\.Generation\s*='
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
$ranRules.Add('hand-built handles')

# --- 6. Every FRoadAgent field written only through its own mutator ----------------------
# Generalised (issue #295) from #174's enumerated field list to ANY `Agent.<field> =` /
# `Agents[i].<field> =` - see this rule's own comment above for why the enumerated form was
# a list that had to agree with RoadAgent.h's field list and did not. `[^=]` after the `=`
# excludes `==`/`!=`/`>=`/`<=` comparisons, which a bare `=` check would otherwise also match
# (an assignment's `=` is a leading substring of all four). RoadAgent.cpp itself is exempt -
# it is the one file allowed to write these fields, being where the mutators are defined.
#
# WIDENED TO MEMBER CHAINS (issue #429): `Agents[Index].Follower.Plan.Result =` is a write of the
# agent's route by hand, and the one-level pattern never saw it - `\.(?<field>\w+)\s*=` fails at
# the second dot. The chain is now any run of `.member` / `[index]` after the root, and what is
# allow-listed is the WHOLE chain (e.g. 'Class', 'Follower.Plan.Result'), so allowing one nested
# write does not allow its siblings.
# The chain OPENS WITH A MEMBER, as the one-level pattern's did: `PrevAgent[Slot] =` is an int array
# whose name ends in Agent, not an agent's field.
# AND THROUGH A POINTER, AND COMPOUND (#429 review): `Agent->Phase =` is the same write through an
# FRoadAgent*, and `Agent.Follower.Travelled -= Dropped` - the very shape RebaseTravelled exists to
# name - was an assignment the plain `=` could not see (`-=`, `+=`, `*=`, `/=`, `%=`, `|=`, `&=`,
# `^=`). `(?!=)` after the `=` keeps `==` out; `<=`, `>=` and `!=` never match, because nothing
# before their `=` is a member name or one of the compound operators.
$agentFieldPattern = '(Agent|Agents\[[A-Za-z0-9_]+\])(?<chain>(\.|->)\w+(\.\w+|\[[^\]]*\])*)\s*[-+*/%|&^]?=(?!=)'
# THE ONE DELIBERATE REMAINDER (issue #295): Class stays PUBLIC on FRoadAgent (see its own
# comment in RoadAgent.h) and is set directly at its two birth sites, both in
# GroundTraffic.cpp (DispatchArrival, AdmitDispatched). Allow-listed BY FILE AND FIELD NAME,
# not by exempting the file outright - a future direct write of any OTHER field in
# GroundTraffic.cpp still fails this rule. A LISTED FILE THAT STOPS EXISTING, or a field name
# that stops appearing there, is not this rule's problem to notice; it just stops matching.
# AND ONE CHAIN (issue #429): UGroundTraffic::StrandForTest marks the live plan unreachable by hand -
# a test hook reached only through FGroundTrafficTestAccess, standing in for the rebuild's Strand
# (see its declaration), and not a route change: nothing is re-planned, the plan just dies.
$agentFieldAllowlist = @{ 'GroundTraffic.cpp' = @('Class', 'Follower.Plan.Result') }
# TAKING THE PLAN'S ADDRESS IS A WRITE WAITING TO HAPPEN (issue #429): the rebuild holds
# `Plan = &Agent.Follower.Plan` and writes Steps, Start and Result through the pointer, which no
# assignment pattern can see. Outside RoadAgent.cpp only the rebuild's re-resolve may - it
# re-points a live plan's HANDLES in place after the graph was rebuilt, which is not a change of
# route (FPlanReResolver::ReResolvePlan; the route changes it makes go through the seam, FRoadAgent::
# ApplyRouteChange) - and only as often as it does today: the three plan selections in
# OnGraphRebuilt and ReResolvePlan's "is this the follower's plan" comparison. A fifth is a new
# writer, and the cap says so rather than the file being waved through.
# ANY ROOT, AN INDEX OR A POINTER (#429 review): `&Agents[i].Follower.Plan` and `&Truck->Follower.Plan`
# take the same address. The `&` must be a unary one - not the second of `&&`, and not a binary `&`
# after an operand (an identifier, `)` or `]`, spaces allowed): `Phase == Taxiing && Truck->Follower.Plan`
# reads the plan, it does not take its address.
$agentPlanAddressPattern = '(?<![\w\)\]&]\s*)&(?!&)\s*\w+(\[[^\]]*\])?(\.|->)(Follower|Pushback)\.Plan\b'
$agentPlanAddressAllowed = @{ 'GroundTrafficRebuild.cpp' = 4 }
foreach ($tree in $trees) {
    foreach ($file in Get-Sources $tree @('.cpp')) {
        if ($file.Name -eq 'RoadAgent.cpp') { continue }
        if ($file.FullName -match '\\(AirsideTests|AirportOpsTests)\\') { continue }
        $hits = Select-String -Path $file.FullName -Pattern $agentFieldPattern
        foreach ($h in $hits) {
            # A WHY comment naming the banned shape (this rule's own fix does, at
            # GroundTrafficRebuild.cpp) is not the shape itself - same exemption as rule 5.
            if ($h.Line.Trim().StartsWith('//')) { continue }
            $field = [regex]::Match($h.Line, $agentFieldPattern).Groups['chain'].Value -replace '^(\.|->)', ''
            $allowedFields = $agentFieldAllowlist[$file.Name]
            if ($allowedFields -and ($allowedFields -contains $field)) { continue }
            $failures.Add("agent-field-write: $($file.FullName):$($h.LineNumber) writes FRoadAgent's $field by hand outside RoadAgent.cpp; add or use its mutator: $($h.Line.Trim())")
        }
        $addressHits = @(Select-String -Path $file.FullName -Pattern $agentPlanAddressPattern | Where-Object { -not $_.Line.Trim().StartsWith('//') })
        $cap = if ($agentPlanAddressAllowed.ContainsKey($file.Name)) { $agentPlanAddressAllowed[$file.Name] } else { 0 }
        if ($addressHits.Count -gt $cap) {
            foreach ($h in $addressHits) {
                $failures.Add("agent-plan-address: $($file.FullName):$($h.LineNumber) takes the address of an agent's live plan ($($addressHits.Count) in this file, $cap allowed) - change a route through UGroundTraffic::ChangeRoute / FRoadAgent::ApplyRouteChange (#429): $($h.Line.Trim())")
            }
        }
    }
}
$ranRules.Add('agent field writes')

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
$ranRules.Add('tool colour')

# --- 8. No GetWorld/UWorld/GEngine in Model/ or Solve/ ------------------------------------
# Issue #191. Model/ and Solve/ are the "plain UObjects, testable with NewObject and no
# world" / "dependency-free geometry" layers CLAUDE.md's Architecture section describes - a
# GetWorld() call, a UWorld reference or a GEngine reach-through is exactly the dependency
# that promise rules out, since none of the three exist in a world-free test. Applied to the
# production tree only, like rule 7 - a comment that explains why a function does NOT call
# GetWorld (there is at least one, guarding against a regression) is not the thing this rule
# exists to catch, the same exemption every token-ban rule above gives comments. Doc blocks
# here are `/** ... * ... */`, like rule 1c above, so the exemption checks for a leading `*`
# or `/*` too, not only `//` - rules 5/6/7 never needed that because none of their banned
# tokens happen to come up inside this codebase's block comments the way UWorld does here.
$worldPattern = '\b(GetWorld|UWorld|GEngine)\b'
foreach ($module in $modules) {
    foreach ($half in 'Public', 'Private') {
        foreach ($layer in 'Model', 'Solve') {
            $dir = Join-Path $module (Join-Path $half $layer)
            foreach ($file in Get-Sources $dir @('.h', '.cpp')) {
                $hits = Select-String -Path $file.FullName -Pattern $worldPattern
                foreach ($h in $hits) {
                    if ($h.Line.Trim() -match '^(//|/\*|\*)') { continue }
                    $failures.Add("model-solve-world: $($file.FullName):$($h.LineNumber) $layer/ must stay world-free: $($h.Line.Trim())")
                }
            }
        }
    }
}
$ranRules.Add('model/solve world-free')

# --- 9. Every Test* assertion in a test module names a reason ----------------------------
# Issue #191's review comment. FAutomationTestBase's TestTrue/TestEqual/TestNull/... all take
# a description FIRST - the text a failure actually prints - and the shapes this codebase
# uses for it are: a TEXT(...) literal, an FString (often FString::Printf, sometimes
# dereferenced through a pointer with a leading *), a bare (possibly dotted) variable someone
# built the message into earlier (Each.Why, a loop's own Reason), or a dereferenced computed
# expression such as *(Label + TEXT(" solves")). What is NOT one of those - a bare comparison,
# a literal number, anything that reads as the CONDITION rather than a description of it - is
# exactly the shape issue #191's review found twice, both a variable holding the reason with
# no #include-visible trace of that, which the negative lookahead's \w+(\.\w+)*\s*[,)] admits.
# The whitespace-then-token check sits INSIDE the lookahead, not before it, because a `\s*`
# outside one still backtracks past real content to a bare whitespace character, satisfying
# the negative lookahead against nothing.
$reasonModules = @($airsideTests, $opsTests)
$reasonPattern = '(?<![A-Za-z0-9_])Test\w+\((?!\s*(TEXT|\*?FString|\*?\(|\w+(\.\w+)*\s*[,)]))'
foreach ($module in $reasonModules) {
    foreach ($file in Get-Sources $module @('.h', '.cpp')) {
        $text = Get-Content -Raw -Path $file.FullName
        foreach ($m in [regex]::Matches($text, $reasonPattern)) {
            $line = ($text.Substring(0, $m.Index) -split "`n").Count
            $failures.Add("assertion-without-reason: $($file.FullName):$line calls an assertion with no recognisable reason argument: $($m.Value.Trim())")
        }
    }
}
$ranRules.Add('assertion reasons')

# --- 10. Any FOutputDevice subclass in a test module overrides CanBeUsedOnMultipleThreads --
# Issue #216: an FOutputDevice that does not override this defaults to false, which UE 5.8's
# dedicated log-redirector thread (LaunchEngineLoop.cpp's TryStartDedicatedPrimaryThread,
# active unless "-NoLogThread" - Run-AirsideTests.ps1 does not pass it) then files as BUFFERED
# instead of delivered synchronously - a spy scoped to one AddOutputDevice/RemoveOutputDevice
# bracket can see its lines late or never under full-suite log volume, independent of when
# that bracket closes. RoadRebuildLogQuietTest's FLogRoadMeshLogSpy shipped without the
# override and flaked across five sightings before the mechanism was found (#216);
# GroundTrafficTest's near-identical FLogAirsideTrafficSpy then flaked the same way with the
# same missing line. Both now derive from AirsideTestWorld.h's shared FLogLineSpy, which
# carries the override once - this rule is what stops a THIRD hand-written spy of this shape
# from omitting it again. Scoped to AirsideTests and AirportOpsTests in full (dedicated test
# modules, the way rules 4/5/6 read them) plus only the *Test.cpp files of AirportMgr and
# AirsideEditor (mixed production/test modules - the rule has no business in, say,
# RoadNetworkActor.cpp). Comment lines excluded, the same exemption rules 5/6/7 give a WHY
# comment that names the banned shape rather than being it.
$airportMgr = Join-Path $Root 'Source\AirportMgr'
$outputDeviceFiles = New-Object System.Collections.Generic.List[System.IO.FileInfo]
$outputDeviceFiles.AddRange([System.IO.FileInfo[]](Get-Sources $airsideTests @('.h', '.cpp')))
$outputDeviceFiles.AddRange([System.IO.FileInfo[]](Get-Sources $opsTests @('.h', '.cpp')))
$outputDeviceFiles.AddRange([System.IO.FileInfo[]]((Get-Sources $airportMgr @('.h', '.cpp')) | Where-Object { $_.Name -like '*Test.cpp' }))
$outputDeviceFiles.AddRange([System.IO.FileInfo[]]((Get-Sources $editor @('.h', '.cpp')) | Where-Object { $_.Name -like '*Test.cpp' }))
foreach ($file in $outputDeviceFiles) {
    # Word boundary after FOutputDevice so FOutputDeviceRedirector/FOutputDeviceConsole/etc
    # (real engine classes with the same prefix, no relation to this rule) are not mistaken
    # for a direct subclass of FOutputDevice itself.
    $hits = Select-String -Path $file.FullName -Pattern ':\s*(public\s+)?FOutputDevice\b'
    if ($hits.Count -eq 0) { continue }
    $text = Get-Content -Raw -Path $file.FullName
    $hasOverride = $text -match 'CanBeUsedOnMultipleThreads'
    foreach ($h in $hits) {
        if ($h.Line.Trim() -match '^(//|/\*|\*)') { continue }
        if (-not $hasOverride) {
            $failures.Add("output-device-spy: $($file.FullName):$($h.LineNumber) declares an FOutputDevice subclass with no CanBeUsedOnMultipleThreads override in this file (issue #216's buffered-device race - derive from AirsideTestWorld.h's FLogLineSpy instead): $($h.Line.Trim())")
        }
    }
}
$ranRules.Add('output-device spies')

# --- 11. Declared but not consumed -------------------------------------------------------
# Issue #255. All three sub-rules search this ONE list - built once, not per-symbol, because
# 11c alone checks ~15 verbs and re-walking the tree per verb is exactly the cost rule 4's
# original per-symbol loop already accepted at a much smaller N.
$allTreeFiles = @()
foreach ($tree in $trees) { $allTreeFiles += Get-Sources $tree @('.h', '.cpp') }

# 11a. A DECLARE_*DELEGATE* member with zero binders anywhere, or zero broadcasters anywhere.
# FILE-SCOPED PAIRING, not next-line: RoadEditFacade.h's FOnNetworkChanged OnChanged sits
# directly under its DECLARE_MULTICAST_DELEGATE_OneParam line, but OpsEvents.h's dynamic
# delegates sit a UPROPERTY(BlueprintAssignable) away - so this matches "<Type> <Name>;"
# ANYWHERE in the same header the macro is in, not only the line after it. Declaring file
# must be production (test fixtures script their own delegates on purpose, the way rules 4-6
# already exempt them); the binder/broadcaster SEARCH covers the whole tree, since a real one
# may legitimately be a test spy.
foreach ($tree in $trees) {
    foreach ($file in Get-Sources $tree @('.h')) {
        if ($file.FullName -match '\\(AirsideTests|AirportOpsTests)\\') { continue }
        $text = Get-Content -Raw -Path $file.FullName
        $delegateTypes = [regex]::Matches($text, 'DECLARE_(MULTICAST_)?DELEGATE\w*\(\s*(F\w+)') |
            ForEach-Object { $_.Groups[2].Value } | Select-Object -Unique
        foreach ($type in $delegateTypes) {
            foreach ($mm in [regex]::Matches($text, '(?m)^.*\b' + [regex]::Escape($type) + '\s+(\w+)\s*;')) {
                if ($mm.Value -match 'DECLARE_') { continue }
                $member = $mm.Groups[1].Value
                $line = ($text.Substring(0, $mm.Index) -split "`n").Count
                $binderPattern = '\b' + [regex]::Escape($member) + '\.(Add\w*|Bind\w*)\('
                $broadcastPattern = '\b' + [regex]::Escape($member) + '\.(Broadcast|Execute)\('
                if (-not (Select-String -Path $allTreeFiles.FullName -Pattern $binderPattern -Quiet)) {
                    $failures.Add("unconsumed-delegate: $($file.FullName):$line $type $member has zero .Add*/.Bind* binders anywhere - delete it or bind it (issue #169's OnChanged shape)")
                }
                if (-not (Select-String -Path $allTreeFiles.FullName -Pattern $broadcastPattern -Quiet)) {
                    $failures.Add("unconsumed-delegate: $($file.FullName):$line $type $member has zero .Broadcast/.Execute callers anywhere - delete it or fire it (issue #169's OnChanged shape)")
                }
            }
        }
    }
}

# 11b. A UI_COMMAND with no MapAction anywhere. MapAction's first argument is the command
# field, so "within 80 characters of MapAction(" catches the codebase's own style
# (Toolkit->GetToolkitCommands()->MapAction(Commands.CancelGesture, ...)) without needing to
# parse the call across its (often multi-line) remaining arguments.
foreach ($tree in $trees) {
    foreach ($file in Get-Sources $tree @('.cpp')) {
        $text = Get-Content -Raw -Path $file.FullName
        foreach ($m in [regex]::Matches($text, 'UI_COMMAND\(\s*(\w+)\s*,')) {
            $name = $m.Groups[1].Value
            $line = ($text.Substring(0, $m.Index) -split "`n").Count
            $mapPattern = 'MapAction\([^,]{0,80}\b' + [regex]::Escape($name) + '\b'
            if (-not (Select-String -Path $allTreeFiles.FullName -Pattern $mapPattern -Quiet)) {
                $failures.Add("unconsumed-uicommand: $($file.FullName):$line UI_COMMAND($name, ...) has no MapAction binding it anywhere (issue #184's shape: the LAST MapAction of a command wins, so an unreached one is dead)")
            }
        }
    }
}

# 11c. An IBuildTool hook (RoadBuildTool.h) with no caller anywhere outside its own
# declaration - issue #185's shape, generalised past the two verbs (OnCommit, BuildReadout)
# that issue happened to name. A caller line is excluded when it IS the declaration
# ("virtual") or an out-of-line override definition ("ClassName::Method") - neither is a
# driver reaching the hook, both are the interface restating its own name.
$toolInterfaceFile = Join-Path $plugin 'Public\Tool\RoadBuildTool.h'
if (-not (Test-Path $toolInterfaceFile)) {
    # Same no-op-that-reads-as-pass shape as a dropped struct below, one level up: a renamed
    # or deleted file, not just a renamed struct inside it, must not let this sub-rule vanish
    # quietly - rules 15/16 fail this way already; 11c did not.
    $failures.Add("unconsumed-tool-verb: $toolInterfaceFile is named by rule 11c but does not exist - update the rule, do not let it check nothing")
}
if (Test-Path $toolInterfaceFile) {
    $text = Get-Content -Raw -Path $toolInterfaceFile
    $structStart = $text.IndexOf('struct AIRSIDE_API IBuildTool')
    # Issue #291: a renamed struct or a dropped AIRSIDE_API used to make this WHOLE sub-rule a
    # silent no-op - $structStart -lt 0 fell through both branches below with nothing added to
    # $failures, unlike rules 15/16 which FAIL when their named file goes missing. The file
    # exists (the Test-Path above passed) but the one struct this rule knows how to read does
    # not, which is exactly the shape a rename or an API-macro drop leaves behind.
    if ($structStart -lt 0) {
        $failures.Add("unconsumed-tool-verb: $toolInterfaceFile could not find 'struct AIRSIDE_API IBuildTool' - renamed, reworded or lost AIRSIDE_API; rule 11c cannot check any hook until this is fixed")
    }
    if ($structStart -ge 0) {
        $braceOpen = $text.IndexOf('{', $structStart)
        $depth = 0
        $i = $braceOpen
        for (; $i -lt $text.Length; $i++) {
            if ($text[$i] -eq '{') { $depth++ }
            elseif ($text[$i] -eq '}') { $depth--; if ($depth -eq 0) { break } }
        }
        $body = $text.Substring($braceOpen, $i - $braceOpen + 1)
        $bodyStartLine = ($text.Substring(0, $braceOpen) -split "`n").Count
        foreach ($m in [regex]::Matches($body, 'virtual\s+[\w:&*<>,\s]+?\s+(\w+)\s*\(')) {
            $method = $m.Groups[1].Value
            if ($method -eq 'IBuildTool') { continue } # the destructor, ~IBuildTool
            $line = $bodyStartLine + (($body.Substring(0, $m.Index) -split "`n").Count - 1)
            $callerPattern = '\b' + [regex]::Escape($method) + '\s*\('
            $hasCaller = $false
            foreach ($h in (Select-String -Path $allTreeFiles.FullName -Pattern $callerPattern)) {
                if ($h.Path -eq $toolInterfaceFile) { continue }
                if ($h.Line -match 'virtual') { continue }
                if ($h.Line -match ('::\s*' + [regex]::Escape($method) + '\s*\(')) { continue }
                $hasCaller = $true
                break
            }
            if (-not $hasCaller) {
                $failures.Add("unconsumed-tool-verb: $($toolInterfaceFile):$line IBuildTool::$method has no caller anywhere outside its own declaration (issue #185's OnCommit/BuildReadout shape)")
            }
        }
    }
}
$ranRules.Add('unconsumed declarations')

# --- 12. Comment-only facts (WARNING, not a failure) --------------------------------------
# Issue #255: the five criticals of the 2026-09-21 review were all comments that had been
# true at ten nodes and were never re-checked - "measured before it is indexed", "hand-
# authored at tens of nodes". A comment MAY explain a decision; it may never be the only
# thing enforcing one. This counts comment lines asserting a fact about OTHER code with no
# `// ENFORCED BY:` marker within 3 lines naming what actually holds it true (the marker
# convention itself lives in CLAUDE.md's "Conventions"). A COUNT, not a failure - so today's
# backlog is visible without breaking the build over comments that predate this rule - quoted
# in the PR that adds this rule; promote the check to a failure once that count reaches zero.
#
# Issue #291 widened the phrase list: the 2026-09-25 review found five more shapes stating the
# same kind of unenforced fact that the original four verbs missed entirely - "checks all three
# every run", "nothing here matches", "no other test", "every test in", "the one place its name
# appears" - each read as a claim about coverage or uniqueness with nothing named to keep it true.
# Issue #295's PR review widened it again: "the only caller" required a leading "the", so
# "ArmDepartureIfRunway's only caller" and "The one production door Id is assigned through ...
# nowhere else" (RoadAgent.h) evaded it by wording alone while asserting the exact same kind of
# fact. `only caller` (no leading "the"), `nowhere else` and `one (production )?door` close that
# - the same generalisation rule 6 got from an enumerated field list to ANY field.
$commentFactPattern = 'the only (caller|file|place|site)|only caller|nowhere else|one (production )?door|never (called|happens|runs)|no (edit|change) (is )?needed|nothing (else )?(reads|calls|binds)|checks all three every run|nothing here matches|no other test|every test in|the one place its name appears'
$commentFactWarnings = New-Object System.Collections.Generic.List[string]
foreach ($tree in $trees) {
    foreach ($file in Get-Sources $tree @('.h', '.cpp')) {
        $lines = Get-Content -Path $file.FullName
        for ($idx = 0; $idx -lt $lines.Count; $idx++) {
            $line = $lines[$idx].Trim()
            if ($line -notmatch '^(//|/\*|\*)') { continue }
            if ($line -notmatch $commentFactPattern) { continue }
            $windowStart = [Math]::Max(0, $idx - 3)
            $windowEnd = [Math]::Min($lines.Count - 1, $idx + 3)
            if (($lines[$windowStart..$windowEnd] -join "`n") -match 'ENFORCED BY:') { continue }
            $commentFactWarnings.Add("comment-only-fact: $($file.FullName):$($idx + 1) $line")
        }
    }
}

# --- 13. GraphProbe is for tests and tools only --------------------------------------------
# The permissive routing policy - no runway filter, no penalty, no congestion - survives only
# where it is NAMED. Four production call sites silently had exactly this policy before
# 2026-09-21 because FRouteQuery's defaults were the permissive ones, and an aeroplane taxied
# down a runway. ERouteErrand::GraphProbe is the one place that behaviour is still reachable,
# and a production caller reaching for it is that bug coming back wearing a name.
#
# Scoped to the two production modules the way rule 1 is: the test modules are its intended
# home, and Tool/ is exempt because a tool asking "is there any way from here to there" is a
# shape question with no agent behind it. Testing/ (issue #312's TestGraph::Probe) is exempt
# for the same reason as Tool/, not because it sits under Airside/'s production module path:
# WITH_DEV_AUTOMATION_TESTS-gated, AIRSIDE_API only so the OTHER test modules can reach it (see
# AirsideTestGraph.h's own top comment), and its whole job is naming GraphProbe once so the 48
# test call sites #312 fixed do not have to.
#
# QUALIFIED USES ONLY (ERouteErrand::GraphProbe), so the enumerator's own declaration and the
# paragraphs explaining it in RoutePolicy.h are not mistaken for callers of it.
foreach ($module in $modules) {
    foreach ($half in 'Public', 'Private') {
        $dir = Join-Path $module $half
        foreach ($file in Get-Sources $dir @('.h', '.cpp')) {
            if ($file.FullName -like '*\Tool\*') { continue }
            if ($file.FullName -like '*\Testing\*') { continue }
            # RoutePolicy.h/.cpp DECLARE the errand and write its row; naming it there is
            # the definition, not a use of it. Exempting the defining file by name rather
            # than exempting 'case' labels everywhere, so a production switch that tried to
            # special-case GraphProbe is still caught.
            if ($file.Name -eq 'RoutePolicy.h' -or $file.Name -eq 'RoutePolicy.cpp') { continue }
            $hits = Select-String -Path $file.FullName -Pattern 'ERouteErrand::GraphProbe'
            foreach ($h in $hits) {
                # A WHY comment naming the banned token is not the thing itself - the same
                # exemption rules 5 and 7 make.
                $t = $h.Line.Trim()
                if ($t.StartsWith('//') -or $t.StartsWith('*')) { continue }
                $failures.Add("graph-probe: $($file.FullName):$($h.LineNumber) ERouteErrand::GraphProbe is for tests and Tool/ only - production code names the errand it means: $t")
            }
        }
    }
}
$ranRules.Add('graph-probe')

# --- 14. A run's width comes from FKitSpec::RunWidthUu --------------------------------------
# Five sites multiplied a module's width by a run length by hand until 2026-09-22 - the
# sampler, the bands strategy, the tool's outline twice and the presenter - and the shed's end
# caps then had to be added to all five or a building would stand wider than the ground it
# reserved. RunWidthUu in PlotYard.h is the one product; this fails on the hand-written shape
# coming back. Test modules are exempt: they assert AGAINST the product, which is the point.
$runWidthPatterns = @(
    'WidthUu\s*\*=?\s*[\w.]*(RunLength|Length|Lit|Dark|Count)\b',
    '\b[\w.]*(RunLength|Length|Lit|Dark)\s*\*\s*[\w.]*WidthUu\b'
)
foreach ($module in $modules) {
    foreach ($half in 'Public', 'Private') {
        $dir = Join-Path $module $half
        foreach ($file in Get-Sources $dir @('.h', '.cpp')) {
            if ($file.Name -eq 'PlotYard.h') { continue }
            foreach ($pattern in $runWidthPatterns) {
                $hits = Select-String -Path $file.FullName -Pattern $pattern
                foreach ($h in $hits) {
                    $t = $h.Line.Trim()
                    if ($t.StartsWith('//') -or $t.StartsWith('*')) { continue }
                    $failures.Add("run-width: $($file.FullName):$($h.LineNumber) multiplies a width by a run length by hand; use FKitSpec::RunWidthUu, which adds the end caps: $t")
                }
            }
        }
    }
}
$ranRules.Add('run-width')

# --- 15. Ground-only consumers take FChassis, never FAirframe ------------------------------
# FChassis was split out of FAirframe on 2026-09-23 (Model/Chassis.h says why): a service
# vehicle was carried as an aeroplane with its climb zeroed, and every ground consumer took
# the whole bundle to read five fields of it. These files only ROLL things or size concrete
# for them; naming FAirframe in code here is that coupling coming back. Comment lines are
# exempt, the way rules 5, 7 and 13 exempt a WHY comment that names the banned token.
#
# A LISTED FILE THAT IS MISSING FAILS. A rename would otherwise turn this rule into a check
# of nothing, which reports exactly like a pass.
$chassisOnly = @(
    'Public\Model\Chassis.h', 'Private\Model\Chassis.cpp', 'Public\Model\Vehicle.h',
    'Public\Model\RouteFollower.h', 'Private\Model\RouteFollower.cpp',
    'Public\Model\ReverseRun.h', 'Private\Model\ReverseRun.cpp',
    'Public\Model\SpeedProfile.h', 'Private\Model\SpeedProfile.cpp',
    'Public\Build\AnchorLink.h', 'Private\Build\AnchorLink.cpp',
    'Public\Build\RoadGuidelineBuilder.h', 'Private\Build\RoadGuidelineBuilder.cpp',
    'Public\Build\RoadNetworkSolver.h', 'Private\Build\RoadNetworkSolver.cpp',
    'Public\Profiles\RoadProfile.h', 'Private\Profiles\RoadProfile.cpp'
)
foreach ($rel in $chassisOnly) {
    $path = Join-Path $plugin $rel
    if (-not (Test-Path $path)) {
        $failures.Add("chassis-only: $path is listed in rule 15 but does not exist - update the list, do not let the rule check nothing")
        continue
    }
    foreach ($h in (Select-String -Path $path -Pattern '\bFAirframe\b')) {
        $t = $h.Line.Trim()
        if ($t.StartsWith('//') -or $t.StartsWith('*') -or $t.StartsWith('/*')) { continue }
        $failures.Add("chassis-only: $($path):$($h.LineNumber) names FAirframe; a ground-only consumer takes FChassis (Model/Chassis.h): $t")
    }
}
$ranRules.Add('chassis-only')

# --- 16. Turn paths never pair guidelines by index across arms -------------------------------
# Until 2026-09-23 FRoadGuidelineBuilder paired guideline N of one arm with guideline N of the
# next over the smaller count. Offsets are per segment A->B and arms meet at mixed ends, so on
# two-lane roads that paired an ARRIVING lane with an ARRIVING lane, and a one-lane arm meeting
# a two-lane arm lost a lane. The shape it had: a Min over two profiles' Guidelines.Num().
$turnBuilder = Join-Path $plugin 'Private\Build\RoadGuidelineBuilder.cpp'
if (-not (Test-Path $turnBuilder)) {
    $failures.Add("turn-index-pairing: $turnBuilder is named by rule 16 but does not exist - update the rule, do not let it check nothing")
} else {
    $turnText = Get-Content -Raw $turnBuilder
    foreach ($m in [regex]::Matches($turnText, 'Min\(\s*\w+->Guidelines\.Num\(\)')) {
        $line = ($turnText.Substring(0, $m.Index) -split "`n").Count
        $failures.Add("turn-index-pairing: $($turnBuilder):$line pairs turn-path guidelines by index; pair arriving lanes with leaving lanes instead")
    }
}
$ranRules.Add('turn-index-pairing')

# --- 17. IsPlotted() is not "is a depot" ----------------------------------------------------
# A stand can be plotted too (2026-09-23's "drawn stands" work), so the outline-means-depot
# shape RebuildFrom shipped (Outline.Num() >= 3, no kind check) is exactly the bug
# Airside.Present.PlotPresenter.StandOutlineIsNotADepot pins. RoadEntity.h is IsPlotted()'s own
# definition; RoadSurfacePresenter.cpp, RoadEditFacadeSurfaces.cpp and SelectTool.cpp are
# allow-listed WHOLE because each use there is correctly kind-neutral (pad paving, the ground
# pick, and the selection highlight all want the same behaviour for a plotted stand as for a
# plotted depot) - see the rule's own comment above for why each one is safe. Every other file
# in the tree may still call IsPlotted(), but ONLY on a line that also names IsDepot() or
# IsStand() - stating the kind explicitly, not leaning on IsPlotted() to mean one.
$isPlottedAllowFiles = @('RoadEntity.h', 'RoadSurfacePresenter.cpp', 'RoadEditFacadeSurfaces.cpp', 'SelectTool.cpp', 'StagedPlotTool.cpp')
foreach ($tree in $trees) {
    foreach ($file in Get-Sources $tree @('.h', '.cpp')) {
        if ($isPlottedAllowFiles -contains $file.Name) { continue }
        $hits = Select-String -Path $file.FullName -Pattern 'IsPlotted\s*\(\s*\)'
        foreach ($h in $hits) {
            $t = $h.Line.Trim()
            if ($t.StartsWith('//') -or $t.StartsWith('*') -or $t.StartsWith('/*')) { continue }
            if ($t -match 'IsDepot\s*\(\s*\)' -or $t -match 'IsStand\s*\(\s*\)') { continue }
            $failures.Add("is-plotted-not-depot: $($file.FullName):$($h.LineNumber) IsPlotted() is not 'is a depot' - a drawn stand is plotted too; ask IsDepot(): $t")
        }
    }
}
$ranRules.Add('is-plotted-not-depot')

# --- 18. No Acos in production: the angle between two directions is RoadGeom::AngleBetween ---
# 2026-09-24, samples/t-junctions.png. acos(dot) has a square-root singularity at +/-1: a dot one
# ulp off -1 - which a snap guide's round trip leaves for 40 of 89 bearings - reads 1.5e-8 rad off
# pi, and RoadPlacement's `sin < 1e-9` straight-through test then refused a T the solver (1e-6)
# would have drawn. The same shape sat at six sites; GuideArbiter's 1e-9-degree TieEpsilon was
# swamped by it too. AngleBetween is atan2(|cross|, dot), accurate everywhere. RoadGeom.cpp is
# not exempt because AngleBetween does not need acos either. Test modules are exempt (a test
# may measure what acos says, as Airside.Tool.TJunctionFitsBothWays does on purpose).
foreach ($module in $modules) {
    foreach ($file in Get-Sources $module @('.h', '.cpp')) {
        foreach ($h in (Select-String -Path $file.FullName -Pattern '\bAcos\s*\(')) {
            $t = $h.Line.Trim()
            if ($t -match '^(//|/\*|\*)') { continue }
            $failures.Add("no-acos: $($file.FullName):$($h.LineNumber) measures an angle with acos, which is 1.5e-8 rad wrong one ulp from +/-1; use RoadGeom::AngleBetween (fold with Min(T, pi - T) for a line): $t")
        }
    }
}
$ranRules.Add('no-acos')

# --- 19. RoadEntity.h must not include Airframe.h or AgentMotion.h -------------------------
# Issue #300, a regression of #176: the split (#176) moved the airframe physics bundle and
# FAgentMotion out of RoadEntity.h into their own headers, but left both #included from
# RoadEntity.h anyway "so every existing includer keeps compiling unchanged" - which quietly
# put every one of RoadEntity.h's ~40+ includers back on the airframe/motion rebuild list the
# split existed to remove them from. RoadEntity.h names neither FAirframe nor FAgentMotion, so
# nothing in it needs either header; a file that DOES name one includes it directly (or
# forward-declares it where a pointer/reference suffices). This is a single-file check, not
# the general include-direction table in rule 1, because the two headers are siblings within
# Model/ and no directory boundary would catch this shape.
$roadEntityHeader = Join-Path $Root 'Plugins\Airside\Source\Airside\Public\Model\RoadEntity.h'
if (Test-Path $roadEntityHeader) {
    foreach ($h in (Select-String -Path $roadEntityHeader -Pattern '#include\s+"Model/(Airframe|AgentMotion)\.h"')) {
        $failures.Add("roadentity-no-airframe-motion: $($roadEntityHeader):$($h.LineNumber) RoadEntity.h must not include Airframe.h or AgentMotion.h (#300): $($h.Line.Trim())")
    }
}
$ranRules.Add('roadentity-no-airframe-motion')

# --- 19. Runway/taxiway profile triples are TestProfiles', nowhere else in AirsideTests or --
#         AirportOpsTests
# Issue #310: TestProfiles::Runway/NarrowRunway/Taxiway existed since #102 (f0714c80), and 28
# non-fixture sites across 12 files kept retyping the same MakeTransient triple anyway - some
# of them (EditToolTest, MeshFreshnessTest, RunwayDisconnectTest) newer than the helper itself.
# AirsideTestFixtures.cpp is the one legal definer. #310 scoped this to AirsideTests only:
# TestProfiles then lived in AirsideTests/Private/AirsideTestFixtures.h, Private to that module,
# so AirportOpsTests could not call it and its OWN copies of these triples (FlightBoardTest,
# FuelServiceTest, OfferGeneratorTest, OpsRuntimeTest, OpsSaveTest) were reported but not fixed.
# #311 moved TestProfiles to Airside/Public/Testing/AirsideTestGraph.h and migrated those five
# files onto it, so the rule widens to AirportOpsTests too - the same "one legal definer"
# reasoning, now with two modules that can each retype the triple instead of one.
#
# ALL THREE ARGUMENTS, exactly paired (4500/1500/450, 1800/1500/180, 2300/1500/230): the 2-arg
# MakeTransient(Width, LaneWidth) overload this codebase also uses (LeadInSweepTest,
# RoadNetworkTest, RoadProfileTest, ...) takes the engine's own default ExitLength, which is
# NOT what TestProfiles::Runway/NarrowRunway/Taxiway produce - matching on the first two
# arguments alone flagged a dozen of those as false positives before this comment.
$profileTriplePattern = 'MakeTransient\(\s*(4500\.0,\s*1500\.0,\s*450\.0|1800\.0,\s*1500\.0,\s*180\.0|2300\.0,\s*1500\.0,\s*230\.0)\s*\)'
foreach ($file in (Get-Sources $airsideTests @('.h', '.cpp')) + (Get-Sources $opsTests @('.h', '.cpp'))) {
    if ($file.Name -eq 'AirsideTestFixtures.cpp' -or $file.Name -eq 'AirsideTestGraph.cpp') { continue }
    foreach ($h in (Select-String -Path $file.FullName -Pattern $profileTriplePattern)) {
        $t = $h.Line.Trim()
        if ($t.StartsWith('//') -or $t.StartsWith('*') -or $t.StartsWith('/*')) { continue }
        $failures.Add("profile-triple: $($file.FullName):$($h.LineNumber) retypes a TestProfiles triple; call TestProfiles::Runway/NarrowRunway/Taxiway instead: $t")
    }
}
$ranRules.Add('profile-triple')

# --- 20. No new *ForTest( on ARoadNetworkActor - RoadNetworkActor.h has an EXPLICIT allow-list -
# Issue #298, a regression of #80 (closed on 12 test-only members deleted/retargeted): the actor
# grew SIX MORE pure forwarders back (LastAgentPhaseForTest, LastAgentTaxiSpeedCapForTest,
# FacadeOuterForTest, PresenterOuterForTest, ResolveStandDefinitionForTest, ResolveProfileForTest)
# between #80 closing and this issue's review finding them. #80 had no lint pinning the shrink it
# made, so nothing caught the regrowth until a human review did, member by member, again. This
# rule is that pin: the allow-list below is the full CLOSED set of *ForTest members #298 left on
# this header (RebuildCountForTest and friends, each already justified at its own declaration -
# GetTraffic()/GetPresenter()/GetEditFacade() exist precisely so a NEW one is never needed). A
# real forwarder-shaped member here fails; growing the list back to fix the failure is the same
# regression #298 fixed, done again in the lint instead of the header - the allow-list is meant
# to SHRINK, not to grow to match whatever the header happens to declare today.
#
# DECLARATIONS ONLY, not calls: LayerComponentForTest's own body calls
# GetPresenter()->GetLayerComponentForTest(Layer) - a different class's member, reached through
# `->` - which this must not flag as a new member on THIS actor. The negative lookbehind excludes
# any match preceded by `.` or `>` (i.e. reached through a member-access operator); a genuine
# declaration's name is never preceded by either.
$roadNetworkActorHeader = Join-Path $plugin 'Public\Present\RoadNetworkActor.h'
$forTestAllowList = @(
    'SetDeltaSmoothingForTest',
    'MakeSurfaceSettingsForTest',
    'RebuildCountForTest',
    'TopologyRebuildCountForTest',
    'LayerComponentForTest'
)
if (-not (Test-Path $roadNetworkActorHeader)) {
    # Same no-op-that-reads-as-pass trap rules 11c/15/16 guard against: a rename must not let
    # this rule silently check nothing.
    $failures.Add("fortest-allowlist: $roadNetworkActorHeader is named by rule 19 but does not exist - update the rule, do not let it check nothing")
}
if (Test-Path $roadNetworkActorHeader) {
    $text = Get-Content -Raw -Path $roadNetworkActorHeader
    $lines = $text -split "`n"
    foreach ($m in [regex]::Matches($text, '(?<![.>])\b(\w*ForTest)\s*\(')) {
        $name = $m.Groups[1].Value
        $line = ($text.Substring(0, $m.Index) -split "`n").Count
        if ($lines[$line - 1].Trim() -match '^(//|/\*|\*)') { continue }
        if ($forTestAllowList -contains $name) { continue }
        $failures.Add("fortest-allowlist: $($roadNetworkActorHeader):$line $name( is a new *ForTest member on ARoadNetworkActor, not in the issue #298 allow-list - a test wants GetTraffic()/GetPresenter()/GetEditFacade()->... or an already-public Resolve*, not a new forwarder (see #80 and #298's own history above)")
    }
}
$ranRules.Add('no-new-fortest-forwarders')

# --- 21. AirsideVehicleCodes:: never compared with ==/!= ------------------------------------
# Issue #308 (review of PR #335): AirsideVehicleCodes::Rig/UtilityTow used to be a LOOK-UP KEY -
# UAirsideSettings::ResolveVehicleViewFor branched "if (Vehicle.TypeCode ==
# FName(AirsideVehicleCodes::Rig)) return ResolveRigView(); ..." - the TypeCode ladder #308
# deleted. The constants stay (FVehicle::TypeCode still names what the inspector and the
# dispatch log say a vehicle is), but the banner comment above their declaration - in
# AirsideSettings.cpp until #430 moved them to Model/VehicleCodes.h - claims "NO LONGER A LOOK-UP
# LADDER" with nothing mechanical behind it. #430 made the code the catalogue's JOIN KEY
# (UAirsideSettings::ResolveVehicle), through a table of resolvers that each name their own
# vehicle - a lookup by the vehicle's own TypeCode, never a comparison with a constant.
# Comparing AirsideVehicleCodes:: against anything with ==/!= IS the ladder shape coming back,
# so it is banned outright rather than merely discouraged - a vehicle's look must come from its
# own FVehicle::Mesh/Tow[].Mesh (see ResolveVehicleViewFor's own comment), never from a branch
# on which constant its TypeCode equals. Two patterns: the constant on the LEFT of ==/!=, and on
# the RIGHT (allowing for FName(...) wrapping either side, as the deleted ladder used).
$vehicleCodeComparePatterns = @(
    'AirsideVehicleCodes::\w+\s*\)?\s*(==|!=)',
    '(==|!=)\s*(FName\s*\(\s*)?AirsideVehicleCodes::\w+'
)
foreach ($module in $modules) {
    foreach ($file in Get-Sources $module @('.h', '.cpp')) {
        foreach ($pattern in $vehicleCodeComparePatterns) {
            foreach ($h in (Select-String -Path $file.FullName -Pattern $pattern)) {
                $t = $h.Line.Trim()
                if ($t -match '^(//|/\*|\*)') { continue }
                $failures.Add("no-vehiclecode-compare: $($file.FullName):$($h.LineNumber) AirsideVehicleCodes:: compared with ==/!= - the TypeCode ladder #308 deleted; give the vehicle its own FVehicle::Mesh/Tow[].Mesh instead: $t")
            }
        }
    }
}
$ranRules.Add('no-vehiclecode-compare')

# --- 22. FRouteQuery constructed only in RouteSearch.cpp (issue #312) ----------------------
# THE ONE WRITER of AvoidRunways off a resolved FRoutePolicy is FRouteQuery::For - see
# RouteSearch.cpp's own comment on Query.AvoidRunways = Query.Policy.Avoidance. A caller that
# builds `FRouteQuery Q;` by hand and fills Errand/Policy/Start/Goal/Class itself skips that
# one line and gets the permissive ERunwayAvoidance::None for every errand, silently - the
# six-helper, 48-site shape #312 found across the test suite (TestGraph::Probe, added by the
# same PR, is the one legal wrapper for a test's plain GraphProbe query; FRouteQuery::For
# directly for anything Probe's signature does not cover - a banned edge, a vehicle gate, a
# tow seed, congestion).
#
# BARE DECLARATION ONLY (`FRouteQuery \w+;`, no initialiser) - `FRouteQuery Q = FRouteQuery::
# For(...)` or `FRouteQuery Q = SomeOtherQuery;` already goes through the one writer or copies
# a query that did, so neither trips this rule. Scoped to $trees, unlike rule 5's handle rule -
# test modules are NOT exempt here, on the issue's own instruction: a test's query is fed to
# the same RunSearch a production one is, so the same one-writer contract applies.
#
# THE ALLOW-LIST, one entry per (File, VarName), matching rule 6's "file AND field name" shape
# rather than exempting a whole file: each remaining site was checked by hand (issue #312's PR)
# and either already sets AvoidRunways correctly (FuelService.cpp's two moved to FRouteQuery::For
# when it became JobBoard*.cpp, 2026-09-28, and ArrivalPlanner.cpp's to ArrivalPlanner::TaxiInQuery
# in #429's review - For() takes an unset goal as readily as a set one -
# RoadEditFacadeSurfaces.cpp, RouteSearchTest.cpp - the last two because FindToGoals takes a
# GOALS ARRAY, not the one Goal either For() or Probe() needs) or deliberately builds an
# incomplete or hand-swept query to test the refusal/sweep itself (RoutePolicyTest.cpp's
# no-errand cases; RouteStepDistanceTest.cpp's FRouteRunwayAvoidanceTest, which sets
# AvoidRunways BY HAND across several Finds precisely to test ERunwayAvoidance one layer under
# the policy that normally chooses it - see that test's own comment). Any OTHER bare
# `FRouteQuery Var;` in one of these files, or a second one of an already-listed (File, Var),
# still fails - the allow-list is not a whole-file exemption.
$routeQueryAllowList = @(
    @{ File = 'Plugins\Airside\Source\Airside\Private\Present\RoadEditFacadeSurfaces.cpp'; Var = 'Query'; Count = 1 }
    @{ File = 'Plugins\Airside\Source\AirsideTests\Private\RoutePolicyTest.cpp'; Var = 'Q'; Count = 2 }
    @{ File = 'Plugins\Airside\Source\AirsideTests\Private\RouteSearchTest.cpp'; Var = 'Query'; Count = 1 }
    @{ File = 'Plugins\Airside\Source\AirsideTests\Private\RouteStepDistanceTest.cpp'; Var = 'Q'; Count = 1 }
)
$routeQueryAllowUsed = @{}
$routeQueryPattern = 'FRouteQuery\s+(?<var>\w+);'
foreach ($tree in $trees) {
    foreach ($file in Get-Sources $tree @('.h', '.cpp')) {
        if ($file.Name -eq 'RouteSearch.cpp') { continue }
        foreach ($h in (Select-String -Path $file.FullName -Pattern $routeQueryPattern)) {
            $t = $h.Line.Trim()
            if ($t -match '^(//|/\*|\*)') { continue }
            $var = [regex]::Match($t, $routeQueryPattern).Groups['var'].Value
            $entry = $routeQueryAllowList | Where-Object {
                (Test-AllowedPathSuffix $file $_.File) -and $_.Var -eq $var
            } | Select-Object -First 1
            $key = "$($file.FullName)|$var"
            if ($entry -ne $null -and ([int]$routeQueryAllowUsed[$key]) -lt $entry.Count) {
                $routeQueryAllowUsed[$key] = [int]$routeQueryAllowUsed[$key] + 1
                continue
            }
            $failures.Add("route-query-one-writer: $($file.FullName):$($h.LineNumber) hand-builds FRouteQuery $var outside RouteSearch.cpp - use TestGraph::Probe (tests) or FRouteQuery::For (anything else), the one writer of AvoidRunways off the resolved policy (issue #312): $t")
        }
    }
}
# THE ALLOW-LIST ITSELF DRIFTS, same failure mode rule 15's file-existence check guards - an
# entry whose site was fixed properly (or renamed) since would sit here allowing nothing,
# unnoticed, until a NEW hand-built query reused that Var name and rode through on the old
# entry's remaining count. Any entry never matched at all is exactly that - walk the same
# usage map the main loop populated (keyed "FullPath|Var"), by suffix, the way
# Test-AllowedPathSuffix matches everywhere else in this script.
foreach ($allowed in $routeQueryAllowList) {
    $matchedAny = $false
    foreach ($key in $routeQueryAllowUsed.Keys) {
        $parts = $key -split '\|', 2
        if ($parts[1] -eq $allowed.Var -and (Test-AllowedPathSuffix ([System.IO.FileInfo]$parts[0]) $allowed.File)) { $matchedAny = $true }
    }
    if (-not $matchedAny) {
        $failures.Add("route-query-one-writer: allow-list entry $($allowed.File) / $($allowed.Var) matched nothing - the site was fixed, renamed or deleted; remove or update this entry so it cannot silently cover a future, unrelated hand-built query")
    }
}
$ranRules.Add('route-query-one-writer')

# --- 23. One pavement scale: no second surface enum ------------------------------------------
# 2026-09-27, shared pavement: ERoadSurface was a second surface enum beside ERunwaySurface,
# mapped onto it by hand, so a road's grass and a runway's grass were two values that happened
# to agree. EPavement in Model/Pavement.h is the one scale; a buildable that offers fewer steps
# says so with a list (URoadProfile::AllowedPavements), not a new enum.
foreach ($module in $modules) {
    foreach ($file in Get-Sources $module @('.h')) {
        foreach ($h in (Select-String -Path $file.FullName -Pattern '\benum\s+class\s+E\w*(Surface|Pavement)\b')) {
            if ($file.FullName -like '*\Public\Model\Pavement.h') { continue }
            # FWantedClaim::ESurface (TrafficClaims) names a traffic-claim KIND (runway edge /
            # holding position), not a ground surface - the pattern's \bESurface\b still matches
            # its bare name, so it is excluded by name rather than by path.
            if ($h.Line -match '\bESurface\b') { continue }
            $failures.Add("one-pavement-scale: $($file.FullName):$($h.LineNumber) declares a second surface scale; use EPavement and an allowed list: $($h.Line.Trim())")
        }
    }
}
$ranRules.Add('one-pavement-scale')

# --- 24. AirsideTestWorld.h names ARoadNetworkActor/AAirsideBuildingsActor only as forward
# declarations, never a direct #include of either's header (2026-09-27, the header-fanout PR).
# FAirsideTestWorld's constructor used to be INLINE and called SpawnActor<ARoadNetworkActor>()
# / SpawnActor<AAirsideBuildingsActor>() - templates that need T complete at the point of
# instantiation, which pulled Present/RoadNetworkActor.h (a header that itself drags in
# RoadSurfacePresenter, BuildSession, RoadEditTarget and the rest - see that header's own long
# comment) into every one of the 120+ TUs that reach this header through
# AirsideTestFixtures.h, whether or not the test ever touched the actor. Measured before/after
# with Tools/HeaderFanout.py: RoadNetworkActor.h's transitive fan-out fell from 174 to 104
# .cpp files, AirsideBuildingsActor.h's from (not separately measured before, since it rode
# in alongside RoadNetworkActor.h) to 11, once the constructor and destructor moved out of
# line to AirsideTestWorld.cpp, the one TU that still needs both complete types. A future
# convenience include of either header back into AirsideTestWorld.h would silently re-widen
# every one of those TUs' rebuild set the same way - this rule is what fails the build instead
# of that regression riding along unnoticed the way the Piper fallback and the hand-built
# handle each did before their own rules existed (see rules 4 and 5's history).
$airsideTestWorldHeader = Join-Path $plugin 'Public\Testing\AirsideTestWorld.h'
if (-not (Test-Path $airsideTestWorldHeader)) {
    # Same no-op-that-reads-as-pass trap rules 11c/15/19/20 guard against: a rename or move
    # must not let this rule silently check nothing.
    $failures.Add("testworld-fanout: $airsideTestWorldHeader is named by rule 24 but does not exist - update the rule, do not let it check nothing")
}
else {
    $hits = Select-String -Path $airsideTestWorldHeader -Pattern '#include\s+"(Present/RoadNetworkActor\.h|Present/AirsideBuildingsActor\.h)"'
    foreach ($h in $hits) {
        $failures.Add("testworld-fanout: $($airsideTestWorldHeader):$($h.LineNumber) AirsideTestWorld.h must forward-declare ARoadNetworkActor/AAirsideBuildingsActor, not include their headers - the constructor and destructor bodies belong in AirsideTestWorld.cpp (2026-09-27 fan-out cut): $($h.Line.Trim())")
    }
}
$ranRules.Add('testworld-fanout')

# --- 25. FRoadRebuildBatch is a NAMED STACK LOCAL -------------------------------------------
# The batch's whole contract is its scope (Tool/RoadEditTarget.h): N edits, one rebuild at the
# close. Two shapes break it and a regex sees both:
#   - AN UNNAMED TEMPORARY, `FRoadRebuildBatch(Target);` or `FRoadRebuildBatch{Target};` - it
#     opens and closes on the same line, batches nothing, and compiles (the [[nodiscard]] on the
#     constructor is only a warning).
#   - A BATCH HELD PAST ONE CALL - by pointer, reference, smart pointer or template argument
#     (`FRoadRebuildBatch*`, `FRoadRebuildBatch&`, `<FRoadRebuildBatch>`): a batch that outlives
#     the function that opened it spans frames, and every frame in between draws a stale graph.
# WHAT NO REGEX SEES, said here rather than pretended: a bulk edit that never opens a batch at
# all. FRigCourseLayout::Lay lays thirty-odd edits through ONE lambda call site, so counting
# PlaceNode/ConnectNodes sites per file sees one; "a loop of mutators" is a control-flow fact.
# That shape is pinned by AirportMgr.RigCourse.BatchedLayMatchesUnbatched's one-rebuild count
# instead. The declaring header is exempt - it names the type's own constructor and deleted copy.
$batchPatterns = @(
    '(^|[^\w~:])FRoadRebuildBatch\s*[\(\{]',
    'FRoadRebuildBatch\s*[\*&>]'
)
foreach ($tree in $trees) {
    foreach ($file in Get-Sources $tree @('.h', '.cpp')) {
        if (Test-AllowedPathSuffix $file 'Public\Tool\RoadEditTarget.h') { continue }
        foreach ($pattern in $batchPatterns) {
            foreach ($h in (Select-String -Path $file.FullName -Pattern $pattern)) {
                $t = $h.Line.Trim()
                if ($t -match '^(//|/\*|\*)') { continue }
                $failures.Add("rebuild-batch-is-a-local: $($file.FullName):$($h.LineNumber) FRoadRebuildBatch used as a temporary or held past its scope - declare it as a named local, ``FRoadRebuildBatch Batch(Target);``, so it closes where the bulk edit ends: $t")
            }
        }
    }
}
$ranRules.Add('rebuild-batch-is-a-local')

# --- 26. BOTH DRIVERS DRAW THE GRID OVERLAY -------------------------------------------------
# GridOverlay::Describe is context the tool does not own (Tool/GridOverlay.h), so each driver
# calls it beside the tool's own BuildPreview - and a driver that stopped would show a grid in
# one of PIE and the editor mode and not the other, with every test green: the tests reach the
# emitter through FBuildSession, never through ARoadBuildHUD::DrawHUD or the editor tool's
# Render, which need a live viewport. So the call is asserted by file (2026-09-27, world grid).
$gridOverlayCallers = @(
    (Join-Path $Root 'Source\AirportMgr\RoadBuildHUD.cpp'),
    (Join-Path $editor 'Private\RoadBuildEditorTool.cpp')
)
foreach ($caller in $gridOverlayCallers) {
    if (-not (Test-Path $caller)) {
        # Rule 24's trap: a moved file must not let this rule check nothing.
        $failures.Add("grid-overlay-both-drivers: $caller is named by rule 26 but does not exist - update the rule")
        continue
    }
    if (-not (Select-String -Path $caller -Pattern 'GridOverlay::Describe\(' -Quiet)) {
        $failures.Add("grid-overlay-both-drivers: $caller no longer calls GridOverlay::Describe - the grid overlay would show in one driver and not the other")
    }
}
$ranRules.Add('grid-overlay-both-drivers')

# --- 27. BOTH DRIVERS REMEMBER TOOL SURFACES ------------------------------------------------
# FBuildSession::SetToolPreferences is the seam a surface pick is remembered through, and a
# session never handed a store remembers nothing - which is every test's session, deliberately,
# so the player's ini cannot steer the suite. So nothing but this rule sees a driver stop
# wiring it: BeginPlay and URoadBuildEdMode::Enter both need a live world or editor viewport
# the headless suite does not have (2026-09-28, remembered surface).
$preferenceCallers = @(
    (Join-Path $Root 'Source\AirportMgr\RoadBuildController.cpp'),
    (Join-Path $editor 'Private\RoadBuildEdMode.cpp')
)
foreach ($caller in $preferenceCallers) {
    if (-not (Test-Path $caller)) {
        $failures.Add("tool-preferences-both-drivers: $caller is named by rule 27 but does not exist - update the rule")
        continue
    }
    if (-not (Select-String -Path $caller -Pattern 'SetToolPreferences\(MakeShared<FConfigToolPreferences>' -Quiet)) {
        $failures.Add("tool-preferences-both-drivers: $caller no longer hands its session an FConfigToolPreferences - one driver would forget the player's surface pick")
    }
}
$ranRules.Add('tool-preferences-both-drivers')

# --- 28. The planners choose a runway END only through the in-use resolver ---------------------
# 2026-09-28, samples/deadlock.png: ArrivalPlanner landed at the threshold NEAREST the approach
# focus, DeparturePlanner::PlanAny tried BOTH thresholds and kept the shorter taxi, and the two
# rules met nose to nose on a connector. The fix is one rule, FRunwayFacts::InUse, read through
# RunwayQuery::InUseEnd (InUseRunwayAt / InUseRunwayNearest). A planner calling RunwayExtentAt or
# NearestRunwayThreshold itself is choosing an end by proximity again, which is the shape removed.
# WHAT NO REGEX SEES: a planner that calls the in-use resolver and then Reversed()s the answer.
# That is pinned by Airside.Model.RunwayInUse.DepartsFromTheEndInUse and .LandsOverTheEndInUse.
$inUsePlanners = @(
    (Join-Path $plugin 'Private\Model\ArrivalPlanner.cpp'),
    (Join-Path $plugin 'Private\Model\DeparturePlanner.cpp')
)
# #432: THE GAME MODULE TOO. The Land panel chose "the runway a landing would use" by NearestRunwayThreshold itself
# (LandChoices.cpp) - a view deciding a gameplay verdict, stale since #412 made the planner land on whichever runway
# takes the arrival. A view asks the model (UOpsRuntime::QuoteLanding, ArrivalPlanner::FirstLandingRunway); every
# non-test .cpp under Source\AirportMgr is read with the planners.
$inUsePlanners += @(Get-Sources (Join-Path $Root 'Source\AirportMgr') @('.cpp') | Where-Object { $_.Name -notlike '*Test.cpp' } | ForEach-Object { $_.FullName })
foreach ($path in $inUsePlanners) {
    if (-not (Test-Path $path)) {
        $failures.Add("runway-end-in-use: $path is named by rule 28 but does not exist - update the rule, do not let it check nothing")
        continue
    }
    foreach ($h in (Select-String -Path $path -Pattern '\b(RunwayExtentAt|NearestRunwayThreshold)\s*\(')) {
        $t = $h.Line.Trim()
        if ($t -match '^//') { continue }
        $failures.Add("runway-end-in-use: $($path):$($h.LineNumber) chooses a runway end by proximity; use InUseRunwayAt/InUseRunwayNearest (FRunwayFacts::InUse): $t")
    }
}
$ranRules.Add('runway-end-in-use')

# --- 29. BUTTON LOOKS LIVE IN UI/ ------------------------------------------------------------
# UI library step 1 (2026-09-28): four widgets each carried the enabled/selected -> colour rule
# and two typed the rounded-white FButtonStyle recipe by hand; one copy drifted (the inspector's
# Depart painted lighter when disabled). UUiButton::LookFor and ::Build are now the one home, so
# a SetBackgroundColor( or an FButtonStyle anywhere else in the game module is that shape coming
# back (#255: enforce the shape, not the site). UI\ itself is exempt - it is where they live.
# SetBrushColor( is deliberately NOT flagged: the bar's borders call it with meaning-named slots
# (Surface, Well), which is the shape this codebase wants.
$gameSource = Join-Path $Root 'Source\AirportMgr'
$uiDir = Join-Path $gameSource 'UI'
if (-not (Test-Path $uiDir)) {
    $failures.Add("button-looks-in-ui: $uiDir is named by rule 29 but does not exist - update the rule")
}
Get-ChildItem -Path $gameSource -Recurse -Include *.cpp, *.h |
    # The SEPARATOR matters: without it '...\AirportMgr\UI' is a prefix of '...\AirportMgr\UIStyle.cpp',
    # and the style - the likeliest home for a convenient FButtonStyle helper - was silently exempt.
    Where-Object { -not $_.FullName.StartsWith($uiDir + [IO.Path]::DirectorySeparatorChar) -and $_.Name -notlike '*Test.cpp' } |
    ForEach-Object {
        $file = $_
        $hits = Select-String -Path $file.FullName -Pattern 'SetBackgroundColor\(|FButtonStyle' |
            Where-Object { $_.Line -notmatch '^\s*//' -and $_.Line -notmatch '^\s*\*' }
        foreach ($h in $hits) {
            $failures.Add("button-looks-in-ui: $($file.Name):$($h.LineNumber) hand-rolls a button look - use UUiButton (SetState / Build)")
        }
    }
$ranRules.Add('button-looks-in-ui')

# --- 30. THE GAME'S WINDOW HOST REMEMBERS THE LAYOUT ------------------------------------------
# UUiWindowHost::SetLayoutStore is the seam a window's placement is remembered through, and a host
# never handed a store remembers nothing - which is every test's host, deliberately, so the
# player's ini cannot steer the suite. So nothing but this rule sees the HUD stop wiring it:
# CreateAll needs a local player the headless suite does not have (UI library step 3, 2026-09-28).
$hudLayer = Join-Path $Root 'Source\AirportMgr\BuildHudLayer.cpp'
if (-not (Test-Path $hudLayer)) {
    $failures.Add("layout-store-wired: $hudLayer is named by rule 30 but does not exist - update the rule")
} elseif (-not (Select-String -Path $hudLayer -Pattern 'SetLayoutStore\(MakeShared<FUserSettingsLayoutStore>' -Quiet)) {
    $failures.Add("layout-store-wired: $hudLayer no longer hands the window host an FUserSettingsLayoutStore - the player's window layout would be forgotten every launch")
}
$ranRules.Add('layout-store-wired')

# --- 31. THE BUS IS WIRED IN ONE FUNCTION ------------------------------------------------------
# FOpsEventBus (spec 2026-09-29 ops-event-bus): UOpsRuntime::WireBus is the whole subscription map,
# so "what reacts to X?" has one answer you can read. A Subscribe< or RegisterPass( anywhere else in
# production is a subscription nobody can find being consumed - the list-declared-here, read-there
# bug CLAUDE.md names three times. FOpsEventBus's own check() only fires when the stray call runs;
# this sees it at rest. Test sources are exempt: a test wires a bare bus of its own.
$busWiring = Join-Path $Root 'Plugins\AirportOps\Source\AirportOps\Private\Present\OpsRuntime.cpp'
if (-not (Test-Path $busWiring)) {
    $failures.Add("bus-wired-once: $busWiring is named by rule 31 but does not exist - update the rule")
}
foreach ($busTree in @((Join-Path $Root 'Plugins\AirportOps\Source\AirportOps'), (Join-Path $Root 'Source\AirportMgr'))) {
    foreach ($file in Get-Sources $busTree @('.h', '.cpp')) {
        if ($file.FullName -eq $busWiring -or $file.Name -like 'OpsEventBus.*' -or $file.Name -like '*Test.cpp') { continue }
        $hits = Select-String -Path $file.FullName -Pattern '(\.|->)Subscribe<|RegisterPass\(' |
            Where-Object { $_.Line -notmatch '^\s*//' -and $_.Line -notmatch '^\s*\*' }
        foreach ($h in $hits) {
            $failures.Add("bus-wired-once: $($file.Name):$($h.LineNumber) subscribes to the ops bus outside UOpsRuntime::WireBus")
        }
    }
}
$ranRules.Add('bus-wired-once')

# --- 32. A REFUSED COMMIT IS ANNOUNCED ----------------------------------------------------------
# Ops alerts spec (2026-09-29): "cannot afford" at commit was a log line and nothing else. Every
# mutator's affordability guard now goes through URoadEditFacade::AffordOrRefuse, which broadcasts
# OnRefused; a bare !CanAfford( is legal only in a Why* PREVIEW evaluator (asked every frame, so
# it must stay silent), and says so on the same line with "// preview". A new mutator that
# guards with a bare !CanAfford( is the silent refusal coming back.
$facadeDir = Join-Path $Root 'Plugins\Airside\Source\Airside\Private\Present'
$facadeFiles = @(Get-ChildItem -Path $facadeDir -Filter 'RoadEditFacade*.cpp' -ErrorAction SilentlyContinue)
if ($facadeFiles.Count -eq 0) {
    $failures.Add("refusal-is-announced: no RoadEditFacade*.cpp under $facadeDir - update rule 32")
}
foreach ($file in $facadeFiles) {
    # ANY CanAfford( call, not only the negated form (review: `if (CanAfford(Q)) ... else` would slip
    # past a !-only pattern). Legal: the definition, AffordOrRefuse's own check (marked), a preview.
    $hits = Select-String -Path $file.FullName -Pattern '\bCanAfford\(' |
        Where-Object { $_.Line -notmatch '^\s*//' -and $_.Line -notmatch '//\s*(preview|announces)' -and $_.Line -notmatch 'URoadEditFacade::CanAfford\(' -and $_.Line -notmatch 'Purse->CanAfford\(' }
    foreach ($h in $hits) {
        $failures.Add("refusal-is-announced: $($file.Name):$($h.LineNumber) guards a commit with a bare CanAfford( - use AffordOrRefuse, or mark a Why* preview with // preview")
    }
}
$ranRules.Add('refusal-is-announced')

# --- 33. THE ARRIVAL QUEUE IS A PASS ------------------------------------------------------------
# Ops batch 3 PR D (spec 2026-09-29-ops-batch3 §5): UFlightBoard::TickQueue used to be called from
# UOpsRuntime::Tick every frame, asking the runway live because nothing announced a taxiing crossing
# clearing. Airside now derives OnRunwayFreed, and the queue is the bus's "ArrivalQueue" pass. A
# second production caller of TickQueue( - back in Tick, or anywhere else - is the per-frame poll
# returning beside the pass, and the quiet airport's zero-cost with it. The one legal caller is
# UOpsRuntime::RunArrivalQueue; tests call it directly on a bare board and are exempt.
# HARDENED in PR D's review (M3): any `TickQueue(` call - member, bare (from inside UFlightBoard) or
# spaced - and any `&X::TickQueue` taken as a pointer; the definition line alone is exempt; a
# comment is stripped before matching; and "which function am I in" is re-read at EVERY column-0
# definition, free functions included, so nothing after RunArrivalQueue inherits its exemption.
$queuePassFile = Join-Path $Root 'Plugins\AirportOps\Source\AirportOps\Private\Present\OpsRuntime.cpp'
if (-not (Test-Path $queuePassFile)) {
    $failures.Add("queue-is-a-pass: $queuePassFile is named by rule 33 but does not exist - update the rule")
}
else {
    $queueRunner = Select-String -Path $queuePassFile -Pattern 'void UOpsRuntime::RunArrivalQueue\('
    if ($null -eq $queueRunner) {
        $failures.Add("queue-is-a-pass: UOpsRuntime::RunArrivalQueue not found in $queuePassFile - update rule 33")
    }
}
foreach ($queueTree in @((Join-Path $Root 'Plugins\AirportOps\Source\AirportOps'), (Join-Path $Root 'Source\AirportMgr'))) {
    foreach ($file in Get-Sources $queueTree @('.cpp')) {
        if ($file.Name -like '*Test.cpp') { continue }
        $lines = Get-Content -LiteralPath $file.FullName
        # WHICH FUNCTION each call sits in: the nearest preceding column-0 definition line - a member
        # (X::Y() or a free function (Y(), whichever; a free function resets it like anything else.
        $current = ''
        for ($i = 0; $i -lt $lines.Count; $i++) {
            $line = $lines[$i]
            if ($line -match '^\s*(\*|/\*)') { continue }
            $code = ($line -replace '//.*$', '')
            if ($code -match '^[A-Za-z_][\w:<>,\*& ]*?\b((?:\w+::)?~?\w+)\s*\(') { $current = $Matches[1] }
            $isDefinition = $code -match '^[A-Za-z_].*\bUFlightBoard::TickQueue\s*\('
            $isCall = (-not $isDefinition) -and ($code -match '\bTickQueue\s*\(' -or $code -match '&\s*\w+::TickQueue\b')
            if ($isCall -and $current -ne 'UOpsRuntime::RunArrivalQueue') {
                $failures.Add("queue-is-a-pass: $($file.Name):$($i + 1) calls TickQueue( from $current - the queue runs only as the ArrivalQueue pass (UOpsRuntime::RunArrivalQueue)")
            }
        }
    }
}
$ranRules.Add('queue-is-a-pass')

# --- 34. THE SIM TIME SCALE IS SET ON CHANGE ----------------------------------------------------
# Ops batch 3 PR E (spec 2026-09-29-ops-batch3 §6): UOpsRuntime::Tick re-set the actor's time scale
# every frame to the same double. It is now set only where the speed can change - Attach,
# StepSpeed, TogglePause and LoadFromSlot all end in UOpsRuntime::ApplySpeed - and that is only
# correct while NOTHING ELSE writes the scale: a second writer (a per-frame one back in Tick, or a
# reset elsewhere) is either the poll returning or a scale the next speed change never hears of.
# The one legal caller is UOpsRuntime::ApplySpeed; the setter's own definition and tests are
# exempt. A DIRECT WRITE of the member (SimTimeScale =, +=, ...) anywhere but the setter's own line
# and the member's declaration is the same second writer, bypassing the setter, and fails too.
# HARDENED in PR E's review: "which function am I in" is re-read at column-0 definitions AND at
# indented ones (a type, a name, "(" and no ";" - a free function in an anonymous namespace, a
# class's inline member) and at every `namespace`, so nothing after ApplySpeed inherits its
# exemption; string literals and /* */ comments are stripped before matching, so neither a log line
# naming the setter nor a commented-out call counts; AirsideEditor is scanned as well.
function Strip-ArchCode([string] $Line, [ref] $InBlock) {
    $code = $Line
    if ($InBlock.Value) {
        $end = $code.IndexOf('*/')
        if ($end -lt 0) { return '' }
        $code = $code.Substring($end + 2)
        $InBlock.Value = $false
    }
    $code = $code -replace '"(?:[^"\\]|\\.)*"', '""'
    $code = $code -replace '/\*.*?\*/', ''
    # WHICHEVER OPENS FIRST: a // comment that mentions /* (a path glob, say) is a line comment, not a block.
    $start = $code.IndexOf('/*')
    $lineComment = $code.IndexOf('//')
    if ($start -ge 0 -and ($lineComment -lt 0 -or $start -lt $lineComment)) { $code = $code.Substring(0, $start); $InBlock.Value = $true; return $code }
    return ($code -replace '//.*$', '')
}
function Get-ArchDefinition([string] $Code) {
    if ($Code -match '^\s*namespace\b') { return 'namespace' }
    if ($Code -match '^[A-Za-z_][\w:<>,\*& ]*?\b((?:\w+::)?~?\w+)\s*\(') { return $Matches[1] }
    if ($Code -match '^\s+(?!(?:return|if|else|for|while|switch|case|do|new|delete|throw|co_return)\b)[A-Za-z_][\w:<>,\*&]*(?:\s+[\w:<>,\*&]+)*\s+[\*&]?((?:\w+::)?~?\w+)\s*\([^;]*$') {
        return $Matches[1]
    }
    return $null
}
$scaleTrees = @((Join-Path $Root 'Plugins\AirportOps\Source\AirportOps'), (Join-Path $Root 'Source\AirportMgr'),
    (Join-Path $Root 'Plugins\Airside\Source\Airside'), (Join-Path $Root 'Plugins\Airside\Source\AirsideEditor'))
$scaleOwner = Join-Path $Root 'Plugins\AirportOps\Source\AirportOps\Private\Present\OpsRuntime.cpp'
if (-not (Test-Path $scaleOwner) -or $null -eq (Select-String -Path $scaleOwner -Pattern 'void UOpsRuntime::ApplySpeed\(')) {
    $failures.Add("scale-on-change: UOpsRuntime::ApplySpeed not found in $scaleOwner - update rule 34, do not let it check nothing")
}
foreach ($scaleTree in $scaleTrees) {
    foreach ($file in Get-Sources $scaleTree @('.cpp', '.h')) {
        if ($file.Name -like '*Test.cpp') { continue }
        $lines = Get-Content -LiteralPath $file.FullName
        $current = ''
        $inBlock = $false
        for ($i = 0; $i -lt $lines.Count; $i++) {
            $code = Strip-ArchCode $lines[$i] ([ref]$inBlock)
            if ($code.Trim() -eq '') { continue }
            $definition = Get-ArchDefinition $code
            if ($null -ne $definition) { $current = $definition }
            $isSetter = $code -match '\bvoid\s+SetSimTimeScale\s*\('
            $isCall = (-not $isSetter) -and ($code -match '\bSetSimTimeScale\s*\(' -or $code -match '&\s*\w+::SetSimTimeScale\b')
            if ($isCall -and $current -ne 'UOpsRuntime::ApplySpeed') {
                $failures.Add("scale-on-change: $($file.Name):$($i + 1) calls SetSimTimeScale( from $current - the actor's scale is set only by UOpsRuntime::ApplySpeed, where the speed changes")
            }
            $isWrite = (-not $isSetter) -and ($code -notmatch '\bdouble\s+SimTimeScale\b') -and ($code -match '\bSimTimeScale\s*[-+*/]?=(?!=)')
            if ($isWrite) {
                $failures.Add("scale-on-change: $($file.Name):$($i + 1) writes SimTimeScale directly - only SetSimTimeScale does, and only UOpsRuntime::ApplySpeed calls it")
            }
        }
    }
}
$ranRules.Add('scale-on-change')

# --- 35. RUNWAY FACTS GO THROUGH THE FACADE -----------------------------------------------------
# Ops batch 3 PR E's review (I1): URoadNetwork::SetRunwayFacts moved no revision - not EditRevision,
# not GuidelineRevision. Three gates key on GetGuidelineRevision to see a runway's facts change (the
# held taxi out's replan, FInspectorCardKey, FLandChoicesKey), and they saw it only because every
# production write went through URoadEditFacade, whose Topology rebuild re-made the guideline graph.
# #446 MOVED THE CLOCK INTO THE MODEL (URoadNetwork::NoteFactChanged) and made the facade's notify a
# Facts one that re-derives nothing, so the gates no longer depend on this rule. It STAYS for what the
# facade still adds: the undo step, and the rebuild that repaints the runway and announces
# OnNetworkChanged to the buildings, ops and the controller - a raw model write gets none of them.
# A write that skipped the facade would leave the picture and ops stale, silently. So the NETWORK's setter may
# be called only from RoadEditFacade*.cpp and Testing/ (and the test modules, which are not scanned);
# the facade's own SetRunwayFacts(int32, ...) - reached as Facade-> or through IRoadEditTarget as
# Target-> - is the door everything else uses. Declarations and definitions are exempt; comments and
# string literals are stripped first (rule 34's helpers).
$factsTrees = @((Join-Path $Root 'Plugins\AirportOps\Source\AirportOps'), (Join-Path $Root 'Source\AirportMgr'),
    (Join-Path $Root 'Plugins\Airside\Source\Airside'), (Join-Path $Root 'Plugins\Airside\Source\AirsideEditor'))
$factsFacade = @(Get-ChildItem -Path (Join-Path $Root 'Plugins\Airside\Source\Airside\Private\Present') -Filter 'RoadEditFacade*.cpp' -ErrorAction SilentlyContinue)
if ($factsFacade.Count -eq 0 -or $null -eq ($factsFacade | Select-String -Pattern 'bool URoadEditFacade::SetRunwayFacts\(')) {
    $failures.Add("facts-through-facade: URoadEditFacade::SetRunwayFacts not found - update rule 35, do not let it check nothing")
}
foreach ($factsTree in $factsTrees) {
    foreach ($file in Get-Sources $factsTree @('.cpp', '.h')) {
        if ($file.Name -like '*Test.cpp' -or $file.Name -like 'RoadEditFacade*.cpp') { continue }
        if ($file.FullName -match '[\\/]Testing[\\/]') { continue }
        $lines = Get-Content -LiteralPath $file.FullName
        $inBlock = $false
        for ($i = 0; $i -lt $lines.Count; $i++) {
            $code = Strip-ArchCode $lines[$i] ([ref]$inBlock)
            if ($code -notmatch '\bSetRunwayFacts\s*\(') { continue }
            if ($code -match '\bbool\s+(?:\w+::)?SetRunwayFacts\s*\(') { continue }
            $legal = $code -match '\b(?:Facade|Target)\s*->\s*SetRunwayFacts\s*\('
            if (-not $legal) {
                $failures.Add("facts-through-facade: $($file.Name):$($i + 1) calls SetRunwayFacts( on something other than the facade or an edit target - a raw network write gets no undo step, no repaint and no OnNetworkChanged, so the runway on screen and ops never hear it; go through URoadEditFacade::SetRunwayFacts")
            }
        }
    }
}
$ranRules.Add('facts-through-facade')

# --- 36. RETIRED (#445) - see rule 59 ---------------------------------------------------------
# It held the funnels UOpsRuntime::DirtyJobBoard / DirtyArrivalQueue / ArmSafetyNet, which existed only to set a hand-kept "covered" flag the two
# net-watched passes read to tell a run an event asked for from a run only the safety net asked for. That fact is the bus's now
# (EPassCause, carried by FPassRun) and the net is FOpsSafetyNet's, so there is no flag and no funnel to keep used: a raw
# MarkDirty(TEXT("JobBoard")) IS the event cause. What the old rule protected - a real event's run being misread as the net's - can now be
# done wrong one way only, by minting EPassCause::SafetyNet outside the net, which rule 59 holds. The number is left here so a comment that
# names "rule 36" finds this.

# --- 37. A PERSISTENT OBJECT SAVES NO POINTER TO A RUNTIME OBJECT -------------------------------
# Issue #425: OpsSave writes every non-Transient UPROPERTY of an IOpsPersistent through FObjectAndNameAsStringProxyArchive,
# which writes a UObject reference as its PATH and re-finds it by path on load. For a content asset that is right; for a
# runtime object it is an address no later session can resolve. The flight board's flights and its six wiring pointers
# all came back null in a new process - silently, because null is a working state for every one of them (no Allocator:
# every accept refused) - and the Transient exception had been applied at one site (UOfferGenerator::Airport) by hand.
# So, in a class whose base list names IOpsPersistent - OR a class a persistent owner saves BY VALUE through the same
# archive, named in $savedByValueClasses (UFlight: UFlightBoard::Serialize writes its tagged properties inline) - a
# UPROPERTY whose declaration holds an object pointer (TObjectPtr<, TWeakObjectPtr<, TLazyObjectPtr<, TScriptInterface<,
# FWeakObjectPtr, a raw T*, bare or in a container) must be UPROPERTY(Transient ...) or be listed in
# $persistentRefAllowed as Class::Member - CONTENT ASSETS ONLY. A runtime object that IS state is saved by value by its
# owner, not by pointer. NOT SEEN, deliberately: a pointer inside a saved USTRUCT member (FEntityInstance::Definition is
# content, rebound on load by RebindStandDefinitions), and a class that inherits IOpsPersistent through another class.
# HARDENED in #452's review, so it cannot pass by reading nothing: comments and string literals are stripped (rule 34's
# stripper) BEFORE a declaration is looked for, so `UPROPERTY() // note` reads the next line; a UPROPERTY( whose
# parentheses do not close on its line fails rather than being skipped; the classes matched must equal the count of
# `public IOpsPersistent` base mentions in code (a base list wrapped onto a second line is otherwise missed); and it
# fails if it finds no persistent class, a $savedByValueClasses entry that matches no class, or a stale allow-list entry.
$persistentRefAllowed = @()   # 'UClass::Member' - content assets only; empty on 2026-09-30
$savedByValueClasses = @('UFlight')   # saved inline by an IOpsPersistent owner - see UFlightBoard::Serialize
$persistentPointerPattern = 'TObjectPtr<|TWeakObjectPtr<|TLazyObjectPtr<|TScriptInterface<|\bFWeakObjectPtr\b|\b[A-Z]\w*\s*\*'
$persistentClassCount = 0
$persistentBaseMentions = 0
$savedByValueSeen = @{}
$persistentAllowedSeen = @{}
# The text between UPROPERTY's own parentheses and the rest of the line after them, or $null when they do not close on
# this line. BALANCED, not [^)]*: meta=(ClampMin="0") nests a pair inside the specifier list.
function Split-ArchUProperty([string] $Code) {
    $at = [regex]::Match($Code, '^\s*UPROPERTY\s*\(')
    if (-not $at.Success) { return $null }
    $depth = 1
    for ($c = $at.Index + $at.Length; $c -lt $Code.Length; $c++) {
        if ($Code[$c] -eq '(') { $depth++ }
        elseif ($Code[$c] -eq ')') {
            $depth--
            if ($depth -eq 0) {
                return @($Code.Substring($at.Index + $at.Length, $c - $at.Index - $at.Length), $Code.Substring($c + 1))
            }
        }
    }
    return $null
}
foreach ($file in Get-Sources (Join-Path $ops 'Public') @('.h')) {
    $lines = Get-Content -LiteralPath $file.FullName
    $inClass = ''
    $inBlock = $false
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $code = Strip-ArchCode $lines[$i] ([ref]$inBlock)
        $persistentBaseMentions += ([regex]::Matches($code, '\bpublic\s+IOpsPersistent\b')).Count
        $classMatch = [regex]::Match($code, '^\s*class\s+(?:\w+_API\s+)?(\w+)\s*:([^{;]*)')
        if ($classMatch.Success) {
            $name = $classMatch.Groups[1].Value
            if ($classMatch.Groups[2].Value -match '\bpublic\s+IOpsPersistent\b') { $inClass = $name; $persistentClassCount++; continue }
            if ($savedByValueClasses -contains $name) { $inClass = $name; $savedByValueSeen[$name] = $true; continue }
        }
        if ($inClass -eq '') { continue }
        # The class's own closing brace is the one at column 0; a nested struct's is indented.
        if ($code -match '^};') { $inClass = ''; continue }
        if ($code -notmatch '^\s*UPROPERTY\s*\(') { continue }
        $parts = Split-ArchUProperty $code
        if ($null -eq $parts) {
            $failures.Add("persistent-refs-transient: $($file.Name):$($i + 1) $inClass has a UPROPERTY( that does not close on its line - rule 37 reads one line; put the specifiers on one line")
            continue
        }
        $spec = $parts[0]
        # The declaration: the rest of this line, or the next line when the macro stands alone (comments already gone).
        $decl = $parts[1].Trim()
        if ($decl -eq '' -and $i + 1 -lt $lines.Count) {
            $lookahead = $inBlock
            $decl = (Strip-ArchCode $lines[$i + 1] ([ref]$lookahead)).Trim()
        }
        if ($decl -notmatch $persistentPointerPattern) { continue }
        if ($spec -match '\bTransient\b') { continue }
        $member = [regex]::Match($decl, '(\w+)\s*(?:\[[^\]]*\])?\s*(?:=[^;]*)?;').Groups[1].Value
        $key = "$($inClass)::$member"
        if ($persistentRefAllowed -contains $key) { $persistentAllowedSeen[$key] = $true; continue }
        $failures.Add("persistent-refs-transient: $($file.Name):$($i + 1) $key is a saved object pointer on a class OpsSave saves - it is written as a PATH, which a later session resolves to null (#425). Mark it UPROPERTY(Transient) and wire it in UOpsRuntime's constructor; save runtime state by value; allow-list content assets only")
    }
}
if ($persistentClassCount -eq 0) {
    $failures.Add("persistent-refs-transient: found no class deriving IOpsPersistent under $ops\Public - it moved; update rule 37, do not let it check nothing")
}
if ($persistentClassCount -ne $persistentBaseMentions) {
    $failures.Add("persistent-refs-transient: $persistentBaseMentions 'public IOpsPersistent' base mention(s) in code but $persistentClassCount class line(s) matched - a base list wraps onto a second line, or IOpsPersistent is inherited outside Public/; put the class head on one line or update rule 37")
}
foreach ($byValue in $savedByValueClasses) {
    if (-not $savedByValueSeen.ContainsKey($byValue)) {
        $failures.Add("persistent-refs-transient: $byValue is listed as saved by value but no 'class $byValue :' was found under $ops\Public - it moved or went; update rule 37")
    }
}
foreach ($allowed in $persistentRefAllowed) {
    if (-not $persistentAllowedSeen.ContainsKey($allowed)) {
        $failures.Add("persistent-refs-transient: allow-list entry $allowed matches no saved pointer - it moved or went; update rule 37")
    }
}
$ranRules.Add('persistent-refs-transient')

# --- 38. A SERVICE VEHICLE'S STATE, AGENT AND JOB ARE WRITTEN ONLY BY ITS LIFECYCLE -------------
# Issue #428: FServiceVehicle was a passive USTRUCT whose State, AgentId and CurrentJob were assigned by whichever board
# function happened to be running - fifteen `State =` writes, nine `AgentId =` writes, "leave the road" spelled six ways -
# and the enum's contract (which states have an agent, which have a job) was checked nowhere, so "Serving with no job"
# shipped and lived on behind a per-Step backstop. FServiceVehicleLifecycle (Private/Model/ServiceVehicleLifecycle.cpp)
# is now the one writer of the three fields, and asserts the invariant on each transition; this is what keeps it so.
# THREE SHAPES OF WRITE, outside the lifecycle file, each a hit (comments and string literals are stripped first - rule
# 34's helper):
#   a. `.State = EServiceVehicleState::...` through ANY variable - the enum on the right names the vehicle.
#   b. A write of State, AgentId or CurrentJob through a VEHICLE-NAMED variable (Vehicle, Vehicles[i], Truck, Bowser -
#      `\w*(Vehicle|Truck|Bowser)\w*`) with ANY right-hand side: `Vehicle.State = To;` names no enum, and shape a alone
#      let it through. In any file.
#   c. In a file that holds vehicles by any name (it spells FServiceVehicle / EServiceVehicleState, or reaches them through
#      GetVehicles( / FindVehicle( / FindVehicleMutable( / VehicleForAgent( ): ANY `.AgentId =` or `.CurrentJob =` write.
#      That is the `for (auto& V : Board->GetVehicles()) V.AgentId = 0;` shape, whose variable names no vehicle. It is
#      scoped to vehicle-holding files because AgentId is also an FFlight / claim / rig-runner field elsewhere. A GAP, SAID:
#      `.State = <variable>` through a variable that names no vehicle (`auto& V ... V.State = To;`) is invisible to a regex
#      - `Job.State = State;` is the job's - and only shape a (the enum on the right) or b (the variable's name) sees it.
# Test files are exempt: a test stages a vehicle where no transition reaches, to see what the board does with it
# (FServiceVehicleLifecycle::SeedStateForTest is the one blessed way, and a test's own poke is allowed). The rule fails,
# rather than checking nothing, when the lifecycle file is gone or its writes no longer match shape b. Mutation-checked on
# 2026-09-30 against a scratch copy: an injected `Vehicle.State = To;`, `for (auto& V : ...GetVehicles()) V.AgentId = 0;`
# and `Vehicle.State = EServiceVehicleState::Idle;` each FAIL, and renaming the lifecycle file FAILs.
$lifecycleFile = Join-Path $ops 'Private\Model\ServiceVehicleLifecycle.cpp'
$vehicleEnumWrite  = '(?:\.|->)State\s*=\s*EServiceVehicleState::'
$vehicleNamedWrite = '\b\w*(?:Vehicle|Truck|Bowser)\w*(?:\[[^\]]*\])?(?:\.|->)(?:State|AgentId|CurrentJob)\s*=(?!=)'
$vehicleHeldFile   = '\bFServiceVehicle\b|\bEServiceVehicleState\b|\bGetVehicles\s*\(|\bFindVehicle(?:Mutable)?\s*\(|\bVehicleForAgent\s*\('
$vehicleHeldWrite  = '(?:\.|->)(?:AgentId|CurrentJob)\s*=(?!=)'
$lifecycleWrites = 0
if (-not (Test-Path $lifecycleFile)) {
    $failures.Add("vehicle-lifecycle-one-writer: $lifecycleFile not found - update rule 38, do not let it check nothing")
} else {
    foreach ($line in (Get-Content -LiteralPath $lifecycleFile)) {
        if ($line -match $vehicleNamedWrite) { $lifecycleWrites++ }
    }
    if ($lifecycleWrites -eq 0) {
        $failures.Add("vehicle-lifecycle-one-writer: no State/AgentId/CurrentJob write found in $lifecycleFile - the rule's patterns no longer match; update rule 38")
    }
}
$lifecycleTrees = @($ops, (Join-Path $Root 'Source\AirportMgr'))
foreach ($lifecycleTree in $lifecycleTrees) {
    foreach ($file in Get-Sources $lifecycleTree @('.cpp', '.h')) {
        if ($file.Name -like '*Test.cpp' -or $file.Name -like '*Test.h' -or $file.Name -eq 'ServiceVehicleLifecycle.cpp') { continue }
        if ($file.FullName -match '[\\/]Testing[\\/]') { continue }
        $lines = Get-Content -LiteralPath $file.FullName
        $holdsVehicles = $null -ne ($lines | Select-String -Pattern $vehicleHeldFile | Select-Object -First 1)
        $inBlock = $false
        for ($i = 0; $i -lt $lines.Count; $i++) {
            $code = Strip-ArchCode $lines[$i] ([ref]$inBlock)
            if ($code.Trim() -eq '') { continue }
            if ($code -match $vehicleEnumWrite -or $code -match $vehicleNamedWrite -or ($holdsVehicles -and $code -match $vehicleHeldWrite)) {
                $failures.Add("vehicle-lifecycle-one-writer: $($file.Name):$($i + 1) writes a vehicle's State/AgentId/CurrentJob by hand - go through FServiceVehicleLifecycle (UJobBoard::Lifecycle), which asserts the (State, AgentId, CurrentJob) invariant: $($code.Trim())")
            }
        }
    }
}
$ranRules.Add('vehicle-lifecycle-one-writer')

# --- 39. WHICH RUNWAYS A KIND OF TRAFFIC MAY USE IS ONE ENUMERATION -----------------------------
# Issue #433: ArrivalPlanner and DeparturePlanner each filtered the runways by ERunwayUse and ranked what was left
# (free, then dedicated, then shortest) in their own code, and RunwayAdmission::CheckArrival's "can it depart
# again" half walked EVERY runway and filtered nothing - so on a field of arrivals-only strips it admitted jets
# that could land and never depart, and the stand stayed blocked for ever. The filter, the end in use and the
# ranking are now RunwayQuery::ArrivalRunways / DepartureRunways / RankRunway, and the three consumers ask them.
# In those three files, a walk of AirsideCapability's summary, a read of ERunwayUse (RunwayUse:: or the enum itself - \b cannot fall
# between the E and the R, so RunwayUse:: alone misses `== ERunwayUse::ArrivalsOnly`) or a question put
# to the occupancy (IsAnyHeld) is the second definition returning. It also fails when a consumer stops calling its
# enumerator or ranking (the ban alone would pass a file that enumerates nothing) and when a file the rule names is
# gone. Comments and string literals are stripped first (rule 34's stripper), so a WHY comment can name the bans.
# NOT BANNED, deliberately: UGroundTraffic's runway-seed list (every runway, whatever its use, is what it wants)
# and UAirport::HasRunway / ARoadBuildController::HasRunway (is there a runway at all - not whether one takes a kind).
# WHAT NO REGEX SEES: a consumer that calls the enumerator and then re-filters its answer with a use test spelled
# some other way. That is pinned by Airside.Model.RunwayUse.EveryModePairLandsOnlyWhatCanLeave.
$runwayKindConsumers = @(
    @{ Path = (Join-Path $plugin 'Private\Model\ArrivalPlanner.cpp');   Calls = @('ArrivalRunways', 'RankRunway') },
    @{ Path = (Join-Path $plugin 'Private\Model\DeparturePlanner.cpp'); Calls = @('DepartureRunways', 'RankRunway') },
    @{ Path = (Join-Path $plugin 'Private\Model\RunwayAdmission.cpp');  Calls = @('DepartureRunways') }
)
$runwayKindBanned = @(
    @{ Pattern = '\bAirsideCapability::Summarise\w*\s*\(|\bSummariseRunways\s*\(';
       Why = 'walks every runway; ask RunwayQuery::ArrivalRunways / DepartureRunways for the ones this traffic may use' },
    @{ Pattern = '\bERunwayUse\b|\bRunwayUse::';
       Why = 'reads ERunwayUse itself; the use filter and the dedicated rule live in RunwayQuery (ArrivalRunways / DepartureRunways / RankRunway)' },
    @{ Pattern = '(\.|->)IsAnyHeld\s*\(';
       Why = 'asks the occupancy about a strip itself; RunwayQuery::IsChainHeld / RankRunway is the one reading of "held"' }
)
foreach ($consumer in $runwayKindConsumers) {
    $consumerName = Split-Path $consumer.Path -Leaf
    if (-not (Test-Path $consumer.Path)) {
        $failures.Add("runway-kind-enumeration: $($consumer.Path) is named by rule 39 but does not exist - update the rule, do not let it check nothing")
        continue
    }
    $seenCalls = @{}
    $lines = Get-Content -LiteralPath $consumer.Path
    $inBlock = $false
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $code = Strip-ArchCode $lines[$i] ([ref]$inBlock)
        foreach ($banned in $runwayKindBanned) {
            if ($code -match $banned.Pattern) {
                $failures.Add("runway-kind-enumeration: $($consumerName):$($i + 1) $($banned.Why) (#433): $($code.Trim())")
            }
        }
        foreach ($call in $consumer.Calls) {
            if ($code -match ('\bRunwayQuery::' + $call + '\s*\(')) { $seenCalls[$call] = $true }
        }
    }
    foreach ($call in $consumer.Calls) {
        if (-not $seenCalls.ContainsKey($call)) {
            $failures.Add("runway-kind-enumeration: $consumerName no longer calls RunwayQuery::$call( - it has its own copy again, or rule 39 is stale (#433)")
        }
    }
}
$ranRules.Add('runway-kind-enumeration')

# --- 40. A REFUSAL AFTER AN EDIT SCOPE OPENS ROLLS THE SCOPE BACK ----------------------------
# Issue #437: FRoadEditScope was a Memento with no restore - an uncommitted scope drops its undo snapshot and leaves the
# graph as the body left it - so a mutator that wrote and THEN refused left a changed graph with no undo entry, and the
# only revert in the facade hung on there being an undo history, which the editor world does not have. Two consequences
# shipped: a refused drop-to-merge stayed merged and was committed into the level by the editor's drag transaction, and
# model refusal rules were re-typed above the scope so that no refusal had to happen inside one. The scope can roll
# back now (Rollback(), in both worlds), and this is the shape that keeps it used: in URoadEditFacade's translation
# units, every `return false;` / `return INDEX_NONE;` that follows an `FRoadEditScope <name>(` inside its block has a
# `<name>.Rollback()` in the SAME BRACE BLOCK before it, or in a block that encloses it (one still open where the
# return is). Block-aware, not a window of N lines (the first cut of this rule was 12 physical lines): a Rollback in
# an earlier `if (!A) { <name>.Rollback(); return false; }` sits within any window of the next `if (!B) { return
# false; }` and let it through, and a Rollback in a block that has closed does not run on the path to the return.
# Uniform on purpose, not "only the ones that wrote": whether a refusal wrote something is exactly what the next
# mutator to grow a second write will get wrong, and a restore on a refusal that wrote nothing is a copy, not a bug. A
# `return true;` is not a refusal - a no-op that succeeded returns uncommitted on purpose and is left alone. A lambda
# body returning false after a scope opens would read as a refusal of the enclosing function; none exists (2026-09-30)
# and the rule would say so. Fails if it finds no scope or no Rollback() at all, so it cannot pass by reading nothing.
$rollbackScopeCount = 0
$rollbackCallCount = 0
foreach ($file in Get-Sources (Join-Path $plugin 'Private\Present') @('.cpp')) {
    if ($file.Name -notlike 'RoadEditFacade*.cpp') { continue }
    $lines = Get-Content -LiteralPath $file.FullName
    $inBlock = $false
    $blocks = New-Object System.Collections.ArrayList   # one hashtable per open brace: scope names rolled back in it
    $open = New-Object System.Collections.ArrayList     # @{ Name; Depth; Line } - Depth is the brace depth it opened at
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $code = Strip-ArchCode $lines[$i] ([ref]$inBlock)
        # TOKENS IN SOURCE ORDER, so `{ Edit.Rollback(); return false; }` on one line reads as it runs.
        # THE SCOPE'S OPEN PAREN OR BRACE IS A LOOKAHEAD, never part of its token (#460's review): brace-init,
        # `FRoadEditScope Edit{H, N, L};`, used to consume the `{` while its `}` still popped a block, so the stack
        # ran one short, the scope was dropped as closed, and every later return in that function went unchecked.
        foreach ($token in [regex]::Matches($code, '\{|\}|\bFRoadEditScope\s+(\w+)\s*(?=[({])|\b(\w+)\.Rollback\s*\(|\breturn\s+(?:false|INDEX_NONE)\s*;')) {
            $text = $token.Value
            if ($text -eq '{') { [void]$blocks.Add(@{}) }
            elseif ($text -eq '}') {
                if ($blocks.Count -gt 0) { $blocks.RemoveAt($blocks.Count - 1) }
                for ($k = $open.Count - 1; $k -ge 0; $k--) {
                    if ($blocks.Count -lt $open[$k].Depth) { $open.RemoveAt($k) }
                }
            }
            elseif ($text -match '^FRoadEditScope') {
                [void]$open.Add(@{ Name = $token.Groups[1].Value; Depth = $blocks.Count; Line = $i })
                $rollbackScopeCount++
            }
            elseif ($text -match '\.Rollback') {
                $rollbackCallCount++
                if ($blocks.Count -gt 0) { $blocks[$blocks.Count - 1][$token.Groups[2].Value] = $true }
            }
            else {
                foreach ($scope in $open) {
                    $rolled = $false
                    # From the block the scope was declared in, inward to where the return sits.
                    for ($d = [Math]::Max($scope.Depth - 1, 0); $d -lt $blocks.Count; $d++) {
                        if ($blocks[$d].ContainsKey($scope.Name)) { $rolled = $true; break }
                    }
                    if (-not $rolled) {
                        $failures.Add("scope-refusal-rolls-back: $($file.Name):$($i + 1) refuses after $($scope.Name) opened (line $($scope.Line + 1)) without $($scope.Name).Rollback() in its own or an enclosing brace block - an uncommitted scope restores nothing, so whatever the body wrote stays with no undo step (#437)")
                    }
                }
            }
        }
    }
}
if ($rollbackScopeCount -eq 0) {
    $failures.Add("scope-refusal-rolls-back: found no FRoadEditScope under Private\Present\RoadEditFacade*.cpp - it moved; update rule 40, do not let it check nothing")
}
if ($rollbackCallCount -eq 0) {
    $failures.Add("scope-refusal-rolls-back: found no .Rollback( call under Private\Present\RoadEditFacade*.cpp - the rule would pass on code that never rolls back; update rule 40")
}
$ranRules.Add('scope-refusal-rolls-back')

# --- 41. A PERSISTENT OBJECT WITH A REVISION BUMPS IT ON A LOAD --------------------------------
# Issue #426: a save game deserialises INTO the live objects, so a view gated on an object's Revision() sees the same
# object and asks the same counter - and ULedger's never moved on a load, so the bar's balance and the ledger rows kept
# the pre-load money until the next fee. UJobBoard and UFlightBoard each bumped in their own Serialize, a rule nothing
# stated. So: a class under $ops\Public whose base list names IOpsPersistent and which declares a Revision() accessor
# must declare `void Serialize(FArchive& Ar) override` (whose body bumps on IsLoading - pinned by
# AirportMgr.UI.LedgerPanelGate for the ledger and AirportOps.Model.FlightSave.PreV6BlobRestoresNoFlights for the board -
# both straight through OpsSave::DeserializeObject, so no OnBeforeRestore bump can stand in for the one in Serialize).
# It reads the declaration, not the body: a Serialize that forgets to bump is the tests' to catch. Fails if it finds no
# persistent class with a revision at all - it moved, and must not check nothing.
$revisionOwners = @{}
$revisionSerializers = @{}
foreach ($file in Get-Sources (Join-Path $ops 'Public') @('.h')) {
    $lines = Get-Content -LiteralPath $file.FullName
    $inClass = ''
    $inBlock = $false
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $code = Strip-ArchCode $lines[$i] ([ref]$inBlock)
        $classMatch = [regex]::Match($code, '^\s*class\s+(?:\w+_API\s+)?(\w+)\s*:([^{;]*)')
        if ($classMatch.Success) {
            $inClass = if ($classMatch.Groups[2].Value -match '\bpublic\s+IOpsPersistent\b') { $classMatch.Groups[1].Value } else { '' }
            continue
        }
        if ($inClass -eq '') { continue }
        if ($code -match '^};') { $inClass = ''; continue }
        if ($code -match '\bRevision\s*\(\s*\)\s*const\b') { $revisionOwners[$inClass] = "$($file.Name):$($i + 1)" }
        if ($code -match '\bvoid\s+Serialize\s*\(\s*FArchive\s*&\s*\w*\s*\)\s*override\b') { $revisionSerializers[$inClass] = $true }
    }
}
if ($revisionOwners.Count -eq 0) {
    $failures.Add("persistent-revision-bumps-on-load: found no IOpsPersistent class with a Revision() under $ops\Public - it moved; update rule 41, do not let it check nothing")
}
foreach ($owner in $revisionOwners.Keys) {
    if (-not $revisionSerializers.ContainsKey($owner)) {
        $failures.Add("persistent-revision-bumps-on-load: $($revisionOwners[$owner]) $owner is saved (IOpsPersistent) and has a Revision() a view is gated on, but no Serialize override - a load restores it with no Post-like call, so the view keeps the pre-load answer (#426). Override Serialize and bump on Ar.IsLoading(), as UJobBoard, UFlightBoard and ULedger do")
    }
}
$ranRules.Add('persistent-revision-bumps-on-load')

# --- 42. NO REGISTERED AUTOMATION TEST NAME IS A DOTTED PREFIX OF ANOTHER ---------------------
# 2026-09-30 test-suite review: Airside.Solve.IcaoCode (14 assertions) and Airside.Tool.BuildSession (10) had NEVER run
# since a dotted sibling registered under each (#351, 2026-09-26; #413, 2026-09-29). UE 5.8's automation tree turns a
# bare name into a GROUP node once `Name.Child` exists and drops the bare test's own RunTest - no error, no "not found",
# only a run count one short (memory unreal-automation-test-tree-drops-bare-parent, first met 2026-09-06). The
# count line is the only other thing that sees it, and nobody diffs it. So: collect every registered name and fail when
# one is `A` and another starts with `A.`. Names are read from IMPLEMENT_*_AUTOMATION_TEST( Class, "Name", ... ) with
# comments STRIPPED first and newlines kept (InspectFactsTest.cpp has a whole comment between the two arguments; a
# per-line regex reads that macro as nameless). A COMPLEX test's own name is a parent by design - UE expands it to
# `Name.<case>` at run time from GetTests - and is checked like any other: only a REGISTERED sibling can shadow it.
# It cannot pass by parsing nothing: zero names fail, and so does any AUTOMATION_TEST macro token it could not read a
# name from (a shape it does not understand is a test it is not checking). Scans Plugins\*\Source and Source\AirportMgr
# recursively, i.e. both test modules, AirsideEditor's tests and the game module's, wherever a test lives.
# Mutation check (2026-09-30): on origin/main it FAILs naming IcaoCode and BuildSession; a scratch copy with a bare
# `Airside.Foo` and a `Airside.Foo.Bar` registered in two files fails too, and one with the second macro's name
# commented out passes (the comment is not a registration).
$testNamePattern = '\bIMPLEMENT_(SIMPLE|COMPLEX)_AUTOMATION_TEST\s*\(\s*\w+\s*,\s*"([^"\r\n]+)"'
$testMacroPattern = '\bIMPLEMENT_\w*AUTOMATION_TEST\w*\b'
# Strings kept (the names are in them), comments dropped; a block comment leaves its newlines behind so line numbers
# stay true. The char-literal arm is one char or one escape - never `'000'` of a digit separator, which would otherwise
# swallow up to the next apostrophe and any `//` on the way.
$commentStripper = [regex]'"(?:[^"\\r\n]|\.)*"|''(?:\[^\r\n]|[^''\\r\n])''|//[^\r\n]*|/\*[\s\S]*?\*/'
$commentEvaluator = [System.Text.RegularExpressions.MatchEvaluator]{
    param($m)
    $v = $m.Value
    if ($v.StartsWith('"') -or $v.StartsWith("'")) { return $v }
    if ($v.StartsWith('//')) { return '' }
    return ([regex]::Replace($v, '[^\r\n]', ''))
}
$testRoots = @(Get-ChildItem -Path (Join-Path $Root 'Plugins') -Directory | ForEach-Object { Join-Path $_.FullName 'Source' })
$testRoots += (Join-Path $Root 'Source\AirportMgr')
$registeredTests = @{}   # name -> "File.cpp:line" of its first registration
$registeredKinds = @{}   # name -> 'S' (IMPLEMENT_SIMPLE_) or 'C' (IMPLEMENT_COMPLEX_)
$testMacroTokens = 0
$namedRegistrations = 0
foreach ($testRoot in $testRoots) {
    foreach ($file in Get-Sources $testRoot @('.cpp', '.h')) {
        $raw = [System.IO.File]::ReadAllText($file.FullName)
        if ($raw.IndexOf('AUTOMATION_TEST', [System.StringComparison]::Ordinal) -lt 0) { continue }
        $stripped = $commentStripper.Replace($raw, $commentEvaluator)
        $testMacroTokens += ([regex]::Matches($stripped, $testMacroPattern)).Count
        foreach ($m in [regex]::Matches($stripped, $testNamePattern)) {
            $name = $m.Groups[2].Value
            $namedRegistrations++
            $line = ($stripped.Substring(0, $m.Index) -split "`n").Count
            $where = "$($file.Name):$line"
            if ($registeredTests.ContainsKey($name)) {
                $failures.Add("test-name-prefix: '$name' is registered twice ($($registeredTests[$name]) and $where) - the automation tree keeps one and the other never runs")
            } else {
                $registeredTests[$name] = $where
                $registeredKinds[$name] = if ($m.Groups[1].Value -eq 'COMPLEX') { 'C' } else { 'S' }
            }
        }
    }
}
if ($registeredTests.Count -eq 0) {
    $failures.Add("test-name-prefix: parsed no automation test names under Plugins\*\Source and Source\AirportMgr - the macro shape or the folders moved; update rule 42, do not let it check nothing")
}
if ($testMacroTokens -ne $namedRegistrations) {
    # A macro with no readable "Name" (a concatenated or macro-built name, a different argument order) is a
    # registration this rule cannot see, so it must not pass silently.
    $failures.Add("test-name-prefix: found $testMacroTokens IMPLEMENT_*AUTOMATION_TEST tokens but read a name from $namedRegistrations - a macro whose name this rule cannot read is a test it is not checking; widen `$testNamePattern")
}
foreach ($parent in ($registeredTests.Keys | Sort-Object)) {
    $children = @($registeredTests.Keys | Where-Object { $_.StartsWith($parent + '.', [System.StringComparison]::Ordinal) } | Sort-Object)
    if ($children.Count -gt 0) {
        $childList = ($children | ForEach-Object { "$_ ($($registeredTests[$_]))" }) -join ', '
        $failures.Add("test-name-prefix: '$parent' ($($registeredTests[$parent])) is a dotted prefix of $($children.Count) registered test(s): $childList - UE's automation tree makes the bare name a group and its own RunTest NEVER RUNS, silently. Rename it to a leaf (e.g. '$parent.<WhatItPins>'); see memory unreal-automation-test-tree-drops-bare-parent")
    }
}
$ranRules.Add('test-name-prefix')
$complexTestCount = @($registeredKinds.Values | Where-Object { $_ -eq 'C' }).Count
Write-Host "Check-Architecture: rule 42 read $($registeredTests.Count) registered automation test name(s) ($($registeredTests.Count - $complexTestCount) simple, $complexTestCount complex; a complex one expands to several at run time)." -ForegroundColor Yellow
if ($TestNamesOut -ne '') {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $TestNamesOut) | Out-Null
    ($registeredTests.Keys | Sort-Object | ForEach-Object { "$($registeredKinds[$_])|$_" }) | Set-Content -LiteralPath $TestNamesOut -Encoding UTF8
}

# --- 43. FLEET MEMBERSHIP HAS ONE DOOR, AND FLEET MONEY IS POSTED FROM ITS FILE ------------------
# Issue #443: a vehicle joined or left the fleet through four doors with different side effects - the player's purchase
# (ledger + FleetChanged), the starter seeding (neither), the player's sale (ledger + FleetChanged) and the depot removal,
# where the JOB BOARD posted fleet money itself with its own wording and published nothing, so a FleetChanged subscriber
# missed half the changes. FServiceFleet (Private/Model/ServiceFleet.cpp) is now the one owner of Add and Withdraw and of
# the three things every change owes: its Fleet ledger line, its FFleetChangedEvent, and (on an add) the re-opening of the
# jobs a missing vehicle had refused. THREE SHAPES, each a hit outside that file (comments and string literals stripped,
# rule 34's helper), in production code (AirportOps and the game module; tests stage fleets by hand on purpose):
#   a. A write of the fleet's containers - Vehicles / SeededDepots / NextVehicleId - by ANY member call. A board function
#      that adds or removes a vehicle without the door skips the ledger, the event and the re-open, which is the disease.
#   b. `ELedgerCategory::Fleet` in anything but a `case` label (a reader naming its column, LedgerViewModels' label). The
#      category is posted only from the door, so a second poster with its own wording cannot come back.
#   c. In a .cpp, a vehicle row's Price or ResaleValue read directly (`Spec->Price`, `Spec.Price`, `.ResaleValue(`): the price
#      the card quotes, the purchase judges and charges and the ledger posts is FServiceFleet::PriceOf/ResaleOf, read once,
#      so the three cannot drift (#461 review found three direct reads beside "THE ONE READ"). WIDENED (#266, the #461
#      final review): it matched the variable NAME `Spec` alone, so `VehicleSpecs[Type].Price`, `SpecIt->Price` and
#      `JobBoard->SpecFor(Type).Price` all read the row past it. Now any `Spec`/`Specs`-prefixed name, subscripted or not,
#      and a SpecFor(...) call's result. Mutation-checked 2026-09-30: each of those three shapes typed into
#      FacilityPurchases.cpp fails this rule, and the unwidened pattern passed all three. CASE-SENSITIVE (-cmatch) AND A
#      CAMEL-CASE SUFFIX ONLY (#469 review): -match read `InspectorRow.Price` as "spec...Price", and `Spec\w*` read
#      `SpecialOffer.Price` as a spec; the name must now be `Spec`/`Specs` alone or followed by a capital (`SpecIt`,
#      `VehicleSpecs`). Mutation-checked: both of those pass, the three shapes above still fail.
#      THE ROW MOVED (#430): the price and resale value live on the catalogue's FServiceVehicleType now, joined once by
#      FServiceFleet::ResolveCatalogue (the scenario's FFuelVehicleSpec is read at that join alone). So (c) also reads the
#      row's shapes - `TypeFor(...).Price`, a `Catalogue`/`GetCatalogue()` row's Price or ResaleValue, a `Kind`/`Type`
#      local's - and ResaleValue as a FIELD as well as a call. And (a) counts the catalogue and its StarterFleet as the
#      fleet's containers: ServiceFleet.cpp's ResolveCatalogue is their one production writer. (a) READS ASSIGNMENT TOO
#      (#475 review): `Board->StarterFleet = {...}`, `Catalogue[X] = ...` and `Catalogue.FindOrAdd(X) = ...` all passed a
#      method-call-only pattern, and StarterFleet is a public UPROPERTY. A DECLARATION is not a write - UScenario's
#      `TArray<FName> StarterFleet = {...}` default - so a name preceded by a type (a word or a template's `>`, then
#      whitespace; `->` is not a type) is skipped. Mutation-checked 2026-09-30: each of the three shapes, and a bare
#      `StarterFleet = Other;` in a member body, fails; the scenario's default and `Board.StarterFleet;` reads pass.
# The rule fails, rather than checking nothing, when ServiceFleet.cpp is gone or no longer matches either shape.
$fleetDoorFile = Join-Path $ops 'Private\Model\ServiceFleet.cpp'
$fleetContainerWrite = '\b(?:Vehicles|SeededDepots|Catalogue|StarterFleet)\s*(?:\.|->)\s*(?:Add|AddUnique|AddDefaulted|Emplace|Insert|Append|Remove|RemoveAt|RemoveAll|RemoveSwap|Reset|Empty|FindOrAdd)\w*\s*\(|(?:\+\+\s*NextVehicleId\b|\bNextVehicleId\s*(?:\+\+|\+=))|(?<!(?:\w|(?<!-)>)\s{1,40})\b(?:Catalogue|StarterFleet)\s*(?:\[[^\]]*\])?\s*=(?!=)'
$fleetMoneyPost      = '\bELedgerCategory::Fleet\b'
$fleetPriceRead      = 'Specs?(?:[A-Z]\w*)?(?:\[[^\]]*\])?(?:\.|->)Price\b|\bSpecFor\([^)]*\)\.Price\b|(?:\.|->)ResaleValue\b|\bTypeFor\s*\([^)]*\)\s*\.\s*Price\b|\b(?:Get)?Catalogue\b[^;]*?(?:\.|->)\s*Price\b|\b(?:Kind|Type)\s*(?:\.|->)\s*Price\b'
$fleetDoorContainerWrites = 0
$fleetDoorMoneyPosts = 0
if (-not (Test-Path $fleetDoorFile)) {
    $failures.Add("fleet-one-door: $fleetDoorFile not found - update rule 43, do not let it check nothing")
} else {
    $doorInBlock = $false
    foreach ($line in (Get-Content -LiteralPath $fleetDoorFile)) {
        $code = Strip-ArchCode $line ([ref]$doorInBlock)
        if ($code -match $fleetContainerWrite) { $fleetDoorContainerWrites++ }
        if ($code -match $fleetMoneyPost) { $fleetDoorMoneyPosts++ }
    }
    if ($fleetDoorContainerWrites -eq 0 -or $fleetDoorMoneyPosts -eq 0) {
        $failures.Add("fleet-one-door: $fleetDoorFile shows $fleetDoorContainerWrites container write(s) and $fleetDoorMoneyPosts Fleet posting(s) - the rule's patterns no longer match the door; update rule 43")
    }
}
foreach ($fleetTree in @($ops, (Join-Path $Root 'Source\AirportMgr'))) {
    foreach ($file in Get-Sources $fleetTree @('.cpp', '.h')) {
        if ($file.Name -like '*Test.cpp' -or $file.Name -like '*Test.h' -or $file.Name -eq 'ServiceFleet.cpp') { continue }
        if ($file.FullName -match '[\\/]Testing[\\/]') { continue }
        $lines = Get-Content -LiteralPath $file.FullName
        $inBlock = $false
        for ($i = 0; $i -lt $lines.Count; $i++) {
            $code = Strip-ArchCode $lines[$i] ([ref]$inBlock)
            if ($code.Trim() -eq '') { continue }
            if ($code -match $fleetContainerWrite) {
                $failures.Add("fleet-one-door: $($file.Name):$($i + 1) writes the fleet's containers by hand - go through FServiceFleet (UJobBoard::Fleet), whose Add/Withdraw own the ledger line, the FFleetChangedEvent and the re-open: $($code.Trim())")
            }
            if ($file.Extension -eq '.cpp' -and $code -cmatch $fleetPriceRead) {
                $failures.Add("fleet-one-door: $($file.Name):$($i + 1) reads a vehicle row's Price or ResaleValue directly - ask FServiceFleet::PriceOf/ResaleOf, the one read the quote, the judgement, the charge and the ledger line share: $($code.Trim())")
            }
            if ($code -match $fleetMoneyPost -and $code -notmatch '^\s*case\s+ELedgerCategory::Fleet\s*:') {
                $failures.Add("fleet-one-door: $($file.Name):$($i + 1) names ELedgerCategory::Fleet outside ServiceFleet.cpp - fleet money is posted only by FServiceFleet::Add/Withdraw: $($code.Trim())")
            }
        }
    }
}
$ranRules.Add('fleet-one-door')

# --- 44. THE FUELLED TOLERANCE IS THE POLICY'S, TYPED ONCE ----------------------------------------
# Issue #443: the half-litre "fuelled" slack was typed five times in three files (a constant on the board, a literal twice
# in ServiceRolePolicy.cpp, a literal twice in ServiceBid.cpp). Changing one made the bid price a different number of trips
# than the truck makes, because the two files disagreed about when a job is done. It is IServiceRolePolicy::DoneWithin()
# now (FFuelRolePolicy::FuelledWithinLitres, the one number), and the two files that used to type it read it.
# A hit is a bare `0.5` number in either file, comments and string literals stripped. It cannot see 0.50, .5 or 1/2 - a
# GAP, said; the number that was typed was 0.5, and the policy header carries the reason it is one number.
# The rule fails, rather than checking nothing, when either file is gone.
foreach ($toleranceName in @('ServiceBid.cpp', 'ServiceRolePolicy.cpp')) {
    $toleranceFile = Join-Path $ops "Private\Model\$toleranceName"
    if (-not (Test-Path $toleranceFile)) {
        $failures.Add("fuelled-tolerance-once: $toleranceFile not found - update rule 44, do not let it check nothing")
        continue
    }
    $lines = Get-Content -LiteralPath $toleranceFile
    $inBlock = $false
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $code = Strip-ArchCode $lines[$i] ([ref]$inBlock)
        if ($code -match '(?<![\w.])0\.5(?!\d)') {
            $failures.Add("fuelled-tolerance-once: ${toleranceName}:$($i + 1) types the half-litre tolerance - read IServiceRolePolicy::DoneWithin(), the one figure the bid and the vehicle both use: $($code.Trim())")
        }
    }
}
$ranRules.Add('fuelled-tolerance-once')

# --- 45. WHAT A DEPOT'S MODULES GIVE IT IS READ THROUGH FDepotCapability ---------------------------
# Issue #443, ruled 2026-09-30 (#266): "the modules that count are only the ones that are placed". FEntityInstance::Modules
# is the OWNED list; the plot seats only min(owned, ceiling) of each kind, and the presenter DROPS the rest. What a depot's
# modules give it was decided in three places over the owned list with three rules - pumps and the plotless exemption in
# UJobBoard, bays in UFacilityPurchases with no exemption, the census in DepotKit - so a module the presenter could not
# seat still granted a bay and a pump the player could not see. Model/DepotCapability.h is the one view (Seat/Of); the job
# board and the purchase rules ask it, and this is what keeps them from walking the owned list for capability again.
# A HIT is a walk of a depot's module list (`Depot|Facility|Entity|Home|Instance ... .Modules`, comments and string literals
# stripped) in the board's files or the purchase service, outside the two places that mean OWNED on purpose:
# UFacilityPurchases::OwnedOf (the purchase ceiling compares owned to reserved) and ::DailyUpkeep (a module is paid for
# by owning it). A GAP, SAID: a reader that names its variable something else (`D.Modules`) is invisible to a regex.
# The rule fails, rather than checking nothing, when the view is no longer named in JobBoard.h and FacilityPurchases.cpp.
$capabilityDir = Join-Path $ops 'Private\Model'
$capabilityFiles = @('JobBoard.cpp', 'JobBoardBid.cpp', 'JobBoardDrive.cpp', 'ServiceFleet.cpp', 'FacilityPurchases.cpp')
# THE CENSUS TOO (#461 review): DepotKit::ReportIncomplete reads owned counts and the legacy test through the view, and
# is scanned with no allowance. AND THE PLOT PRESENTER since #266: it lights the bays it draws from FDepotCapability::Seat
# against its reservation, where it used to clamp the owned list run by run - a second implementation of "placed" that
# agreed with Seat only by arithmetic. Scanned with no allowance, and required to name the view.
$presenterPath = Join-Path $Root 'Plugins\Airside\Source\Airside\Private\Present\PlotPresenter.cpp'
$capabilityPaths = @($capabilityFiles | ForEach-Object { Join-Path $capabilityDir $_ }) + @((Join-Path $Root 'Plugins\Airside\Source\Airside\Private\Build\DepotKit.cpp'), $presenterPath)
$capabilityOwnedReaders = @('UFacilityPurchases::OwnedOf', 'UFacilityPurchases::DailyUpkeep')
$capabilityModulesRead = '\b(?:Depot|Facility|Entity|Home|Instance)\w*(?:\[[^\]]*\])?(?:\.|->)Modules\b'
foreach ($capabilityPath in $capabilityPaths) {
    $capabilityName = Split-Path -Leaf $capabilityPath
    if (-not (Test-Path $capabilityPath)) {
        $failures.Add("capability-from-the-view: $capabilityPath not found - update rule 45, do not let it check nothing")
        continue
    }
    $lines = Get-Content -LiteralPath $capabilityPath
    $inBlock = $false
    $current = ''
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $code = Strip-ArchCode $lines[$i] ([ref]$inBlock)
        if ($code.Trim() -eq '') { continue }
        $definition = Get-ArchDefinition $code
        if ($null -ne $definition) { $current = $definition }
        if ($code -match $capabilityModulesRead -and ($capabilityOwnedReaders -notcontains $current)) {
            $failures.Add("capability-from-the-view: ${capabilityName}:$($i + 1) walks a depot's OWNED module list (in '$current') - what its modules give it (bays, pumps) is FDepotCapability's, seated against the plot: $($code.Trim())")
        }
    }
}
foreach ($viewReader in @(@((Join-Path $ops 'Public\Model\JobBoard.h'), 'JobBoard.h'), @((Join-Path $ops 'Private\Model\FacilityPurchases.cpp'), 'FacilityPurchases.cpp'), @((Join-Path $Root 'Plugins\Airside\Source\Airside\Private\Build\DepotKit.cpp'), 'DepotKit.cpp'), @($presenterPath, 'PlotPresenter.cpp'))) {
    $viewPath = $viewReader[0]
    if ((Test-Path $viewPath) -and $null -eq (Select-String -Path $viewPath -Pattern 'FDepotCapability' -Quiet)) {
        $failures.Add("capability-from-the-view: $($viewReader[1]) no longer names FDepotCapability - it reads what a depot's modules give it some other way; update rule 45 or restore the view")
    }
}
$ranRules.Add('capability-from-the-view')

# --- 46. WHO THE RESOLVER MAY TURN IS ONE PREDICATE: FRoadAgent::IsReplannable ------------------
# Issue #455: "may this agent be sent along another route" was spelled `Phase == Taxiing` at three sites - ReplanAt's
# guard, FDeadlockResolver::CanReplanAtBlockedStep and the alert's filter - and only the first two were ever written
# that way. The third said "is an aircraft", so a cycle through a truck backing along a bay's leg (which cannot go
# round) dropped out of the alert for being made of vehicles. Now one predicate; the resolver's file must not spell
# the phase itself. Two askers of CanBeTurnedAtItsBlock (IsReplannable plus WHERE the agent is refused: the candidate
# test and the alert's filter - a second spelling would let them disagree, and a truck gate-refused at its bay's reverse
# leg that the resolver will not turn but the alert counted as a way out makes a ring of two of them wait for ever with
# no alert), one asker of IsReplannable in the resolver's file (the yield's candidate list) and ReplanAt's guard in the
# rebuild file must still ask. Comments and string literals are stripped first (rule 34's stripper), so a WHY comment
# can name the banned spelling.
# WHAT NO REGEX SEES: a `switch (Agent.Phase)` that names Taxiing and answers the same question by another route -
# pinned by Airside.Model.Traffic.IsOnRouteClassifiesEveryPhase (the replannable column) and
# Airside.Model.Traffic.Deadlock.CycleThroughAReversingTruckIsAnAlert / RingOfTwoGateRefusedTrucksIsAnAlert (the alert
# asks what the resolver asks).
$replannableFile = Join-Path $plugin 'Private\Model\GroundTrafficDeadlock.cpp'
$replannableRebuildFile = Join-Path $plugin 'Private\Model\GroundTrafficRebuild.cpp'
foreach ($path in @($replannableFile, $replannableRebuildFile)) {
    if (-not (Test-Path $path)) {
        $failures.Add("replannable-predicate: $path is named by rule 46 but does not exist - update the rule, do not let it check nothing")
    }
}
if ((Test-Path $replannableFile) -and (Test-Path $replannableRebuildFile)) {
    $asks = 0
    $turnAsks = 0
    $lines = Get-Content -LiteralPath $replannableFile
    $inBlock = $false
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $code = Strip-ArchCode $lines[$i] ([ref]$inBlock)
        if ($code -match 'Phase\s*[!=]=\s*EAgentPhase::Taxiing') {
            $failures.Add("replannable-predicate: $(Split-Path $replannableFile -Leaf):$($i + 1) spells the phase itself; ask FRoadAgent::IsReplannable, the one place that says who may be turned (#455): $($code.Trim())")
        }
        if ($code -match '\bIsReplannable\s*\(') { $asks++ }
        if ($code -match '\bCanBeTurnedAtItsBlock\s*\(') { $turnAsks++ }
    }
    if ($asks -lt 1) {
        $failures.Add("replannable-predicate: $(Split-Path $replannableFile -Leaf) no longer asks IsReplannable (the yield's candidate list) - it has its own copy of the rule again, or rule 46 is stale (#455)")
    }
    if ($turnAsks -lt 2) {
        $failures.Add("replannable-predicate: $(Split-Path $replannableFile -Leaf) asks CanBeTurnedAtItsBlock $turnAsks time(s), not the 2 it must (CanReplanAtBlockedStep and AlertCycles) - one has its own copy of the rule again, or rule 46 is stale (#455)")
    }
    $replanGuard = $false
    $lines = Get-Content -LiteralPath $replannableRebuildFile
    $inBlock = $false
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $code = Strip-ArchCode $lines[$i] ([ref]$inBlock)
        if ($code -match '\bIsReplannable\s*\(') { $replanGuard = $true }
    }
    if (-not $replanGuard) {
        $failures.Add("replannable-predicate: $(Split-Path $replannableRebuildFile -Leaf) no longer asks IsReplannable (ReplanAt's guard) - it has its own copy of the rule again, or rule 46 is stale (#455)")
    }
}
$ranRules.Add('replannable-predicate')

# --- 47. EVERY SESSION CALL PIE MAKES, THE EDITOR MODE MAKES -----------------------------------
# Issue #440, the fifth instance of one shape (#33, #185, #304, then the variant rows and the sticky verbs): the two
# drivers, ARoadBuildController (PIE) and URoadBuildEdMode (editor), are meant to reach every tool control, and a
# control added to one reached the other only if someone remembered. Rules 26 and 27 pinned it ONE FEATURE AT A TIME,
# so each new driver-facing surface was a fresh chance to miss the editor - FBuildSession::SelectActiveVariant had one
# production caller, the controller, for the whole life of the variant rows. This is the per-SEAM form: every
# FBuildSession method the controller calls as `Session.X(` must be called as `Session.X(`, `Sess().X(` or
# `GetSession().X(` in a production file under AirsideEditor\Private. A CALL, not a mention:
# `URoadBuildEdMode::CancelActiveGesture` is a method NAMED like the session's and calls something else.
# DEFAULT-DENY, NOT "NON-CONST ONLY" (#468's review): const is not a read on this class. Select(Kind, Id) const writes
# the mutable Selection, RecordPlaneHit const writes LastPlaneHitValue, MakeContext const moves the held grid frame -
# the first cut of this rule skipped all three. So every call counts, and a driver may skip one only through a table:
#   $sessionReads - pure reads the editor has no use for. Each must be declared const at class scope (every overload)
#     and still be called by the controller; a non-const or stale entry fails.
#   $editorSessionDoors - what the editor calls INSTEAD, and why; or, for a PIE-only control, the premise that makes it
#     PIE-only, as a pattern that must stay ABSENT from the editor. Checked both ways: the replacement must appear (or
#     the absent pattern must stay absent), and a row whose method the controller no longer calls is stale and fails.
# The class is parsed from BuildSession.h at brace depth 0, so a call inside an inline body is not read as a declaration.
# Rules 26 and 27 STAY (2026-09-30): 26 asserts GridOverlay::Describe, which is not a session call at all, in
# RoadBuildHUD.cpp, which is not the controller; 27 asserts the ARGUMENT (an FConfigToolPreferences store), which a
# call-name check cannot see - an editor session handed a different store would pass this rule and fail 27.
$sessionHeader = Join-Path $plugin 'Public\Tool\BuildSession.h'
$sessionController = Join-Path $Root 'Source\AirportMgr\RoadBuildController.cpp'
$sessionReads = @(
    @{ Method = 'ResolveSnap';                 Reason = "the HUD's snap readout (ARoadBuildController::ResolveSnap); the editor resolves snaps inside GetFrameContext" },
    @{ Method = 'MakeContextCallCountForTest'; Reason = "a test counter getter, forwarded for PlayerTickBuildsOneContext" }
)
$editorSessionDoors = @(
    @{
        Method  = 'ToggleGestureMode'
        Instead = '\bBuildVerbRegistry\s*\('
        Reason  = "the controller's own Session.ToggleGestureMode is its test door (ClickModifierTest); both drivers' production path is a BuildVerbRegistry() entry's Apply, which the editor reaches through URoadBuildEdMode::ApplyVerb"
    },
    @{
        Method  = 'CancelActiveGesture'
        Instead = '->\s*OnCancel\s*\('
        Reason  = "deliberate: each editor tool instance is pinned to one palette entry, so the editor's Escape ends the gesture (IBuildTool::OnCancel) and never returns to Select - see URoadBuildEditorTool::CancelGesture"
    },
    @{
        Method  = 'GetGestureMode'
        Instead = '\bBuildVerbRegistry\s*\('
        Reason  = "the editor reads the sticky mode through a BuildVerbRegistry() entry's IsActive (URoadBuildEdMode::IsVerbActive), which calls it - the lit palette toggle PIE's bar row is the twin of"
    },
    @{
        Method  = 'MakeContext'
        Instead = '(?:\bSession|\bSess\(\)|\bGetSession\(\))\s*\.\s*GetFrameContext\s*\('
        Reason  = "the editor builds every context through GetFrameContext, which runs MakeContext on a key miss; PIE's click path bypasses that cache on purpose (issue #303) - a real divergence, pinned here so it stays deliberate"
    },
    @{
        Method  = 'CancelStage'
        Absent  = '\bUiWindowHost\b|\bSettingsPanel\b|\bIsModalOpen\b'
        Reason  = "PIE-only: opening the Settings window drops a drag in flight (ARoadBuildController::DropGestureForModal) and abandons only the tool's stage; the editor mode has no modal window of its own - when AirsideEditor names a window host or a modal, it needs this call too (#448)"
    },
    @{
        Method  = 'Select'
        Absent  = '\bAlert\w*'
        Reason  = "PIE-only: the alert panel's Go (ARoadBuildController::SelectAndFocus) selects the alert's subject, and the editor mode has no alerts - when AirsideEditor names an alert, it needs this call too"
    },
    @{
        Method  = 'OnSelectionChanged'
        Absent  = '\bUInspectorWidget\b|\bUBuildHudLayer\b|\bInspectorWidget\b'
        Reason  = "PIE-only SUBSCRIPTION: the controller's handler (ARoadBuildController::OnSelectionChanged) forwards the change to the inspector through the HUD layer, and the editor mode has neither - but it FIRES the event all the same, because its tools write the selection through the contexts the session hands them (Airside.Editor.SelectionChangeIsAnnouncedLikePie). When AirsideEditor names an inspector or a HUD layer, it needs this subscription too (#446)"
    }
)
if (-not (Test-Path $sessionHeader) -or -not (Test-Path $sessionController)) {
    $failures.Add("session-api-both-drivers: $sessionHeader or $sessionController is missing - rule 47 names files that moved; update it, do not let it check nothing")
} else {
    # FBuildSession's body at brace depth 0 - declarations only; inline bodies and nested structs blanked.
    $inBlock = $false
    $headerLines = Get-Content -LiteralPath $sessionHeader
    $headerParts = New-Object System.Collections.Generic.List[string]
    for ($i = 0; $i -lt $headerLines.Count; $i++) { $headerParts.Add((Strip-ArchCode $headerLines[$i] ([ref]$inBlock))) }
    $headerCode = $headerParts -join "`n"
    $classOpen = [regex]::Match($headerCode, '\bclass\s+AIRSIDE_API\s+FBuildSession\b[^;{]*\{')
    $sessionDecls = ''
    if ($classOpen.Success) {
        $depth0 = New-Object System.Text.StringBuilder
        $depth = 1
        for ($p = $classOpen.Index + $classOpen.Length; $p -lt $headerCode.Length -and $depth -gt 0; $p++) {
            $ch = $headerCode[$p]
            if ($ch -eq '{') { $depth++; [void]$depth0.Append(' ') }
            elseif ($ch -eq '}') { $depth--; [void]$depth0.Append(' ') }
            elseif ($depth -eq 1) { [void]$depth0.Append($ch) }
            else { [void]$depth0.Append(' ') }
        }
        $sessionDecls = $depth0.ToString()
    }
    if ($sessionDecls -eq '') {
        $failures.Add("session-api-both-drivers: found no 'class AIRSIDE_API FBuildSession {' in $sessionHeader - it moved; update rule 47")
    }

    $controllerLines = Get-Content -LiteralPath $sessionController
    $controllerCalls = [ordered]@{}
    $inBlock = $false
    for ($i = 0; $i -lt $controllerLines.Count; $i++) {
        $code = Strip-ArchCode $controllerLines[$i] ([ref]$inBlock)
        foreach ($m in [regex]::Matches($code, '\bSession\s*\.\s*(\w+)\s*\(')) {
            if (-not $controllerCalls.Contains($m.Groups[1].Value)) { $controllerCalls[$m.Groups[1].Value] = $i + 1 }
        }
    }
    if ($controllerCalls.Count -eq 0) {
        $failures.Add("session-api-both-drivers: found no Session.X( call in $sessionController - the controller's session moved; rule 47 must not check nothing")
    }

    $editorCode = New-Object System.Text.StringBuilder
    foreach ($file in Get-Sources (Join-Path $editor 'Private') @('.cpp')) {
        if ($file.Name -like '*Test.cpp') { continue }
        $inBlock = $false
        foreach ($line in (Get-Content -LiteralPath $file.FullName)) { [void]$editorCode.AppendLine((Strip-ArchCode $line ([ref]$inBlock))) }
    }
    $editorText = $editorCode.ToString()

    foreach ($name in $controllerCalls.Keys) {
        $decls = [regex]::Matches($sessionDecls, "\b$name\s*\(")
        if ($decls.Count -eq 0) {
            if ($sessionDecls -ne '') {
                $failures.Add("session-api-both-drivers: RoadBuildController.cpp:$($controllerCalls[$name]) calls Session.$name but FBuildSession declares no $name at class scope - rule 47 cannot place it; update the rule")
            }
            continue
        }

        $read = $sessionReads | Where-Object { $_.Method -eq $name } | Select-Object -First 1
        if ($null -ne $read) {
            # A READ IS ALL-CONST: to the matching ')' of every declaration, then the next token must be `const`.
            foreach ($decl in $decls) {
                $parens = 0
                $p = $decl.Index + $decl.Length - 1
                for (; $p -lt $sessionDecls.Length; $p++) {
                    if ($sessionDecls[$p] -eq '(') { $parens++ }
                    elseif ($sessionDecls[$p] -eq ')') { $parens--; if ($parens -eq 0) { break } }
                }
                if ($sessionDecls.Substring([Math]::Min($p + 1, $sessionDecls.Length)) -notmatch '^\s*const\b') {
                    $failures.Add("session-api-both-drivers: rule 47 lists $name as a read ($($read.Reason)), but FBuildSession declares a non-const $name - a writer cannot be skipped as a read")
                }
            }
            continue
        }

        $door = $editorSessionDoors | Where-Object { $_.Method -eq $name } | Select-Object -First 1
        if ($null -ne $door) {
            if ($door.ContainsKey('Instead') -and $editorText -notmatch $door.Instead) {
                $failures.Add("session-api-both-drivers: rule 47's row for $name says the editor reaches it through /$($door.Instead)/ instead ($($door.Reason)), and nothing under AirsideEditor\Private matches that any more")
            }
            if ($door.ContainsKey('Absent') -and $editorText -match $door.Absent) {
                $failures.Add("session-api-both-drivers: rule 47's row for $name is PIE-only because /$($door.Absent)/ is absent from the editor ($($door.Reason)) - AirsideEditor\Private now matches it, so the premise is gone: call Session.$name there too, or re-justify the row")
            }
            continue
        }

        if ($editorText -notmatch "(?:\bSession|\bSess\(\)|\bGetSession\(\))\s*\.\s*$name\s*\(") {
            $failures.Add("session-api-both-drivers: RoadBuildController.cpp:$($controllerCalls[$name]) calls Session.$name, and no production file under AirsideEditor\Private calls it on its session - PIE can reach this and the editor mode cannot (#440). Give the editor its call, or add a row to rule 47 (a read, or what it calls instead) saying why")
        }
    }
    foreach ($row in @($sessionReads) + @($editorSessionDoors)) {
        if (-not $controllerCalls.Contains($row.Method)) {
            $failures.Add("session-api-both-drivers: rule 47 has a row for $($row.Method), which RoadBuildController.cpp no longer calls on its session - delete the row, do not let the table rot")
        }
    }
}
$ranRules.Add('session-api-both-drivers')

# --- 48. AN AGENT'S TRANSITION HAS ONE DOOR, AND OPS TESTS HEAR IT THROUGH A BUS ----------------------
# Issue #436, two halves of one shape - the phase event whose CAUSE was not delivered, and the fixture that hid how
# late it was delivered.
# (a) ONE BROADCAST. Rule 4's 'OnAgentPhaseChanged.Broadcast' row keeps the call out of every other production file;
#     this holds GroundTraffic.cpp to exactly ONE - the one inside UGroundTraffic::Announce, which every operation and
#     AdvanceOnce hand an FAgentTransition. A second call there is a second door, and the first thing a second door
#     drops is the Cause.
# (b) NO SYNCHRONOUS RELAY IN THE OPS TESTS. FFuelFixture::RelayPhases called UJobBoard::OnAgentPhase INSIDE the
#     traffic's broadcast, and 52 tests plus two more fixtures ran an ordering production never has: production
#     publishes into the ops bus and handles the event a drain later (UOpsRuntime::OnAgentPhase). An
#     OnAgentPhaseChanged.AddLambda under AirportOpsTests whose body calls ->OnAgentPhase( / .OnAgentPhase( is that
#     relay again. A test that exists to pin re-entrancy keeps ONE named synchronous relay - listed in
#     $syncRelayAllowed by the name of the function it sits in: FFuelFixture::RelayPhasesSynchronously, which
#     AirportOps.Fuel.Lifecycle.OwnRetirementsAreNotLosses needs to see the board unhook a vehicle BEFORE retiring its
#     agent - an order a drain-late delivery hides. (Airside.Model.Traffic.ReofferStandsRetireReentrancy is the Model/
#     side's pin, in AirsideTests, and calls RetireAgent rather than a board.)
# WHAT NO REGEX SEES: a relay that calls the board through a helper of its own name - the body is read for
# OnAgentPhase( only. Pinned from the other side by AirportOps.Present.Bus.StaleParkedOpensNoTurnaround and
# AirportOps.Model.Bus.SameFrameRedirectStaysTaxiIn, which fail if the board decides on anything but the event.
$announceFile = Join-Path $plugin 'Private\Model\GroundTraffic.cpp'
if (-not (Test-Path $announceFile)) {
    $failures.Add("agent-transition-one-door: $announceFile is named by rule 48 but does not exist - update the rule, do not let it check nothing")
}
else {
    $broadcasts = 0
    $announces = $false
    $lines = Get-Content -LiteralPath $announceFile
    $inBlock = $false
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $code = Strip-ArchCode $lines[$i] ([ref]$inBlock)
        if ($code -match '\bOnAgentPhaseChanged\.Broadcast\s*\(') { $broadcasts++ }
        if ($code -match '^void\s+UGroundTraffic::Announce\s*\(') { $announces = $true }
    }
    if ($broadcasts -ne 1) {
        $failures.Add("agent-transition-one-door: GroundTraffic.cpp broadcasts OnAgentPhaseChanged $broadcasts time(s), not the 1 inside UGroundTraffic::Announce - build an FAgentTransition and Announce it (#436)")
    }
    if (-not $announces) {
        $failures.Add("agent-transition-one-door: GroundTraffic.cpp no longer defines UGroundTraffic::Announce - rule 48 is stale or the one door moved (#436)")
    }
}
$syncRelayAllowed = @('RelayPhasesSynchronously')
foreach ($file in Get-Sources $opsTests @('.cpp', '.h')) {
    $lines = Get-Content -LiteralPath $file.FullName
    $inBlock = $false
    $codeLines = @()
    for ($i = 0; $i -lt $lines.Count; $i++) { $codeLines += ,(Strip-ArchCode $lines[$i] ([ref]$inBlock)) }
    for ($i = 0; $i -lt $codeLines.Count; $i++) {
        if ($codeLines[$i] -notmatch '\bOnAgentPhaseChanged\.AddLambda\s*\(') { continue }
        # THE LAMBDA'S BODY: from the AddLambda line until its parentheses close - the call's own `(` balanced.
        $depth = 0
        $body = ''
        for ($j = $i; $j -lt $codeLines.Count -and $j -lt $i + 40; $j++) {
            $body += $codeLines[$j] + "`n"
            $depth += ([regex]::Matches($codeLines[$j], '\(')).Count - ([regex]::Matches($codeLines[$j], '\)')).Count
            if ($depth -le 0 -and $j -gt $i) { break }
            if ($depth -le 0 -and $codeLines[$j] -match '\)\s*;') { break }
        }
        if ($body -notmatch '(->|\.)OnAgentPhase\s*\(') { continue }
        # WHICH FUNCTION it sits in: the nearest line above that opens one at the file's own indent.
        $owner = ''
        for ($k = $i; $k -ge 0; $k--) {
            if ($codeLines[$k] -match '^\s*(?:void|bool|int32|auto)\s+(?:\w+::)*(\w+)\s*\(') { $owner = $Matches[1]; break }
        }
        if ($syncRelayAllowed -contains $owner) { continue }
        $failures.Add("agent-transition-one-door: $($file.FullName):$($i + 1) relays OnAgentPhaseChanged to a board SYNCHRONOUSLY (in $owner) - publish into an FOpsEventBus and drain, as UOpsRuntime does (#436); a re-entrancy pin names its one helper in rule 48's `$syncRelayAllowed")
    }
}
$ranRules.Add('agent-transition-one-door')
# --- 49. THE INSPECTOR'S REFRESH STAYS SMALL (#441) --------------------------------------------
# UInspectorWidget::RefreshWith was 605 lines - five cards, the purchase rows and five hand-kept memo keys in one function -
# and every next card (service vehicle, road, #386's runway occupancy) had to be threaded through all of them. The shape that
# replaced it: a card per EInspectorCard returns a view, the widget picks one and paints it. THE SHAPE is what this holds, by
# its symptom: a RATCHET on the body's length, every line counted (a WHY comment included - a comment is not what grows a
# function back, and this must not be gameable by deleting one). 120 is the issue's own figure; the body was 53 lines on
# 2026-09-30 when this landed, so a card's branch slipping back into it fails long before 120 and the headroom is for a
# seam, not a card. Raise it only with a reason in the PR - a rule that only ever moves up is not a ratchet.
# DOES NOT SEE: the same branches moved into another function of the file. Rule 50 is the other half (the describes).
$inspectorWidgetFile = Join-Path $Root 'Source\AirportMgr\InspectorWidget.cpp'
$inspectorRefreshMax = 120
if (-not (Test-Path $inspectorWidgetFile)) {
    $failures.Add("inspector-refresh-length: $inspectorWidgetFile not found - update rule 49, do not let it check nothing")
} else {
    $inspectorLines = Get-Content -LiteralPath $inspectorWidgetFile
    $refreshStart = -1
    for ($i = 0; $i -lt $inspectorLines.Count; $i++) {
        if ($inspectorLines[$i] -match '^void UInspectorWidget::RefreshWith\(') { $refreshStart = $i; break }
    }
    $refreshOpen = -1
    $refreshClose = -1
    if ($refreshStart -ge 0) {
        # The body is the lines between the first column-0 '{' after the signature and the next column-0 '}'.
        for ($i = $refreshStart; $i -lt $inspectorLines.Count; $i++) {
            if ($refreshOpen -lt 0 -and $inspectorLines[$i] -eq '{') { $refreshOpen = $i; continue }
            if ($refreshOpen -ge 0 -and $inspectorLines[$i] -eq '}') { $refreshClose = $i; break }
        }
    }
    if ($refreshOpen -lt 0 -or $refreshClose -lt 0) {
        $failures.Add("inspector-refresh-length: could not find the body of UInspectorWidget::RefreshWith in $inspectorWidgetFile (a column-0 '{' and '}' after its signature) - it moved or was reshaped; update rule 49, do not let it check nothing")
    } else {
        $refreshBody = $refreshClose - $refreshOpen - 1
        if ($refreshBody -gt $inspectorRefreshMax) {
            $failures.Add("inspector-refresh-length: UInspectorWidget::RefreshWith is $refreshBody lines (max $inspectorRefreshMax) - a card's branch belongs in its IInspectorCard (InspectorCards.h), and the widget only paints the view it returns (#441)")
        }
    }
}
$ranRules.Add('inspector-refresh-length')

# --- 50. THE CARDS OWN THE DESCRIBES (#441) ------------------------------------------------------
# InspectFacts::DescribeAgent / DescribeRunway / DescribeTaxiway / DescribeStand were called from the widget's own Refresh,
# which is how its branch for each kind grew in there: a widget that describes is a widget that composes. They are the cards'
# now (InspectorAircraftCard.cpp, InspectorNetworkCards.cpp), each beside the change key built from what it reads, so no
# InspectFacts::Describe* call may appear in InspectorWidget.cpp - or in InspectorFacilityRows.cpp, which renders a quote and
# describes nothing. Comments and string literals are stripped first (rule 34's stripper), so a WHY comment can name them.
# THE QUALIFIED CALL IS NOT THE ONLY SPELLING: `using namespace InspectFacts;` (or a namespace alias of it) followed by an
# unqualified DescribeAgent( passes a check that only reads `InspectFacts::`, so the using-directive, the alias and the
# unqualified four names are banned in the same files. A member call (`Board->DescribeAgent(`, UJobBoard's own) is not these
# and is not matched: the unqualified pattern refuses a name preceded by `.`, `>`, `:` or a word character.
# And the half that stops it checking nothing: each card file must still make the calls it owns.
# DOES NOT SEE: a describe reached through a helper in another file. AirportMgr.Inspector.EveryKindHasACard is the runtime half.
$inspectorDescribeBanned = @(
    (Join-Path $Root 'Source\AirportMgr\InspectorWidget.cpp'),
    (Join-Path $Root 'Source\AirportMgr\InspectorFacilityRows.cpp')
)
$inspectorDescribeBans = @(
    @{ Pattern = '\bInspectFacts::Describe\w*\s*\('; What = 'calls InspectFacts::Describe*' },
    @{ Pattern = '\busing\s+namespace\s+InspectFacts\b'; What = 'opens namespace InspectFacts (an unqualified Describe* call would pass the qualified check)' },
    @{ Pattern = '\bnamespace\s+\w+\s*=\s*InspectFacts\b'; What = 'aliases namespace InspectFacts' },
    @{ Pattern = '(?<![\w:.>])Describe(Agent|Runway|Taxiway|Stand)\s*\('; What = 'calls an unqualified Describe(Agent|Runway|Taxiway|Stand)' }
)
$inspectorDescribeOwners = @(
    @{ Path = (Join-Path $Root 'Source\AirportMgr\InspectorAircraftCard.cpp'); Calls = @('DescribeAgent') },
    @{ Path = (Join-Path $Root 'Source\AirportMgr\InspectorNetworkCards.cpp'); Calls = @('DescribeRunway', 'DescribeTaxiway', 'DescribeStand') }
)
foreach ($path in $inspectorDescribeBanned) {
    if (-not (Test-Path $path)) {
        $failures.Add("cards-own-the-describes: $path is named by rule 50 but does not exist - update the rule, do not let it check nothing")
        continue
    }
    $lines = Get-Content -LiteralPath $path
    $inBlock = $false
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $code = Strip-ArchCode $lines[$i] ([ref]$inBlock)
        foreach ($ban in $inspectorDescribeBans) {
            if ($code -match $ban.Pattern) {
                $failures.Add("cards-own-the-describes: $(Split-Path $path -Leaf):$($i + 1) $($ban.What) - the cards own the describes (InspectorCards.h), and the widget paints the view one returns (#441): $($code.Trim())")
            }
        }
    }
}
foreach ($owner in $inspectorDescribeOwners) {
    if (-not (Test-Path $owner.Path)) {
        $failures.Add("cards-own-the-describes: $($owner.Path) is named by rule 50 but does not exist - update the rule, do not let it check nothing")
        continue
    }
    $seen = @{}
    $lines = Get-Content -LiteralPath $owner.Path
    $inBlock = $false
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $code = Strip-ArchCode $lines[$i] ([ref]$inBlock)
        foreach ($call in $owner.Calls) {
            if ($code -match ('\bInspectFacts::' + $call + '\s*\(')) { $seen[$call] = $true }
        }
    }
    foreach ($call in $owner.Calls) {
        if (-not $seen.ContainsKey($call)) {
            $failures.Add("cards-own-the-describes: $(Split-Path $owner.Path -Leaf) no longer calls InspectFacts::$call( - the describe moved again, or rule 50 is stale (#441)")
        }
    }
}
$ranRules.Add('cards-own-the-describes')

# --- 51. "THE NETWORK CHANGED" IS ANNOUNCED, NEVER POLLED -------------------------------------------
# Issue #446. UOpsRuntime::Tick compared the network pointer and GetGuidelineRevision() against a remembered pair every
# frame and published FNetworkChangedEvent when either moved - a frame late (SaveToSlot patched that with a status
# refresh of its own) and blind to a fact edit that re-derived no graph. ARoadNetworkActor::OnNetworkChanged now
# announces every rebuild, and UOpsRuntime::OnNetworkChanged bridges it onto the bus. The SHAPE a poll needs is a
# revision read beside the event it publishes, so:
# (a) ONE PUBLISHER: FNetworkChangedEvent is constructed ( `FNetworkChangedEvent{` / `FNetworkChangedEvent(` ) in
#     production code only inside UOpsRuntime::OnNetworkChanged. A second construction - back in Tick, or anywhere -
#     is a second door, and the first thing a second door is is a poll.
# (b) NO REVISION READ IN OpsRuntime.cpp BUT THE BRIDGE'S: GetGuidelineRevision( there is legal only inside
#     UOpsRuntime::OnNetworkChanged, which fills the event from the network it was handed. (GetEditRevision is the
#     depot-reservation memo's key in ReservedSlotsOf - a memo, not a change detector - and is not matched.)
# (c) THE BRIDGE EXISTS AND IS BOUND, so this rule cannot pass by the whole mechanism going missing.
# Comments and string literals are stripped first (rule 34's helpers); "which function" is rule 34's Get-ArchDefinition.
# WHAT NO REGEX SEES: a poll keyed on another clock (EditRevision, an entity count) that dirties passes without the
# event. Pinned from the other side by AirportOps.Present.Bus.NetworkChangedPublishedOnceWithNoTick, which fails if
# the event does not arrive with no Tick.
$bridgeFile = Join-Path $ops 'Private\Present\OpsRuntime.cpp'
if (-not (Test-Path $bridgeFile)) {
    $failures.Add("network-change-announced: $bridgeFile is named by rule 51 but does not exist - update the rule, do not let it check nothing")
}
else {
    $bridgeText = Get-Content -Raw -LiteralPath $bridgeFile
    if ($bridgeText -notmatch 'void\s+UOpsRuntime::OnNetworkChanged\s*\(') {
        $failures.Add("network-change-announced: UOpsRuntime::OnNetworkChanged not found in OpsRuntime.cpp - the bridge moved or went; update rule 51, do not let it check nothing")
    }
    if ($bridgeText -notmatch 'OnNetworkChanged\.AddUObject\s*\(\s*(?:this|&Runtime)\s*,\s*&UOpsRuntime::OnNetworkChanged\s*\)') {
        $failures.Add("network-change-announced: OpsRuntime.cpp never binds Target->OnNetworkChanged to UOpsRuntime::OnNetworkChanged - the bridge is unwired, and ops would hear no network change at all (#446)")
    }
    $lines = Get-Content -LiteralPath $bridgeFile
    $current = ''
    $inBlock = $false
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $code = Strip-ArchCode $lines[$i] ([ref]$inBlock)
        $def = Get-ArchDefinition $code
        if ($null -ne $def) { $current = $def }
        if ($code -match '\bGetGuidelineRevision\s*\(' -and $current -ne 'UOpsRuntime::OnNetworkChanged') {
            $failures.Add("network-change-announced: OpsRuntime.cpp:$($i + 1) reads GetGuidelineRevision( in $current - ops hears the network change from ARoadNetworkActor::OnNetworkChanged (UOpsRuntime::OnNetworkChanged), it does not compare revisions (#446)")
        }
    }
}
foreach ($announceTree in @($ops, (Join-Path $Root 'Source\AirportMgr'), $plugin)) {
    foreach ($file in Get-Sources $announceTree @('.cpp', '.h')) {
        if ($file.Name -like '*Test.cpp' -or $file.FullName -match '[\\/]Testing[\\/]') { continue }
        $lines = Get-Content -LiteralPath $file.FullName
        $current = ''
        $inBlock = $false
        for ($i = 0; $i -lt $lines.Count; $i++) {
            $code = Strip-ArchCode $lines[$i] ([ref]$inBlock)
            $def = Get-ArchDefinition $code
            if ($null -ne $def) { $current = $def }
            if ($code -match '^\s*struct\b') { continue }
            if ($code -match '\bFNetworkChangedEvent\s*[\{\(]' -and $current -ne 'UOpsRuntime::OnNetworkChanged') {
                $failures.Add("network-change-announced: $($file.Name):$($i + 1) constructs FNetworkChangedEvent in $current - its one publisher is UOpsRuntime::OnNetworkChanged, the bridge from the actor's announcement (#446)")
            }
        }
    }
}
$ranRules.Add('network-change-announced')

# --- 52. ONE ANSWER TO "WHICH AIRPORT" ------------------------------------------------------------
# Issue #446. Four lookups found the world's ARoadNetworkActor with two rules: ARoadNetworkActor::Find and ops'
# catch-up scan took the FIRST TActorIterator hit, ops also took whichever SPAWNED first (an OnActorSpawned hook) and
# re-checked IsValid every tick, and the buildings actor refused to guess between two. With two network actors the
# driver built into one, ops ran the other, and the depots vanished. URoadNetworkRegistry (a UWorldSubsystem the actor
# registers with) is the one answer now, and refuses a second actor with an Error. So in production code (the test
# modules and *Test.cpp are exempt: a test may count actors on purpose):
# (a) no TActorIterator/TActorRange/TObjectIterator/TObjectRange<ARoadNetworkActor>, and no UGameplayStatics
#     GetActorOfClass / GetAllActorsOfClass(WithTag) naming ARoadNetworkActor, anywhere - ARoadNetworkActor::Find
#     forwards to the registry (the object and gameplay-statics forms added in #446's review: each is a scan too);
# (b) no AddOnActorSpawnedHandler under AirportOps - the spawn hook ops used to find the airport by;
# (c) the registry exists and its refusal is an Error, so the rule cannot pass with the one answer gone.
$registryFile = Join-Path $plugin 'Private\Present\RoadNetworkRegistry.cpp'
if (-not (Test-Path $registryFile)) {
    $failures.Add("one-airport-lookup: $registryFile is named by rule 52 but does not exist - update the rule, do not let it check nothing")
}
elseif ((Get-Content -Raw -LiteralPath $registryFile) -notmatch 'UE_LOG\s*\(\s*LogAirside\s*,\s*Error') {
    $failures.Add("one-airport-lookup: RoadNetworkRegistry.cpp no longer logs an Error when it refuses a second airport - the refusal must be loud (#446)")
}
foreach ($lookupTree in @($plugin, $editor, $ops, (Join-Path $Root 'Source\AirportMgr'))) {
    foreach ($file in Get-Sources $lookupTree @('.cpp', '.h')) {
        if ($file.Name -like '*Test.cpp' -or $file.FullName -match '[\\/]Testing[\\/]') { continue }
        $lines = Get-Content -LiteralPath $file.FullName
        $inBlock = $false
        for ($i = 0; $i -lt $lines.Count; $i++) {
            $code = Strip-ArchCode $lines[$i] ([ref]$inBlock)
            if ($code -match '\bT(?:Actor|Object)(?:Iterator|Range)\s*<\s*ARoadNetworkActor\s*>' -or $code -match '\bGet(?:All)?Actors?OfClass\w*\s*\([^;]*ARoadNetworkActor') {
                $failures.Add("one-airport-lookup: $($file.Name):$($i + 1) searches the world for an ARoadNetworkActor - ask URoadNetworkRegistry (ARoadNetworkActor::Find), the one answer, which refuses a second airport loudly (#446)")
            }
            if ($file.FullName.StartsWith($ops) -and $code -match '\bAddOnActorSpawnedHandler\s*\(') {
                $failures.Add("one-airport-lookup: $($file.Name):$($i + 1) hooks OnActorSpawned in AirportOps - ops hears the world's airport from URoadNetworkRegistry::OnAirportChanged (#446)")
            }
        }
    }
}
$ranRules.Add('one-airport-lookup')

# --- 53. OPS AND THE GAME MODULE DO NOT CHANGE A ROUTE FROM THE AGENT'S INTERNALS ----------------------
# Issue #429. Airside had no "change this agent's route" seam, so AirportOps (UJobBoard::DriveVehicleTo) computed
# splice points from Follower.Plan / Follower.Travelled, picked RerouteAgent or a rescue by phase and seeded a tow by
# hand, and the game module's rig yard copied that seed character for character. JobBoard.h states the boundary -
# "Airside knows how a thing MOVES and must never learn what it is FOR" - and these tokens are where it was crossed:
# a tow seed built (FTowSeed, TowSeed.Emplace), a splice composed (RerouteAgent, Follower.Plan / Follower.Travelled),
# and in ops the deadlock resolver's replan called directly (ReplanAt - the Unstick copying the resolver's ban).
# Comments and string literals are stripped first (rule 34's stripper), so a WHY comment may name them.
#
# PART 2 (#429) EMPTIED OPS: UGroundTraffic::SendAgentTo chooses how a vehicle turns, finishes or is rescued;
# ReplanAroundBlocker / ReplanFromNextNode are the Unstick's replans (the resolver's own step and bound);
# ReofferStand its stand (a stranded one's from where it stands); RemainingDriveSeconds the bid's ETA. So in AirportOps it also bans what
# computing a splice point or a ban needs - the plan-geometry statics (UGroundTraffic::CurrentStep / StepFromNode /
# StepStart) and the refusal an agent is held by (GetBlockedStep / GetBlockedResource): with those, the Unstick
# could copy the resolver's ban again, which is how its copy lost the resolver's upper bound.
#
# THE ALLOW-LIST IS PER FILE AND PER TOKEN, and every entry is a debt with its payer named:
# - RigTestCourseTest.cpp reads Follower.Travelled for its speed-limit probe until #301 moves the course's checks
#   onto Airside's own accessors.
# A LISTED TOKEN THAT NO LONGER APPEARS FAILS TOO: an allow-list entry that allows nothing is a pin loosened for
# free, and the next writer in that file would be waved through by it. Delete the entry with the code.
$routeInternals = @(
    @{ Token = 'FTowSeed';           Pattern = '\bFTowSeed\b';            OpsOnly = $false },
    @{ Token = 'TowSeed.Emplace';    Pattern = '\bTowSeed\.Emplace\s*\('; OpsOnly = $false },
    @{ Token = 'RerouteAgent(';      Pattern = '\bRerouteAgent\s*\(';     OpsOnly = $false },
    @{ Token = 'Follower.Travelled'; Pattern = '\bFollower\.Travelled\b'; OpsOnly = $false },
    @{ Token = 'Follower.Plan';      Pattern = '\bFollower\.Plan\b';      OpsOnly = $false },
    @{ Token = 'ReplanAt(';          Pattern = '\bReplanAt\s*\(';         OpsOnly = $true },
    @{ Token = 'CurrentStep(';       Pattern = '\bCurrentStep\s*\(';      OpsOnly = $true },
    @{ Token = 'StepFromNode(';      Pattern = '\bStepFromNode\s*\(';     OpsOnly = $true },
    @{ Token = 'StepStart(';         Pattern = '\bStepStart\s*\(';        OpsOnly = $true },
    @{ Token = 'GetBlockedStep(';    Pattern = '\bGetBlockedStep\s*\(';   OpsOnly = $true },
    @{ Token = 'GetBlockedResource('; Pattern = '\bGetBlockedResource\s*\('; OpsOnly = $true }
)
$routeInternalsAllowed = @{
    'Source\AirportMgr\RigTestCourseTest.cpp'                         = @('Follower.Travelled')
}
$routeInternalsSeen = @{}
$gameModule = Join-Path $Root 'Source\AirportMgr'
foreach ($dir in @($ops, $gameModule)) {
    if (-not (Test-Path $dir)) {
        $failures.Add("route-internals: $dir is named by rule 53 but does not exist - update the rule, do not let it check nothing")
        continue
    }
    foreach ($file in Get-Sources $dir @('.h', '.cpp')) {
        $allowedKey = $routeInternalsAllowed.Keys | Where-Object { Test-AllowedPathSuffix $file $_ } | Select-Object -First 1
        $allowed = if ($allowedKey) { $routeInternalsAllowed[$allowedKey] } else { @() }
        $lines = Get-Content -LiteralPath $file.FullName
        $inBlock = $false
        for ($i = 0; $i -lt $lines.Count; $i++) {
            $code = Strip-ArchCode $lines[$i] ([ref]$inBlock)
            foreach ($t in $routeInternals) {
                if ($t.OpsOnly -and $dir -ne $ops) { continue }
                if ($code -notmatch $t.Pattern) { continue }
                if ($allowed -contains $t.Token) {
                    $routeInternalsSeen["$allowedKey|$($t.Token)"] = $true
                    continue
                }
                $failures.Add("route-internals: $($file.FullName):$($i + 1) names $($t.Token) - Airside changes a route and seeds a tow (UGroundTraffic's operations, FRoadAgent::LiveTowSeed); ops and the game module say where to, not how (#429): $($code.Trim())")
            }
        }
    }
}
foreach ($key in $routeInternalsAllowed.Keys) {
    foreach ($token in $routeInternalsAllowed[$key]) {
        if (-not $routeInternalsSeen.ContainsKey("$key|$token")) {
            $failures.Add("route-internals: rule 53 allows $token in $key, which no longer names it - delete the entry with the code, or the allow-list waves the next writer through (#429)")
        }
    }
}
$ranRules.Add('route-internals')

# --- 54. THE CONTROLLER'S PUBLIC SURFACE IS A CLOSED LIST, AND IT INCLUDES NO AIRPORTOPS MODEL (#448) ---------
# ARoadBuildController was split by #94 into input, session and target, plus forwarding - and grew back: 398/815 lines at the split,
# 952/1662 by 2026-09-30, about 105 public methods, four of AirportOps' Model/ headers' types in its signatures. Every inspector and ops
# feature since (#387, #408, #412, #413, #417) added methods here, because FBuildActionContext gave a verb no selection and no argument and so
# forced each selection verb back through a forwarder on the one object that held the session. The context carries both now, the HUD layer
# owns the window toggle, and the runtime has one resolver - and THIS is what holds the shape, by its symptom (the rule 20 shape, for the
# controller instead of the actor): (a) the header includes no AirportOps Model/ header - the controller is game-framework glue over
# Airside's model, and the ops layer is reached through the context's Runtime (OpsRuntimeResolver); (b) the public method names are exactly the
# allow-list below. A NEW public method fails: a verb on a selection binds in BuildActions.cpp, a window toggles through UBuildHudLayer, a read
# goes to the thing that owns it. A LISTED name the header no longer declares ALSO fails: the list is a ratchet and is meant to SHRINK, not to
# be edited upward to match whatever the header declares today - which is the regression #94 and this issue were.
# DOES NOT SEE: a method added to a SUBOBJECT the controller owns (the camera, the HUD layer) - those have their own headers. A public name
# declared through a macro. A method reached through `friend`. Declarations only are read: bodies are blanked at brace depth 1.
$controllerHeader = Join-Path $Root 'Source\AirportMgr\RoadBuildController.h'
$controllerPublicAllowList = @(
    'ApplySnapToggle',
    'ApplyVerb',
    'ARoadBuildController',
    'CanRedo',
    'CanUndo',
    'CollectToolReadoutForTest',
    'CursorOnRoadPlane',
    'DepotForSelection',
    'EndPlayForTest',
    'GetActiveTool',
    'GetActiveToolIndex',
    'GetActiveVariantAxes',
    'GetFrameContext',
    'GetGestureMode',
    'GetHud',
    'GetLastLandRequestForTest',
    'GetSelection',
    'GetSession',
    'GetTarget',
    'GetToolReadout',
    'GetToolReadoutRevision',
    'GetViewFocus',
    'HasAgent',
    'HasNetworkContent',
    'HasRunway',
    'HasRunwayRecomputeCountForTest',
    'IsGuidelineOverlayOn',
    'IsModalOpen',
    'IsPaused',
    'IsPrimaryPressedForTest',
    'IsSettingsShowing',
    'IsWatchingAgent',
    'KeyWaitsForModal',
    'LandAircraftNearViewFocus',
    'MakeContextCallCountForTest',
    'MakeToolContext',
    'MakeVariantContext',
    'MovePrimaryForTest',
    'OnActionKeyForTest',
    'OnBuild',
    'OnClearNetwork',
    'OnRedo',
    'OnToggleGuidelines',
    'OnUndo',
    'PlayerTickForTest',
    'PressPrimaryForTest',
    'QuickLoad',
    'QuickSave',
    'ResolveSnap',
    'RetireFrameCachesForTest',
    'RevealedDepotFor',
    'SelectActiveVariant',
    'SelectAndFocus',
    'SelectedAgentFactsThisFrame',
    'SelectedRunwayFactsThisFrame',
    'SelectForTest',
    'SelectTool',
    'SetBuildingsForTest',
    'SetTargetForTest',
    'StepSpeed',
    'ToggleGestureMode',
    'TogglePause',
    'ToggleSettings',
    'ToggleWatchAgent',
    'WantsRoadNodesDrawn',
    'ZoomIn',
    'ZoomOut'
)
if (-not (Test-Path $controllerHeader) -or -not (Test-Path (Join-Path $ops 'Public\Model'))) {
    $failures.Add("controller-stays-thin: $controllerHeader or AirportOps' Public\Model is missing - rule 54 names paths that moved; update it, do not let it check nothing")
} else {
    $opsModelHeaders = @(Get-ChildItem -Path (Join-Path $ops 'Public\Model') -Filter '*.h' -File | ForEach-Object { $_.Name })
    $controllerLines = Get-Content -LiteralPath $controllerHeader
    $inBlock = $false
    $controllerParts = New-Object System.Collections.Generic.List[string]
    for ($i = 0; $i -lt $controllerLines.Count; $i++) {
        $code = Strip-ArchCode $controllerLines[$i] ([ref]$inBlock)
        $controllerParts.Add($code)
    }
    for ($i = 0; $i -lt $controllerLines.Count; $i++) {
        if ($controllerLines[$i] -match '^\s*#\s*include\s+"Model/([\w/]+\.h)"') {
            $included = ($Matches[1] -split '/')[-1]
            if ($opsModelHeaders -contains $included) {
                $failures.Add("controller-stays-thin: RoadBuildController.h:$($i + 1) includes AirportOps' Model/$included - the controller is glue over Airside's model; reach the ops layer through FBuildActionContext::Runtime (OpsRuntimeResolver), and keep a verb's types in BuildActions.cpp (#448)")
            }
        }
    }
    $controllerCode = $controllerParts -join "`n"
    $controllerOpen = [regex]::Match($controllerCode, '\bclass\s+AIRPORTMGR_API\s+ARoadBuildController\b[^;{]*\{')
    if (-not $controllerOpen.Success) {
        $failures.Add("controller-stays-thin: found no 'class AIRPORTMGR_API ARoadBuildController {' in $controllerHeader - it moved; update rule 54")
    } else {
        $flat = New-Object System.Text.StringBuilder
        $depth = 1
        $controllerBody = $controllerCode.Substring($controllerOpen.Index + $controllerOpen.Length)
        for ($p = 0; $p -lt $controllerBody.Length -and $depth -gt 0; $p++) {
            $ch = $controllerBody[$p]
            if ($ch -eq '{') { $depth++; [void]$flat.Append(' ') }
            elseif ($ch -eq '}') { $depth--; [void]$flat.Append(' ') }
            elseif ($depth -eq 1) { [void]$flat.Append($ch) }
            else { [void]$flat.Append(' ') }
        }
        $publicNames = New-Object System.Collections.Generic.SortedSet[string]
        $segments = [regex]::Split($flat.ToString(), '\b(public|protected|private)\s*:')
        for ($s = 1; $s -lt $segments.Count; $s += 2) {
            if ($segments[$s] -ne 'public') { continue }
            foreach ($m in [regex]::Matches($segments[$s + 1], '(?<![\w:.>])(~?[A-Za-z_]\w*)\s*\(')) {
                $name = $m.Groups[1].Value
                if (@('UPROPERTY', 'UFUNCTION', 'UCLASS', 'USTRUCT', 'GENERATED_BODY', 'meta', 'static_assert', 'TEXT') -contains $name) { continue }
                [void]$publicNames.Add($name)
            }
        }
        if ($publicNames.Count -eq 0) {
            $failures.Add("controller-stays-thin: read no public method from $controllerHeader - the parse stopped seeing the class; rule 54 must not check nothing")
        }
        foreach ($name in $publicNames) {
            if ($controllerPublicAllowList -notcontains $name) {
                $failures.Add("controller-stays-thin: RoadBuildController.h declares a public $name( that is not in rule 54's allow-list (#448) - a selection verb binds in BuildActions.cpp (FBuildActionContext::Selection, ::Arg), a window toggles through UBuildHudLayer::ToggleWindow, the runtime comes from OpsRuntimeResolver. Growing the list to fit is the regression this rule exists to stop")
            }
        }
        foreach ($name in $controllerPublicAllowList) {
            if (-not $publicNames.Contains($name)) {
                $failures.Add("controller-stays-thin: rule 54 lists $name( but RoadBuildController.h no longer declares it public - delete the entry, so the list ratchets down with the header")
            }
        }
    }
}
$ranRules.Add('controller-stays-thin')

# --- 55. THE OPS RUNTIME HAS ONE DOOR IN THE GAME MODULE (#448) -----------------------------------------------
# UOpsRuntimeSubsystem::Get( was called from nine widgets, the action context and the controller, beside a controller-held test override, a bar
# runtime of its own and five bypasses of the override - so a headless test that handed one place a runtime left the rest with none. It is called
# from OpsRuntimeResolver.cpp alone now; everything else asks OpsRuntimeResolver::Resolve (or a panel's OpsRuntime(), which does). Comments and
# string literals are stripped (rule 34's stripper), so a WHY comment can name the subsystem. And the half that stops it checking nothing: the
# resolver must still make the call.
# DOES NOT SEE: the subsystem reached by another spelling (GetGameInstance()->GetSubsystem<UOpsRuntimeSubsystem>()) - a call that does not name
# `UOpsRuntimeSubsystem::Get(`. AirportMgr.Actions.OneResolverForTheOpsRuntime is the runtime half (an override reaches the context and the bar).
$resolverFile = Join-Path $Root 'Source\AirportMgr\OpsRuntimeResolver.cpp'
if (-not (Test-Path $resolverFile)) {
    $failures.Add("ops-runtime-one-door: $resolverFile is named by rule 55 but does not exist - update the rule, do not let it check nothing")
} else {
    $resolverCalls = 0
    foreach ($file in Get-Sources (Join-Path $Root 'Source\AirportMgr') @('.cpp', '.h')) {
        $isResolver = $file.FullName -eq $resolverFile
        $lines = Get-Content -LiteralPath $file.FullName
        $inBlock = $false
        for ($i = 0; $i -lt $lines.Count; $i++) {
            $code = Strip-ArchCode $lines[$i] ([ref]$inBlock)
            if ($code -match '\bUOpsRuntimeSubsystem\s*::\s*Get\s*\(') {
                if ($isResolver) { $resolverCalls++ }
                else { $failures.Add("ops-runtime-one-door: $($file.Name):$($i + 1) calls UOpsRuntimeSubsystem::Get( - the game module asks OpsRuntimeResolver::Resolve (a panel: OpsRuntime()), so one answer reaches the context, the bar, the inspector and a test's stand-in (#448): $($code.Trim())") }
            }
        }
    }
    if ($resolverCalls -lt 1) {
        $failures.Add("ops-runtime-one-door: OpsRuntimeResolver.cpp no longer calls UOpsRuntimeSubsystem::Get( - the door moved, or rule 55 is stale (#448)")
    }
}
$ranRules.Add('ops-runtime-one-door')

# --- 56. THE CONTROLLER READS THE HELD KEYS ONCE (#448) -------------------------------------------------------
# MakeToolContext and PlayerTick's frame context each typed the same five-line block of positional bools - Ctrl, Shift, Alt, the hover pick -
# a thousand lines apart, and inserting Alt ahead of the hover agent once turned the agent id into a bool there. ReadInputState reads them into
# the session's FBuildInputState, once, and both callers take it. THE SYMPTOM HELD: the Shift read appears exactly once in the file - a second
# copy of the block would put a second one beside it. Comments and strings are stripped (rule 34's stripper). And exactly ONE, not "at most":
# zero means the read moved and this rule checks nothing.
# DOES NOT SEE: a held key read through another call (IsInputKeyDown(FKey(...)), WasInputKeyJustPressed) - the pin is the shape the bug had.
$inputReadFile = Join-Path $Root 'Source\AirportMgr\RoadBuildController.cpp'
if (-not (Test-Path $inputReadFile)) {
    $failures.Add("input-read-once: $inputReadFile is named by rule 56 but does not exist - update the rule, do not let it check nothing")
} else {
    $shiftReads = 0
    $inputLines = Get-Content -LiteralPath $inputReadFile
    $inBlock = $false
    for ($i = 0; $i -lt $inputLines.Count; $i++) {
        $code = Strip-ArchCode $inputLines[$i] ([ref]$inBlock)
        $shiftReads += ([regex]::Matches($code, 'IsInputKeyDown\s*\(\s*EKeys::LeftShift\s*\)')).Count
    }
    if ($shiftReads -ne 1) {
        $failures.Add("input-read-once: RoadBuildController.cpp reads IsInputKeyDown(EKeys::LeftShift) $shiftReads time(s), not once - the held keys are read in ReadInputState and handed on as an FBuildInputState, never retyped at a second site (#448)")
    }
}
$ranRules.Add('input-read-once')

# --- 60. THE STARTER FLEET IS SEEDED BY THE ANNOUNCEMENT'S PASS, NOT DISCOVERED BY A POLL -------------
# Issue #443. A depot's starter fleet was seeded by the first loop of UJobBoard::SyncFleet, which every job board Step ran:
# each pass walked every entity to find a depot it had not seen, although placement is announced (#446's
# FNetworkChangedEvent). It is UOpsRuntime's "FleetSeed" pass now, woken by that announcement and by the attach's and a
# load's MarkAllDirty, and registered before the "JobBoard" pass. The SHAPE a poll needs is a seeding call from the job
# board's own pass, so in production code (*Test.cpp and Testing/ exempt - a fixture seeds by hand, and UJobBoard::TickForTest
# is the fixtures' mirror of the two passes, held to tests by rule 4's 'UJobBoard::TickForTest' row):
# (a) `SeedStarterFleets(` is CALLED only in UJobBoard::SeedStarterFleets (the forwarder, JobBoard.cpp), UJobBoard::TickForTest
#     (JobBoard.h, the mirror), and OpsRuntime.cpp. Its definitions and declarations are exempt. A call from Step, SyncFleet,
#     OnAgentPhase or anywhere else is the poll coming back;
# (b) IN OpsRuntime.cpp the call is legal in exactly ONE place: the body of the "FleetSeed" RegisterPass (from its `(` to
#     the matching `)`). The same file must register that pass BEFORE the "JobBoard" one (a drain seeds, then bids), and
#     subscribe FNetworkChangedEvent to it - so the rule cannot pass by the wiring going missing. A call inside the
#     "JobBoard" pass's lambda, or in any handler, is in WireBus and so passes (a), but is the exact shape #443 removed
#     (the job board's own pass seeding): (b) reads the pass BODIES, and fails it.
# Comments are stripped first, with the pass names KEPT (Strip-ArchComments: Strip-ArchCode blanks string literals, and (b)
# matches TEXT("FleetSeed")) - so a commented-out call or subscription does not count. (a)'s "which function" is rule 34's
# Get-ArchDefinition, on Strip-ArchCode text.
# WHAT NO REGEX SEES: a second seeding loop written by hand (Fleet().Add(..., Seeded) over the depots) - rule 43 keeps the
# containers to the fleet's door and this rule keeps the ONE loop's caller, but a new loop through the public Add is not a
# container write. Pinned from the other side by AirportOps.Present.Fleet.PlacedDepotIsSeededByTheAnnouncement, which goes
# red when the pass is not woken by the announcement.
# COMMENTS STRIPPED, STRING LITERALS KEPT: rule 60(b) matches pass names, which are strings - Strip-ArchComments, with the helpers at the
# top of this file since #446's review (rule 4 needs it too).
# THE TEXT OF ONE CALL: from the `(` at OpenIndex to its matching `)` (string-aware, so a paren in a literal is not counted).
# Empty when the parens never balance.
function Get-ArchCallSpan([string] $Text, [int] $OpenIndex) {
    $depth = 0
    $inString = $false
    for ($i = $OpenIndex; $i -lt $Text.Length; $i++) {
        $c = $Text[$i]
        if ($inString) {
            if ($c -eq '\') { $i++ } elseif ($c -eq '"') { $inString = $false }
            continue
        }
        if ($c -eq '"') { $inString = $true }
        elseif ($c -eq '(') { $depth++ }
        elseif ($c -eq ')') { $depth--; if ($depth -eq 0) { return $Text.Substring($OpenIndex, $i - $OpenIndex + 1) } }
    }
    return ''
}
$seedAllowed = @{
    'JobBoard.cpp'   = @('UJobBoard::SeedStarterFleets')
    'JobBoard.h'     = @('TickForTest')
    'OpsRuntime.cpp' = @('UOpsRuntime::WireBus')
}
foreach ($seedTree in @($ops, (Join-Path $Root 'Source\AirportMgr'))) {
    foreach ($file in Get-Sources $seedTree @('.cpp', '.h')) {
        if ($file.Name -like '*Test.cpp' -or $file.Name -like '*Test.h' -or $file.FullName -match '[\\/]Testing[\\/]') { continue }
        $lines = Get-Content -LiteralPath $file.FullName
        $current = ''
        $inBlock = $false
        for ($i = 0; $i -lt $lines.Count; $i++) {
            $code = Strip-ArchCode $lines[$i] ([ref]$inBlock)
            $def = Get-ArchDefinition $code
            if ($null -ne $def) { $current = $def }
            if ($code -notmatch '\bSeedStarterFleets\s*\(') { continue }
            # A DEFINITION OR DECLARATION, not a call: `int32 X::SeedStarterFleets(` / `int32 SeedStarterFleets(`.
            if ($code -match '^\s*(?:(?:virtual|static|inline)\s+)*(?:int32|void|bool)\s+(?:\w+::)?SeedStarterFleets\s*\(') { continue }
            $legal = $seedAllowed.ContainsKey($file.Name) -and ($seedAllowed[$file.Name] -contains $current)
            if (-not $legal) {
                $failures.Add("starter-fleet-seeded-on-announcement: $($file.Name):$($i + 1) calls SeedStarterFleets in $current - the starter fleet is seeded by UOpsRuntime's `"FleetSeed`" pass (woken by FNetworkChangedEvent and MarkAllDirty), not found by a poll in the job board's own pass (#443): $($code.Trim())")
            }
        }
    }
}
$seedWiringFile = Join-Path $ops 'Private\Present\OpsRuntime.cpp'
if (-not (Test-Path $seedWiringFile)) {
    $failures.Add("starter-fleet-seeded-on-announcement: $seedWiringFile is named by rule 60 but does not exist - update the rule, do not let it check nothing")
}
else {
    $seedStripped = New-Object System.Collections.Generic.List[string]
    $seedBlock = $false
    foreach ($seedLine in (Get-Content -LiteralPath $seedWiringFile)) { $seedStripped.Add((Strip-ArchComments $seedLine ([ref]$seedBlock))) }
    $seedText = $seedStripped -join "`n"
    $seedPasses = [regex]::Matches($seedText, 'RegisterPass\s*\(\s*TEXT\s*\(\s*"([^"]+)"\s*\)')
    $seedPass = $null
    $boardPass = $null
    foreach ($passMatch in $seedPasses) {
        if ($passMatch.Groups[1].Value -eq 'FleetSeed') { $seedPass = $passMatch }
        if ($passMatch.Groups[1].Value -eq 'JobBoard') { $boardPass = $passMatch }
    }
    if ($null -eq $seedPass) {
        $failures.Add("starter-fleet-seeded-on-announcement: OpsRuntime.cpp registers no `"FleetSeed`" pass - a starter depot would never be seeded (#443)")
    }
    else {
        if ($null -eq $boardPass -or $seedPass.Index -gt $boardPass.Index) {
            $failures.Add("starter-fleet-seeded-on-announcement: the `"FleetSeed`" pass must be registered BEFORE the `"JobBoard`" pass - a drain seeds, then bids (#443)")
        }
        # THE PASS BODY: RegisterPass( ... ) balanced, so the lambda and nothing after it. Every call in the file must be in it.
        $passOpen = $seedText.IndexOf('(', $seedPass.Index)
        $passBody = Get-ArchCallSpan $seedText $passOpen
        if ($passBody -eq '') {
            $failures.Add("starter-fleet-seeded-on-announcement: OpsRuntime.cpp's `"FleetSeed`" RegisterPass call has no balanced closing paren - the rule cannot read its body; update rule 60, do not let it check nothing")
        }
        elseif ($passBody -notmatch '\bSeedStarterFleets\s*\(') {
            $failures.Add("starter-fleet-seeded-on-announcement: the `"FleetSeed`" pass's body never calls SeedStarterFleets - it seeds nothing (#443)")
        }
        $callsInFile = [regex]::Matches($seedText, '\bSeedStarterFleets\s*\(').Count
        $callsInPass = [regex]::Matches($passBody, '\bSeedStarterFleets\s*\(').Count
        if ($callsInFile -ne $callsInPass) {
            $failures.Add("starter-fleet-seeded-on-announcement: OpsRuntime.cpp calls SeedStarterFleets $($callsInFile - $callsInPass) time(s) outside the `"FleetSeed`" pass's body - in another pass (the `"JobBoard`" one seeding is the poll #443 removed) or a handler; the one legal place is the FleetSeed pass")
        }
    }
    if ($seedText -notmatch 'Subscribe\s*<\s*FNetworkChangedEvent\s*>\s*\(\s*EOpsTier::Sim\s*,\s*TEXT\s*\(\s*"FleetSeed"\s*\)') {
        $failures.Add("starter-fleet-seeded-on-announcement: OpsRuntime.cpp never subscribes FNetworkChangedEvent to the `"FleetSeed`" pass - a depot placed after the attach would wait for an unrelated event (#443)")
    }
}
$ranRules.Add('starter-fleet-seeded-on-announcement')

# --- 61. THE SHOP REFUSES A KIND NO STAND ADMITS ---------------------------------------------------
# Issue #478. A catalogue row larger than every stand's design vehicle could be bought and then refused every job it bid on
# as VehicleTooLarge. UFacilityPurchases::JudgeVehicle asks UJobBoard::AnyStandAdmits before it sells one, and the quote and
# the command share that judgement (QuoteEqualsCommandForEveryOffer). The SHAPE removed is a purchase judgement that
# never asks whether the kind can work anywhere, so: JudgeVehicle's body in FacilityPurchases.cpp must name AnyStandAdmits(
# (comments and strings stripped), and the rule fails rather than checking nothing when either is gone.
# WHAT NO REGEX SEES: that the answer is used as a refusal rather than ignored - pinned by
# AirportOps.Model.Facility.KindNoStandAdmitsIsRefused, which goes red when the refusal is not returned.
$shopFile = Join-Path $ops 'Private\Model\FacilityPurchases.cpp'
if (-not (Test-Path $shopFile)) {
    $failures.Add("shop-asks-stand-admission: $shopFile is named by rule 61 but does not exist - update the rule, do not let it check nothing")
}
else {
    $shopLines = Get-Content -LiteralPath $shopFile
    $shopCurrent = ''
    $shopInBlock = $false
    $shopJudgeSeen = $false
    $shopAsks = $false
    for ($i = 0; $i -lt $shopLines.Count; $i++) {
        $code = Strip-ArchCode $shopLines[$i] ([ref]$shopInBlock)
        $def = Get-ArchDefinition $code
        if ($null -ne $def) { $shopCurrent = $def }
        if ($shopCurrent -eq 'UFacilityPurchases::JudgeVehicle') {
            $shopJudgeSeen = $true
            if ($code -match '\bAnyStandAdmits\s*\(') { $shopAsks = $true }
        }
    }
    if (-not $shopJudgeSeen) {
        $failures.Add("shop-asks-stand-admission: UFacilityPurchases::JudgeVehicle not found in FacilityPurchases.cpp - the judgement moved or went; update rule 61, do not let it check nothing")
    }
    elseif (-not $shopAsks) {
        $failures.Add("shop-asks-stand-admission: UFacilityPurchases::JudgeVehicle no longer asks UJobBoard::AnyStandAdmits - a kind larger than every stand's design vehicle could be bought and would refuse every job as VehicleTooLarge (#478)")
    }
}
$ranRules.Add('shop-asks-stand-admission')

# --- 57. A FLIGHT'S PHASE IS WRITTEN ONLY BY UFlightBoard::TransitionTo ---------------------------
# Issue #442: UFlight::Phase was a public field thirteen places wrote (FlightBoard.cpp x12, OfferGenerator.cpp), and each site
# hand-rolled its own subset of {the pending-offer count, the clock handle, the stand release, ByAgent/AgentId, a bus publish,
# MoveToHistory, the revision}: the three writers that cancelled a flight did three different things, and the load's was right only
# because of where LoadFromSlot calls it. The field is private now (the compiler holds the line) and TransitionTo owns the effects per
# (from, to) row; this holds the SHAPE, in the text, so a future friend class or a widened field cannot bring the second writer back:
#   a. `->Phase = EFlightPhase::X` / `.Phase = EFlightPhase::X` - any object, the enum on the right names the flight.
#   b. `Flight.Phase = x` / `Flights[i]->Phase = x` - a flight-named variable, ANY right-hand side (`FlightInbound.Phase = To` names no enum).
#   c. SetPhaseForTest( in a production file. The test seam writes the phase with none of the row's effects; a production caller is a
#      thirteenth writer under another name. Tests (*Test.cpp/.h) may call it - a fixture stages a flight where no transition reaches.
#   d. MoveToHistory( called outside TransitionTo's body (its own definition aside): the move into History is a terminal row's effect,
#      and a caller of its own is the terminal transition that forgot the count, the publish or the unhook (FlightBoard.h says so).
#   e. In FlightBoard.cpp ITSELF - `(.|->)Phase = x` through ANY variable, whatever it is called. UFlightBoard is the field's FRIEND, so the
#      compiler lets the board write it through `Each`, `Due` or `Loaded[i]` - the one class that can still bring a second writer back - and
#      (a) and (b) saw only a flight-named variable or the enum on the right (`Each->Phase = Next;` matched neither).
# All of (a), (b) and (e) are hits OUTSIDE the body of UFlightBoard::TransitionTo, in the production ops and game trees (comments and strings
# are stripped first - rule 34's helper). Elsewhere the compiler holds the line (Phase is private) and (a)/(b) are the text-level second.
# WHAT NO REGEX SEES: a write to the field through a pointer-to-member, or a helper that takes the flight and the phase by parameter - which
# reads as a call, not an assignment; the field's own test-seam (SetPhaseForTest) is (c). The rule FAILS rather than checking nothing when
# TransitionTo is gone from FlightBoard.cpp or writes no phase. Mutation-checked on 2026-09-30 (restored after each; the PR body has the
# output): `Each->Phase = Next;` in DemoteRestoredMidFlight, `Each->Phase = EFlightPhase::Cancelled;` in ArrivalViewModels.cpp,
# `Flight.Phase = NewPhase;` and `SetPhaseForTest(...)` and `MoveToHistory(` in OpsAlerts.cpp each FAIL, and renaming TransitionTo FAILs.
$flightBoardFile = Join-Path $ops 'Private\Model\FlightBoard.cpp'
$phaseWriteEnum  = '(?:\.|->)Phase\s*=(?!=)\s*EFlightPhase::'
$phaseWriteNamed = '\b\w*[Ff]light\w*(?:\[[^\]]*\])?(?:\.|->)Phase\s*=(?!=)'
$phaseWriteAny   = '(?:\.|->)Phase\s*=(?!=)'
$phaseTestSeam   = '\bSetPhaseForTest\s*\('
$phaseHistoryCall = '\bMoveToHistory\s*\('
$phaseRowWrites  = 0
$phaseRowFound   = $false
$phaseTrees      = @($ops, (Join-Path $Root 'Source\AirportMgr'))
if (-not (Test-Path $flightBoardFile)) {
    $failures.Add("flight-phase-one-writer: $flightBoardFile not found - update rule 57, do not let it check nothing")
}
foreach ($phaseTree in $phaseTrees) {
    foreach ($file in Get-Sources $phaseTree @('.cpp', '.h')) {
        $isTest = $file.Name -like '*Test.cpp' -or $file.Name -like '*Test.h' -or $file.FullName -match '[\\/]Testing[\\/]'
        $lines = Get-Content -LiteralPath $file.FullName
        $inBlock = $false
        $inRow = $false
        for ($i = 0; $i -lt $lines.Count; $i++) {
            $code = Strip-ArchCode $lines[$i] ([ref]$inBlock)
            if ($file.FullName -eq $flightBoardFile) {
                # THE ROW'S BODY: from its column-0 definition to the first column-0 closing brace.
                if ($lines[$i] -match '^void\s+UFlightBoard::TransitionTo\s*\(') { $inRow = $true; $phaseRowFound = $true }
                elseif ($inRow -and $lines[$i] -match '^\}') { $inRow = $false }
            }
            if ($code.Trim() -eq '') { continue }
            if ($code -match $phaseTestSeam -and -not $isTest -and $file.Name -ne 'Flight.h') {
                $failures.Add("flight-phase-one-writer: $($file.FullName):$($i + 1) calls SetPhaseForTest in production code - it is the test seam (no count, hook, release or publish); go through UFlightBoard::TransitionTo: $($code.Trim())")
            }
            if ($isTest) { continue }
            if ($code -match $phaseHistoryCall -and -not $inRow -and $code -notmatch '^\s*void\s+UFlightBoard::MoveToHistory\s*\(' -and $code -notmatch '^\s*void\s+MoveToHistory\s*\(') {
                $failures.Add("flight-phase-one-writer: $($file.FullName):$($i + 1) calls MoveToHistory outside UFlightBoard::TransitionTo - a terminal transition is the row's, with its count, publish and unhook (#442): $($code.Trim())")
            }
            if ($code -match $phaseWriteEnum -or $code -match $phaseWriteNamed -or ($file.FullName -eq $flightBoardFile -and $code -match $phaseWriteAny)) {
                if ($inRow) { $phaseRowWrites++; continue }
                $failures.Add("flight-phase-one-writer: $($file.FullName):$($i + 1) writes a flight's phase outside UFlightBoard::TransitionTo - the row owns the side effects (#442): $($code.Trim())")
            }
        }
    }
}
if ((Test-Path $flightBoardFile) -and -not $phaseRowFound) {
    $failures.Add("flight-phase-one-writer: FlightBoard.cpp no longer defines UFlightBoard::TransitionTo - rule 57 is stale or the one writer moved (#442)")
}
elseif ((Test-Path $flightBoardFile) -and $phaseRowWrites -eq 0) {
    $failures.Add("flight-phase-one-writer: TransitionTo writes no phase the rule can see - its pattern no longer matches the body; update rule 57, do not let it check nothing")
}
$ranRules.Add('flight-phase-one-writer')

# --- 58. EFlightPhase IS GROUPED ONLY IN Flight.h -------------------------------------------------
# Issue #442: "unarrived" was Accepted||Inbound at five sites, "on the ground" was Landing..Departing by ordinal range at four, and the
# taxi's direction was `Current >= Turnaround` - the declaration order load-bearing, a comment the only thing saying so, and a new phase
# (Diverted) about twelve edits none of which was a compile error. The groupings are FlightPhase::IsUnarrived / IsOnGround / IsTerminal /
# IsLive / HasReachedStand / IsArriving now, each read off StageOf's ONE exhaustive switch in Flight.h. Four shapes, in any production ops or
# game file but Flight.h (comments and strings stripped, and continuation lines JOINED up to the statement's end - up to eight lines, to
# `;`, `{`, `}` or a `:` - before matching, so a condition wrapped over two lines is one statement):
#   a. A relational operator against an enumerator or a phase read: `Phase >= EFlightPhase::Landing`, `A.GetPhase() < B.GetPhase()`.
#   b. Two EFlightPhase:: enumerators joined by || or && in one statement: `Phase == EFlightPhase::Accepted || Phase == EFlightPhase::Inbound`.
#   c. A phase cast to an integer - `static_cast<int32>(Flight.GetPhase())`, `static_cast<int32>(EFlightPhase::Landing)` - the ordinal by
#      another road. (Casting it for a UEnum lookup goes through a variable, which this does not match: FlightBoardText::PhaseName.)
#   d. A switch on EFlightPhase (one whose body has a `case EFlightPhase::`) that has a `default:`, or that is not inside
#      AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN/_END. Without the macro C4062 is off on this toolchain, and a `default` makes a new phase someone's
#      fallback: either way "Diverted is a build error" is false at that switch. DescribeStatus (ArrivalViewModels.cpp) had one.
# NOT BANNED: one comparison of a phase with one enumerator (`== EFlightPhase::Inbound`), a switch that names every phase inside the macro,
# and ANYTHING in a test file - a test may assert "it went round again or landed". WHAT NO REGEX SEES: a grouping spelled over two statements
# (`bool A = ...; bool B = ...; A || B`) or through a variable holding an enumerator; a statement longer than eight lines. Mutation-checked on
# 2026-09-30 (restored after each; the PR body has the output): `GetPhase() >= EFlightPhase::Landing`, `== Accepted || ... == Inbound` on one line
# AND over two, `!= Accepted && != Inbound`, a cast-wrapped ordinal comparison, a switch with a default and a switch outside the macro each
# FAIL. The rule fails when Flight.h no longer defines StageOf, or when it finds no switch on EFlightPhase at all (TransitionTo is one).
$flightHeader  = Join-Path $ops 'Public\Model\Flight.h'
$phaseRelation = '(?<!-)(?:<=|>=|<|>)\s*EFlightPhase::|EFlightPhase::\w+\s*(?:<=|>=|<(?!<)|>(?!>))|GetPhase\s*\(\s*\)\s*(?:<=|>=|<(?!<)|>(?!>))|(?<!-)(?:<=|>=|<|>)\s*[\w\.\->\[\]\*\(\)]*GetPhase\s*\(\s*\)'
$phaseGrouping = 'EFlightPhase::\w+[^;{}]*?(?:\|\||&&)[^;{}]*?EFlightPhase::\w+'
$phaseCast     = 'static_cast\s*<\s*(?:u?int\w*|long|size_t)\s*>\s*\([^;]*(?:EFlightPhase::|GetPhase\s*\()'
$phaseSwitchesSeen = 0
if (-not (Test-Path $flightHeader) -or $null -eq (Select-String -Path $flightHeader -Pattern 'inline\s+EFlightStage\s+StageOf\s*\(')) {
    $failures.Add("flight-phase-groupings: Flight.h no longer defines FlightPhase::StageOf - rule 58 is stale or the one table moved (#442); update it, do not let it check nothing")
}
foreach ($phaseTree in $phaseTrees) {
    foreach ($file in Get-Sources $phaseTree @('.cpp', '.h')) {
        if ($file.Name -like '*Test.cpp' -or $file.Name -like '*Test.h' -or $file.FullName -match '[\\/]Testing[\\/]' -or $file.Name -eq 'Flight.h') { continue }
        $lines = Get-Content -LiteralPath $file.FullName
        $inBlock = $false
        $codeLines = New-Object System.Collections.Generic.List[string]
        $exhaustiveAt = New-Object System.Collections.Generic.List[bool]
        $exhaustive = $false
        for ($i = 0; $i -lt $lines.Count; $i++) {
            $code = Strip-ArchCode $lines[$i] ([ref]$inBlock)
            if ($code -match '\bAIRSIDE_EXHAUSTIVE_SWITCH_BEGIN\b') { $exhaustive = $true }
            $codeLines.Add($code)
            $exhaustiveAt.Add($exhaustive)
            if ($code -match '\bAIRSIDE_EXHAUSTIVE_SWITCH_END\b') { $exhaustive = $false }
        }
        # (a) (b) (c): STATEMENTS, not lines.
        $statement = ''
        $statementStart = 0
        $statementLines = 0
        for ($i = 0; $i -lt $codeLines.Count; $i++) {
            $trimmed = $codeLines[$i].Trim()
            $flush = $false
            if ($trimmed -eq '') {
                $flush = $statement -ne ''
            } else {
                if ($statement -eq '') { $statementStart = $i }
                $statement += ' ' + $trimmed
                $statementLines++
                $flush = ($trimmed -match '[;{}:]$') -or ($statementLines -ge 8)
            }
            if ($flush) {
                if ($statement -match 'EFlightPhase::|GetPhase\s*\(' -and ($statement -match $phaseRelation -or $statement -match $phaseGrouping -or $statement -match $phaseCast)) {
                    $failures.Add("flight-phase-groupings: $($file.FullName):$($statementStart + 1) groups EFlightPhase by comparison, cast or an OR of names - ask FlightPhase::IsUnarrived / IsOnGround / IsTerminal / IsLive / HasReachedStand / IsArriving (Flight.h) instead (#442): $($statement.Trim())")
                }
                $statement = ''
                $statementLines = 0
            }
        }
        # (d) SWITCHES: a default, or no exhaustive macro around it.
        for ($i = 0; $i -lt $codeLines.Count; $i++) {
            if ($codeLines[$i] -notmatch '\bswitch\s*\(') { continue }
            $open = -1
            for ($j = $i; $j -lt [Math]::Min($i + 4, $codeLines.Count); $j++) { if ($codeLines[$j].Contains('{')) { $open = $j; break } }
            if ($open -lt 0) { continue }
            $depth = 0
            $flat = New-Object System.Text.StringBuilder
            $closed = $false
            for ($j = $open; $j -lt $codeLines.Count -and -not $closed; $j++) {
                foreach ($ch in $codeLines[$j].ToCharArray()) {
                    if ($ch -eq '{') { $depth++; if ($depth -gt 1) { [void]$flat.Append(' ') } }
                    elseif ($ch -eq '}') { $depth--; if ($depth -eq 0) { $closed = $true; break } else { [void]$flat.Append(' ') } }
                    elseif ($depth -eq 1) { [void]$flat.Append($ch) }
                    else { [void]$flat.Append(' ') }
                }
                [void]$flat.Append(' ')
            }
            $body = $flat.ToString()
            if ($body -notmatch '\bcase\s+EFlightPhase::') { continue }
            $phaseSwitchesSeen++
            if ($body -match '\bdefault\s*:') {
                $failures.Add("flight-phase-groupings: $($file.FullName):$($i + 1) switches on EFlightPhase with a default - name every phase, so a new one is a build error here and not this switch's fallback (#442)")
            }
            if (-not $exhaustiveAt[$i]) {
                $failures.Add("flight-phase-groupings: $($file.FullName):$($i + 1) switches on EFlightPhase outside AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN/_END - C4062 is off on this toolchain, so a new phase would pass it silently (#442)")
            }
        }
    }
}
if ($phaseSwitchesSeen -eq 0) {
    $failures.Add("flight-phase-groupings: found no switch on EFlightPhase in the production trees - UFlightBoard::TransitionTo is one; rule 58's switch check no longer sees its shape; update it, do not let it check nothing")
}
$ranRules.Add('flight-phase-groupings')

# --- 81. NO default: IN A SWITCH ON EAgentPhase (#444) ----------------------------------------------------------------
# What each agent phase MEANS was spelled by its consumers, behind default: arms - StatusOf's `case Taxiing: default:` put a
# push and a reverse on the card as "Taxiing" while the flight board said Manoeuvring; DistanceAlongPlan, SpeedAlongPlan and
# DescribeMotion gave a phase they did not name the follower's figures; FRoadAgent::Advance would have returned false ("drop
# this agent") for one. Reversing (2026-09-17) and Stranded (2026-09-28) each fell into somebody's default on arrival. What a
# phase means is FAgentPhaseTraits' row now (Model/AgentPhase.h, whose count fails the build for a phase with no row); a
# switch that still decides per phase names EVERY phase. A switch whose body has a `case EAgentPhase::` (rule 58(d)'s shape:
# comments and strings stripped, the body flattened to its own depth so a nested switch's default is not this one's), in any
# production file of Airside, AirsideEditor, AirportOps or the game module, FAILS when:
#   a. it has a `default:` - a new phase becomes that switch's fallback, silently;
#   b. it is not inside AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN/_END - C4062 is off on this toolchain, so "no default" alone is a
#      comment, not a contract (ExhaustiveSwitch.h): a phase missing from the switch would compile and fall through.
# NOT BANNED: an `if` on one phase, a grouping by `||` (not linted - see below), and anything in a test. WHAT NO REGEX SEES:
# a phase set spelled as an OR of comparisons (`Phase == Parked || Phase == Stranded`) - rule 58(b)'s shape for flight
# phases; for agent phases the remaining ones are transition tests and arms of one-off decisions (#444's PR lists them), and
# the pin on the MEANING is Airside.Model.Traffic.IsOnRouteClassifiesEveryPhase (every column, every phase). Nothing is
# allow-listed: FTurnarounds::OnAircraftPhase (#427's split of UJobBoard::OnAgentPhase) has no switch on a phase. The rule fails,
# rather than passes, when it sees no switch on EAgentPhase at all - FRoadAgent::Advance is one. THE BODY IS READ FROM THE
# SWITCH'S OWN BRACE (#496's review): the scan used to start at the first `{` anywhere on the switch's line, so a brace EARLIER on
# that line (`case X: { switch (Phase) {`) opened the scan one level too high, blanked the switch's body as nested and skipped it.
# Mutation-checked 2026-10-01 (the PR body has the output), that shape included.
$agentPhaseTrees = @($plugin, $editor, $ops, (Join-Path $Root 'Source\AirportMgr'))
$agentPhaseSwitchesSeen = 0
foreach ($agentPhaseTree in $agentPhaseTrees) {
    foreach ($file in Get-Sources $agentPhaseTree @('.cpp', '.h')) {
        if ($file.Name -like '*Test.cpp' -or $file.Name -like '*Test.h' -or $file.FullName -match '[\\/]Testing[\\/]') { continue }
        $lines = Get-Content -LiteralPath $file.FullName
        if (-not ($lines -match 'case\s+EAgentPhase::')) { continue }
        $inBlock = $false
        $codeLines = New-Object System.Collections.Generic.List[string]
        $exhaustiveAt = New-Object System.Collections.Generic.List[bool]
        $exhaustive = $false
        for ($i = 0; $i -lt $lines.Count; $i++) {
            $code = Strip-ArchCode $lines[$i] ([ref]$inBlock)
            if ($code -match '\bAIRSIDE_EXHAUSTIVE_SWITCH_BEGIN\b') { $exhaustive = $true }
            $codeLines.Add($code)
            $exhaustiveAt.Add($exhaustive)
            if ($code -match '\bAIRSIDE_EXHAUSTIVE_SWITCH_END\b') { $exhaustive = $false }
        }
        for ($i = 0; $i -lt $codeLines.Count; $i++) {
          foreach ($switchAt in [regex]::Matches($codeLines[$i], '\bswitch\s*\(')) {
            # THE SWITCH'S OWN BRACE: the first `{` AFTER the keyword, on its line or the next three - never one before it.
            $open = -1
            $openCol = 0
            for ($j = $i; $j -lt [Math]::Min($i + 4, $codeLines.Count); $j++) {
                $from = if ($j -eq $i) { $switchAt.Index } else { 0 }
                $col = $codeLines[$j].IndexOf('{', $from)
                if ($col -ge 0) { $open = $j; $openCol = $col; break }
            }
            if ($open -lt 0) { continue }
            $depth = 0
            $flat = New-Object System.Text.StringBuilder
            $closed = $false
            for ($j = $open; $j -lt $codeLines.Count -and -not $closed; $j++) {
                $text = if ($j -eq $open) { $codeLines[$j].Substring($openCol) } else { $codeLines[$j] }
                foreach ($ch in $text.ToCharArray()) {
                    if ($ch -eq '{') { $depth++; if ($depth -gt 1) { [void]$flat.Append(' ') } }
                    elseif ($ch -eq '}') { $depth--; if ($depth -eq 0) { $closed = $true; break } else { [void]$flat.Append(' ') } }
                    elseif ($depth -eq 1) { [void]$flat.Append($ch) }
                    else { [void]$flat.Append(' ') }
                }
                [void]$flat.Append(' ')
            }
            $body = $flat.ToString()
            if ($body -notmatch '\bcase\s+EAgentPhase::') { continue }
            $agentPhaseSwitchesSeen++
            if ($body -match '\bdefault\s*:') {
                $failures.Add("agent-phase-switch: $($file.FullName):$($i + 1) switches on EAgentPhase with a default - name every phase, so a new one is a build error here and not this switch's fallback; what a phase MEANS is FAgentPhaseTraits' row (#444)")
            }
            if (-not $exhaustiveAt[$i]) {
                $failures.Add("agent-phase-switch: $($file.FullName):$($i + 1) switches on EAgentPhase outside AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN/_END - C4062 is off on this toolchain, so a new phase would pass it silently (#444)")
            }
          }
        }
    }
}
if ($agentPhaseSwitchesSeen -eq 0) {
    $failures.Add("agent-phase-switch: found no switch on EAgentPhase in the production trees - FRoadAgent::Advance is one; rule 81's switch check no longer sees its shape; update it, do not let it check nothing")
}
$ranRules.Add('agent-phase-switch')

# --- 66. THE WINDOWS A KEY OPENS ARE TOGGLED WINDOWS (#447) ----------------------------------------------------
# Whether a window is open is the HOST'S (UUiWindowHost::Toggle/IsShown), and what its close button means is FUiWindowSpec::bToggled: a toggled window's
# close IS the toggle, an untoggled one's sticks. UBuildHudLayer::PanelFor is the list of windows a key or a bar button toggles, and every panel on it must
# say `bToggled = true` in its WantsWindow - a LIST THAT MUST AGREE (CLAUDE.md): a panel added to PanelFor and not marked would have its close stick, and the
# next key press "open" it hidden, which is the bug four panels' bShowing resyncs existed to stop. (`bool bShowing` itself is a rule-4 row.)
# DOES NOT SEE: a window toggled by a path other than PanelFor. AirportMgr.UI.WindowHost.ToggleIsTheHostsAndCloseIsToggle is the behaviour half.
$hudLayerCpp = Join-Path $Root 'Source\AirportMgr\BuildHudLayer.cpp'
$hudLayerH   = Join-Path $Root 'Source\AirportMgr\BuildHudLayer.h'
if (-not (Test-Path $hudLayerCpp) -or -not (Test-Path $hudLayerH)) {
    $failures.Add("toggled-windows-agree: BuildHudLayer.cpp/.h are named by rule 66 but one does not exist - update the rule, do not let it check nothing")
} else {
    $hudCppText = Get-Content -LiteralPath $hudLayerCpp -Raw
    $hudHText   = Get-Content -LiteralPath $hudLayerH -Raw
    $panelMembers = @([regex]::Matches($hudCppText, 'case\s+EHudWindow::\w+\s*:\s*return\s+Layer\.(\w+)\s*;') | ForEach-Object { $_.Groups[1].Value })
    if ($panelMembers.Count -lt 1) {
        $failures.Add("toggled-windows-agree: no 'case EHudWindow::X: return Layer.<Panel>;' rows found in BuildHudLayer.cpp's PanelFor - it moved, or rule 66 is stale (#447)")
    }
    foreach ($member in $panelMembers) {
        $decl = [regex]::Match($hudHText, 'TObjectPtr<(\w+)>\s+' + [regex]::Escape($member) + '\s*;')
        if (-not $decl.Success) {
            $failures.Add("toggled-windows-agree: PanelFor returns Layer.$member but BuildHudLayer.h declares no TObjectPtr<...> $member - update rule 66 or the declaration (#447)")
            continue
        }
        $cls = $decl.Groups[1].Value
        $panelCpp = Join-Path $Root ('Source\AirportMgr\' + $cls.Substring(1) + '.cpp')
        if (-not (Test-Path $panelCpp)) {
            $failures.Add("toggled-windows-agree: $cls (Layer.$member) has no $($cls.Substring(1)).cpp beside BuildHudLayer - update rule 66")
            continue
        }
        # COMMENTS STRIPPED (rule 34's stripper): a WHY comment that says `bToggled = true` is not the assignment.
        $panelCode = @()
        $inPanelBlock = $false
        foreach ($panelLine in (Get-Content -LiteralPath $panelCpp)) { $panelCode += (Strip-ArchCode $panelLine ([ref]$inPanelBlock)) }
        $panelText = $panelCode -join "`n"
        if ($panelText -notmatch '\bbToggled\s*=\s*true\b') {
            $failures.Add("toggled-windows-agree: $($cls.Substring(1)).cpp is on UBuildHudLayer's PanelFor (a key or a bar button toggles it) but its WantsWindow does not set bToggled = true - its close would stick and the next key press open it hidden (#447)")
        }
    }
}
$ranRules.Add('toggled-windows-agree')

# --- 67. NIGHT IS DEFINED ONCE: THE SKY FOLLOWS THE CLOCK (#447) -------------------------------------------------------
# FSunPath hard-coded 06:00-12:00-18:00 while the clock's daylight is the scenario's DawnHour..DuskHour (6..20): for two game hours a day the field was lit as
# night while the time compression, the demand curve and the inbox's shading said day. FSunPath takes the hours; ASunDriver::MakePath hands it the clock's.
# THE TWO HALVES, each a way it silently un-wires: SunPath.cpp reads DawnHour and DuskHour (and keeps no quarter-day constant of its own), and SunDriver.cpp's
# MakePath sets BOTH from the clock's resolver. DOES NOT SEE: MakePath setting them from the wrong thing (AirportMgr.Sky.SunDriver.DuskIsTheClocks reads what it sets).
$sunPathCpp = Join-Path $Root 'Source\AirportMgr\SunPath.cpp'
$sunDriverCpp = Join-Path $Root 'Source\AirportMgr\SunDriver.cpp'
if (-not (Test-Path $sunPathCpp) -or -not (Test-Path $sunDriverCpp)) {
    $failures.Add("night-defined-once: SunPath.cpp/SunDriver.cpp are named by rule 67 but one does not exist - update the rule, do not let it check nothing")
} else {
    $sunPathCode = @()
    $inBlock = $false
    foreach ($line in (Get-Content -LiteralPath $sunPathCpp)) { $sunPathCode += (Strip-ArchCode $line ([ref]$inBlock)) }
    $sunPathText = $sunPathCode -join "`n"
    if ($sunPathText -notmatch '\bDawnHour\b' -or $sunPathText -notmatch '\bDuskHour\b') {
        $failures.Add("night-defined-once: SunPath.cpp no longer reads DawnHour and DuskHour - the sky has a day of its own again (#447)")
    }
    if ($sunPathText -match '\bQuarterDay\b') {
        $failures.Add("night-defined-once: SunPath.cpp has a QuarterDay constant - the hard-coded 06:00-18:00 day that parted from the clock's (#447)")
    }
    # COMMENTS STRIPPED: a WHY comment quoting the assignment is not the assignment.
    $sunDriverCode = @()
    $inBlock = $false
    foreach ($line in (Get-Content -LiteralPath $sunDriverCpp)) { $sunDriverCode += (Strip-ArchCode $line ([ref]$inBlock)) }
    $sunDriverText = $sunDriverCode -join "`n"
    $makePath = [regex]::Match($sunDriverText, 'FSunPath\s+ASunDriver::MakePath\s*\([^)]*\)\s*const\s*\{(?<body>.*?)\n\}', 'Singleline')
    if (-not $makePath.Success) {
        $failures.Add("night-defined-once: ASunDriver::MakePath not found in SunDriver.cpp - it moved, or rule 67 is stale (#447)")
    } elseif ($makePath.Groups['body'].Value -notmatch 'Path\.DawnHour\s*=\s*Clock\s*->\s*DawnHour\s*;' -or $makePath.Groups['body'].Value -notmatch 'Path\.DuskHour\s*=\s*Clock\s*->\s*DuskHour\s*;') {
        $failures.Add("night-defined-once: ASunDriver::MakePath does not hand the clock's dawn and dusk to the path (Path.DawnHour = Clock->DawnHour; Path.DuskHour = Clock->DuskHour;) - the sky would follow a day the clock does not (#447)")
    }
}
$ranRules.Add('night-defined-once')

# --- 68. TEXT IS NOT COMPARED TO A LITERAL, AND A STAND OR A DEPOT IS NOT PRINTED BY INDEX (#447, #490) -----------------
# Two shapes that shipped, each checked over STATEMENTS (comments and strings stripped, continuation lines JOINED to the statement's end - up to eight
# lines, to `;`, `{`, `}` or a `:`, rule 58's joining) because BOTH were split across two lines and a line-by-line row could not see either:
#   a. `.EqualTo(` then `NSLOCTEXT(`: the arrivals panel recovered "holding" by comparing the status's localised text against a copy of the word -
#      `Row->GetStatus().EqualTo(` ending one line and `NSLOCTEXT("AirportMgr", "ArrivalHolding", "HOLDING"))` starting the next. A reworded status or a
#      translation un-tinted it silently. The row says IsHolding(); ULedgerRowViewModel::bOutgoing is the same idea done right.
#   b. A stand's entity INDEX in player-facing text: `FString::Printf`, `FText::AsNumber` or `FText::Format` with `Stand.Index` in the same statement. The
#      stand card and the painted sign say StandNumber; the depot card's backlog and the JobUnserviceable alert printed the index ("stand 0" beside a sign
#      reading 4). OpsNames::StandLabel is the ops layer's one naming. OpsEventBus.cpp's event Describe strings are LOG text (beside the flight and agent
#      ids, where the index is what a developer reads), and UE_LOG lines are not Printf/AsNumber/Format calls, so neither is player-facing text here.
#   c. THE DEPOT TWIN OF b (#490): the depot card's title and the job board's "to depot / refilling at depot / at depot" lines printed the depot's entity index
#      (`Fuel depot {0}` over `S.Index`; `Vehicle.Home.Index`), which a delete recycles into a different depot. OpsNames::DepotLabel is the one naming (the depot's
#      DepotNumber). Flagged: `FString::Printf`, `FString::Format`, `FText::AsNumber` or `FText::Format` in a statement with `Home.Index` or `Depot.Index`, OR
#      with a string literal naming a depot ("depot", any case) and `.Index` after it. The literal half is read off the statement with its STRINGS KEPT (the
#      code-stripped statement the other halves read has them emptied). The card names the depot in a statement of its own (`const FString DepotName =
#      OpsNames::DepotLabel(...)`) and formats that, so the fix does not trip the rule it is under. OpsEventBus.cpp's Describe lines are log text, as for b.
# DOES NOT SEE: an index carried in a local first (`const int32 S = Job.Stand.Index;`) and printed from that, or a statement over eight lines - the
# behaviour half is AirportOps.Model.StandLabel.AlertBacklogAndCardSayTheSameNumber (stands) and AirportOps.Model.DepotLabel.VehicleLinesSayTheDepotsNumber (depots). Test files are exempt. Mutation-checked with the EXACT two lines
# from 891ecb66 ArrivalsPanelWidget.cpp:130-131 (the PR body has the output).
$textRuleTrees = @((Join-Path $Root 'Plugins\Airside\Source'), (Join-Path $Root 'Plugins\AirportOps\Source'), (Join-Path $Root 'Source\AirportMgr'))
$textEqualTo  = '\.EqualTo\s*\(\s*NSLOCTEXT\s*\('
$standPrinted = '(?:FString::Printf|FText::AsNumber|FText::Format)\b[^;{}]*\bStand\s*\.\s*Index\b'
$depotPrinted = '(?:FString::Printf|FString::Format|FText::AsNumber|FText::Format)\b[^;{}]*\b(?:Home|Depot)\s*\.\s*Index\b'
$depotLiteralIndexed = '(?i)(?:FString::Printf|FString::Format|FText::AsNumber|FText::Format)\b[^;{}]*"[^"]*\bdepot\b[^"]*"[^;]*\.\s*Index\b'
$textRuleFiles = 0
foreach ($textTree in $textRuleTrees) {
    foreach ($file in Get-Sources $textTree @('.cpp', '.h')) {
        if ($file.Name -like '*Test.cpp' -or $file.Name -like '*Test.h' -or $file.FullName -match '[\\/](Testing|AirsideTests|AirportOpsTests)[\\/]') { continue }
        $textRuleFiles++
        $lines = Get-Content -LiteralPath $file.FullName
        $inBlock = $false
        $inBlockKeepingStrings = $false
        $statement = ''
        $statementKeepingStrings = ''
        $statementStart = 0
        $statementLines = 0
        for ($i = 0; $i -lt $lines.Count; $i++) {
            $trimmed = (Strip-ArchCode $lines[$i] ([ref]$inBlock)).Trim()
            $keptStrings = (Strip-ArchComments $lines[$i] ([ref]$inBlockKeepingStrings)).Trim()
            $flush = $false
            if ($trimmed -eq '') {
                $flush = $statement -ne ''
            } else {
                if ($statement -eq '') { $statementStart = $i }
                $statement += ' ' + $trimmed
                $statementKeepingStrings += ' ' + $keptStrings
                $statementLines++
                $flush = ($trimmed -match '[;{}:]$') -or ($statementLines -ge 8)
            }
            if ($flush) {
                if ($statement -match $textEqualTo) {
                    $failures.Add("text-not-compared-or-indexed: $($file.FullName):$($statementStart + 1) compares localised text against NSLOCTEXT - ask the view model for the fact (UArrivalRowViewModel::IsHolding, ULedgerRowViewModel::IsOutgoing) and do not compare what it prints (#447): $($statement.Trim())")
                }
                if ($statement -match $standPrinted -and $file.Name -ne 'OpsEventBus.cpp') {
                    $failures.Add("text-not-compared-or-indexed: $($file.FullName):$($statementStart + 1) prints a stand's entity index to the player - name it with OpsNames::StandLabel, the number on its sign (a delete recycles the index) (#447): $($statement.Trim())")
                }
                if (($statement -match $depotPrinted -or $statementKeepingStrings -match $depotLiteralIndexed) -and $file.Name -ne 'OpsEventBus.cpp') {
                    $failures.Add("text-not-compared-or-indexed: $($file.FullName):$($statementStart + 1) prints a depot's entity index to the player - name it with OpsNames::DepotLabel, its DepotNumber (a delete recycles the index) (#490): $($statementKeepingStrings.Trim())")
                }
                $statement = ''
                $statementKeepingStrings = ''
                $statementLines = 0
            }
        }
    }
}
if ($textRuleFiles -lt 50) {
    $failures.Add("text-not-compared-or-indexed: rule 68 read only $textRuleFiles production file(s) - the trees moved, or the rule checks nothing (#447)")
}
$ranRules.Add('text-not-compared-or-indexed')

# --- 74. THE UNSTICK MENU IS OPENED BY A CALL, NOT A COUNTER (#446) ----------------------------------------------------
# selection.unstick's Execute called ARoadBuildController::RequestUnstickMenu, which bumped UnstickMenuRequests; UInspectorWidget::TickPanel compared it with
# SeenUnstickRequests every tick and opened the popup on a difference - while the same controller reached its other panels by a direct call. The row calls
# UBuildHudLayer::OpenUnstickMenu now, which calls UInspectorWidget::OpenUnstickMenu. THE BAN is rule 4's 'unstick request counter (rule 74)' row (the three names
# and the `Seen...Request(s)` shape, in every production file). THIS BLOCK IS THE HALF THAT STOPS IT CHECKING NOTHING: the row must still reach the HUD layer, and the
# layer must still reach the inspector - a deleted call would pass the ban with an Unstick button that opens nothing.
# DOES NOT SEE: a new counter under a name the row's two patterns do not spell. AirportMgr.Inspector.UnstickRowOpensTheMenuWithoutATick is the behaviour half.
$unstickSites = @(
    @{ Path = (Join-Path $Root 'Source\AirportMgr\BuildActions.cpp'); Needs = '\bHud\s*->\s*OpenUnstickMenu\s*\('; What = "the selection.unstick row's Execute calling Ctx.Hud->OpenUnstickMenu()" },
    @{ Path = (Join-Path $Root 'Source\AirportMgr\BuildHudLayer.cpp'); Needs = '\bInspector\s*->\s*OpenUnstickMenu\s*\('; What = 'UBuildHudLayer::OpenUnstickMenu calling Inspector->OpenUnstickMenu()' }
)
foreach ($site in $unstickSites) {
    if (-not (Test-Path $site.Path)) {
        $failures.Add("unstick-by-call: $($site.Path) is named by rule 74 but does not exist - update the rule, do not let it check nothing")
        continue
    }
    $inBlock = $false
    $siteCode = (Get-Content -LiteralPath $site.Path | ForEach-Object { Strip-ArchCode $_ ([ref]$inBlock) }) -join "`n"
    if ($siteCode -notmatch $site.Needs) {
        $failures.Add("unstick-by-call: $(Split-Path $site.Path -Leaf) no longer has $($site.What) - the popup is opened by a call, not a request count the inspector diffs (#446)")
    }
}
$ranRules.Add('unstick-by-call')

# --- 75. THE SELECTION HAS ONE DOOR, AND EVERY CONTEXT CARRIES IT (#446) -------------------------------------------------
# UInspectorWidget kept LastSelection and compared (Kind, Id) with it every tick to reopen its window, close its menus and disarm a sale; the buildings ghost reveal
# re-read the selection every frame. FBuildSession::OnSelectionChanged announces a change once, from SelectionDoor::Write, which every writer goes through: a tool
# through the FToolContext it is handed (SetSelection / ClearSelection), code and the session itself through FBuildSession. THE BANS are rule 4's rows ('selection
# diffed on a tick', 'selection written outside the door', 'SelectionDoor::Write callers'). THIS BLOCK IS THE HALF THAT STOPS THEM CHECKING NOTHING: the Select tool must
# still write through the context, the session must still write through WriteSelection and hand EVERY context the door (a context with the selection pointer and no
# announcement writes silently in ONE driver - rule 47's shape), and the controller must still subscribe. The editor mode fires it by construction (it builds its
# contexts through the session); rule 47's 'OnSelectionChanged' row records why it does not subscribe.
# THE BUILDINGS GHOST REVEAL STAYS A PER-FRAME GATE, deliberately: it is a function of three sources - the selection, whether the depot is plotted (the network),
# and the lit tool (FBuildSession::WantsPlotGhostsDrawn) - and only the first has an event; a subscriber on the selection alone would miss an undo of a plot and a
# tool change. ShowPlotGhosts is two compares unless the answer moved.
# DOES NOT SEE: a write spelled some other way (a reference bound to the selection and assigned through). Airside.Tool.BuildSession.SelectionChangedFiresOncePerChange and
# .EveryContextCarriesTheSelectionDoor are the behaviour half.
$selectionSites = @(
    @{ Path = (Join-Path $Root 'Plugins\Airside\Source\Airside\Private\Tool\SelectTool.cpp'); Needs = '\bSetSelection\s*\('; What = 'the Select tool writing the selection through Context.SetSelection(...)' },
    @{ Path = (Join-Path $Root 'Plugins\Airside\Source\Airside\Private\Tool\SelectTool.cpp'); Needs = '\bClearSelection\s*\('; What = 'the Select tool clearing the selection through Context.ClearSelection()' },
    @{ Path = (Join-Path $Root 'Plugins\Airside\Source\Airside\Private\Tool\BuildSession.cpp'); Needs = '\bWriteSelection\s*\('; What = "the session writing its selection through WriteSelection" },
    @{ Path = (Join-Path $Root 'Plugins\Airside\Source\Airside\Private\Tool\BuildSession.cpp'); Needs = '\bContext\s*\.\s*BindSelection\s*\('; What = 'MakeContext binding every context to the session selection and its announcement (Context.BindSelection(...))' },
    @{ Path = (Join-Path $Root 'Source\AirportMgr\RoadBuildController.cpp'); Needs = '\bSession\s*\.\s*OnSelectionChanged\s*\(\s*\)\s*\.\s*AddUObject\s*\('; What = "the controller subscribing to the session's selection event" }
)
foreach ($site in $selectionSites) {
    if (-not (Test-Path $site.Path)) {
        $failures.Add("selection-one-door: $($site.Path) is named by rule 75 but does not exist - update the rule, do not let it check nothing")
        continue
    }
    $inBlock = $false
    $siteCode = (Get-Content -LiteralPath $site.Path | ForEach-Object { Strip-ArchCode $_ ([ref]$inBlock) }) -join "`n"
    if ($siteCode -notmatch $site.Needs) {
        $failures.Add("selection-one-door: $(Split-Path $site.Path -Leaf) no longer has $($site.What) - the selection changes through one door that announces it, and the inspector does not diff it (#446)")
    }
}
$ranRules.Add('selection-one-door')

# --- 76. A WINDOW THAT CAN FOLD DOES NOT COMPOSE WHILE FOLDED, AND THE BAR DESCRIBES THE RUNWAY ONCE A FRAME (#446) --------------
# UUiWindowHost ticks every hosted panel, folded or not (a collapsed window stops Slate ticking its contents, and the tick is what decides to show it again), and the
# arrivals and offers windows composed every row's strings and painted every text on every one of those ticks - about six and a dozen FText::Format a row - for a body
# nobody could see. A foldable panel now asks UAirportMgrPanelWidget::IsFolded() and keeps only what the title bar shows (its badge). THE LIST THAT MUST AGREE: every
# production panel whose WantsWindow says `bCollapsible = true` must call IsFolded() in its own file - a new foldable window without the gate fails here, and so does a
# gate deleted from one of the two. A FLOOR on the count (two today) stops the rule reading nothing. (The bar's runway describe is rule 4's 'InspectFacts::DescribeRunway
# callers (rule 76)' row; this block checks BuildActions.cpp still reads through the controller's per-frame answer.)
# DOES NOT SEE: a panel that folds through some other spelling than bCollapsible, or one that calls IsFolded() and composes anyway. AirportMgr.UI.Arrivals.FoldedPanelComposesNothing
# and AirportMgr.UI.OfferInbox.FoldedPanelComposesNothing are the behaviour half; AirportMgr.Actions.RunwayRowsDescribeOncePerFrame is the runway's.
$foldableCount = 0
foreach ($file in Get-Sources (Join-Path $Root 'Source\AirportMgr') @('.cpp')) {
    if ($file.Name -like '*Test.cpp') { continue }
    $inBlock = $false
    $fileCode = (Get-Content -LiteralPath $file.FullName | ForEach-Object { Strip-ArchCode $_ ([ref]$inBlock) }) -join "`n"
    if ($fileCode -match '\bbCollapsible\s*=\s*true\b') {
        $foldableCount++
        if ($fileCode -notmatch '\bIsFolded\s*\(\s*\)') {
            $failures.Add("foldable-panels-gate: $($file.Name) sets bCollapsible = true but never asks IsFolded() - a folded window is still ticked by the host, and a panel that composes every row for a body nobody can see is the per-tick cost #446 removed: skip the composing and keep the title bar's badge")
        }
    }
}
if ($foldableCount -lt 2) {
    $failures.Add("foldable-panels-gate: rule 76 found $foldableCount panel(s) setting bCollapsible = true under Source\AirportMgr - the arrivals and offers windows are two; the rule reads nothing or the windows moved (#446)")
}
$runwayRowsCpp = Join-Path $Root 'Source\AirportMgr\BuildActions.cpp'
if (-not (Test-Path $runwayRowsCpp)) {
    $failures.Add("foldable-panels-gate: $runwayRowsCpp is named by rule 76 but does not exist - update the rule, do not let it check nothing")
} else {
    $inBlock = $false
    $runwayRowsCode = (Get-Content -LiteralPath $runwayRowsCpp | ForEach-Object { Strip-ArchCode $_ ([ref]$inBlock) }) -join "`n"
    if ($runwayRowsCode -notmatch '\bSelectedRunwayFactsThisFrame\s*\(') {
        $failures.Add("foldable-panels-gate: BuildActions.cpp no longer reads the selected runway through ARoadBuildController::SelectedRunwayFactsThisFrame - the bar's two runway rows ask it four times a tick, and a describe each is what #446 removed")
    }
}
$ranRules.Add('foldable-panels-gate')

# --- 94. THE EDITOR DESCRIBES A FRAME'S PREVIEW IN ONE PLACE, AND ONLY RENDER AND ITS SEAM ASK FOR IT (#462, #463) ------------
# Render's tail and CachePreviewLabelsForTest were two hand-kept copies of "describe the preview, cache the labels", so a Render that cached nothing
# left Airside.Editor.DrawHUDShowsRefusalLabels green (it never called Render), and the call-counting test Airside.Editor.RenderCachesLabelsOnce
# (deleted 2026-10) counted only its own seam: DrawHUD calling BuildPreview a second time every frame - the doubled cost issue #304's review round 2
# fixed - passed it. URoadBuildEditorTool::DescribeFrame is the one place now; Render and the seam both call it. In RoadBuildEditorTool.cpp, comments
# and strings stripped (rule 34's stripper):
# (a) BuildPreview( is called from DescribeFrame and from nowhere else - not DrawHUD, not Render, not the seam;
# (b) DescribeFrame( is called from Render and from CachePreviewLabelsForTest and from nowhere else - a second describe THROUGH the one function
#     (DrawHUD calling it) doubles every tool's preview cost every frame as surely as a second BuildPreview does, which (a) alone cannot see;
# (c) Render and CachePreviewLabelsForTest each call DescribeFrame( - so the seam a test stands in for Render with IS Render's body;
# (d) the half that stops it checking nothing: DescribeFrame exists and calls BuildPreview.
# DOES NOT SEE: a describe reached through some other wrapper of BuildPreview (a new helper, called from DrawHUD, that calls DescribeFrame's
# callee by another route) until the wrapper is named in (a)/(b). Airside.Editor.DrawHUDShowsRefusalLabels is the behaviour half (a label the tool
# describes reaches the cache DrawHUD reads).
$editorToolFile = Join-Path $Root 'Plugins\Airside\Source\AirsideEditor\Private\RoadBuildEditorTool.cpp'
if (-not (Test-Path $editorToolFile)) {
    $failures.Add("editor-preview-described-once: $editorToolFile is named by rule 94 but does not exist - update the rule, do not let it check nothing")
} else {
    $describeCallers = @('URoadBuildEditorTool::Render', 'URoadBuildEditorTool::CachePreviewLabelsForTest')
    $lines = Get-Content -LiteralPath $editorToolFile
    $inBlock = $false
    $current = ''
    $describeCallsIn = @{}
    $buildPreviewInDescribe = 0
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $code = Strip-ArchCode $lines[$i] ([ref]$inBlock)
        $def = Get-ArchDefinition $code
        if ($null -ne $def) { $current = $def }
        if ($code -match '\bBuildPreview\s*\(') {
            if ($current -eq 'URoadBuildEditorTool::DescribeFrame') { $buildPreviewInDescribe++ }
            else { $failures.Add("editor-preview-described-once: RoadBuildEditorTool.cpp:$($i + 1) calls BuildPreview( in $current - a frame's preview is described by URoadBuildEditorTool::DescribeFrame alone, which Render and the headless seam both call; a second call from DrawHUD (or a Render of its own) doubles every tool's preview cost every frame and leaves the seam a copy that can drift (#304 review round 2, #463): $($code.Trim())") }
        }
        if ($code -match '\bDescribeFrame\s*\(' -and $code -notmatch 'URoadBuildEditorTool::DescribeFrame\s*\(') {
            if ($describeCallers -contains $current) { $describeCallsIn[$current] = 1 + [int]$describeCallsIn[$current] }
            else { $failures.Add("editor-preview-described-once: RoadBuildEditorTool.cpp:$($i + 1) calls DescribeFrame( in $current - only Render and CachePreviewLabelsForTest describe a frame; a third caller (DrawHUD, say) describes it again and doubles every tool's preview cost every frame (#304 review round 2, #463): $($code.Trim())") }
        }
    }
    foreach ($caller in $describeCallers) {
        if ([int]$describeCallsIn[$caller] -lt 1) {
            $failures.Add("editor-preview-described-once: $caller no longer calls DescribeFrame( - Render and the seam a test stands in for it with must share the one description, or the test measures a copy (#463)")
        }
    }
    if ($buildPreviewInDescribe -lt 1) {
        $failures.Add("editor-preview-described-once: URoadBuildEditorTool::DescribeFrame not found calling BuildPreview( in RoadBuildEditorTool.cpp - it moved or went; update rule 94, do not let it check nothing")
    }
}
$ranRules.Add('editor-preview-described-once')

# --- 95. THE CTRL-CHORD HANDLER HANDS ITS KEY TO THE ONE KEY HANDLER (#463) -----------------------------------------------
# ARoadBuildController binds a plain key to OnActionKey and a Ctrl chord to OnCtrlActionKey, which polls WasInputKeyJustPressed - a call a headless
# test cannot drive. Each handler once carried its own copy of the modal wait and the TryRun, and AirportMgr.Actions.ChordsWaitUnderModal asserted
# only the predicate (KeyWaitsForModal) the chord handler could stop asking. RunActionForKey is the one handler now - the modal wait, the IsEnabled
# gate and the log line - and the chord handler only finds WHICH key was pressed. In the game module's production sources, comments and strings
# stripped:
# (a) OnCtrlActionKey calls NOTHING but the key poll, the registry and RunActionForKey (an allow-list: BuildActions, IsValid,
#     WasInputKeyJustPressed, RunActionForKey) - so TryRun, Execute, IsEnabled, a handler like OnUndo, or a second gate cannot be added beside the
#     hand-off - and it does call RunActionForKey;
# (b) the half that stops it checking nothing: RunActionForKey still calls KeyWaitsForModal( and TryRun(;
# (c) KeyWaitsForModal( is called from RunActionForKey and from nowhere else, in every non-test .cpp under Source\AirportMgr - the one predicate has
#     one caller (the declaration and the definition are not calls).
# DOES NOT SEE: the poll itself (WasInputKeyJustPressed) picking the wrong key - nothing headless reaches it - or a run path spelled without a
# call-shaped token. AirportMgr.Actions.ChordsWaitUnderModal drives RunActionForKey with the Ctrl chord and asserts the undo it would run does not
# run under a modal.
$controllerSourceFile = Join-Path $Root 'Source\AirportMgr\RoadBuildController.cpp'
if (-not (Test-Path $controllerSourceFile)) {
    $failures.Add("chord-handler-shares-the-gate: $controllerSourceFile is named by rule 95 but does not exist - update the rule, do not let it check nothing")
} else {
    $chordAllowedCalls = @('BuildActions', 'IsValid', 'WasInputKeyJustPressed', 'RunActionForKey')
    $chordSkippedTokens = @('for', 'if', 'while', 'switch', 'return', 'sizeof', 'static_cast', 'TEXT')
    $lines = Get-Content -LiteralPath $controllerSourceFile
    $inBlock = $false
    $current = ''
    $chordRuns = 0
    $gateWaits = 0
    $gateRuns = 0
    $chordSeen = $false
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $code = Strip-ArchCode $lines[$i] ([ref]$inBlock)
        $def = Get-ArchDefinition $code
        if ($null -ne $def) { $current = $def }
        if ($current -eq 'ARoadBuildController::OnCtrlActionKey') {
            $chordSeen = $true
            if ($code -match 'ARoadBuildController::OnCtrlActionKey\s*\(') { continue }
            foreach ($call in [regex]::Matches($code, '\b(\w+)\s*\(')) {
                $callee = $call.Groups[1].Value
                if ($chordSkippedTokens -contains $callee) { continue }
                if ($callee -eq 'RunActionForKey') { $chordRuns++ }
                if ($chordAllowedCalls -notcontains $callee) {
                    $failures.Add("chord-handler-shares-the-gate: RoadBuildController.cpp:$($i + 1) OnCtrlActionKey calls $callee( itself - the chord handler only finds which key was pressed and hands it to RunActionForKey, whose modal wait AirportMgr.Actions.ChordsWaitUnderModal drives; a run path or a gate of its own here is untested (#463): $($code.Trim())")
                }
            }
        }
        if ($current -eq 'ARoadBuildController::RunActionForKey') {
            if ($code -match '\bKeyWaitsForModal\s*\(') { $gateWaits++ }
            if ($code -match '\bTryRun\s*\(') { $gateRuns++ }
        }
    }
    if (-not $chordSeen) {
        $failures.Add("chord-handler-shares-the-gate: ARoadBuildController::OnCtrlActionKey not found in RoadBuildController.cpp - it moved or went; update rule 95, do not let it check nothing")
    } elseif ($chordRuns -lt 1) {
        $failures.Add("chord-handler-shares-the-gate: OnCtrlActionKey no longer calls RunActionForKey( - a Ctrl chord would run its action by some other road, past the modal wait the shared handler holds (#463)")
    }
    if ($gateWaits -lt 1 -or $gateRuns -lt 1) {
        $failures.Add("chord-handler-shares-the-gate: RunActionForKey no longer calls KeyWaitsForModal( and TryRun( ($gateWaits, $gateRuns) - the one key handler lost its gate, or the rule names a function that moved (#463)")
    }
    foreach ($file in Get-Sources (Join-Path $Root 'Source\AirportMgr') @('.cpp')) {
        if ($file.Name -like '*Test.cpp') { continue }
        $lines = Get-Content -LiteralPath $file.FullName
        $inBlock = $false
        $current = ''
        for ($i = 0; $i -lt $lines.Count; $i++) {
            $code = Strip-ArchCode $lines[$i] ([ref]$inBlock)
            $def = Get-ArchDefinition $code
            if ($null -ne $def) { $current = $def }
            if ($code -match '\bKeyWaitsForModal\s*\(' -and $code -notmatch 'ARoadBuildController::KeyWaitsForModal\s*\(') {
                if (-not ($file.FullName -eq $controllerSourceFile -and $current -eq 'ARoadBuildController::RunActionForKey')) {
                    $failures.Add("chord-handler-shares-the-gate: $($file.Name):$($i + 1) calls KeyWaitsForModal( in $current - the modal wait has one caller, RunActionForKey, which both key handlers reach; a second caller is a second gate (#463): $($code.Trim())")
                }
            }
        }
    }
}
$ranRules.Add('chord-handler-shares-the-gate')

# --- 62. A PUBLIC NON-UFUNCTION MEMBER OF ARoadNetworkActor HAS A PRODUCTION CALLER ---------------------------------------------
# Issue #450, the half of rule 20 that issue named. Rule 20 matches `*ForTest(` only, so a member that is test-only under an ORDINARY
# name escaped it: GetSegmentEnds had zero callers, and GetNewestAgent, GetApronSurfaceZ and GetStandDefinitions were called by tests
# alone - a partial regrowth of the #80/#298 shape, one plainly-named forwarder at a time. This reads the actor header's PUBLIC
# section, takes every single-line member declaration that is not a UFUNCTION, an override, a *ForTest (rule 20's own) or a
# destructor, and requires its NAME to be CALLED from some production file. Production = everything under the plugin and game source
# trees except the two test modules and any *Test.cpp.
# WHAT IS NOT A CALL, because each of these made a dead member look alive (found reviewing PR #491: run on caaba9e8 the first draft
# flagged only GetStandDefinitions):
#   (a) the member's OWN declaration (the header line) and its definition line (a column-0 `ARoadNetworkActor::Name(` line in
#       RoadNetworkActor.cpp) - a qualified `ARoadNetworkActor::Name(` anywhere else IS a call;
#   (b) a FORWARDER'S BODY: a call to Name inside `ARoadNetworkActor::Name`'s own definition (`return Facade->GetSegmentEnds(...)`) -
#       the forwarder is the thing being asked about, so it cannot vouch for itself;
#   (c) a same-named DECLARATION in another header (`ARoadAgentActor* GetNewestAgent() const;` on UAirsideTraffic).
# WHAT A REGEX STILL CANNOT SEE, stated rather than claimed around: it matches the NAME and never the receiver, so a same-named CALL on
# another class (`Traffic->GetNewestAgent()`) counts as a call of the actor's member; and a declaration split over several lines is
# neither read as a member nor recognised as a declaration elsewhere. It catches a member whose name nobody calls. The fix is a test
# that goes through GetTraffic()/GetPresenter()/GetEditFacade(), or a deletion; the allow-list below is for a member that is
# DELIBERATELY test-only, dated and reasoned, and is meant to shrink (rule 20's list is, too).
$actorPublicAllowList = @{
    # A read-back of SetSimTimeScale, used by 14 sites in AirportOpsTests (OpsRuntimeTest, PerFrameGateTest) and SimTimeScaleTest to
    # observe that the ops runtime's ApplySpeed reached the actor. Not a forwarder - it returns the actor's own field - but it has no
    # production reader either. Found by this rule's first run, 2026-09-30; retiring it means a test-side read of the member.
    'GetSimTimeScale' = 'a read-back the ops-runtime tests use to observe ApplySpeed (2026-09-30)'
}
$actorHeaderFile = Join-Path $plugin 'Public\Present\RoadNetworkActor.h'
$actorSourceSuffix = 'Private\Present\RoadNetworkActor.cpp'
if (-not (Test-Path $actorHeaderFile)) {
    $failures.Add("actor-public-members-called: $actorHeaderFile is named by rule 62 but does not exist - update the rule, do not let it check nothing")
}
else {
    $actorHeaderLines = Get-Content -LiteralPath $actorHeaderFile
    $actorClassStart = -1
    for ($i = 0; $i -lt $actorHeaderLines.Count; $i++) {
        if ($actorHeaderLines[$i] -match '^class AIRSIDE_API ARoadNetworkActor\b') { $actorClassStart = $i; break }
    }
    if ($actorClassStart -lt 0) {
        $failures.Add("actor-public-members-called: class ARoadNetworkActor not found in $actorHeaderFile - update rule 62, do not let it check nothing")
    }
    else {
        # ONE TAB OF INDENT = a class member (two or more is a body); a return type, a name, a parameter list and a `;` or an inline body.
        $memberDeclaration = '^\t(?!\t)(?:virtual\s+|static\s+|FORCEINLINE\s+|inline\s+)*[\w:<>,\*&\s]+?[\s\*&](\w+)\s*\([^;{]*\)\s*(?:const)?\s*(?:override)?\s*(?:;|\{.*\})\s*$'
        $publicMembers = New-Object System.Collections.Generic.List[object]
        $actorInBlock = $false
        $actorAccess = 'private'
        $actorPrevCode = ''
        for ($i = $actorClassStart; $i -lt $actorHeaderLines.Count; $i++) {
            $code = Strip-ArchCode $actorHeaderLines[$i] ([ref]$actorInBlock)
            if ($code.Trim() -eq '') { continue }
            if ($code -match '^(public|protected|private):') { $actorAccess = $Matches[1]; $actorPrevCode = ''; continue }
            if ($actorAccess -eq 'public' -and $code -match $memberDeclaration) {
                $memberName = $Matches[1]
                $skip = ($actorPrevCode -match 'UFUNCTION') -or ($code -match '^\s*UFUNCTION') -or ($code -match '\boverride\b') `
                    -or ($memberName -like '*ForTest') -or $memberName.StartsWith('~')
                if (-not $skip) { $publicMembers.Add(@{ Name = $memberName; Line = $i + 1 }) }
            }
            $actorPrevCode = $code
        }
        if ($publicMembers.Count -lt 10) {
            $failures.Add("actor-public-members-called: only $($publicMembers.Count) public member declarations parsed from $actorHeaderFile - the parse went wrong (expected dozens); update rule 62, do not let it check nothing")
        }
        else {
            $memberNames = @($publicMembers | ForEach-Object { $_.Name } | Sort-Object -Unique)
            $nameAlternation = ($memberNames | ForEach-Object { [regex]::Escape($_) }) -join '|'
            # A CALL: the name, optionally qualified by THIS class (a qualified call of another class's same-named member is left out by
            # the lookbehind, which a qualifier-less `::Name(` would otherwise pass).
            $callPattern = [regex]('(?<![\w:])(?<qual>ARoadNetworkActor::)?(?<name>' + $nameAlternation + ')\s*\(')
            $declarationShaped = @{}
            foreach ($name in $memberNames) {
                $declarationShaped[$name] = [regex]('^\s*(?:\[\[\w+\]\]\s*)?(?:UFUNCTION\([^)]*\)\s*)?(?:virtual\s+|static\s+|inline\s+|FORCEINLINE\s+)*(?!return\b)[\w:<>,\*&]+(?:\s+[\w:<>,\*&]+)*[\s\*&]' + [regex]::Escape($name) + '\s*\([^;{]*\)\s*(?:const)?\s*(?:override)?\s*(?:final)?\s*;\s*$')
            }
            $called = @{}
            $sourcesRead = 0
            foreach ($tree in $trees) {
                foreach ($file in Get-Sources $tree @('.h', '.cpp')) {
                    if ($file.FullName -match '\\(AirsideTests|AirportOpsTests)\\') { continue }
                    if ($file.Name -like '*Test.cpp' -or $file.Name -like '*Tests.cpp') { continue }
                    $sourcesRead++
                    $isActorHeader = ($file.FullName -eq $actorHeaderFile)
                    $isActorSource = (Test-AllowedPathSuffix $file $actorSourceSuffix)
                    $isHeader = ($file.Extension -eq '.h')
                    $fileLines = [System.IO.File]::ReadAllLines($file.FullName)
                    # WHICH FUNCTION AM I IN, read only where a forwarder body can be: the actor's own .cpp.
                    $currentDefinition = ''
                    $definitionInBlock = $false
                    for ($n = 0; $n -lt $fileLines.Length; $n++) {
                        $text = $fileLines[$n]
                        if ($isActorSource) {
                            $definitionCode = Strip-ArchCode $text ([ref]$definitionInBlock)
                            $definitionName = Get-ArchDefinition $definitionCode
                            if ($null -ne $definitionName) { $currentDefinition = $definitionName }
                        }
                        if ($text.IndexOf('(') -lt 0) { continue }
                        $trimmed = $text.TrimStart()
                        if ($trimmed.StartsWith('//') -or $trimmed.StartsWith('*') -or $trimmed.StartsWith('/*')) { continue }
                        foreach ($m in $callPattern.Matches($text)) {
                            $callee = $m.Groups['name'].Value
                            $qualified = $m.Groups['qual'].Success
                            # (a) ITS OWN DECLARATION, and ITS OWN DEFINITION LINE (column 0 of the actor's .cpp).
                            if ($isActorHeader -and ($publicMembers | Where-Object { $_.Name -eq $callee -and $_.Line -eq ($n + 1) })) { continue }
                            if ($isActorSource -and $qualified -and $text.Length -gt 0 -and -not [char]::IsWhiteSpace($text[0])) { continue }
                            # (b) A FORWARDER'S BODY: the call sits inside ARoadNetworkActor::<itself>.
                            if ($isActorSource -and $currentDefinition -eq ('ARoadNetworkActor::' + $callee)) { continue }
                            # (c) A SAME-NAMED DECLARATION in another header.
                            if ($isHeader -and -not $isActorHeader -and -not $qualified -and $declarationShaped[$callee].IsMatch($text)) { continue }
                            $called[$callee] = $true
                        }
                    }
                }
            }
            if ($sourcesRead -lt 200) {
                $failures.Add("actor-public-members-called: rule 62 read only $sourcesRead production file(s) - the trees moved, or the rule checks nothing (#450)")
            }
            foreach ($member in $publicMembers) {
                if ($called.ContainsKey($member.Name)) { continue }
                if ($actorPublicAllowList.ContainsKey($member.Name)) { continue }
                $failures.Add("actor-public-members-called: $($actorHeaderFile):$($member.Line) $($member.Name)( is a public member of ARoadNetworkActor that no production file calls - a test wants GetTraffic()/GetPresenter()/GetEditFacade()->..., or the member is dead and goes (issue #450, the #80/#298 shape under an ordinary name)")
            }
            foreach ($allowed in $actorPublicAllowList.Keys) {
                if (-not ($memberNames -contains $allowed)) {
                    $failures.Add("actor-public-members-called: '$allowed' is in rule 62's allow-list but is no longer a public member of ARoadNetworkActor - delete the entry, the list is meant to shrink")
                }
                elseif ($called.ContainsKey($allowed)) {
                    $failures.Add("actor-public-members-called: '$allowed' is in rule 62's allow-list but now has a production caller - delete the entry, the list is meant to shrink")
                }
            }
        }
    }
}
$ranRules.Add('actor-public-members-called')

# --- 59. THE SAFETY-NET CAUSE IS MINTED BY THE NET ALONE, AND THE PASS BOOKKEEPING STAYS OUT OF THE ROOT -------
# #445, REPLACING rule 36. The job board's and the arrival queue's passes told a run an event asked for from a run only the net asked for by a
# hand-set pair of flags on UOpsRuntime (covered / safety due) plus a wanted bool, a funnel function each and a frame counter for
# one-clearance-a-frame - about nine fields the class's own contract ("GROWS BY FORWARDING") called false, which review corrected three times.
# The bus carries the cause for EVERY pass now (MarkDirty(Pass, EPassCause), FPassRun) and FOpsSafetyNet owns the net, so the one way left to
# get it wrong is to mark a pass as the net's from somewhere that is not the net: the run would then be suspect for finding work, and a
# real event's run would be logged as a missing event. So:
#   a. EPassCause::SafetyNet is named in code only by OpsEventBus.h/.cpp (the enum and its default) and OpsSafetyNet.cpp (Fire) in production;
#      tests may name it (they stage a net run).
#   b. UOpsRuntime.h/.cpp declare no pass-why bookkeeping again: no `b...SafetyDue` / `b...Covered` / `b...NetWanted` / `...ClearedFrame`
#      field, no DrainFrame, no DirtyJobBoard/DirtyArrivalQueue funnel. A fourth net-watched pass is one Want() call on the net.
# WHAT NO REGEX SEES: a caller that builds the cause from a variable or a cast (`static_cast<EPassCause>(0)`) - the enum is three values and
# the test AirportOps.Model.Bus.PassRunCarriesItsCause pins what each means. The rule FAILS rather than checking nothing when the net no
# longer names the cause.
$causeAllowed = @('Public\Model\OpsEventBus.h', 'Private\Model\OpsEventBus.cpp', 'Private\Model\OpsSafetyNet.cpp')
$causeMinter = Join-Path $ops 'Private\Model\OpsSafetyNet.cpp'
$causeMinterSeen = $false
foreach ($causeTree in @($ops, (Join-Path $Root 'Source\AirportMgr'))) {
    foreach ($file in Get-Sources $causeTree @('.cpp', '.h')) {
        if ($file.Name -like '*Test.cpp' -or $file.Name -like '*Test.h') { continue }
        $causeOk = $false
        foreach ($allowedFile in $causeAllowed) { if (Test-AllowedPathSuffix $file $allowedFile) { $causeOk = $true } }
        $lines = Get-Content -LiteralPath $file.FullName
        $inBlock = $false
        for ($i = 0; $i -lt $lines.Count; $i++) {
            $code = Strip-ArchCode $lines[$i] ([ref]$inBlock)
            if ($code -notmatch '\bEPassCause::SafetyNet\b') { continue }
            if ($causeOk) {
                if ($file.FullName -eq $causeMinter) { $causeMinterSeen = $true }
                continue
            }
            $failures.Add("safety-cause-minted-by-the-net: $($file.Name):$($i + 1) names EPassCause::SafetyNet - only FOpsSafetyNet marks a pass as the net's; an event's run marked so is reported as a missing event (#445): $($code.Trim())")
        }
    }
}
if (-not (Test-Path $causeMinter)) {
    $failures.Add("safety-cause-minted-by-the-net: $causeMinter is named by rule 59 but does not exist - update the rule, do not let it check nothing")
}
elseif (-not $causeMinterSeen) {
    $failures.Add("safety-cause-minted-by-the-net: OpsSafetyNet.cpp no longer names EPassCause::SafetyNet - the net moved or was renamed; update rule 59, do not let it check nothing")
}
foreach ($rootFile in @((Join-Path $ops 'Public\Present\OpsRuntime.h'), (Join-Path $ops 'Private\Present\OpsRuntime.cpp'))) {
    if (-not (Test-Path $rootFile)) {
        $failures.Add("safety-cause-minted-by-the-net: $rootFile is named by rule 59 but does not exist - update the rule")
        continue
    }
    $lines = Get-Content -LiteralPath $rootFile
    $inBlock = $false
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $code = Strip-ArchCode $lines[$i] ([ref]$inBlock)
        if ($code -match '\b(?:bool|uint64)\s+b?\w*(?:SafetyDue|Covered|NetWanted|ClearedFrame)\b|\bDrainFrame\b|\bDirty(?:JobBoard|ArrivalQueue)\s*\(') {
            $failures.Add("safety-cause-minted-by-the-net: $([System.IO.Path]::GetFileName($rootFile)):$($i + 1) brings pass bookkeeping back onto the composition root - the cause is the bus's (FPassRun), the net is FOpsSafetyNet's, one-clearance-a-frame is the flight board's (#445): $($code.Trim())")
        }
    }
}
$ranRules.Add('safety-cause-minted-by-the-net')

# --- 71. EVERY OBJECT HOLDING A RAW BUS POINTER IS LISTED IN UOpsRuntime::Publishers ------------------------------
# #445: the list of objects holding `FOpsEventBus* Bus` was kept by hand in three places - Attach, Detach and the Detach test - and they
# already disagreed (UFacilityPurchases was set in the constructor, never cleared, and in none of them). It is ONE list now, like
# Persistents(): UOpsRuntime::Publishers(), which Attach and Detach loop over and the test walks. What a loop cannot see is a class that
# declares the field and is left off the list, so this compares the two lists BY IDENTITY, not by count (CLAUDE.md: lists that must agree are
# checked by names - a count passes when one class is swapped for another): every class declaring a `FOpsEventBus* Bus = nullptr;` field in a
# header under AirportOps Public/ (FOpsSafetyNet's own binding is skipped by file: it marks passes, it publishes nothing) against the TYPES of
# the members Publishers() lists - `Out.Add({ TEXT("X"), &Member->Bus })` names a member, and OpsRuntime.h declares its `TObjectPtr<Type>`.
# Both differences are named. And the hand-kept shape does not come back: no `X->Bus = &Bus;` line in OpsRuntime.cpp.
# WHAT NO REGEX SEES: a publisher that reaches the bus another way (a captured `Bus` reference, a getter) - there are none today, and the
# header says to add a field and a list entry. The rule FAILS rather than checking nothing when Publishers(), a declaration or a member's type
# is gone.
$publisherFile = Join-Path $ops 'Private\Present\OpsRuntime.cpp'
$publisherHeader = Join-Path $ops 'Public\Present\OpsRuntime.h'
$publisherDeclared = @{}
foreach ($file in Get-Sources (Join-Path $ops 'Public') @('.h')) {
    if ($file.Name -eq 'OpsSafetyNet.h') { continue }
    $lines = Get-Content -LiteralPath $file.FullName
    $inBlock = $false
    $currentClass = ''
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $code = Strip-ArchCode $lines[$i] ([ref]$inBlock)
        if ($code -match '^\s*(?:class|struct)\s+(?:[A-Z_]+_API\s+)?(\w+)\s*(?::|\{|$)') { $currentClass = $Matches[1] }
        if ($code -match '\bFOpsEventBus\s*\*\s*Bus\s*=\s*nullptr\s*;') {
            $publisherDeclared[$currentClass] = "$($file.Name):$($i + 1)"
        }
    }
}
if (-not (Test-Path $publisherFile) -or -not (Test-Path $publisherHeader)) {
    $failures.Add("every-bus-publisher-is-listed: $publisherFile or $publisherHeader is named by rule 71 but does not exist - update the rule")
}
else {
    $headerText = Get-Content -Raw -LiteralPath $publisherHeader
    $lines = Get-Content -LiteralPath $publisherFile
    $inBlock = $false
    $inList = $false
    $listFound = $false
    $publisherListed = @{}
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $code = Strip-ArchCode $lines[$i] ([ref]$inBlock)
        if ($lines[$i] -match '^TArray<UOpsRuntime::FOpsBusPublisher>\s+UOpsRuntime::Publishers\s*\(') { $inList = $true; $listFound = $true }
        elseif ($inList -and $lines[$i] -match '^\}') { $inList = $false }
        elseif ($inList -and $lines[$i] -match '\bOut\.Add\(\s*\{[^&]*&\s*(\w+)\s*->\s*Bus\b') {
            $member = $Matches[1]
            if ($headerText -match "TObjectPtr<(\w+)>\s+$member\s*;") {
                $publisherListed[$Matches[1]] = $member
            }
            else {
                $failures.Add("every-bus-publisher-is-listed: Publishers() lists member '$member' but OpsRuntime.h declares no TObjectPtr<Type> $member - the rule cannot name its class; update rule 71, do not let it check nothing")
            }
        }
        if ($code -match '\b\w+\s*->\s*Bus\s*=\s*&\s*Bus\s*;') {
            $failures.Add("every-bus-publisher-is-listed: OpsRuntime.cpp:$($i + 1) points a publisher at the bus by hand - add it to UOpsRuntime::Publishers(), which Attach and Detach loop over (#445): $($code.Trim())")
        }
    }
    if (-not $listFound) {
        $failures.Add("every-bus-publisher-is-listed: UOpsRuntime::Publishers() not found in OpsRuntime.cpp - the list moved; update rule 71, do not let it check nothing")
    }
    elseif ($publisherDeclared.Count -eq 0) {
        $failures.Add("every-bus-publisher-is-listed: found no 'FOpsEventBus* Bus = nullptr;' field under AirportOps Public/ - the declaration's shape changed; update rule 71, do not let it check nothing")
    }
    else {
        foreach ($class in $publisherDeclared.Keys) {
            if (-not $publisherListed.ContainsKey($class)) {
                $failures.Add("every-bus-publisher-is-listed: $class declares an FOpsEventBus* Bus ($($publisherDeclared[$class])) but UOpsRuntime::Publishers() does not list it - it is never pointed at the bus, or never taken back (#445)")
            }
        }
        foreach ($class in $publisherListed.Keys) {
            if (-not $publisherDeclared.ContainsKey($class)) {
                $failures.Add("every-bus-publisher-is-listed: UOpsRuntime::Publishers() lists $class (member $($publisherListed[$class])) but it declares no FOpsEventBus* Bus field - the entry points at nothing, or the field was renamed (#445)")
            }
        }
    }
}
$ranRules.Add('every-bus-publisher-is-listed')

# --- 72. A SAVE AND A LOAD ASK THE BUS WHETHER IT IS DRAINING ------------------------------------------------------
# #445: SaveToSlot guards a call from inside a drain (a Blueprint autosave on a notification would re-enter Drain and assert); LoadFromSlot had no
# guard, and from a Presentation handler it would Discard the queue while Drain still held its moved-out batch - the rest of it, events naming the
# agents of the airport being replaced, dispatched against the restored boards. Both bodies must ask Bus.IsDraining(); the test
# AirportOps.Present.Bus.LoadFromAHandlerIsRefused measures what the load does when it is. WHAT NO REGEX SEES: a guard that asks and does the
# wrong thing - that is the test's. The rule FAILS rather than checking nothing when either body is gone.
$reentryFile = Join-Path $ops 'Private\Present\OpsRuntime.cpp'
if (Test-Path $reentryFile) {
    $reentryLines = Get-Content -LiteralPath $reentryFile
    foreach ($entry in @('SaveToSlot', 'LoadFromSlot')) {
        $inBody = $false
        $found = $false
        $asks = $false
        $inBlock = $false
        for ($i = 0; $i -lt $reentryLines.Count; $i++) {
            $code = Strip-ArchCode $reentryLines[$i] ([ref]$inBlock)
            if ($reentryLines[$i] -match "^bool\s+UOpsRuntime::$entry\s*\(") { $inBody = $true; $found = $true }
            elseif ($inBody -and $reentryLines[$i] -match '^\}') { $inBody = $false }
            elseif ($inBody -and $code -match '\bBus\s*\.\s*IsDraining\s*\(') { $asks = $true }
        }
        if (-not $found) {
            $failures.Add("bus-reentry-guarded: UOpsRuntime::$entry not found in OpsRuntime.cpp - update rule 72, do not let it check nothing")
        }
        elseif (-not $asks) {
            $failures.Add("bus-reentry-guarded: UOpsRuntime::$entry never asks Bus.IsDraining() - called from a Presentation handler it would re-enter the drain or discard its batch (#445)")
        }
    }
}
else {
    $failures.Add("bus-reentry-guarded: $reentryFile is named by rule 72 but does not exist - update the rule")
}
$ranRules.Add('bus-reentry-guarded')

# --- 77. A MODEL FILE DOES NOT GROW BACK: A LINE BUDGET THAT MAY ONLY FALL (#427) ----------------------------------------
# UJobBoard reached 3,331 lines on 2026-09-30 (JobBoard.h 816, JobBoard.cpp 1493, JobBoardBid.cpp 639, JobBoardDrive.cpp 383)
# and about fourteen responsibilities - the #32 shape ARoadNetworkActor had at 2,313 - because every feature entered through
# the one class and nothing said no. #427 and the PRs before it took the owners out (FServiceFleet, Airside's SendAgentTo,
# FTurnarounds, ServiceText); this is what keeps the next feature from walking back in. THE SHAPE, by its symptom: every
# Plugins\*\Source\*\Private\Model\*.cpp is held to $modelLineDefault lines, and a file already over that on the date below
# is held to its own figure. A RATCHET, three ways:
#  (a) a file over its figure fails. A new responsibility shrinks something, or its PR names the raise and edits the row
#      with the reason - a raise with none is the rule switched off, which review refuses (rule 49's convention);
#  (b) a listed file that has FALLEN more than $modelLineSlack lines below its figure fails too, until the figure is lowered
#      to today's count: a ceiling nobody lowers lets a file grow back into the room an extraction made;
#  (c) a listed file that is gone, or now fits the default, fails until its row goes, so the list cannot rot into a
#      record of files that were once big.
# Every line counts, a WHY comment included (rule 49's reason: a budget must not be gameable by deleting one).
# RoadNetwork.cpp's figure was raised 2200 -> 2287 by #495 (#490/#450 load migrations: the depot number backfill, EnsureStandFrontages and the stand-entrance
# default in PlaceEntity, each with its WHY comment) - the one raise, named in the row, that this rule's (a) asks for.
# THE FIGURES ARE 2026-10-01's, measured after #427's extraction (JobBoard.cpp 1558 at main 9bb30096 -> 1048 at d2bb63cd,
# #491's lines included). The default is 800: on that
# date the largest Model file under it was VehicleFit.cpp at 768, so no file was given room it had never had.
# DOES NOT SEE: a responsibility added to a NEW file beside the old one (which is the extraction this wants, and review's to
# judge), a header, or a Public\ file. Nor does it see a function moved to a sibling .cpp of the same class to dodge a
# figure - JobBoardBid.cpp and JobBoardDrive.cpp are held by the default like any other file.
# EACH ROW CARRIES ITS DATED REASON (#494 review): what the file held on the date it was measured, and why that was not yet
# split - so a raise has a sentence to argue with, and a row whose reason has gone stale is visibly so.
$modelLineDefault = 800
$modelLineSlack = 50
$modelLineBudget = [ordered]@{
    # 2026-10-01: the graph's whole authoring and query surface (111 member definitions - nodes, segments, entities, runway
    # facts, restore). One class's API, not a pile of responsibilities; no extraction named yet. RAISED 2200 -> 2287 the same
    # day by #495: #490/#450 load migrations (depot backfill in EnsureStandNumbers, EnsureStandFrontages, the stand-entrance
    # default in PlaceEntity).
    'Plugins\Airside\Source\Airside\Private\Model\RoadNetwork.cpp'            = 2287
    # 2026-10-01: the traffic Mediator - dispatch, Announce, goals, depart, holds, Advance and the freed diff. #429 and #436
    # took route changes and transitions to one door each; the rebuild half already lives in GroundTrafficRebuild.cpp.
    # LOWERED 2121 -> 1954 the same day by #444: the retry pass and its two arms went to GroundTrafficWaiters.cpp (one
    # responsibility). RAISED 1954 -> 1955 the same day by #497: the refusal's sentence handed to OnArrivalRefused (#471).
    'Plugins\Airside\Source\Airside\Private\Model\GroundTraffic.cpp'          = 1955
    # 2026-10-01: the claim arbiter, one algorithm (Run and its windows, crossings and ranking - 13 functions, long ones);
    # splitting it would scatter one invariant across files.
    'Plugins\Airside\Source\Airside\Private\Model\TrafficClaims.cpp'          = 1898
    # 2026-10-01: a flight's lifecycle - offers, quotes, accept, the arrival queue, cancels, restore, fees (50 member
    # definitions); one class's state machine, not yet split. RAISED 1746 -> 1779 the same day by #497: UFlightBoard::Rehold
    # (every re-hold a plan's stand) and the queue pass's call into the stand-hold rule - the rule itself went to
    # UStandAllocator::Reconcile; #442 item 4's UArrivalQueue is where the queue's share belongs. AND 1779 -> 1803 by the #497
    # review: a failed re-hold dated (FReholdMiss) and a failed dispatch's re-hold clearing its copy.
    'Plugins\AirportOps\Source\AirportOps\Private\Model\FlightBoard.cpp'      = 1803
    # 2026-10-01: one agent's follower - engine, gear, taxi, tow, pushback and reverse legs (32 member definitions); one
    # struct's motion, not yet split. RAISED 1583 -> 1610 the same day by #444: the wait's one door (WaitFor/EndWait, in place
    # of three flag mutators and a bare write) and the exhaustive-switch reasons on DescribeMotion and Advance (a phase added is
    # a build error there now). No natural cut: what is left of the size is Advance's per-phase arms and the engine/gear
    # state, the struct's own; a sibling file would only dodge.
    'Plugins\Airside\Source\Airside\Private\Model\RoadAgent.cpp'              = 1610
    # 2026-10-01: re-resolution after a graph rebuild (splice, re-resolve, rescue) - already the extraction out of
    # GroundTraffic.cpp; 7 functions, each long. RAISED 1582 -> 1589 the same day by #444: the retarget branch's guard for a
    # held departure, with its reason and its test. RAISED 1589 -> 1591 by #497: who settles a refused re-hold (#442).
    # RAISED 1591 -> 1610 by #498: a pushing departure's push and taxi out each re-resolved to its own goal (a line each, the
    # push's false "NOT REPLANNED" comment rewritten as its contract) and the rebuild's three lines naming the plan; reasons and pins.
    'Plugins\Airside\Source\Airside\Private\Model\GroundTrafficRebuild.cpp'   = 1610
    # 2026-10-01: the route search (A* over the guideline graph, plan building, run description) - one algorithm.
    'Plugins\Airside\Source\Airside\Private\Model\RouteSearch.cpp'            = 1191
    # 2026-10-01: what #427 left of UJobBoard - the jobs, the vehicles' lifecycle and Step's one sequence; bidding and
    # driving are already JobBoardBid.cpp and JobBoardDrive.cpp.
    'Plugins\AirportOps\Source\AirportOps\Private\Model\JobBoard.cpp'         = 1048
    # 2026-10-01: the ArrivalPlanner namespace - runway, exit and stand choice for one arrival; one planner, not yet split.
    # RAISED 943 -> 957 the same day by the #497 review: WhyEveryStandRefused asked before the exits' searches (the Land panel's
    # hitch) and the wording helpers asked reach without occupancy, each with its reason.
    'Plugins\Airside\Source\Airside\Private\Model\ArrivalPlanner.cpp'         = 957
}
$modelLineFiles = @()
foreach ($pluginDir in (Get-ChildItem -LiteralPath (Join-Path $Root 'Plugins') -Directory)) {
    $sourceDir = Join-Path $pluginDir.FullName 'Source'
    if (-not (Test-Path $sourceDir)) { continue }
    foreach ($moduleDir in (Get-ChildItem -LiteralPath $sourceDir -Directory)) {
        $modelDir = Join-Path $moduleDir.FullName 'Private\Model'
        if (-not (Test-Path $modelDir)) { continue }
        $modelLineFiles += @(Get-ChildItem -LiteralPath $modelDir -File -Filter '*.cpp')
    }
}
if ($modelLineFiles.Count -lt 40) {
    $failures.Add("model-line-budget: rule 77 found only $($modelLineFiles.Count) Plugins\*\Source\*\Private\Model\*.cpp file(s) - the trees moved, or the rule checks nothing (#427)")
}
$modelLineSeen = @{}
foreach ($file in $modelLineFiles) {
    $relative = $file.FullName.Substring($Root.TrimEnd('\', '/').Length + 1).Replace('/', '\')
    $count = @(Get-Content -LiteralPath $file.FullName).Count
    $key = $null
    foreach ($candidate in $modelLineBudget.Keys) {
        if ($candidate -ieq $relative) { $key = $candidate; break }
    }
    if ($null -eq $key) {
        if ($count -gt $modelLineDefault) {
            $failures.Add("model-line-budget: $relative is $count lines, over the $modelLineDefault every Model .cpp is held to - a new responsibility belongs in an owner of its own (FTurnarounds, ServiceText, FServiceFleet are the pattern), or the PR names why this file grows and adds a dated row to rule 77 (#427)")
        }
        continue
    }
    $modelLineSeen[$key] = $true
    $figure = $modelLineBudget[$key]
    if ($count -gt $figure) {
        $failures.Add("model-line-budget: $relative is $count lines, over its figure of $figure - shrink something, or raise the row in rule 77 with the reason in the PR; the figures may only fall (#427)")
    } elseif ($count -le $modelLineDefault) {
        $failures.Add("model-line-budget: $relative is $count lines, inside the default of $modelLineDefault - remove its row from rule 77, the default holds it now (#427)")
    } elseif ($count -lt $figure - $modelLineSlack) {
        $failures.Add("model-line-budget: $relative is $count lines, more than $modelLineSlack under its figure of $figure - lower the row in rule 77 to $count, so the room it made cannot be grown back into (#427)")
    }
}
foreach ($key in $modelLineBudget.Keys) {
    if (-not $modelLineSeen.ContainsKey($key)) {
        $failures.Add("model-line-budget: rule 77 holds $key to $($modelLineBudget[$key]) lines, but no such Model .cpp exists - remove or rename its row (#427)")
    }
}
$ranRules.Add('model-line-budget')

# --- 84. A STAND HOLD IS A PLAN'S STAND - JUDGED BY STATEMENT, NOT BY LINE (#471, #497 review) ---------------------------
# UStandAllocator::Hold(Traffic, Network, Flight, Stand) is called by UFlightBoard alone - TryAccept with its quote's stand,
# Rehold with a fresh plan's - and with nothing but an FArrivalQuote's Stand: a stand picked any other way (by size, by name,
# by the flight's own last Stand) is a re-hold that skipped the plan, the reach-blind shape #471 removed with Reserve. These
# were two rule-4 rows until the #497 review, and a row matches ONE LINE, so a call wrapped over two lines, or one with a dotted
# argument (`*Rig.Net`), passed both. Now each production ops and game file's code (comments and strings stripped) is JOINED
# into statements the way rule 58 joins them (to `;`, `{`, `}` or `:`, at most eight lines); every `.Hold(` / `->Hold(` call's
# arguments are split at bracket depth 0; and a call of FOUR - FTestTwoRunways::Hold takes three - must be in FlightBoard.cpp
# with a last argument of `Quote.Stand` or `Plan.Stand`. Tests are exempt: they hold a fixture's stand by hand.
# DOES NOT SEE: a Hold reached through a member pointer, a plan's stand copied into a local of another name first (or any other
# stand put in a local NAMED Plan), a template argument list with a comma inside it, a statement longer than eight lines.
# Mutation-checked 2026-10-01: a call outside the board, a call wrapped over two lines with a dotted argument, and a by-size
# stand each FAIL. The rule fails when it sees no four-argument Hold call at all - the board's two are its shape.
$holdTrees = @($ops, (Join-Path $Root 'Source\AirportMgr'))
$holdCallsSeen = 0
foreach ($holdTree in $holdTrees) {
    foreach ($file in Get-Sources $holdTree @('.cpp', '.h')) {
        if ($file.Name -like '*Test.cpp' -or $file.Name -like '*Test.h' -or $file.FullName -match '[\\/](Testing|AirportOpsTests)[\\/]') { continue }
        $holdLines = Get-Content -LiteralPath $file.FullName
        $holdInBlock = $false
        $holdStatement = ''
        $holdStart = 0
        $holdCount = 0
        for ($i = 0; $i -le $holdLines.Count; $i++) {
            $holdFlush = $false
            if ($i -eq $holdLines.Count) {
                $holdFlush = $holdStatement -ne ''
            } else {
                $trimmed = (Strip-ArchCode $holdLines[$i] ([ref]$holdInBlock)).Trim()
                if ($trimmed -eq '') {
                    $holdFlush = $holdStatement -ne ''
                } else {
                    if ($holdStatement -eq '') { $holdStart = $i }
                    $holdStatement += ' ' + $trimmed
                    $holdCount++
                    $holdFlush = ($trimmed -match '[;{}:]$') -or ($holdCount -ge 8)
                }
            }
            if (-not $holdFlush) { continue }
            foreach ($call in [regex]::Matches($holdStatement, '(?:\.|->)\s*Hold\s*\(')) {
                $holdArgs = New-Object System.Collections.Generic.List[string]
                $depth = 0
                $current = ''
                for ($k = $call.Index + $call.Length; $k -lt $holdStatement.Length; $k++) {
                    $ch = $holdStatement[$k]
                    if ($ch -eq '(' -or $ch -eq '[' -or $ch -eq '{') { $depth++ }
                    elseif ($ch -eq ')' -or $ch -eq ']' -or $ch -eq '}') {
                        if ($depth -eq 0) { $holdArgs.Add($current.Trim()); break }
                        $depth--
                    }
                    elseif ($ch -eq ',' -and $depth -eq 0) { $holdArgs.Add($current.Trim()); $current = ''; continue }
                    $current += $ch
                }
                if ($holdArgs.Count -ne 4) { continue }
                $holdCallsSeen++
                if ($file.Name -ne 'FlightBoard.cpp') {
                    $failures.Add("stand-hold-is-a-plans-stand: $($file.FullName):$($holdStart + 1) calls UStandAllocator::Hold outside UFlightBoard - hold through TryAccept or Rehold, which hold the stand a plan taxis to (#471): $($holdStatement.Trim())")
                }
                elseif ($holdArgs[3] -notmatch '^(Quote|Plan)\.Stand$') {
                    $failures.Add("stand-hold-is-a-plans-stand: $($file.FullName):$($holdStart + 1) holds a stand no arrival plan chose ($($holdArgs[3])) - hold an FArrivalQuote's Stand, from PlanQuote or the cached verdict (#471): $($holdStatement.Trim())")
                }
            }
            $holdStatement = ''
            $holdCount = 0
        }
    }
}
if ($holdCallsSeen -eq 0) {
    $failures.Add("stand-hold-is-a-plans-stand: rule 84 found no four-argument Hold call - UFlightBoard::TryAccept and Rehold make two; the rule checks nothing, update it (#471)")
}
$ranRules.Add('stand-hold-is-a-plans-stand')

# --- 78. "A TURNAROUND BEGAN" IS ONE DERIVATION, READ BY BOTH BOARDS (#427) ------------------------------------------------
# UFlightBoard (does a Parked flight enter Turnaround, and on which stand) and UJobBoard (does a Parked aircraft open a
# turnaround) each called StandAtNode for itself, behind different gates - the flight board on To == Parked, the job board on
# the Parked cause - so the two derivations of one fact agreed only while the traffic model kept reporting every Parked phase
# with the Parked cause. FTurnarounds::BeganAt is the one derivation now. THE SHAPE removed is a second call, so:
#  (a) `StandAtNode(` appears in production code only ON its declaration line in Flight.h, ON its definition line in
#      Flight.cpp, and INSIDE FTurnarounds::BeganAt's body in Turnarounds.cpp - comments and strings stripped, test files
#      exempt (a test helper may ask a node). BY LINE AND BY FUNCTION, not by file (#494 review): a file allow-list let a
#      second call into Flight.cpp - FlightPhaseFromTransition asking the node for itself, the very derivation BeganAt
#      replaced - or into any other function of Turnarounds.cpp, and the rule stayed green. "Which function" is
#      Get-ArchDefinition, as (b) reads it; a declaration or definition line is one at column 0 that Get-ArchDefinition
#      names StandAtNode;
#  (b) both boards READ the one derivation: `FTurnarounds::BeganAt(` / `BeganAt(` is called inside UFlightBoard::OnAgentPhase
#      (FlightBoard.cpp) and inside FTurnarounds::OnAircraftPhase (Turnarounds.cpp);
#  (c) THE FRIEND TOUCHES WHAT IT WAS LET IN FOR. FTurnarounds is a friend of UJobBoard (as FServiceFleet is), and a friend
#      sees every private member - so every `R.X` / `R->X` in Turnarounds.cpp AND Turnarounds.h, where R is `Board` or any
#      name either file declares as a `UJobBoard&` / `UJobBoard*` (a parameter renamed `Jobs`, an alias), names a member
#      of $beganBoardReach (#494 review: the identifier Board alone let a renamed parameter, or an inline body in the
#      header, reach anything):
#      the counter it moves (RevisionCount), the two job doors (OpenJob, DropJobsOf), and the board's public reads and
#      wiring. A write of the board's Jobs, Vehicles or NextJobId, a vehicle transition (Lifecycle) or a recall from here
#      would put a second owner on the jobs - the shape #427 took apart.
# WHAT NO REGEX SEES: a third board deriving the fact some other way (reading the live agent's goal, say), and in (c) a
# receiver reached through `auto&` or a member pointer rather than a declared UJobBoard reference. The behaviour
# half is AirportOps.Model.Turnarounds.BothBoardsBeginAtTheOneStand, and the stale-goal half AirportOps.Model.Bus.SameFrameRedirectStaysTaxiIn.
# MUTATION-CHECKED 2026-10-01, each red alone: StandAtNode called inside FlightPhaseFromTransition (Flight.cpp) and inside a
# second Turnarounds.cpp function; a `UJobBoard& Jobs` parameter writing Jobs.Vehicles in Turnarounds.cpp; an inline
# Board.NextJobId read in Turnarounds.h.
$beganDeclarers = @('Public\Model\Flight.h', 'Private\Model\Flight.cpp')
$beganReader = 'Private\Model\Turnarounds.cpp'
$beganFiles = 0
foreach ($tree in $trees) {
    foreach ($file in Get-Sources $tree @('.cpp', '.h')) {
        if ($file.Name -like '*Test.cpp' -or $file.Name -like '*Test.h' -or $file.Name -like '*TestHelpers.h' -or $file.FullName -match '[\\/](Testing|AirsideTests|AirportOpsTests)[\\/]') { continue }
        $beganFiles++
        $isDeclarer = $false
        foreach ($suffix in $beganDeclarers) { if (Test-AllowedPathSuffix $file $suffix) { $isDeclarer = $true; break } }
        $isReader = Test-AllowedPathSuffix $file $beganReader
        $lines = Get-Content -LiteralPath $file.FullName
        $inBlock = $false
        $current = ''
        for ($i = 0; $i -lt $lines.Count; $i++) {
            $code = Strip-ArchCode $lines[$i] ([ref]$inBlock)
            $def = Get-ArchDefinition $code
            if ($null -ne $def) { $current = $def }
            if ($code -notmatch '\bStandAtNode\s*\(') { continue }
            # THE DECLARATION OR DEFINITION ITSELF: at column 0, and the name Get-ArchDefinition reads off it is StandAtNode.
            if ($isDeclarer -and $def -eq 'StandAtNode' -and $code -match '^[A-Za-z_]') { continue }
            # THE ONE READER: inside BeganAt's body, by the function the line sits in.
            if ($isReader -and $current -eq 'FTurnarounds::BeganAt') { continue }
            $where = if ($current -ne '') { " (in $current)" } else { '' }
            $failures.Add("turnaround-began-once: $($file.FullName):$($i + 1) calls StandAtNode$where - ask FTurnarounds::BeganAt, the one derivation of 'a turnaround began' both boards read (#427): $($code.Trim())")
        }
    }
}
if ($beganFiles -lt 50) {
    $failures.Add("turnaround-began-once: rule 78 read only $beganFiles production file(s) - the trees moved, or the rule checks nothing (#427)")
}
foreach ($reader in @(
        @{ File = (Join-Path $ops 'Private\Model\FlightBoard.cpp'); Function = 'UFlightBoard::OnAgentPhase' },
        @{ File = (Join-Path $ops 'Private\Model\Turnarounds.cpp'); Function = 'FTurnarounds::OnAircraftPhase' })) {
    if (-not (Test-Path $reader.File)) {
        $failures.Add("turnaround-began-once: $($reader.File) is named by rule 78 but does not exist - update the rule, do not let it check nothing")
        continue
    }
    $lines = Get-Content -LiteralPath $reader.File
    $inBlock = $false
    $current = ''
    $seen = $false
    $reads = $false
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $code = Strip-ArchCode $lines[$i] ([ref]$inBlock)
        $def = Get-ArchDefinition $code
        if ($null -ne $def) { $current = $def }
        if ($current -eq $reader.Function) {
            $seen = $true
            if ($code -match '\bBeganAt\s*\(') { $reads = $true }
        }
    }
    if (-not $seen) {
        $failures.Add("turnaround-began-once: $($reader.Function) not found in $($reader.File) - it moved or went; update rule 78, do not let it check nothing")
    } elseif (-not $reads) {
        $failures.Add("turnaround-began-once: $($reader.Function) no longer reads FTurnarounds::BeganAt - the two boards would derive 'a turnaround began' each for itself again (#427)")
    }
}
$beganBoardReach = @('RevisionCount', 'OpenJob', 'DropJobsOf', 'FindJob', 'JobForAircraft', 'LitresOwedFor', 'PostServiceFee', 'Bus')
$turnaroundsFiles = @((Join-Path $ops 'Private\Model\Turnarounds.cpp'), (Join-Path $ops 'Public\Model\Turnarounds.h'))
$turnaroundsCode = @{}
foreach ($turnaroundsFile in $turnaroundsFiles) {
    if (-not (Test-Path $turnaroundsFile)) {
        $failures.Add("turnaround-began-once: $turnaroundsFile is named by rule 78(c) but does not exist - update the rule, do not let it check nothing")
        continue
    }
    $inBlock = $false
    $turnaroundsCode[$turnaroundsFile] = @(Get-Content -LiteralPath $turnaroundsFile | ForEach-Object { Strip-ArchCode $_ ([ref]$inBlock) })
}
# THE RECEIVERS, BY TYPE: Board, and every name either file declares as a UJobBoard reference or pointer - so a parameter
# renamed, or a local alias of the board, is read like Board is. One set for both files: the header's parameter names
# and the .cpp's need not match.
$beganReceivers = @('Board')
foreach ($code in $turnaroundsCode.Values) {
    foreach ($line in $code) {
        foreach ($m in [regex]::Matches($line, '\bUJobBoard\s*[&*]\s*(\w+)')) {
            if ($beganReceivers -notcontains $m.Groups[1].Value) { $beganReceivers += $m.Groups[1].Value }
        }
    }
}
$beganReceiverPattern = '\b(' + (($beganReceivers | ForEach-Object { [regex]::Escape($_) }) -join '|') + ')\s*(?:\.|->)\s*(\w+)'
$reachSeen = 0
foreach ($turnaroundsFile in $turnaroundsCode.Keys) {
    $code = $turnaroundsCode[$turnaroundsFile]
    $short = Split-Path $turnaroundsFile -Leaf
    for ($i = 0; $i -lt $code.Count; $i++) {
        if ($code[$i] -match '\bLifecycle\s*\(') {
            $failures.Add("turnaround-began-once: ${short}:$($i + 1) moves a vehicle (Lifecycle) - a turnaround hands its jobs back through UJobBoard::DropJobsOf, and the vehicles are the board's (#427): $($code[$i].Trim())")
        }
        foreach ($m in [regex]::Matches($code[$i], $beganReceiverPattern)) {
            $reachSeen++
            if ($beganBoardReach -notcontains $m.Groups[2].Value) {
                $failures.Add("turnaround-began-once: ${short}:$($i + 1) reaches $($m.Groups[1].Value).$($m.Groups[2].Value) - the turnaround owner is a friend of UJobBoard for RevisionCount and the OpenJob/DropJobsOf doors only; the jobs and vehicles are the board's to write (#427): $($code[$i].Trim())")
            }
        }
    }
}
if ($reachSeen -eq 0) {
    $failures.Add("turnaround-began-once: rule 78(c) found no reach through a UJobBoard receiver ($($beganReceivers -join ', ')) in Turnarounds.cpp or .h - the board is no longer passed by reference, so the rule checks nothing; update it")
}
$ranRules.Add('turnaround-began-once')

# --- 87. A PRESENT-LAYER RELAY OF THE TRAFFIC MODEL ADDS BEHAVIOUR, OR IS NOT THERE (#445 item 6) --------------------------
# UAirsideTraffic re-declared five of UGroundTraffic's delegates and re-broadcast each from a one-line handler. Four added
# nothing: each Airside event cost a declaration, a handler and a bind on this layer as well as the model's, hand-paired,
# and the five binds were guarded by IsBoundToObject of the first alone. The model's delegates are public, and a listener
# binds them through GetModel() (UOpsRuntime's bridges do). What stays is the PHASE relay, because it adds the view: the cube
# is spawned before any listener hears Gone -> X and destroyed before one hears X -> Gone. THE SHAPE, both halves:
#  (a) AirsideTraffic.h declares no delegate type (DECLARE_*DELEGATE*) and no `FOn... On...;` member but the kept relays' -
#      the type qualified or not (`UGroundTraffic::FOnRunwayFreed OnRunwayFreed;` is the same relay, #499 review);
#  (b) every `.Broadcast(` in AirsideTraffic.cpp sits inside a kept relay's function, and that function still calls each
#      behaviour the relay is kept for - a relay whose behaviour moved out is a pure forwarder again. AirsideTraffic.h has
#      NO `.Broadcast(` at all: no kept relay's body lives there, so one there is an inline relay (#499 review).
# WHAT NO REGEX SEES: the same shape on another Present object, or a member typed TMulticastDelegate<...> directly. The
# behaviour half - the view is there when the relay is heard - is Airside.Present.PhaseRelayShowsTheViewFirst; the bridges'
# half is AirportOps.Present.Bus.ReattachDoesNotDouble.
# RED ON 2026-10-01 against main d2bb63cd (the four pure relays: 4 delegate types, 4 members, 4 re-broadcasts), and
# mutation-checked after, each red alone: SpawnView dropped from OnModelPhaseChanged; a `UGroundTraffic::FOnRunwayFreed
# OnRunwayFreed;` member; an inline `OnAgentPhaseChanged.Broadcast(` body in the header.
$relayKept = @{ 'OnAgentPhaseChanged' = @{ Function = 'UAirsideTraffic::OnModelPhaseChanged'; Behaviour = @('SpawnView', 'DestroyView') } }
$relayHeader = Join-Path $Root 'Plugins\Airside\Source\Airside\Public\Present\AirsideTraffic.h'
$relaySource = Join-Path $Root 'Plugins\Airside\Source\Airside\Private\Present\AirsideTraffic.cpp'
if (-not (Test-Path $relayHeader) -or -not (Test-Path $relaySource)) {
    $failures.Add("relay-adds-behaviour: AirsideTraffic.h/.cpp named by rule 87 do not exist - update the rule, do not let it check nothing")
}
else {
    $inBlock = $false
    $code = @(Get-Content -LiteralPath $relayHeader | ForEach-Object { Strip-ArchCode $_ ([ref]$inBlock) })
    $membersSeen = 0
    for ($i = 0; $i -lt $code.Count; $i++) {
        if ($code[$i] -match '\bDECLARE_\w*DELEGATE\w*\s*\(') {
            $failures.Add("relay-adds-behaviour: AirsideTraffic.h:$($i + 1) declares a delegate type - a model notification is UGroundTraffic's own delegate, bound through GetModel(); relay it here only to add behaviour, and add the relay to rule 87 (#445): $($code[$i].Trim())")
        }
        if ($code[$i] -match '^\s*((?:\w+::)*FOn\w+)\s+(On\w+)\s*;') {
            $membersSeen++
            if (-not $relayKept.ContainsKey($Matches[2])) {
                $failures.Add("relay-adds-behaviour: AirsideTraffic.h:$($i + 1) declares the relay $($Matches[2]) - a pure forwarder of the model's delegate; bind UGroundTraffic's through GetModel() (#445): $($code[$i].Trim())")
            }
        }
        if ($code[$i] -match '\.Broadcast\s*\(') {
            $failures.Add("relay-adds-behaviour: AirsideTraffic.h:$($i + 1) broadcasts in the header - an inline relay; a kept relay's body is in AirsideTraffic.cpp, where rule 87 reads what it adds (#445): $($code[$i].Trim())")
        }
    }
    if ($membersSeen -eq 0) {
        $failures.Add("relay-adds-behaviour: rule 87 found no delegate member in AirsideTraffic.h - the phase relay moved or the member shape changed, so the rule checks nothing; update it")
    }
    $keptFunctions = @{}
    foreach ($kept in $relayKept.Values) { $keptFunctions[$kept.Function] = @{} }
    $inBlock = $false
    $code = @(Get-Content -LiteralPath $relaySource | ForEach-Object { Strip-ArchCode $_ ([ref]$inBlock) })
    $current = ''
    for ($i = 0; $i -lt $code.Count; $i++) {
        $def = Get-ArchDefinition $code[$i]
        if ($null -ne $def) { $current = $def }
        if ($keptFunctions.ContainsKey($current)) {
            foreach ($m in [regex]::Matches($code[$i], '\b(\w+)\s*\(')) { $keptFunctions[$current][$m.Groups[1].Value] = $true }
        }
        if ($code[$i] -match '\.Broadcast\s*\(' -and -not $keptFunctions.ContainsKey($current)) {
            $failures.Add("relay-adds-behaviour: AirsideTraffic.cpp:$($i + 1) re-broadcasts from $current - a relay that adds nothing; listeners bind the model's delegate through GetModel() (#445): $($code[$i].Trim())")
        }
    }
    foreach ($kept in $relayKept.Values) {
        $calls = $keptFunctions[$kept.Function]
        if ($calls.Count -eq 0) {
            $failures.Add("relay-adds-behaviour: $($kept.Function) not found in AirsideTraffic.cpp - it moved or went; update rule 87, do not let it check nothing")
            continue
        }
        foreach ($behaviour in $kept.Behaviour) {
            if (-not $calls.ContainsKey($behaviour)) {
                $failures.Add("relay-adds-behaviour: $($kept.Function) no longer calls $behaviour - the behaviour the relay is kept for; without it the relay is a pure forwarder, so cut it and bind the model's delegate (#445)")
            }
        }
    }
}
$ranRules.Add('relay-adds-behaviour')

# --- Verdict -------------------------------------------------------------------------------
# Issue #291: this line used to be typed by hand and had already drifted (solve-purity was
# missing from it, unnoticed) - it now names whatever actually ran, from $ranRules, so the two
# lists cannot go out of sync again the way two independently-typed lists always eventually do.
Write-Host "Check-Architecture: $($commentFactWarnings.Count) comment-only-fact warning(s) (rule 12; see Tools/Check-Architecture.ps1's own comment)." -ForegroundColor Yellow
if ($failures.Count -eq 0) {
    Write-Host "Check-Architecture: PASS ($($ranRules -join ', '))" -ForegroundColor Green
    exit 0
}

foreach ($f in $failures) { Write-Host "  FAIL  $f" -ForegroundColor Red }
Write-Host ''
Write-Host "Check-Architecture: $($failures.Count) failure(s)." -ForegroundColor Red
exit 1
