# Drawn Stands Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Key 3 draws a stand as a rectangle off a taxiway; its width x depth decides its ICAO letter; arrivals take the smallest free stand their span fits.

**Architecture:** Stand geometry is one `Solve/StandBox` (pose <-> box, both ways). The plot gesture's road-snapping helpers move to `Tool/PlotGesture` and are shared by the depot tool and a new `FStandPlotTool`. The facade gains `PlaceStandInPlot`; the actor resolves a definition per letter; `ArrivalPlanner::ChooseStand` sorts by letter then taxi length; `URoadNetwork::PostLoad` gives legacy stands an outline; stand paint joins the holding-bar paint layer.

**Tech Stack:** UE 5.8 C++, Airside plugin, UE automation tests.

**Spec:** `docs/superpowers/specs/2026-09-23-drawn-stands-design.md`

## Global Constraints

- Worktree `C:\repos\aircraftmgr-stands`, branch `feature/stand-plots`. Build from the worktree with `-NoHotReloadFromIDE` (editor may be open on the main checkout):
  ```
  D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development `
    -Project="C:\repos\aircraftmgr-stands\AirportMgr.uproject" -WaitMutex -NoHotReloadFromIDE
  ./Tools/Run-AirsideTests.ps1 -Project "C:\repos\aircraftmgr-stands\AirportMgr.uproject" -Filter <X>
  ```
- A NEW test .cpp needs TWO builds before it runs (memory: first build says Succeeded without compiling it). Read the runner's `N test(s) run, N failed, N crashed` line, never the exit code.
- `Solve/` includes `CoreMinimal.h` and other `Solve/` headers only. `Model/` never includes `Entities/`.
- Log categories: `LogAirside` in the plugin model, `LogRoadMesh` in the facade/presenters (as the neighbours do). No new `DEFINE_LOG_CATEGORY_STATIC`.
- Comments explain WHY; match the density of the surrounding file. A doc comment touches its declaration.
- Units: uu = cm. Width = entrance edge length; depth = inward extent.
- Say "entrance edge", never "frontage", for stands. Aircraft TAIL is at the entrance edge; NOSE points inward.
- Commits: conventional prefix, concise, end with `Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>`. Every commit must have been built.
- Verify every name used below against the live header before relying on it (memory: "RoadNet plans ship real defects"). If a name differs, use the real one and note it in the commit.

## Review Focus

1. A letter whose `BuildStandTemplate` does not fit its own floor (A, B, E, F were never built): the tool must refuse that letter with a readable reason, never place a stand whose service layout overflows. Pinned in Task 1 + Task 7.
2. Legacy stand whose `DesignWingspan` is the A320's 34.1 m: a 737-800 (35.8 m) must still park there - admission compares LETTERS, not spans. Pinned in Task 4.
3. Drawing the stand on the far side of the taxiway, or dragging the width backwards: winding reverses; pose must still face away from the taxiway. Pinned in Task 2 + Task 5.
4. A drawn stand must not be treated as a depot anywhere (fence, modules, remove label, depot tool's Remove). Pinned in Task 3.
5. An aircraft wider than Code F max, or unknown span 0: not admitted / admitted as today respectively, no crash. Pinned in Task 4.

---

### Task 1: Stand template per letter, verified

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Entities/EntityDefinition.h` (BuildCodeCStandFor / MakeStandTransient decls ~:365-394)
- Modify: `Plugins/Airside/Source/Airside/Private/Entities/EntityDefinition.cpp:173-285`
- Test: `Plugins/Airside/Source/AirsideTests/Private/StandLayoutTest.cpp`

**Interfaces:**
- Produces: `static void UEntityDefinition::BuildStandFor(UEntityDefinition* Definition, UAircraftType* Aircraft, EIcaoCode Letter, const FAirframe& Largest);` (body of today's `BuildCodeCStandFor` with `Letter` a parameter instead of the local `const EIcaoCode Letter = EIcaoCode::C;` at cpp:212). `BuildCodeCStandFor(D, A, Largest)` becomes a one-line forwarder `BuildStandFor(D, A, EIcaoCode::C, Largest)`.
- Produces: `static bool UEntityDefinition::FitsItsLetter(const UEntityDefinition& Stand, EIcaoCode Letter);` = `RequiredExtent.X <= StandWidthForLetter && RequiredExtent.Y <= StandDepthForLetter && LetterForStandSize(RequiredExtent) == ToLetter(Letter)`.
- Produces: `static UEntityDefinition* UEntityDefinition::MakeStandTransient(EIcaoCode Letter, UObject* Outer = GetTransientPackage());` overload (keep the existing zero-arg one working as Code C).

- [ ] **Step 1: Write the failing test** - add to StandLayoutTest.cpp:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandLayoutEveryLetterReportTest,
	"Airside.Entities.StandLayoutEveryLetterReport",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandLayoutEveryLetterReportTest::RunTest(const FString& Parameters)
{
	// A MEASUREMENT FIRST, a verdict second. Only C was ever built; the drawn-stand tool offers
	// all six, so each letter's template either fits its own floor or the tool must refuse it.
	// This test records which, and pins that FitsItsLetter agrees with the raw extents - the
	// tool trusts FitsItsLetter, so it must not be a second opinion.
	for (int32 Index = 0; Index <= static_cast<int32>(EIcaoCode::F); ++Index)
	{
		const EIcaoCode Letter = static_cast<EIcaoCode>(Index);
		UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient(Letter);
		if (!TestNotNull(TEXT("a template"), Stand)) { return false; }

		const bool bFits = UEntityDefinition::FitsItsLetter(*Stand, Letter);
		AddInfo(FString::Printf(TEXT("Code %s: floor %.0f x %.0f, needs %.0f x %.0f, bays %d -> %s"),
			IcaoCode::ToLetter(Letter),
			IcaoCode::StandWidthForLetter(Letter), IcaoCode::StandDepthForLetter(Letter),
			Stand->RequiredExtent.X, Stand->RequiredExtent.Y, Stand->ServiceBays.Num(),
			bFits ? TEXT("FITS") : TEXT("DOES NOT FIT")));

		TestEqual(*FString::Printf(TEXT("FitsItsLetter agrees with the extents for %s"), IcaoCode::ToLetter(Letter)),
			bFits,
			Stand->RequiredExtent.X <= IcaoCode::StandWidthForLetter(Letter)
				&& Stand->RequiredExtent.Y <= IcaoCode::StandDepthForLetter(Letter)
				&& IcaoCode::LetterForStandSize(Stand->RequiredExtent.X, Stand->RequiredExtent.Y) == IcaoCode::ToLetter(Letter));
	}

	// C IS THE SHIPPING STAND and must keep fitting, whatever the others do.
	TestTrue(TEXT("Code C fits its floor"),
		UEntityDefinition::FitsItsLetter(*UEntityDefinition::MakeStandTransient(EIcaoCode::C), EIcaoCode::C));
	return true;
}
```

- [ ] **Step 2: Build twice, run `-Filter Airside.Entities.StandLayout`.** Expected: compile FAIL (no `FitsItsLetter`, no letter overload).
- [ ] **Step 3: Implement.** Rename body per Interfaces; `MakeStandTransient(Letter)` = `NewObject<UEntityDefinition>(Outer)` then `BuildStandFor(D, <largest shipped UAircraftType of Letter or nullptr>, Letter, UAirsideSettings::ResolveLargestServiceVehicle())`. For the aircraft: read how the zero-arg `MakeStandTransient` gets its A320 (cpp:10-30) and pass `nullptr` for non-C letters in this task - Task 5 supplies the real aircraft. Check `BuildStandFor` tolerates `Aircraft == nullptr` (it sets `DesignAircraft`; grep its body for dereferences of `Aircraft` and guard them).
- [ ] **Step 4: Run.** Expected: PASS, and the AddInfo lines list FITS / DOES NOT FIT per letter. **Copy those six lines into the commit message** - they decide which letters Task 7's tool offers.
- [ ] **Step 5: Run the full stand suite** `-Filter Airside.Entities` and `-Filter Airside.Model.Stand` - unchanged green (C forwarder).
- [ ] **Step 6: Commit** `feat(stands): stand template buildable per ICAO letter; measure which fit`.

---

### Task 2: StandBox - one geometry, both directions

**Files:**
- Create: `Plugins/Airside/Source/Airside/Public/Solve/StandBox.h`
- Create: `Plugins/Airside/Source/Airside/Private/Solve/StandBox.cpp`
- Test: `Plugins/Airside/Source/AirsideTests/Private/StandBoxTest.cpp`

**Interfaces:**
- Consumes: `IcaoCode::StandWidthForLetter/StandDepthForLetter/MaxNoseFwdForLetter/LetterForStandSize/Parse`, `RoadGeom::PerpCCW`, `RoadGeom::PolygonArea`.
- Produces (namespace `StandBox`, all `AIRSIDE_API`):
  - `struct FStandPose { FVector2D Position = FVector2D::ZeroVector; FVector2D Facing = FVector2D(1, 0); };` - Position = nose-gear stop mark; Facing = unit, nose direction.
  - `FStandPose PoseFor(const FVector2D& EntranceA, const FVector2D& EntranceB, const FVector2D& Inward, EIcaoCode Letter);`
  - `void BoxAt(const FStandPose& Pose, EIcaoCode Letter, TArray<FVector2D>& OutCorners);` - four corners, positive `PolygonArea`, entrance edge is corners 0->1.
  - `double WidthOf(TArrayView<const FVector2D> Rect);` = |R1-R0|; `double DepthOf(TArrayView<const FVector2D> Rect);` = |R2-R1|. Rect convention: entrance edge 0->1, then inward.
  - `TOptional<EIcaoCode> LetterOf(TArrayView<const FVector2D> Rect);` = `IcaoCode::Parse(LetterForStandSize(WidthOf, DepthOf))`, unset for <4 points or no letter.

```cpp
// StandBox.cpp core
FStandPose PoseFor(const FVector2D& EntranceA, const FVector2D& EntranceB, const FVector2D& Inward, EIcaoCode Letter)
{
	// THE TEMPLATE'S BACK EDGE (X = NoseFwd - Depth, the tail side) IS LAID ON THE ENTRANCE
	// EDGE, centred - see UEntityDefinition::BuildStandTemplate. So the stop mark is
	// Depth - NoseFwd in from it, and every metre the player drew beyond the floor is
	// apron past the nose.
	FStandPose Pose;
	Pose.Facing = Inward.GetSafeNormal();
	Pose.Position = (EntranceA + EntranceB) * 0.5
		+ Pose.Facing * (IcaoCode::StandDepthForLetter(Letter) - IcaoCode::MaxNoseFwdForLetter(Letter));
	return Pose;
}

void BoxAt(const FStandPose& Pose, EIcaoCode Letter, TArray<FVector2D>& OutCorners)
{
	const double HalfWidth = 0.5 * IcaoCode::StandWidthForLetter(Letter);
	const double NoseFwd = IcaoCode::MaxNoseFwdForLetter(Letter);
	const double Depth = IcaoCode::StandDepthForLetter(Letter);
	const FVector2D Back = Pose.Position - Pose.Facing * (Depth - NoseFwd);
	const FVector2D Front = Pose.Position + Pose.Facing * NoseFwd;
	// -PerpCCW: entrance runs so that inward is on its LEFT, which is what makes the
	// quad counter-clockwise (positive area) - the winding the pad triangulator needs.
	const FVector2D Side = -RoadGeom::PerpCCW(Pose.Facing) * HalfWidth;
	OutCorners = { Back - Side, Back + Side, Front + Side, Front - Side };
}
```
(If the positive-area test fails, flip `Side`'s sign - the TEST is the authority, not this derivation.)

- [ ] **Step 1: Failing tests** in StandBoxTest.cpp, prefix `Airside.Solve.StandBox.`:
  - `.RoundTrip`: for each letter L, entrance A=(0,0), B=(W_L,0), Inward=(0,1): `BoxAt(PoseFor(A,B,Inward,L), L)` corners equal `{(0,0),(W,0),(W,D),(0,D)}` within 0.01 (floating arithmetic, not a weld - tolerance is legitimate here), `PolygonArea > 0`, and `LetterOf(box) == L` - "the box a letter builds must read back as that letter".
  - `.TailToEntrance`: pose's Facing == Inward; `Dot(Pose.Position - A, Inward) == Depth - NoseFwd` - "the nose points AWAY from the taxiway".
  - `.FarSide`: entrance A=(W,0), B=(0,0), Inward=(0,-1) (stand drawn below the taxiway, width dragged backwards): Facing == (0,-1), Position.Y < 0.
  - `.NoLetter`: 20 m x 20 m rect -> `LetterOf` unset; 67 x 30 m -> B (spec table row).
- [ ] **Step 2: Build twice, run** `-Filter Airside.Solve.StandBox` -> FAIL (missing header).
- [ ] **Step 3: Implement** as above.
- [ ] **Step 4: Run** -> 4 PASS. Run `./Tools/Check-Architecture.ps1` -> green (Solve include rule).
- [ ] **Step 5: Commit** `feat(stands): StandBox - stand pose and box from one geometry`.

---

### Task 3: Kind, not outline, says "depot"

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Model/RoadEntity.h:303-312` (`IsPlotted` doc + new helpers)
- Modify: every site where `IsPlotted()` means "is a depot": find with `grep -rn "IsPlotted()" Plugins/Airside/Source Source` - known: `Private/Present/PlotPresenter.cpp` (RebuildFrom ~:540, fence), `Private/Tool/PlotPlaceTool.cpp:442` (PlotUnder), `:668`, `Private/Present/RoadEditFacadeSurfaces.cpp` (FindEntityAt - leave: it is kind-neutral and correct for stands too), `Private/Present/RoadSurfacePresenter.cpp:~341` (pad paving - leave: stands get pads).
- Modify: `Tools/Check-Architecture.ps1` (new rule)
- Test: `Plugins/Airside/Source/AirsideTests/Private/PlotPresenterTest.cpp`

**Interfaces:**
- Produces on `FEntityInstance`:
  ```cpp
  /** A stand: its pose node is an aircraft's stop mark. PoseRole is the captured kind - Model/ cannot ask the definition. */
  bool IsStand() const { return PoseRole == EServiceRole::Aircraft; }
  /** A fuel depot, plotted or pre-plot. */
  bool IsDepot() const { return PoseRole == EServiceRole::Fuel; }
  ```
  `IsPlotted()` keeps its body; its doc comment changes to "has a drawn outline - a depot plot OR a drawn stand. Never use it to mean 'depot': ask IsDepot()."

- [ ] **Step 1: Failing test** `Airside.Present.PlotPresenter.StandOutlineIsNotADepot`: in an `FAirsideTestWorld`, place a stand through `IRoadEditTarget::PlaceEntity(..., EPlaceableEntity::Stand)`, then write an outline onto it directly (`Actor->Network` entity array via the same mutable accessor PlotPresenterTest already uses - grep it) and rebuild the buildings actor. Assert: presenter placed 0 modules and 0 fence posts for it (use the same counters the existing PlotPresenterTest cases read). Reason string: "a drawn stand is ground, not a yard - no fence round an aircraft".
- [ ] **Step 2: Run** -> FAIL (presenter fences it).
- [ ] **Step 3: Implement** helpers; replace depot-meaning sites with `IsDepot() && IsPlotted()` (presenter, fence) or `IsDepot()` (PlotUnder: `Entity.IsDepot() ? Under : INDEX_NONE`). DeleteEntity label: `Entity->IsDepot()`.
- [ ] **Step 4: Lint rule** in Check-Architecture.ps1, following the existing numbered-rule pattern (read rule 14 as the template): fail on `IsPlotted()` in any file except `RoadEntity.h`, `RoadSurfacePresenter.cpp`, `RoadEditFacadeSurfaces.cpp` and files that also contain `IsDepot()` on the same line. Message: "IsPlotted() is not 'is a depot' - a drawn stand is plotted too; ask IsDepot()". **Prove it**: add a stray `IsPlotted()` line to PlotPresenter.cpp, run the script, see it fail, remove it (memory: a green rule may measure nothing).
- [ ] **Step 5: Run** `-Filter Airside.Present.Plot` and `-Filter Airside.Tool.Plot` + `Check-Architecture.ps1` -> green.
- [ ] **Step 6: Commit** `refactor(plots): IsDepot/IsStand replace outline-means-depot; lint rule`.

---

### Task 4: Admission - smallest letter that fits, then nearest

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Private/Model/ArrivalPlanner.cpp:10-80`
- Modify: `Plugins/Airside/Source/Airside/Public/Model/ArrivalPlanner.h:128` (doc comment only)
- Test: `Plugins/Airside/Source/AirsideTests/Private/StandChoiceTest.cpp`

**Interfaces:**
- Consumes: `IcaoCode::LetterForWingspan`, `IcaoCode::Parse`, `FEntityInstance::IsStand`, `DesignWingspan`.
- Produces: unchanged `ChooseStand` signature. New file-local helper `TOptional<EIcaoCode> LetterOfSpan(double Uu)` (unset for 0 or wider than F).

Rule, applied in the existing loop over `Reach`:
- Candidates are `bAlive && PoseNode.IsSet() && IsStand()` (depots have a pose node too - they must not be candidates). Keep a parallel `TArray<TOptional<EIcaoCode>> CandidateLetter` from `DesignWingspan`.
- Aircraft letter `Need = LetterOfSpan(Airframe.Wingspan)`. If `Airframe.Wingspan > 0` and `Need` unset (wider than F) -> no stand fits; return unset, log.
- Skip a candidate when both letters are set and `Stand < Need` (ordinal compare). Unknown stand letter (`DesignWingspan == 0`) is admitted, as today.
- Best = lowest stand letter ordinal (unknown ranks as C, the legacy default), tie -> shortest `Reach.Length`, tie -> first (enumeration order, as today).
- One `UE_LOG(LogAirside, Log, TEXT("ChooseStand: span %.1f m -> node %d (Code %s); %d too small, %d held"), ...)` per call that has a winner or saw a refusal. Check `LogAirside` is visible in ArrivalPlanner.cpp (grep `AirsideLog.h`).

- [ ] **Step 1: Failing tests** (fixture: `FTestAirport::Build(TestAirframes::...)` - read `AirsideTestFixtures.h:197-287` and an existing StandChoiceTest case for how stands are laid and how `DesignWingspan` is set; set it directly on the instance if the fixture places via `URoadNetwork::PlaceEntity`):
  - `Airside.Model.StandChoice.SmallestLetterBeatsNearer`: a Code B stand FARTHER and a Code E stand NEARER; King Air span (B) -> chooses B. Reason: "big stands are kept for big aircraft (GDD 'smallest free stand that fits')".
  - `.FallsThroughToBigger`: B held, E free -> King Air gets E.
  - `.TooSmallNeverChosen`: only a B stand, 737 (C) -> unset.
  - `.LegacySpanStillAdmitsC`: stand `DesignWingspan = 3410` (A320), aircraft 3580 (737-800) -> chosen. Reason: "admission compares letters; legacy stands carry the A320's span, not the letter's".
  - `.UnknownSpanAdmitted`: stand `DesignWingspan = 0` -> chosen for a 737.
  - `.WiderThanFNowhere`: aircraft span 9000 -> unset, no crash.
  - `.DepotNeverCandidate`: a fuel depot's pose node nearer than the only stand -> stand chosen (or unset if unreachable), never the depot node.
- [ ] **Step 2: Build twice, run** `-Filter Airside.Model.StandChoice` -> new ones FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `-Filter Airside.Model` (the whole model suite: ChooseStandMultiGoal, StandHold, StandRetarget, Offer tests reach this) -> green. Then `-Filter AirportOps` -> green.
- [ ] **Step 5: Commit** `feat(stands): arrivals take the smallest stand letter that fits, then the nearest`.

---

### Task 5: Definition per letter, and PlaceStandInPlot

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Present/RoadNetworkActor.h` + `Private/Present/RoadNetworkActor.cpp:519-534`
- Modify: `Plugins/Airside/Source/Airside/Public/Tool/RoadEditTarget.h:~314` (new pure virtual)
- Modify: `Plugins/Airside/Source/Airside/Public/Present/RoadEditFacade.h`, `Private/Present/RoadEditFacadeSurfaces.cpp` (new method after PlaceEntityInPlot)
- Modify: every other `IRoadEditTarget` implementor (grep `: public IRoadEditTarget` and test fakes) - forward or stub.
- Test: `Plugins/Airside/Source/AirsideTests/Private/StandPlotPlacementTest.cpp` (new)

**Interfaces:**
- Consumes: Task 1 `MakeStandTransient(Letter, Outer)`, `FitsItsLetter`; Task 2 `StandBox::*`.
- Produces:
  - `UEntityDefinition* ARoadNetworkActor::ResolveStandDefinitionFor(EIcaoCode Letter) const;` - C returns `ResolveStandDefinition()` (honours the authored asset and the per-level override). Others: lazily `MakeStandTransient(Letter, const_cast<ARoadNetworkActor*>(this))` cached in `UPROPERTY() TArray<TObjectPtr<UEntityDefinition>> LetterStandDefinitions;` sized 6, indexed by ordinal. **NOT Transient**: placed instances hold `Definition` by UPROPERTY and the level saves it; a transient object would be nulled on save. Returns null (and logs `LogRoadMesh` Warning once per letter) when `!FitsItsLetter`. `DesignAircraft` = largest shipped `UAircraftType` whose `Code` == letter: add `static UAircraftType* UAirsideSettings::ResolveLargestAircraftOfLetter(EIcaoCode)` in `Content/AirsideSettings` - read how content lists aircraft types (grep `AircraftTypes` in `AirsideContent.h`); null if none (Code D).
  - `virtual int32 IRoadEditTarget::PlaceStandInPlot(const TArray<FVector2D>& Outline, FVector2D EntranceA, FVector2D EntranceB) = 0;` returns entity index or `INDEX_NONE`.
  - `virtual FString IRoadEditTarget::WhyStandRefused(TArrayView<const FVector2D> Outline) const = 0;` - empty string = placeable. ONE evaluator the tool's readout and the facade's commit both ask (the #182 lesson). Reasons, in order: `"the outline crosses itself"`, `"needs %d m more width / %d m more depth"` (vs Code A floor), `"Code %s stands cannot be built yet"` (definition null), `"overlaps stand %d"` / `"overlaps a fuel depot"` (any vertex of one inside the other, or edges crossing - `RoadGeom::PointInPolygon`, `RoadGeom::SegmentsIntersect` or whatever the header names it), `"a taxiway crosses the stand"` (a non-service segment's centreline enters the interior; service roads are ALLOWED - the template's GSE road is in the box's back strip), `"cannot afford ..."`.
- Facade `PlaceStandInPlot` body, mirroring PlaceEntityInPlot's order: `WhyStandRefused` non-empty -> log `LogRoadMesh` Warning "PlaceStandInPlot refused: %s", return INDEX_NONE. Wind CCW (swap entrance on reverse, as PlaceEntityInPlot does). `Letter = *StandBox::LetterOf(rect with entrance first)`; `Inward = PlotYard::InwardOf(Wound, EntranceA, EntranceB)`; `Pose = StandBox::PoseFor(EntranceA, EntranceB, Inward, Letter)`. Quote = `BuildCost::ForEntity` + `QuoteForApron(Wound)`. `FRoadEditScope Edit(..., TEXT("place stand"))`. `FEntityPlacement`: Definition, Anchors, Position = Pose.Position, Heading = `RoadGeom::Bearing(Pose.Facing)`, PoseRole = Aircraft, Outline = Wound, `DesignWingspan = IcaoCode max wingspan of Letter` (find the accessor - `MaxWingspanForWidth(StandWidthForLetter(L))` or the row's MaxWingspan; add `IcaoCode::MaxWingspanForLetter(EIcaoCode)` if none exists, with an IcaoCodeTest line). Check `FEntityPlacement` has `DesignWingspan`; add it if not (it is copied onto the instance by `URoadNetwork::PlaceEntity(const FEntityPlacement&)`). `CommitPurchase`. Log `LogRoadMesh` "PlaceStandInPlot: Code %s stand, %.0f x %.0f m".

- [ ] **Step 1: Failing tests** (`FAirsideTestWorld`, lay a taxiway with `ConnectNodes(..., ERoadKind::Taxiway, ...)` as PlotPlaceToolTest's LayRoad does; give the purse enough money - see how PlotPlacementTest does):
  - `Airside.Present.StandPlot.PlacesCodeC`: a C-floor rect beside the taxiway -> index != NONE; entity `IsStand()`, `IsPlotted()`, heading bearing == Inward (away from taxiway), `LetterForWingspan(DesignWingspan) == LetterOf(outline)` == "C".
  - `.RefusesTooSmall`: 20x20 m -> INDEX_NONE; `WhyStandRefused` contains "more width".
  - `.RefusesOverlap`: second stand overlapping the first -> refused, reason "overlaps stand".
  - `.AllowsServiceRoadInBackStrip`: a service road across the rect's back 4 m -> placed.
  - `.UndoRemoves`: place, undo (read how PlotPlacementTest undoes) -> no live stand.
  - `.UnfitLetterRefused`: for each letter Task 1 recorded as DOES NOT FIT, a rect of that letter's floor -> refused with "cannot be built yet". (If all fit, this test asserts the resolver returns non-null for all six and says so.)
- [ ] **Step 2: Build twice (new file), run** `-Filter Airside.Present.StandPlot` -> FAIL.
- [ ] **Step 3: Implement.** New UPROPERTY on the actor => full build (fine in the worktree).
- [ ] **Step 4: Run** -> PASS; `-Filter Airside.Present` -> green.
- [ ] **Step 5: Commit** `feat(stands): PlaceStandInPlot and a stand definition per letter`.

---

### Task 6: Legacy stands get an outline on load

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Model/RoadNetwork.h`, `Private/Model/RoadNetwork.cpp`
- Test: `Plugins/Airside/Source/AirsideTests/Private/StandPlotPlacementTest.cpp`

**Interfaces:**
- Produces: `int32 URoadNetwork::EnsureStandOutlines();` - for every alive `IsStand()` entity with `Outline.Num() < 3`: `StandBox::BoxAt({Position, (cos Heading, sin Heading)}, EIcaoCode::C, Outline)`; if `DesignWingspan == 0` set it to Code C's max. Returns how many it changed; logs `LogAirside` "EnsureStandOutlines: %d legacy stand(s) given a Code C outline" when > 0. `virtual void PostLoad() override` calls it (check `URoadNetwork` has no PostLoad yet; if a base class does work there, call `Super::PostLoad()` first).
- Heading convention: confirm `Heading` radians -> facing via the same conversion PlaceEntity/GraphOverlay uses (grep `FMath::Cos(Entity.Heading)` or a `RoadGeom::FromBearing`); use that helper, not a hand-rolled one.

- [ ] **Step 1: Failing test** `Airside.Model.StandOutline.LegacyGetsCodeCBox`: place a stand via `IRoadEditTarget::PlaceEntity` (no outline) at (1000,2000) heading 90 deg; call `EnsureStandOutlines()` -> returns 1; outline == `BoxAt` of that pose; the stop mark (Position) is inside the outline; second call returns 0 (idempotent); a depot without outline is untouched.
- [ ] **Step 2: Run** -> FAIL. **Step 3: Implement.** **Step 4: Run** -> PASS; `-Filter Airside.Model` green.
- [ ] **Step 5: Commit** `feat(stands): legacy stands gain their Code C outline on load`.

---

### Task 7: Shared plot gesture helpers + FStandPlotTool on key 3

**Files:**
- Create: `Plugins/Airside/Source/Airside/Public/Tool/PlotGesture.h`, `Private/Tool/PlotGesture.cpp`
- Modify: `Private/Tool/PlotPlaceTool.cpp:12-174` (anonymous-namespace helpers move out), `Public/Tool/PlotPlaceTool.h:46-64` (the `PlotGesture` constants move to PlotGesture.h; PlotPlaceTool.h includes it)
- Create: `Plugins/Airside/Source/Airside/Public/Tool/StandPlotTool.h`, `Private/Tool/StandPlotTool.cpp`
- Modify: `Private/Tool/BuildSession.cpp:47-50` (key 3), and the comment at :80-85 (it says "a stand has no plot" - no longer true)
- Delete: `Public/Tool/StandPlaceTool.h`, `Private/Tool/StandPlaceTool.cpp` ONLY IF grep finds no other user; its tests (StandPlaceToolTest.cpp) are then rewritten against the new tool or deleted where they test press-drag-release specifically - list each deleted test by name in the commit.
- Test: `Plugins/Airside/Source/AirsideTests/Private/StandPlotToolTest.cpp`

**Interfaces:**
- `PlotGesture` namespace (moved verbatim with their WHY comments; comment-line count must not fall): `MinFrontageUu`, `FrontageStepUu`, `AnchorReachUu`, `QuantisedFrontage(double)`, `AnchorIndexAt(double,double)`, `AnchorOffset(int32)`, `KerbOffset(const URoadNetwork&, FRoadSegmentId, bool)`, `IsServiceRoad(const URoadNetwork&, FRoadSegmentId)`, plus new `IsTaxiway(...)` = has an `ETraversalClass::Aircraft` guideline and no GroundVehicle one, and `NearestRoad(const URoadNetwork&, const FVector2D& Cursor, TFunctionRef<bool(const URoadNetwork&, FRoadSegmentId)> Accept, FRoadSegmentId&, double& OutT)`; `NearestServiceRoad` becomes a call with `IsServiceRoad`. **A pure move first, commit it** (depot tests green) before the stand tool.
- `FStandPlotTool : IBuildTool` with `enum class EStandStage : uint8 { Idle, Entrance, Depth, Confirm }` (UENUM in the header, like EPlotStage). Members: `Along`, `Inward`, `Corners[3]` (anchor, entrance end, depth point), `bLastCommitRefused`. `void Rect(const FToolContext&, TArray<FVector2D>& Out) const` - the ONE derivation (preview, readout, commit): corners 0 anchor, 1 = 0 + Along * signed QuantisedFrontage (as `Quad` does), 2 = 1 + Inward * D, 3 = 0 + Inward * D, D = max(0, Dot(GuidedCursor - Corners[0], Inward)) rounded to 100 uu while in Depth stage.
  - Idle click: `NearestRoad(IsTaxiway)`; anchor on the step grid + `KerbOffset` exactly as `FPlotPlaceTool::OnClick` Idle does. Entrance click pins corner 1; Depth click pins D, refuses (stays) if `Target->WhyStandRefused(rect)` says "crosses itself"; Confirm ignores clicks. Cancel steps back one stage. Commit only in Confirm: `Target->PlaceStandInPlot(Rect, Rect[0], Rect[1])`; INDEX_NONE -> stay, `bLastCommitRefused = true`.
  - Remove (`bRemoveModifier`): `FindEntityAt`, accept only `IsStand()`, `DeleteEntity`. Preview doomed polygon + label "remove stand".
  - Preview: Idle anchor dots on the taxiway (as depot) or label "move near a taxiway" `Refused`; the rect lines Pinned/Provisional by stage; from Depth stage on, when a letter exists, the wing keep-out: `IcaoCode::WingKeepOut*` is local-frame - draw the keep-out rectangle `[WingAftForLetter, WingFwdForLetter] x [-W/2, W/2]` transformed by `StandBox::PoseFor` as `Pending` polygon (read `WingKeepOutContains` to confirm the local box before drawing it), plus the lead-in `Line(entrance mid, stop mark, Pending)`; label at the rect centre `"Code C"` (Pending) or the refusal reason (Refused).
  - Readout: Remove -> as depot's. Idle -> warning "Move near a taxiway". Else facts `Stand Points` "N/3", `Size` "%.0f x %.0f m", `Stand` "Code X" or "-", and when a letter below F exists `Next` "Code Y: %d m wider, %d m deeper" (omit zero parts). Warning = `WhyStandRefused` when non-empty (Depth stage onward). `Committable(Stage == Confirm && Why.IsEmpty())`.
- Registry key 3: `{ EKeys::Three, TEXT("Stand"), LOCTEXT("Stand","Stand"), LOCTEXT("StandTooltip", "Place an aircraft stand: click a taxiway to start the entrance, drag along it for width, away from it for depth, then press Build. Bigger stands take bigger aircraft."), [] { return MakeUnique<FStandPlotTool>(); }, EEditHandleKind::None }`. Check the game module for anything keyed on `FStandPlaceTool` or the old tooltip (`grep -rn "StandPlaceTool" Source/`).

- [ ] **Step 1: Move helpers to PlotGesture (pure move).** Build; run `-Filter Airside.Tool.Plot` and `-Filter Airside.Tool.FuelDepot` -> green. Count comment lines in the two touched files before/after (not fewer). Commit `refactor(tools): plot gesture helpers shared in Tool/PlotGesture`.
- [ ] **Step 2: Failing tests** StandPlotToolTest.cpp, prefix `Airside.Tool.StandPlot.` (copy the `OnRoad`/`LayRoad` helpers' approach from PlotPlaceToolTest, but helpers in a NAMED namespace `StandPlotToolFixture` - the unity build collides anonymous duplicates):
  - `.AnchorsOnTaxiwayOnly`: only a service road near -> Idle click leaves Idle; taxiway near -> Entrance.
  - `.WidthSteps`: entrance width quantised to 5 m (read `PlotGesture::FrontageStepUu`, don't retype 500).
  - `.LetterAtThresholds`: drive to Confirm at exactly C floor W x D -> readout fact Stand == "Code C"; 1 cm less depth -> "Code B" (or "-" per table) - thresholds from `IcaoCode`, not literals.
  - `.TooSmallNotCommittable`: 15 x 15 m -> Committable false, warning contains "more".
  - `.CommitPlaces`: Confirm + OnCommit -> one live `IsStand()` entity with a 4-point outline; stage Idle.
  - `.CancelStepsBack`: Confirm -> Depth -> Entrance -> Idle.
  - `.RemoveTakesStandsOnly`: with a stand and a depot placed, Remove over the depot does nothing; over the stand deletes it.
  - `.RegistryKeyThree`: `ToolRegistry()` entry for `EKeys::Three` makes an `FStandPlotTool` (dynamic check via display name "Stand").
- [ ] **Step 3: Build twice, run** -> FAIL. **Step 4: Implement tool + registry.** **Step 5: Run** `-Filter Airside.Tool` -> green (old StandPlaceTool tests rewritten/deleted as listed).
- [ ] **Step 6: Commit** `feat(stands): draw a stand off a taxiway on key 3; size sets its letter`.

---

### Task 8: Stand paint - lead-in, stop bar, letter

**Files:**
- Create: `Plugins/Airside/Source/Airside/Public/Build/StandMarkingBuilder.h`, `Private/Build/StandMarkingBuilder.cpp`
- Modify: `Private/Present/RoadSurfacePresenter.cpp:407-411` (the HoldingPaint lambda also calls the stand builder)
- Test: `Plugins/Airside/Source/AirsideTests/Private/StandMarkingTest.cpp`

**Decision recorded here (deviates from the spec's TextRender):** the letter is PAINT - seven-segment strokes of quads through the same builder - not a `UTextRenderComponent`. A-F are exactly the letters a seven-segment glyph draws (A b C d E F); paint needs no component lifecycle (the transient-subobject-on-duplication trap), is headless-testable, and lies flat by construction. Update the spec's Paint section in this commit.

**Interfaces:**
- Consumes: `MarkingQuads::AddQuad` (read `HoldingPositionMarkingBuilder.cpp` for the exact call and UV1 = 0 convention), `StandBox`, `IcaoCode`.
- Produces: `struct AIRSIDE_API FStandMarkingBuilder { static int32 Build(const URoadNetwork& Network, double MarkingZ, FRoadMeshBuffers& Out, FStandMarkingCensus* Census = nullptr); };` returns stands painted. `FStandMarkingCensus { int32 LeadIns = 0, StopBars = 0, LetterSegments = 0; }`.
  - For each alive `IsStand() && IsPlotted()` entity: letter = `LetterForWingspan(DesignWingspan)`; entrance mid = midpoint of outline edge 0->1 (entrance first is the stored convention from Task 5 - confirm Wound keeps entrance as 0->1 after a reversal; if not, derive entrance mid as `Position - Facing * (Depth - NoseFwd)`, which is exact by `StandBox::PoseFor`, and USE THAT - it needs no winding assumption).
  - Lead-in: 15 cm wide quad from entrance mid to stop mark. Stop bar: 40 cm x 3 m quad across the facing at the stop mark. Letter: 7-segment glyph, 3 m tall, stroke 30 cm, centred 4 m inside the entrance, reading upright to a pilot taxiing IN (glyph "up" = Facing). Segment table `static const uint8 GlyphSegments[6]` for A,b,C,d,E,F (bits a..g).
  - Census counts segments painted so a test can measure without decoding triangles.

- [ ] **Step 1: Failing tests** `Airside.Build.StandMarking.`:
  - `.PaintsOnePerStand`: two drawn stands (C, E) + one depot -> returns 2; census LeadIns 2, StopBars 2.
  - `.GlyphSegments`: C stand -> LetterSegments == 4 (a,d,e,f); E -> 5 (a,d,e,f,g). Reason: "the letter on the ground is the letter admission uses".
  - `.LeadInEndsAtStopMark`: last two vertices of the lead-in quad straddle the entity Position (distance to Position <= stroke half-width + 0.01).
  - `.QuadsFaceUp`: every triangle's normal Z > 0 via the engine's computed normal as other marking tests do (memory: CCW faces DOWN in Unreal - assert on the engine normal).
- [ ] **Step 2: Build twice, run** -> FAIL. **Step 3: Implement + hook into RebuildMarkings** (log line gains `, %d stand(s)`). **Step 4: Run** `-Filter Airside.Build` + `-Filter Airside.Present` -> green.
- [ ] **Step 5: Commit** `feat(stands): paint the lead-in, stop bar and letter`.

---

### Task 9: Composition + full run + handoff

**Files:**
- Test: `Plugins/Airside/Source/AirsideTests/Private/StandPlotToolTest.cpp`
- Modify: `docs/superpowers/specs/2026-09-23-drawn-stands-design.md` (any deviation found during execution, with the reason)

- [ ] **Step 1: Composition test** `Airside.Tool.StandPlot.DrawnStandTakesAnArrival`: in `FAirsideTestWorld`, lay a taxiway network with a runway exit as the existing arrival composition tests do (grep `LandAircraft\|DispatchArrival` in AirsideTests for the smallest existing end-to-end fixture and reuse it), draw a Code C stand THROUGH `FStandPlotTool` clicks + OnCommit, tick until the arrival parks; assert the agent's stand node == the new stand's PoseNode. Reason: "the seam from tool to admission is wired - a stand the tool builds is one the planner chooses".
- [ ] **Step 2: Build twice, run it** -> PASS (if it fails, the fault is in Tasks 4-7 - fix there).
- [ ] **Step 3: Full authoritative run**: `./Tools/Run-AirsideTests.ps1 -Project "C:\repos\aircraftmgr-stands\AirportMgr.uproject"` -> quote the `N test(s) run, 0 failed, 0 crashed` line (baseline 744 + new).
- [ ] **Step 4: Refactor-contract counts** for PlotPlaceTool.cpp/PlotGesture.cpp: `UE_LOG(` count and comment-line count vs main; state both in the PR.
- [ ] **Step 5: Commit** `test(stands): drawn stand takes an arrival end to end`.
- [ ] **Step 6: Push branch; open PR** to main with the template filled (build line, test line, log/comment deltas). PIE verification is the user's: steps in the PR body - key 3, click near a taxiway, drag width then depth, watch the letter change at ~53 m / 67 m widths, Build, land with key 7, check `LogAirside: ChooseStand:` line names the stand.
