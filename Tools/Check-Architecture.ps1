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
    [string] $Root = (Split-Path -Parent $PSScriptRoot)
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
# and RoadNetworkActor.h all discuss these symbols by name in exactly that way.
# Issue #291: every entry below names enough of its PATH to be unambiguous (checked with
# Test-AllowedPathSuffix, not bare-name equality) - 'Private\Entities\AircraftType.cpp', not
# 'AircraftType.cpp'. Confirmed by grep that each is still the file's real current location
# (one hit apiece on today's tree), so this widens what the check can catch without moving
# what it accepts today.
$AllowedCallers = @(
    @{
        Name        = 'PiperMeridian fallback'
        Pattern     = '(?<![A-Za-z])PiperMeridian\w*\s*\('
        ProdAllowed = @('Public\Entities\AircraftType.h', 'Private\Entities\AircraftType.cpp', 'Private\Content\AirsideSettings.cpp')
        TestAllowed = @('Private\AirsideTestFixtures.cpp')
        ProdReason  = 'go through UAirsideSettings::ResolveDefaultAirframe'
        TestReason  = 'go through TestAirframes::Piper() (AirsideTestFixtures.h)'
    },
    @{
        # CONFIRMED BY GREP FOR #255: production callers today are AirsideSettings.cpp
        # (itself), RoadBuildController.cpp and OpsRuntime.cpp. Unlike Piper, tests call this
        # directly from a couple dozen files on purpose - it IS the canonical "give me a
        # plane" accessor test fixtures are supposed to use - so tests are blanket-exempt
        # rather than narrowed to one fixture file.
        Name        = 'UAirsideSettings::ResolveDefaultAirframe'
        Pattern     = 'UAirsideSettings::ResolveDefaultAirframe\s*\('
        ProdAllowed = @('Public\Content\AirsideSettings.h', 'Private\Content\AirsideSettings.cpp', 'Source\AirportMgr\RoadBuildController.cpp', 'Private\Present\OpsRuntime.cpp')
        TestExempt  = $true
        ProdReason  = 'a new production caller resolves the default airframe a second way instead of taking it from context - route it through one of the rows above or extend this row and say why'
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
        # the other sanctioned caller, in the same Solve file.
        Name        = 'VehicleSweep::StepChain'
        Pattern     = '\bStepChain\s*\('
        ProdAllowed = @('Public\Solve\VehicleSweep.h', 'Private\Solve\VehicleSweep.cpp', 'Private\Model\VehicleFit.cpp')
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
        Name        = 'RoadHeal::PlanNodeDeletion'
        Pattern     = 'RoadHeal::PlanNodeDeletion\s*\('
        ProdAllowed = @('Public\Tool\RoadHeal.h', 'Private\Tool\RoadHeal.cpp', 'Private\Present\RoadEditFacade.cpp')
        TestExempt  = $true
        ProdReason  = "go through IRoadEditTarget::PlanNodeDeletion - URoadEditFacade is production's one wrapper"
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
        ProdAllowed = @('Public\Content\AirsideSettings.h', 'Private\Content\AirsideSettings.cpp', 'Public\Present\RoadNetworkActor.h', 'Private\Present\RoadNetworkActor.cpp', 'Private\Debug\RoadJunctionGallery.cpp')
        TestExempt  = $true
        ProdReason  = "go through Content/ or ARoadNetworkActor - see this row's own comment for the one pre-existing exception"
    }
)
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
                if ($h.Line.Trim() -match '^(//|/\*|\*)') { continue }
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
$commentFactPattern = 'the only (caller|file|place|site)|never (called|happens|runs)|no (edit|change) (is )?needed|nothing (else )?(reads|calls|binds)|checks all three every run|nothing here matches|no other test|every test in|the one place its name appears'
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
# shape question with no agent behind it.
#
# QUALIFIED USES ONLY (ERouteErrand::GraphProbe), so the enumerator's own declaration and the
# paragraphs explaining it in RoutePolicy.h are not mistaken for callers of it.
foreach ($module in $modules) {
    foreach ($half in 'Public', 'Private') {
        $dir = Join-Path $module $half
        foreach ($file in Get-Sources $dir @('.h', '.cpp')) {
            if ($file.FullName -like '*\Tool\*') { continue }
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

# --- 19. Runway/taxiway profile triples are TestProfiles', nowhere else in AirsideTests -----
# Issue #310: TestProfiles::Runway/NarrowRunway/Taxiway existed since #102 (f0714c80), and 28
# non-fixture sites across 12 files kept retyping the same MakeTransient triple anyway - some
# of them (EditToolTest, MeshFreshnessTest, RunwayDisconnectTest) newer than the helper itself.
# AirsideTestFixtures.cpp is the one legal definer. Scoped to AirsideTests only: AirsideTests.h
# is Private to that module (its own top comment), so AirportOpsTests cannot call TestProfiles
# and its OWN copies of these triples (FlightBoardTest, FuelServiceTest, OfferGeneratorTest,
# OpsRuntimeTest, OpsSaveTest) are a separate finding, reported but not fixed by #310 - moving
# TestProfiles to a public header is a bigger seam than this issue's scope.
#
# ALL THREE ARGUMENTS, exactly paired (4500/1500/450, 1800/1500/180, 2300/1500/230): the 2-arg
# MakeTransient(Width, LaneWidth) overload this codebase also uses (LeadInSweepTest,
# RoadNetworkTest, RoadProfileTest, ...) takes the engine's own default ExitLength, which is
# NOT what TestProfiles::Runway/NarrowRunway/Taxiway produce - matching on the first two
# arguments alone flagged a dozen of those as false positives before this comment.
$profileTriplePattern = 'MakeTransient\(\s*(4500\.0,\s*1500\.0,\s*450\.0|1800\.0,\s*1500\.0,\s*180\.0|2300\.0,\s*1500\.0,\s*230\.0)\s*\)'
foreach ($file in Get-Sources $airsideTests @('.h', '.cpp')) {
    if ($file.Name -eq 'AirsideTestFixtures.cpp') { continue }
    foreach ($h in (Select-String -Path $file.FullName -Pattern $profileTriplePattern)) {
        $t = $h.Line.Trim()
        if ($t.StartsWith('//') -or $t.StartsWith('*') -or $t.StartsWith('/*')) { continue }
        $failures.Add("profile-triple: $($file.FullName):$($h.LineNumber) retypes a TestProfiles triple; call TestProfiles::Runway/NarrowRunway/Taxiway instead: $t")
    }
}
$ranRules.Add('profile-triple')

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
