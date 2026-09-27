# Stand Far-Side Entry Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A-F stands all build; service vehicles enter and leave only by the far (nose-side) edge; each letter is sized for, and dispatched, its own design vehicle.

**Architecture:** `UAirsideSettings::ResolveStandDesignVehicle(Letter)` names the vehicle. `BuildStandTemplate` takes that `FVehicle`, mirrors its bays to the front edge, and sizes the reverse leg from the tow's reverse limit. `StandBox::EntranceSetback` moves the stop mark so slack sits ahead of the nose. Stand entry links join only roads beyond the far edge. `FuelService` dispatches per job.

**Tech Stack:** UE 5.8 C++, Airside plugin (Solve/Model/Build/Entities/Present/Tool/Content), AirportOps plugin, UE automation tests.

**Spec:** `docs/superpowers/specs/2026-09-26-stand-far-side-entry-design.md`

## Global Constraints

- Worktree `C:\repos\airportmgr2-stand-entry`, branch `feature/stand-entry-far-side`. Never edit `C:\repos\AirportMgr2`.
- Build (worktree, editor may be open on main): ``D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development -Project="C:\repos\airportmgr2-stand-entry\AirportMgr.uproject" -WaitMutex -NoHotReloadFromIDE``.
- Tests: `./Tools/Run-AirsideTests.ps1 -Project "C:\repos\airportmgr2-stand-entry\AirportMgr.uproject" -Filter <prefix>`; read its `N test(s) run, N failed, N crashed` line, never the exit code.
- `Tools/Check-Architecture.ps1` must pass (runs inside the test script).
- Commit messages: NO `Co-Authored-By` trailer. Concise.
- Solve/ = CoreMinimal only. Tool/ describes MEANINGS to `IToolPreviewSink`, never colours; colours only in `PreviewPalette`.
- Content defaults resolved in ONE `UAirsideSettings::Resolve*` function.
- Every `UE_LOG` survives; WHY comments travel with code; comment density matches surroundings. `// ENFORCED BY:` on any new claim about other code.
- Letter design vehicles: A, B -> `ResolveUtilityTowVehicle()`; C, D, E, F -> `ResolveDefaultVehicle()`.
- Tail setback from entrance edge = `MaxTailAft + WingtipClearance(letter)` (A/B 300, C 450, D-F 750). Decided 2026-09-26 while user AFK: keeps a parked tail clear of the taxiway pavement edge by the letter's wingtip clearance instead of flush; flag in PR.
- Floor figures in `IcaoCode.cpp` Rows[] are NOT changed. If a letter overflows, report the `Stand template '%s' ... needs W x D` line in the PR and stop that letter there.

## Review Focus

1. A stand drawn DEEPER than its floor: contacts sit at the template front, not the drawn far edge; a road at the drawn far edge must still join (ServiceLinkRadius 6500). Test in Task 4.
2. A service road in the old taxiway-side strip (between stand and taxiway) must NOT join any bay entry - the whole point. Test in Task 4.
3. Old saves (stand outline stored, pose re-derived): the rebound pose uses the new setback and the aircraft lead-in still joins (`PoseSetbackRespectsShortRoad`, `RebindsAfterLevelLoad`). Task 2/3.
4. Utility tow sent to a C stand (smaller into larger) routes and reverses - `EveryTemplateLegIsDrivableByEveryVehicle` covers tow on C-F legs. Task 3.
5. A stand where some entries join and some do not: new partial Warning fires and `bServiceable` is false. Task 4/5.

---

### Task 1: Design vehicle per letter, size ranking, tow reverse radius

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Content/AirsideSettings.h` (beside `ResolveLargestServiceVehicle`, ~:196), `Private/Content/AirsideSettings.cpp`
- Modify: `Plugins/Airside/Source/Airside/Public/Model/VehicleFit.h`, `Private/Model/VehicleFit.cpp`
- Test: `Plugins/Airside/Source/AirsideTests/Private/StandDesignVehicleTest.cpp` (new)

**Interfaces:**
- Produces:
  - `static FVehicle UAirsideSettings::ResolveStandDesignVehicle(EIcaoCode Letter);`
  - `AIRSIDE_API bool VehicleFit::NoLargerThan(const FVehicle& A, const FVehicle& B);` - true when A may serve wherever B was designed for.
  - `AIRSIDE_API double VehicleFit::TightestReverseRadius(const FVehicle& Vehicle);` - rigid: `Chassis.TightestReversibleRadius()`; tow: max of that and the tightest arc whose `|SteadyHitchRadians|` <= `TowReverseHitchMargin * CriticalHitchRadians`.
  - `constexpr double VehicleFit::TowReverseHitchMargin = 0.7;`

- [ ] **Step 1: Write failing tests** in `StandDesignVehicleTest.cpp`:

```cpp
#include "Content/AirsideSettings.h"
#include "Misc/AutomationTest.h"
#include "Model/VehicleFit.h"
#include "Solve/IcaoCode.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStandDesignVehiclePerLetterTest,
	"Airside.Content.StandDesignVehicle.PerLetter",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FStandDesignVehiclePerLetterTest::RunTest(const FString&)
{
	// THE USER'S RULING 2026-09-26: a letter is designed for the largest vehicle it admits -
	// the utility tow on A/B, the fuel truck from C up.
	const FName Tow = UAirsideSettings::ResolveUtilityTowVehicle().TypeCode;
	const FName Truck = UAirsideSettings::ResolveDefaultVehicle().TypeCode;
	TestEqual(TEXT("A is the tow's"), UAirsideSettings::ResolveStandDesignVehicle(EIcaoCode::A).TypeCode, Tow);
	TestEqual(TEXT("B is the tow's"), UAirsideSettings::ResolveStandDesignVehicle(EIcaoCode::B).TypeCode, Tow);
	for (EIcaoCode L : { EIcaoCode::C, EIcaoCode::D, EIcaoCode::E, EIcaoCode::F })
	{
		TestEqual(FString::Printf(TEXT("%s is the truck's"), IcaoCode::ToLetter(L)),
			UAirsideSettings::ResolveStandDesignVehicle(L).TypeCode, Truck);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVehicleNoLargerThanTest,
	"Airside.Model.VehicleFit.NoLargerThanOrdersTowBeforeTruck",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVehicleNoLargerThanTest::RunTest(const FString&)
{
	// A smaller vehicle may serve a larger stand (inefficient, legal); never the reverse.
	const FVehicle Tow = UAirsideSettings::ResolveUtilityTowVehicle();
	const FVehicle Truck = UAirsideSettings::ResolveDefaultVehicle();
	TestTrue(TEXT("tow fits a truck stand"), VehicleFit::NoLargerThan(Tow, Truck));
	TestFalse(TEXT("truck never fits a tow stand"), VehicleFit::NoLargerThan(Truck, Tow));
	TestTrue(TEXT("reflexive"), VehicleFit::NoLargerThan(Truck, Truck));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTowReverseRadiusTest,
	"Airside.Model.VehicleFit.TowReverseRadiusHoldsTheHitch",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTowReverseRadiusTest::RunTest(const FString&)
{
	// A trailer reversing on this arc holds a steady hitch inside the margin - the figure the
	// stand's reverse leg is laid at, so a tighter one would be a leg TowReverse refuses.
	const FVehicle Tow = UAirsideSettings::ResolveUtilityTowVehicle();
	const double R = VehicleFit::TightestReverseRadius(Tow);
	TestTrue(TEXT("positive"), R > 0.0);
	TestTrue(TEXT("no tighter than the rigid limit"), R >= Tow.Chassis.TightestReversibleRadius());
	const VehicleSweep::FBody Rev = TowReverse::ReverseBody(VehicleFit::BodyOf(Tow));
	const double Crit = TowReverse::CriticalHitchRadians(Rev,
		FMath::DegreesToRadians(Tow.Chassis.Ground.MaxSteerDegrees));
	TestTrue(TEXT("hitch inside margin at R"),
		FMath::Abs(TowReverse::SteadyHitchRadians(Rev, 1.0 / R)) <= VehicleFit::TowReverseHitchMargin * Crit + 1e-6);
	const FVehicle Truck = UAirsideSettings::ResolveDefaultVehicle();
	TestEqual(TEXT("rigid = L/tan"), VehicleFit::TightestReverseRadius(Truck), Truck.Chassis.TightestReversibleRadius());
	return true;
}
```

- [ ] **Step 2: Build; run `-Filter Airside.Content.StandDesignVehicle` and `Airside.Model.VehicleFit`** - expect compile failure (symbols missing).

- [ ] **Step 3: Implement.** `ResolveStandDesignVehicle`: `return Letter <= EIcaoCode::B ? ResolveUtilityTowVehicle() : ResolveDefaultVehicle();` with a header comment giving the ruling and why it is beside `ResolveLargestServiceVehicle` (different question: "what is THIS letter sized for" vs "what must every road turn"). `NoLargerThan`: `A.WidestBody() <= B.WidestBody() && fwd radius <= && TightestReverseRadius(A) <= TightestReverseRadius(B) && ChainLength(A) <= ChainLength(B)`. `TightestReverseRadius`: if `!HasTrailer()` return rigid; else bisect R in [rigid, 100000] 40 iterations on the steady-hitch predicate (monotone: hitch grows as R shrinks); comment the margin's reason (steady state is the floor, the controller needs headroom to correct; 0.7 named, not tuned against a test).

- [ ] **Step 4: Build; run the two filters** - expect all pass. Quote the pass line.

- [ ] **Step 5: Commit** `feat(content,model): per-letter stand design vehicle, NoLargerThan, tow reverse radius`

---

### Task 2: StandBox setback - slack moves ahead of the nose

**Files:**
- Modify: `Public/Solve/IcaoCode.h`/`Private/Solve/IcaoCode.cpp` - add `AIRSIDE_API double WingtipClearanceForLetter(EIcaoCode Code);` reading Rows[]; fix the depth doc (`IcaoCode.cpp:29`, `IcaoCode.h:215`) to "tail setback + aircraft + service ground ahead of the nose".
- Modify: `Public/Solve/StandBox.h`, `Private/Solve/StandBox.cpp`
- Modify: `Private/Build/StandMarkingBuilder.cpp:102-128` (entrance midpoint)
- Modify: `Private/Tool/StandPlotTool.cpp:151-172` only if it recomputes the offset (it calls PoseFor - verify)
- Test: `AirsideTests/Private/StandBoxTest.cpp`, `StandMarkingTest.cpp`

**Interfaces:**
- Produces: `AIRSIDE_API double StandBox::EntranceSetback(EIcaoCode Letter, const FLetterEnvelope& Envelope);` = `Envelope.MaxTailAft + IcaoCode::WingtipClearanceForLetter(Letter)`. Stop mark = entrance mid + Facing * EntranceSetback. `BoxAt`: `Back = Pose - Facing*EntranceSetback`, `Front = Back + Facing*Depth`.

- [ ] **Step 1: Failing test** `Airside.Solve.StandBox.TailAtEntrance`: for every letter with `IcaoCode::FloorEnvelopeForLetter`, `PoseFor` from entrance (0,0)-(W,0), inward (0,1): `Pose.Position.Y == TailAft + clearance` exactly; `BoxAt` front edge Y == Depth; nose `Pose.Position.Y + MaxNoseFwd < Depth` (slack ahead of the nose, reason: service ground lives there). Update `TailToEntrance`/`RoundTrip`/`FarSide` expectations to the new setback.
- [ ] **Step 2: Build, run `-Filter Airside.Solve.StandBox`** - TailAtEntrance fails.
- [ ] **Step 3: Implement** `EntranceSetback`; rewrite `PoseFor`/`BoxAt` comments (the old "back edge is laid on the entrance" paragraph becomes "the tail sits the setback in from the entrance; the far edge carries the service ground"). `StandMarkingBuilder`: replace its `StandDepthForLetter - MaxNoseFwd` with `StandBox::EntranceSetback(...)`. Grep for any other `StandDepthForLetter(` minus `MaxNoseFwd` copy: `grep -rn "MaxNoseFwd" Plugins/Airside/Source/Airside/Private` and route each to the helper.
- [ ] **Step 4: Build, run `-Filter Airside.Solve.StandBox` and `Airside.Build.StandMarking` and `Airside.Build.AnchorLink`** (lead-in and `PoseSetbackRespectsShortRoad` must still pass; if a lead-in test fails because the stop mark is now nearer the taxiway, report the failing figure - do not loosen the test).
- [ ] **Step 5: Commit** `feat(solve): stand tail setback from entrance, slack ahead of nose`

---

### Task 3: Template - design vehicle, bays on the front edge

**Files:**
- Modify: `Public/Entities/EntityDefinition.h` (`BuildStandTemplate`, `BuildStandFor`, `BuildCodeCStandFor` take `const FVehicle&`), `Private/Entities/EntityDefinition.cpp:35-72, 220-340, 340-666`
- Modify: `Private/Present/StandDefinitionCache.cpp:20-72` (C authored asset - see step 5)
- Test: `AirsideTests/Private/StandLayoutTest.cpp`

**Interfaces:**
- Consumes: `ResolveStandDesignVehicle`, `VehicleFit::TightestReverseRadius`, `StandBox::EntranceSetback`.
- Produces: `FServiceBay::EntryLocal` on the FRONT edge (`FrontX - Square`), `EntryHeading = PI`, `ExitHeading = 0`.

- [ ] **Step 1: Failing tests** in `StandLayoutTest.cpp`:
  - `Airside.Entities.StandLayoutEveryLetterBuilds`: for A-F, `MakeStandTransient(L)` then `TestTrue(FitsItsLetter)`; on failure `AddError` with `RequiredExtent` and the floor (so the figure lands in the log).
  - `Airside.Entities.EveryBayContactIsOnTheFarEdge`: for A-F, every bay: `EntryLocal.X == FrontX - Square` within 1 uu, where `FrontX = Depth - EntranceSetback`; `EntryHeading == PI`; `ExitLocal == EntryLocal`; and `EntryLocal.X - BackX > Square` (never near the taxiway edge). Reason string: "service vehicles enter only by the edge opposite the taxiway (user 2026-09-26)".
  - Change `EveryTemplateLegIsDrivableByEveryVehicle`: per letter, vehicles = every vehicle in `{tow, truck}` with `NoLargerThan(V, ResolveStandDesignVehicle(L))`; rigid legs as today; for a tow, build an `FRoutePlan` over arrive+serve+reverse+depart and assert `VehicleFit::JudgePlan(...)` accepts (read how `TowReverseAgentTest.cpp` builds one; reuse its helper rather than a new one).
- [ ] **Step 2: Build, run `-Filter Airside.Entities.Stand`** - expect fails (A/B do not fit; contacts on back edge).
- [ ] **Step 3: Implement** in `BuildStandTemplate`:
  - Signature `(UEntityDefinition&, EIcaoCode, const FVehicle& Design, const FLetterEnvelope&)`; `Radius = Design.Chassis.TightestFollowableRadius()`, `ReverseRadius = VehicleFit::TightestReverseRadius(Design)`.
  - `BackX = -StandBox::EntranceSetback(Letter, Envelope)`, `FrontX = BackX + Depth`. Replace every `BackX + ...` contact/branch/shift construction with its mirror `FrontX - ...`, and each forward-going X delta with a negated one (headings: entry PI, park `Side*0.75*PI`, lane runs aft). Keep every WHY comment, rewording "back edge"/"behind" to "front edge"/"ahead of the nose" and adding one paragraph: why the contacts moved (user 2026-09-26: the taxiway-side strip forced service roads between stand and taxiway).
  - `ContactFloor/Ceiling` binding: the forward-most bay becomes the AFT-most (mirror); re-derive the `Wants` expression with the sign flipped and say so in the comment.
  - `RequiredExtent`: `MinX = BackX` (the setback is ground the stand needs), `Cover(EntryLocal.X + Square, ...)` for the lead-in run beyond the contact.
  - Keep both `UE_LOG` lines; add `design vehicle %s` (TypeCode) to the first.
  - `MakeStandTransient(Letter)`/`BuildCodeCStand`: pass `ResolveStandDesignVehicle(Letter)`. Fixtures in `BuildStandFor` (PassengerDoor, TugStand fixed offsets) unchanged unless a test shows a leg crossing them.
- [ ] **Step 4: Build, run `-Filter Airside.Entities`**. Quote every `Stand template 'X': ... needs W x D` line in the task report. If A or B still overflows, leave its test failing ONLY if the overflow is in the floor figures (spec: user decides), mark it in the report, and continue.
- [ ] **Step 5: Code C authored asset.** `StandDefinitionCache` returns the actor's authored `DA_Stand_CodeC`, whose bays are baked. Find its author (`grep -rn "BuildCodeCStand" Tools Source Plugins`). If the editor is closed (`Get-Process UnrealEditor` empty) re-run that author against the worktree project. If open, instead make the cache rebuild the authored asset's bays into a transient duplicate on resolve (one `BuildCodeCStandFor` call, commented why, test `Airside.Present.StandPlot.PlacesCodeC` asserts far-edge contacts) and note "DA_Stand_CodeC needs re-authoring" in the PR.
- [ ] **Step 6: Build, run `-Filter Airside`** (full plugin) - fix fallout in `StandPlotPlacementTest`/`StandPlotToolTest` in Task 6, not here; list failures in the report.
- [ ] **Step 7: Commit** `feat(entities): stand bays on the far edge, sized for the letter's design vehicle`

---

### Task 4: Entry links join only beyond the far edge

**Files:**
- Modify: `Private/Build/AnchorLink.cpp:439-537` (entry pass), `:999-1081` (Build: partial warning), `Public/Build/AnchorLink.h` if `FPendingLink` gains a field
- Test: `AirsideTests/Private/AnchorLinkTest.cpp` or new `StandFarEdgeLinkTest.cpp`

**Interfaces:**
- Produces: `FPendingLink::HalfPlane` (`TOptional<FVector2D>` outward normal) - `Resolve` rejects a hit whose `dot(Hit - At, Normal) < 0`. Set for lane entries to the stand's world forward (`Facing`).

- [ ] **Step 1: Failing tests** (use `StandFixture.h`):
  - `Airside.Build.StandEntry.TaxiwaySideRoadDoesNotJoin`: C stand, a service road parallel to the entrance edge 400 uu outside it (the old strip). Rebuild. Every bay entry node `!IsServiceNodeConnected`; the "joins nothing" warning is expected (`AddExpectedError` with the substring).
  - `Airside.Build.StandEntry.FarEdgeRoadJoins`: same stand, road 400 uu beyond the front edge. Every entry connected.
  - `Airside.Build.StandEntry.DeepStandFarRoadJoins`: stand drawn floor+3000 deep, road 400 beyond the DRAWN far edge. Every entry connected (lead-in crosses the extra apron).
- [ ] **Step 2: Build, run `-Filter Airside.Build.StandEntry`** - TaxiwaySideRoadDoesNotJoin fails.
- [ ] **Step 3: Implement** the half-plane on `FPendingLink` and its check in `FAnchorLink::Resolve` (proximity branch only), set in `EntryLink` lambda from `Instance->Heading`. Comment: why a half-plane and not a ray (a vehicle may still arrive along the far road from either direction). In `Build`, track per-`LaneOwner` joined/refused counts; when both > 0, `UE_LOG(LogAirside, Warning, TEXT("Stand %d: %d of %d service entrances joined a road - the rest have none beyond the far edge"), ...)`.
- [ ] **Step 4: Build, run `-Filter Airside.Build`** - all pass.
- [ ] **Step 5: Commit** `feat(build): stand service entries join only roads beyond the far edge`

---

### Task 5: `bServiceable` fact and inspector line

**Files:**
- Modify: `Public/Model/InspectFacts.h:66`, `Private/Model/InspectFacts.cpp:~131`
- Modify: `Source/AirportMgr/InspectorWidget.cpp:258-287`
- Test: `AirsideTests/Private/InspectFactsTest.cpp` (find the existing `bReachable` test and sit beside it)

**Interfaces:**
- Produces: `bool FStandFacts::bServiceable = false;` - every declared bay entry node `Network.IsServiceNodeConnected`; false for a stand with no bays.

- [ ] **Step 1: Failing tests** `Airside.Model.InspectFacts.StandServiceableWithFarRoad` / `StandUnserviceableWithoutRoad` (fixtures as Task 4).
- [ ] **Step 2: Run** - fail.
- [ ] **Step 3: Implement**; inspector row "Service road" -> "joined" / "not joined - draw a service road along the far edge", beside "Reachable by taxiway".
- [ ] **Step 4: Build, run `-Filter Airside.Model.InspectFacts`** and `AirportMgr.` HUD/inspector tests - pass.
- [ ] **Step 5: Commit** `feat(model,ui): stand serviceable fact and inspector row`

---

### Task 6: Plot tool - A/B buildable, far edge preview

**Files:**
- Modify: `Public/Tool/ToolPreviewSink.h` (or wherever `EPreviewStyle` lives - `grep -rn "enum class EPreviewStyle"`), `Private/Present/PreviewPalette.cpp`, `Private/Tool/StandPlotTool.cpp:130-172`
- Test: `StandPlotToolTest.cpp` (`:786` `bBuildable`), `StandPlotPlacementTest.cpp` (`UnfitLetterRefused` :329, `PlacesOtherLetters` :374), HUD test that counts `EPreviewStyle` by reflection

**Interfaces:**
- Produces: `EPreviewStyle::ServiceEdge` - the stand's far edge, where its service road goes.

- [ ] **Step 1: Failing tests**: `Airside.Tool.StandPlot.DrawsServiceEdge` - ghost of a C stand emits one `ServiceEdge` segment equal to outline corners 2->3. Change `bBuildable` to `true` for all letters; `PlacesOtherLetters` includes A and B; replace `UnfitLetterRefused` with `EveryLetterBuilds` (all six `ResolveStandDefinitionFor` non-null) - comment why the old refusal is now unreachable.
- [ ] **Step 2: Run `-Filter Airside.Tool.StandPlot` and `Airside.Present.StandPlot`** - fail.
- [ ] **Step 3: Implement** the style, palette colour (distinct from existing; pick near the service-road colour), and the sink call in `StandPlotTool`'s preview.
- [ ] **Step 4: Run both filters + `AirportMgr.HUD`** - pass.
- [ ] **Step 5: Commit** `feat(tool): stand ghost marks the service edge; A and B stands build`

---

### Task 7: Fuel dispatch per stand

**Files:**
- Modify: `Plugins/AirportOps/Source/AirportOps/Public/Model/FuelService.h` (`TruckVehicle` -> removed; `EFuelRefusal::VehicleTooLarge`), `Private/Model/FuelService.cpp:51,115-341,635-704`, `Private/Present/OpsRuntime.cpp:165`
- Test: AirportOps fuel tests (`grep -rn "Fuel" Plugins/AirportOps/Source/*Tests*` for the existing file and fixture)

**Interfaces:**
- Consumes: `ResolveStandDesignVehicle`, `NoLargerThan`, `StandBox::LetterOf(Instance.Outline)`.
- Produces: `FVehicle UFuelService::VehicleFor(const FEntityInstance& Stand) const;` - a `TFunction<FVehicle(EIcaoCode)> ResolveVehicle` member set by OpsRuntime (keeps AirportOps Model free of Content/ if that is the existing layering - check how `TruckVehicle` was injected and mirror it).

- [ ] **Step 1: Failing tests**:
  - `AirportOps.Fuel.TowServesCodeA`: A stand with far-edge road, depot, arrival; dispatched agent's vehicle `TypeCode == UtilityTow`; aircraft departs without "UNFUELLED" (`AddExpectedError` must NOT be needed).
  - `AirportOps.Fuel.TruckServesCodeC`.
  - `AirportOps.Fuel.VehicleTooLargeRefused`: resolver returns the truck for A -> demand Unserviceable with `EFuelRefusal::VehicleTooLarge` and its text.
  - Composition seam: `AirportOps.Fuel.RuntimeResolvesPerStand` - spin OpsRuntime, two stands A and C, assert two different TypeCodes dispatched (fails if a single fixed vehicle is still wired).
- [ ] **Step 2: Run `-Filter AirportOps.Fuel`** - fail.
- [ ] **Step 3: Implement**; `SendTruckHome` uses the agent's own vehicle, not a field. Keep every `UE_LOG`; the dispatch log line gains the vehicle TypeCode.
- [ ] **Step 4: Run `-Filter AirportOps`** - pass.
- [ ] **Step 5: Commit** `feat(ops): fuel dispatches each stand's design vehicle; VehicleTooLarge refusal`

---

### Task 8: Full verification and PR

- [ ] Full build (command in Global Constraints) - quote `Result: Succeeded`.
- [ ] `./Tools/Run-AirsideTests.ps1 -Project ...` (no filter) - quote the `N test(s) run, N failed, N crashed` line. Compare against main's count in the same log directory; list any test that failed before too.
- [ ] Count `UE_LOG(` in touched files before (main) vs after; count comment lines likewise.
- [ ] Push branch, `gh pr create` to main with the template (build line, test line, deltas), the AFK decisions (tail setback; C asset handling; any letter overflow with its figure), and the PIE verification steps: place A, B, C with a service road along the far edge; log `Stand template 'A': ... needs` within floor; FuelService dispatch line naming `UtilityTow` for A; screenshot of entry from the far edge. PR body ends with the Claude Code line.
