# Fuel Service Slice Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every parked aircraft demands fuel; one truck drives from a player-placed depot over service roads to the stand, dwells, and goes home - watchable end to end in PIE.

**Architecture:** Airside gains only nouns it lacks (a service-road `URoadProfile`, an
`EServiceRole` on an entity's pose, a vehicle airframe default, a vehicle view) and never
learns what a truck is FOR. AirportOps gains `UFuelService`, a world-free `UObject` in its
`Model/` layer that reads `UGroundTraffic` and `URoadNetwork` and drives dispatch through
them; `UOpsRuntime` owns it, ticks it and relays the phase events. Crossings need no new
code - `FRoadGuidelineBuilder` already intersects arm class masks per turn path.

**Tech Stack:** UE 5.8.2, C++. Plugins `Airside` (model) and `AirportOps` (game facts),
game module `AirportMgr` (driver). Tests are `IMPLEMENT_SIMPLE_AUTOMATION_TEST` in
`AirsideTests` / `AirportOpsTests`, run by `Tools/Run-AirsideTests.ps1`.

**Spec:** `docs/superpowers/specs/2026-09-07-fuel-service-slice-design.md`

---

## Global Constraints

- **Layer rule, enforced by `Tools/Check-Architecture.ps1`.** `Model/` and `Solve/` never
  include `Build|Tool|Present|Entities`. `Tool/` never includes `Present/`. `Build/` never
  includes `Present|Tool`. Airside never names `AirportOps` in code (include, `AIRPORTOPS_API`,
  or a `UOps*` identifier) - a WHY comment naming it is fine. **This holds inside AirportOps
  too**: `AirportOps/Model/UFuelService` may include Airside's `Model/GroundTraffic.h` and
  `Model/RoadNetwork.h`, and must NOT include `Present/RoadNetworkActor.h` or
  `Present/AirsideTraffic.h`.
- **One `DEFINE_LOG_CATEGORY_STATIC` per name per module** (unity build). AirportOps has
  `LogAirportOps` in `Public/AirportOpsLog.h` - use it, do not define a new static.
- **A phase is an enum, never a set of bools.**
- **One struct per thing.** Do not copy fields into a sibling struct; pass the bundle.
- **Every `UE_LOG` survives a move; every WHY comment travels with its code.**
- **Interface virtuals keep their old names**, as non-virtual forwarders where a signature widens.
- **Comments explain WHY, and especially why an obvious alternative was rejected.** Match the
  surrounding density; do not strip it.
- **Distances are uu; 1 uu = 1 cm. Headings in the model are radians.**
- **Build line** (editor must be CLOSED, or use a worktree with `-NoHotReloadFromIDE`):
  ```
  D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development `
    -Project="C:\repos\AirportMgr2\AirportMgr.uproject" -WaitMutex
  ```
- **Test line:** `./Tools/Run-AirsideTests.ps1 -Filter <Airside.Foo|AirportOps.Foo>`. **Never
  trust the exit code** - read the `N test(s) run, N failed, N crashed` line.
- **Test names must be leaf-distinct.** UE's automation tree drops a bare-named parent once a
  dotted child exists, so never name a test `Airside.Ops.Fuel` and another `Airside.Ops.Fuel.X`.

---

## Spec amendments

Read the spec first. Four of its statements are wrong against the live headers. Each is
changed here for a reason, and the reason goes in a comment at the site (CLAUDE.md: "Design
decisions are revisable - change for reasons, after discussion, reasoning recorded").

1. **§3: "A road never carries a holding position, runway or intermediate."** REJECTED.
   `UGroundTraffic::UpdateCrossing` arms the runway crossing hold at a node whose
   `HoldingPositionFor` is set (`GroundTrafficClaims.cpp`, rule 0a). Skipping GroundVehicle
   arms in the derivation would mean a truck crossing a runway arms nothing, claims nothing,
   and drives onto a live strip - which the very next spec bullet ("the crossing hold holds
   the strip by the truck's body exactly as for an aircraft. Nothing here is special-cased")
   forbids. **No change to holding-position derivation.** The intermediate case is left alone
   too: an intermediate position is inert until M3's sequencer, so a rule forbidding one on a
   road would be a special case that buys nothing.

2. **§3: "A road ending ON a runway is refused by the road tool, the one placement it refuses."**
   REJECTED. A crossing is DRAWN by clicking on the runway (`ERoadSnapKind::Segment` ->
   `SplitSegment`) and then chaining to the far side, so refusing that click forbids crossings
   as well as dead ends. And the dead end is not a hazard: the road's guideline end node is
   pushed clear of the strip by `ExitGeometry::TaxiwayEndFloor`, and its turn paths onto the
   runway's own guideline are Emergency-only, so a truck routed there parks off the asphalt.
   **Reported by the census (Task 3), refused by nothing** - the same treatment §3 already
   gives a road ending against a taxiway's side.

3. **§4/§5/§6: the depot's road connection.** §4 gives the depot "ONE anchor of role Fuel",
   §5 and §6 route from "the depot's pose node". `FAnchorLink::Build` casts the pose lead-in
   as `ETraversalClass::Aircraft` unconditionally, so a depot's pose node would log
   "joins nothing: no derived aircraft guideline" on every rebuild forever. **Resolution: the
   POSE NODE is the depot's road connection** (`UEntityDefinition::PoseRole` decides the class),
   and `DA_FuelDepot` declares NO anchors. Two lead-ins from one small building into one road
   would be a duplicate painted line.

4. **§6: "the sim clock".** REJECTED as the dwell's time base. `USimClock::Now()` is
   day-compressed: at the default `RealSecondsPerGameDay = 1200` a 40 s dwell is 0.55 real
   seconds. The truck's MOTION runs on `USimClock::Multiplier` alone (`ARoadNetworkActor::Tick`
   scales by `SimTimeScale`), and `UGroundTraffic::GetSimSeconds()` accrues in exactly that
   base. **`UFuelService` reads `UGroundTraffic::GetSimSeconds()`**, so the dwell is as long as
   it looks and the service needs no clock of its own.

5. **§6: `InspectFacts::DescribeEntity`.** The function is `DescribeStand`. Widened in place
   rather than renamed - a rename touches four call sites and buys nothing this slice.

---

## File structure

**Airside plugin** (`Plugins/Airside/Source/Airside`)

| File | Responsibility this slice |
|---|---|
| `Public/Profiles/RoadProfile.h` / `Private/Profiles/RoadProfile.cpp` | `FillServiceRoad`, `MakeServiceRoadTransient` |
| `Public/Content/AirsideContent.h` | `ServiceRoadProfile`, `DefaultFuelDepot`, `VehicleMesh` slots |
| `Public/Content/AirsideSettings.h` / `.cpp` | `ResolveDefaultVehicle()` |
| `Public/Model/RoadEntity.h` | `FEntityInstance::PoseRole` |
| `Public/Model/RoadNetwork.h` / `.cpp` | `PlaceEntity` takes `PoseRole` |
| `Public/Model/InspectFacts.h` / `.cpp` | `FAgentFacts::Fuel`, `FStandFacts::PoseRole` |
| `Public/Entities/EntityDefinition.h` / `.cpp` | `PoseRole`, `FootprintExtent`, `BuildFuelDepot`, `MakeFuelDepotTransient` |
| `Private/Build/AnchorLink.cpp` | pose lead-in casts by `PoseRole` |
| `Private/Build/RoadGuidelineBuilder.cpp` | census line for arms with no through path |
| `Public/Tool/RoadEditTarget.h` | `ERoadKind`, `EPlaceableEntity`, widened virtuals + forwarders |
| `Public/Tool/RoadDrawTool.h` / `.cpp` | tool and states carry `ERoadKind` |
| `Public/Tool/StandPlaceTool.h` / `.cpp` | tool carries `EPlaceableEntity` |
| `Private/Tool/StandPreview.cpp` | draws `FootprintExtent` |
| `Private/Tool/BuildSession.cpp` | registry entries for key 9 and key 0 |
| `Public/Present/RoadNetworkActor.h` / `.cpp` | `ServiceRoadProfile`, `FuelDepotDefinition`, resolvers, widened forwarders |
| `Public/Present/RoadEditFacade.h` / `.cpp` | widened `ConnectNodes` / `PlaceStand` |
| `Public/Present/RoadAgentActor.h` / `.cpp` | `SetVehicleBody` |
| `Private/Present/AirsideTraffic.cpp` | `SpawnView` dresses by agent class |

**AirportOps plugin** (`Plugins/AirportOps/Source/AirportOps`)

| File | Responsibility |
|---|---|
| `Public/Model/FuelService.h` (new) | `EFuelDemandState`, `EFuelRefusal`, `FFuelDemand`, `UFuelService` |
| `Private/Model/FuelService.cpp` (new) | the state machine, depot choice, log lines |
| `Public/Model/OpsDefinition.h` | `UScenario::FuelDwellSeconds` |
| `Public/Present/OpsRuntime.h` / `.cpp` | owns, ticks and feeds `UFuelService` |

**Game module** (`Source/AirportMgr`) - `InspectorWidget.cpp` only: the fuel line and the
depot card.

**Tools** - `Tools/Python/build_road_profiles.py` (new), `Tools/Python/build_stand_asset.py`
(depot added).

---

### Task 1: The service-road cross-section

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Profiles/RoadProfile.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Profiles/RoadProfile.cpp`
- Modify: `Plugins/Airside/Source/Airside/Public/Content/AirsideContent.h`
- Modify: `Plugins/Airside/Source/Airside/Public/Present/RoadNetworkActor.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Present/RoadNetworkActor.cpp`
- Test: `Plugins/Airside/Source/AirsideTests/Private/ServiceRoadProfileTest.cpp` (new)

**Interfaces:**
- Consumes: `URoadProfile`, `FProfileBand`, `FProfileGuideline`, `ERoadBandType`,
  `ETraversalClass`, `EGuidelineDir`, `UAirsideSettings::GetContent()`.
- Produces:
  - `static void URoadProfile::FillServiceRoad(URoadProfile* Profile, double LaneWidth, double KerbWidth, double FilletRadius)` (`UFUNCTION(BlueprintCallable)`)
  - `static URoadProfile* URoadProfile::MakeServiceRoadTransient(double LaneWidth = 600.0, double KerbWidth = 60.0, double FilletRadius = 500.0)`
  - `UAirsideContent::ServiceRoadProfile` (`TSoftObjectPtr<URoadProfile>`)
  - `ARoadNetworkActor::ServiceRoadProfile` (`UPROPERTY(EditAnywhere) TObjectPtr<URoadProfile>`)
  - `URoadProfile* ARoadNetworkActor::ResolveServiceRoadProfile() const`

- [ ] **Step 1: Write the failing test**

Create `Plugins/Airside/Source/AirsideTests/Private/ServiceRoadProfileTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadTraffic.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FServiceRoadProfileTest,
	"Airside.Build.RoadProfileGuideline",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FServiceRoadProfileTest::RunTest(const FString& Parameters)
{
	URoadProfile* Road = URoadProfile::MakeServiceRoadTransient();
	if (!TestNotNull(TEXT("a service road profile"), Road)) { return false; }

	// ONE guideline, because a road is a single shared lane in this slice - two mirrored
	// ones are what FProfileGuideline's header calls the case that recovers "lane", and
	// nothing routes on a side yet.
	if (!TestEqual(TEXT("exactly one guideline"), Road->Guidelines.Num(), 1)) { return false; }

	const FProfileGuideline& Lane = Road->Guidelines[0];
	TestEqual(TEXT("it admits ground vehicles, not aircraft"),
		static_cast<int32>(Lane.Class), static_cast<int32>(ETraversalClass::GroundVehicle));
	TestEqual(TEXT("bidirectional: one lane shared both ways"),
		static_cast<int32>(Lane.Direction), static_cast<int32>(EGuidelineDir::Bidirectional));
	TestEqual(TEXT("MaxWingspan 0 - unlimited, because no wing ever uses it"), Lane.MaxWingspan, 0.0);
	TestEqual(TEXT("centred on the road"), Lane.CentreOffset, 0.0);

	// KERBS, not run-offs. The edge-treatment note: a road is kerbed and a taxiway has a
	// paved run-off, and the difference is what tells the two apart on the ground.
	int32 Kerbs = 0;
	for (const FProfileBand& Band : Road->Bands)
	{
		Kerbs += Band.Type == ERoadBandType::Curb ? 1 : 0;
	}
	TestEqual(TEXT("a kerb each side"), Kerbs, 2);

	// NOT CONTINUOUS: a road gives way to a paved junction like a taxiway, and only a
	// runway runs unbroken through one.
	TestFalse(TEXT("not continuous through junctions"), Road->bContinuousThroughJunctions);

	// NO EXIT LENGTH. Exit arcs are a runway's own grading (see URoadProfile::ExitLength);
	// a service road meeting a taxiway is an ordinary junction with no exit to grade.
	TestEqual(TEXT("no exit length"), Road->ExitLength, 0.0);

	TestTrue(TEXT("narrower than a taxiway - a lane, not a movement area"),
		Road->GetTotalWidth() < 2300.0);
	return true;
}

#endif
```

- [ ] **Step 2: Run it and watch it fail**

Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside.Build.RoadProfileGuideline`
Expected: compile failure - `MakeServiceRoadTransient` is not a member of `URoadProfile`.

- [ ] **Step 3: Add `FillServiceRoad` and `MakeServiceRoadTransient`**

In `Public/Profiles/RoadProfile.h`, immediately after the existing `Fill` declaration
(a doc comment touches its declaration - do not insert between the two):

```cpp
	/**
	 * Fills Profile with the SERVICE ROAD cross-section: kerb | lane | kerb, one guideline
	 * of class GroundVehicle.
	 *
	 * A SECOND FILL RATHER THAN A PARAMETER ON THE FIRST, deliberately. Fill's taxiway is a
	 * concrete lane between asphalt run-offs carrying ONE aircraft guideline; this is a
	 * narrow kerbed lane carrying ONE vehicle guideline, and the two differ in band TYPE,
	 * band count, guideline class and exit length. A shared function taking five flags would
	 * be a switch on "which road is this" spelled as parameters, and the caller would still
	 * have to know which combination meant a road.
	 *
	 * NOT CONTINUOUS and NO EXIT LENGTH - see bContinuousThroughJunctions and ExitLength.
	 * A road gives way to a paved junction; only a runway runs through one, and only a
	 * runway grades its own exits.
	 *
	 * Exposed to script for the reason Fill is: the commandlet that writes
	 * DA_RoadProfile_ServiceRoad must lay down the same bands the tests exercise.
	 */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	static void FillServiceRoad(URoadProfile* Profile, double LaneWidth, double KerbWidth,
		double FilletRadius);

	/**
	 * FillServiceRoad plus a NewObject, so there is one description of a service road.
	 *
	 * Defaults are a 6 m lane with 0.6 m kerbs on a 5 m corner: wide enough for two vans to
	 * pass, tight enough that a road reads as a road beside a 23 m taxiway.
	 */
	static URoadProfile* MakeServiceRoadTransient(double LaneWidth = 600.0,
		double KerbWidth = 60.0, double FilletRadius = 500.0);
```

In `Private/Profiles/RoadProfile.cpp`, at the end of the file:

```cpp
URoadProfile* URoadProfile::MakeServiceRoadTransient(double LaneWidth, double KerbWidth,
	double FilletRadius)
{
	URoadProfile* Profile = NewObject<URoadProfile>(GetTransientPackage());
	FillServiceRoad(Profile, LaneWidth, KerbWidth, FilletRadius);
	return Profile;
}

void URoadProfile::FillServiceRoad(URoadProfile* Profile, double LaneWidth, double KerbWidth,
	double FilletRadius)
{
	if (Profile == nullptr)
	{
		return;
	}

	Profile->Bands.Reset();
	Profile->Guidelines.Reset();

	// Clamped for the reason Fill clamps its shoulder: a lane of zero or negative width puts
	// the band boundaries out of order and inverts the ribbon.
	const double Kerb = FMath::Clamp(KerbWidth, 0.0, FMath::Max(LaneWidth, 0.0) * 0.45);
	const double Lane = FMath::Max(LaneWidth - 2.0 * Kerb, 0.0);

	auto AddBand = [Profile](double Width, ERoadBandType Type, const TCHAR* Slot)
	{
		if (Width <= 0.0) { return; }
		FProfileBand Band;
		Band.Width = Width;
		Band.Type = Type;
		Band.MaterialSlot = Slot;
		Profile->Bands.Add(Band);
	};

	// KERBS, NOT RUN-OFFS, and that is the whole visual difference from a taxiway. A taxiway
	// is graded so an aircraft that leaves the pavement survives it; a road has a raised
	// edge that keeps a van on it. The slot names match DA_RoadMaterials (Asphalt, Concrete,
	// Kerb) so a set assigned on the actor skins this without further authoring.
	AddBand(Kerb, ERoadBandType::Curb, TEXT("Kerb"));
	AddBand(Lane, ERoadBandType::Lane, TEXT("Asphalt"));
	AddBand(Kerb, ERoadBandType::Curb, TEXT("Kerb"));

	// ONE guideline, centred, bidirectional, GroundVehicle. Bidirectional because a 6 m lane
	// is one line both ways; the traffic model's own gap and footprint rules are what keep
	// two trucks apart on it, exactly as they do two aircraft on a taxiway.
	FProfileGuideline Centre;
	Centre.CentreOffset = 0.0;
	Centre.Class = ETraversalClass::GroundVehicle;
	Centre.Direction = EGuidelineDir::Bidirectional;
	Centre.Width = Lane;

	// 0 IS UNLIMITED (see FProfileGuideline::MaxWingspan), which is right rather than lax:
	// nothing with a wing is admitted here at all - the CLASS refuses aircraft - so a span
	// limit would be a second, weaker statement of a rule already made exactly.
	Centre.MaxWingspan = 0.0;
	Profile->Guidelines.Add(Centre);

	Profile->CentrelineOffset = -1.0;
	Profile->PreferredFilletRadius = FilletRadius;
	Profile->bContinuousThroughJunctions = false;

	// A ROAD HAS NO EXITS TO GRADE. ExitLength is read only from a continuous profile
	// (see its comment), so this is belt and braces - but a non-zero value here would be an
	// authored number nothing reads, which is the failure this codebase has shipped three times.
	Profile->ExitLength = 0.0;
}
```

- [ ] **Step 4: Run the test and watch it pass**

Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside.Build.RoadProfileGuideline`
Expected: `1 test(s) run, 0 failed, 0 crashed`.

- [ ] **Step 5: Add the content slot and the actor resolver**

In `Public/Content/AirsideContent.h`, after `RunwayProfiles`:

```cpp
	/**
	 * The service-road cross-section a truck drives on.
	 *
	 * AN AUTHORED ASSET, for the reason RunwayProfiles gives: a segment stores a POINTER to
	 * its profile, and URoadNetwork::DefaultProfile repairs a null one with the TAXIWAY
	 * profile. A transient service-road profile would therefore not merely vanish on reload,
	 * it would come back as a taxiway - the road widened to 23 m and, far worse, admitting
	 * aircraft onto a lane laid for vans, with nothing to report it.
	 *
	 * Null is still legal and means the road tool refuses to lay one and says why, which is
	 * the same treatment PlaceRunway gives a missing runway profile.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Defaults")
	TSoftObjectPtr<URoadProfile> ServiceRoadProfile;
```

In `Public/Present/RoadNetworkActor.h`, beside the existing `Profile` property:

```cpp
	/**
	 * Cross-section for SERVICE ROADS laid through this facade - see ERoadKind. Unset falls
	 * back to the content set's ServiceRoadProfile.
	 *
	 * A SEPARATE PROPERTY rather than a list keyed by kind: there are two kinds and a map in
	 * the Details panel would be harder to author than two pickers, for no gain until a
	 * third exists.
	 *
	 * NO FallbackWidth TWIN, unlike Profile. Profile's on-demand RuntimeProfile exists so
	 * the FIRST click of a session lays something; a road that fell back to a transient
	 * profile would come back from a save as a taxiway (see UAirsideContent::ServiceRoadProfile),
	 * so the road tool refuses instead and says which asset is missing.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside")
	TObjectPtr<URoadProfile> ServiceRoadProfile;
```

and beside `ResolveStandDefinition`'s declaration in the public resolver block:

```cpp
	/** The authored value if there is one, else the configured content default. Null is a
	 *  supported state: the road tool refuses and says so. */
	URoadProfile* ResolveServiceRoadProfile() const;
```

In `Private/Present/RoadNetworkActor.cpp`, beside `ResolveStandDefinition`:

```cpp
URoadProfile* ARoadNetworkActor::ResolveServiceRoadProfile() const
{
	if (ServiceRoadProfile != nullptr) { return ServiceRoadProfile; }
	const UAirsideContent* Content = UAirsideSettings::GetContent();
	return Content != nullptr ? Content->ServiceRoadProfile.LoadSynchronous() : nullptr;
}
```

- [ ] **Step 6: Build**

Run the build line from Global Constraints.
Expected: `Result: Succeeded`. (Adding UPROPERTYs means a full build, not Live Coding.)

- [ ] **Step 7: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Profiles/RoadProfile.h `
        Plugins/Airside/Source/Airside/Private/Profiles/RoadProfile.cpp `
        Plugins/Airside/Source/Airside/Public/Content/AirsideContent.h `
        Plugins/Airside/Source/Airside/Public/Present/RoadNetworkActor.h `
        Plugins/Airside/Source/Airside/Private/Present/RoadNetworkActor.cpp `
        Plugins/Airside/Source/AirsideTests/Private/ServiceRoadProfileTest.cpp
git commit -m "feat(airside): service road cross-section - kerbed lane, one GroundVehicle guideline"
```

---

### Task 2: Laying a service road - `ERoadKind` through the tool seam

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Tool/RoadEditTarget.h`
- Modify: `Plugins/Airside/Source/Airside/Public/Tool/RoadDrawTool.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Tool/RoadDrawTool.cpp`
- Modify: `Plugins/Airside/Source/Airside/Private/Tool/BuildSession.cpp`
- Modify: `Plugins/Airside/Source/Airside/Public/Present/RoadEditFacade.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Present/RoadEditFacade.cpp`
- Modify: `Plugins/Airside/Source/Airside/Public/Present/RoadNetworkActor.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Present/RoadNetworkActor.cpp`
- Test: `Plugins/Airside/Source/AirsideTests/Private/ServiceRoadToolTest.cpp` (new)

**Interfaces:**
- Consumes: Task 1's `ARoadNetworkActor::ResolveServiceRoadProfile()`;
  `IRoadEditTarget::ConnectNodes/UpdateGhost`; `ToolRegistry()`; `FRoadDrawTool`.
- Produces:
  - `enum class ERoadKind : uint8 { Taxiway, ServiceRoad };` in `Tool/RoadEditTarget.h`
  - `virtual bool IRoadEditTarget::ConnectNodes(int32 From, int32 To, ERoadKind Kind) = 0;`
    plus non-virtual `bool ConnectNodes(int32 From, int32 To)` defaulting to `Taxiway`
  - `virtual void IRoadEditTarget::UpdateGhost(int32 From, const FRoadSnapResult&, bool bValid, ERoadKind Kind) = 0;`
    plus a non-virtual 3-arg forwarder
  - `explicit FRoadDrawTool(ERoadKind Kind = ERoadKind::Taxiway)`
  - registry index 7: key `EKeys::Nine`, name "Road"

- [ ] **Step 1: Write the failing test**

Create `Plugins/Airside/Source/AirsideTests/Private/ServiceRoadToolTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Model/RoadTraffic.h"
#include "Present/RoadNetworkActor.h"
#include "Profiles/RoadProfile.h"
#include "Tool/BuildSession.h"
#include "Tool/RoadDrawTool.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FServiceRoadToolTest,
	"Airside.Tool.ServiceRoadLaid",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FServiceRoadToolTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("a world"), World)) { return false; }
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };

	ARoadNetworkActor* Actor = World->SpawnActor<ARoadNetworkActor>();
	if (!TestNotNull(TEXT("the actor"), Actor)) { return false; }

	// No content set is configured in an automation run, so the road profile is assigned by
	// hand here - which is also the state a project with no authored asset is in, and the
	// reason the refusal below is worth pinning.
	URoadProfile* RoadProfile = URoadProfile::MakeServiceRoadTransient();
	Actor->ServiceRoadProfile = RoadProfile;

	const int32 A = Actor->PlaceNode(FVector2D(0.0, 0.0));
	const int32 B = Actor->PlaceNode(FVector2D(10000.0, 0.0));
	if (!TestTrue(TEXT("a service road connects"),
		Actor->ConnectNodes(A, B, ERoadKind::ServiceRoad))) { return false; }

	const TArray<FRoadSegment>& Segments = Actor->Network->GetSegments();
	if (!TestEqual(TEXT("one segment"), Segments.Num(), 1)) { return false; }

	// THE POINT OF THE WHOLE TASK: the segment carries the ROAD profile, not the taxiway's.
	// Asserted on the pointer rather than on the width, because a width that happened to
	// match would pass while the guideline class was still Aircraft.
	TestEqual(TEXT("the segment carries the service road profile"),
		Actor->Network->ProfileFor(Segments[0]), static_cast<const URoadProfile*>(RoadProfile));

	// And the default overload still lays a taxiway - every caller written before this task
	// meant that, and there are ~50 of them.
	const int32 C = Actor->PlaceNode(FVector2D(0.0, 10000.0));
	TestTrue(TEXT("the two-argument overload still connects"), Actor->ConnectNodes(A, C));
	TestNotEqual(TEXT("and lays a taxiway, not a road"),
		Actor->Network->ProfileFor(Actor->Network->GetSegments()[1]),
		static_cast<const URoadProfile*>(RoadProfile));

	// A refusal, not a silent taxiway: with no road profile anywhere, laying a road must
	// fail loudly rather than lay a 23 m aircraft lane the player will find out about later.
	Actor->ServiceRoadProfile = nullptr;
	const int32 D = Actor->PlaceNode(FVector2D(10000.0, 10000.0));
	TestFalse(TEXT("no road profile refuses the road"),
		Actor->ConnectNodes(C, D, ERoadKind::ServiceRoad));

	// The registry is ONE list (CLAUDE.md): the tool exists, under key 9, named Road.
	const TConstArrayView<FToolRegistration> Registry = ToolRegistry();
	bool bFoundRoad = false;
	for (const FToolRegistration& Entry : Registry)
	{
		if (Entry.Key == EKeys::Nine)
		{
			bFoundRoad = true;
			TestEqual(TEXT("key 9 is the Road tool"), Entry.Name.ToString(), FString(TEXT("Road")));
			const TUniquePtr<IBuildTool> Tool = Entry.Make();
			TestEqual(TEXT("and its display name agrees with the registry"),
				Tool->GetDisplayName().ToString(), Entry.Name.ToString());
		}
	}
	TestTrue(TEXT("key 9 is registered"), bFoundRoad);
	return true;
}

#endif
```

- [ ] **Step 2: Run it and watch it fail**

Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside.Tool.ServiceRoadLaid`
Expected: compile failure - `ERoadKind` undeclared.

- [ ] **Step 3: Widen the seam**

In `Public/Tool/RoadEditTarget.h`, above `class IRoadEditTarget`:

```cpp
/**
 * Which cross-section a build gesture lays.
 *
 * AN ENUM ON THE SEAM RATHER THAN A URoadProfile*, for the reason the seam's own header
 * gives about indices: a tool has no business naming an asset, and resolving WHICH profile
 * a kind means is the facade's job (ARoadNetworkActor::ResolveProfile /
 * ::ResolveServiceRoadProfile), in one place, where a missing one can be refused once.
 *
 * NOT on FToolContext either: the kind is a fact about the TOOL the player selected, not
 * about the gesture, and a context field would let two tools disagree about it.
 */
enum class ERoadKind : uint8
{
	Taxiway,
	ServiceRoad
};
```

Replace the `ConnectNodes` declaration with:

```cpp
	virtual bool ConnectNodes(int32 FromIndex, int32 ToIndex, ERoadKind Kind) = 0;

	/** Taxiway by default - what every caller before the fuel slice meant. A non-virtual
	 *  overload, so implementers override one signature; they carry
	 *  `using IRoadEditTarget::ConnectNodes;` so this one stays visible on the concrete type. */
	bool ConnectNodes(int32 FromIndex, int32 ToIndex)
	{
		return ConnectNodes(FromIndex, ToIndex, ERoadKind::Taxiway);
	}
```

and the `UpdateGhost` declaration with:

```cpp
	virtual void UpdateGhost(int32 FromNodeIndex, const FRoadSnapResult& Snap, bool bValid,
		ERoadKind Kind) = 0;

	/** Taxiway by default, as ConnectNodes. The ghost must show the width the click will
	 *  actually lay: a 23 m preview over a 6 m road is a lie the player acts on. */
	void UpdateGhost(int32 FromNodeIndex, const FRoadSnapResult& Snap, bool bValid)
	{
		UpdateGhost(FromNodeIndex, Snap, bValid, ERoadKind::Taxiway);
	}
```

- [ ] **Step 4: Implement it on the facade and the actor**

In `Public/Present/RoadEditFacade.h` change the two overrides and add the `using`
declarations beside the existing `using IRoadEditTarget::PlaceRunway;`:

```cpp
	virtual bool ConnectNodes(int32 FromIndex, int32 ToIndex, ERoadKind Kind) override;
	using IRoadEditTarget::ConnectNodes;
	...
	virtual void UpdateGhost(int32 FromNodeIndex, const FRoadSnapResult& Snap, bool bValid,
		ERoadKind Kind) override;
	using IRoadEditTarget::UpdateGhost;
```

In `Private/Present/RoadEditFacade.cpp`, change `ConnectNodes`' signature and replace the
`AddStraightSegment` line (keeping every existing log line and comment) with:

```cpp
	// RESOLVED PER KIND, in the facade rather than in the tool - see ERoadKind. A road with
	// no profile is REFUSED rather than laid as a taxiway, the same choice PlaceRunway
	// makes and for the same reason: the right shape on screen and the wrong behaviour at
	// every junction, with nothing to say so. Worse here than there, because a taxiway
	// admits aircraft onto a lane laid for vans.
	URoadProfile* Chosen = Kind == ERoadKind::ServiceRoad
		? Owner.ResolveServiceRoadProfile()
		: Owner.ResolveProfile();
	if (Kind == ERoadKind::ServiceRoad && Chosen == nullptr)
	{
		UE_LOG(LogRoadMesh, Warning,
			TEXT("ConnectNodes refused: %d -> %d, no service road profile. Author "
				 "DA_RoadProfile_ServiceRoad with Tools/Python/build_road_profiles.py, or set "
				 "ServiceRoadProfile on the actor."), FromIndex, ToIndex);
		return false;
	}

	const FRoadSegmentId Segment = Owner.Network->AddStraightSegment(From, To, Chosen);
```

Move the `FRoadEditScope Edit(...)` line so it still comes AFTER every guard that refuses
without mutating - the new refusal above included. Its existing comment says exactly that
and must stay true.

In `Private/Present/RoadEditFacade.cpp`, `UpdateGhost` forwards the kind:

```cpp
void URoadEditFacade::UpdateGhost(int32 FromNodeIndex, const FRoadSnapResult& Snap, bool bValid,
	ERoadKind Kind)
{
	Actor().UpdateGhost(FromNodeIndex, Snap, bValid, Kind);
}
```

In `Public/Present/RoadNetworkActor.h` widen the two overrides identically, add
`using IRoadEditTarget::ConnectNodes;` and `using IRoadEditTarget::UpdateGhost;` beside
them, and change `MakeGhostSurfaceSettings` to take the kind:

```cpp
	/** The narrower FSurfaceSettings UpdateGhost/BuildGhostBuffers need - see its own
	 *  comment for why this is not MakeSurfaceSettings with most of it discarded. Kind
	 *  chooses the profile, so the ghost is the width the click will lay. */
	URoadSurfacePresenter::FSurfaceSettings MakeGhostSurfaceSettings(ERoadKind Kind);
```

In `Private/Present/RoadNetworkActor.cpp`:

```cpp
URoadSurfacePresenter::FSurfaceSettings ARoadNetworkActor::MakeGhostSurfaceSettings(ERoadKind Kind)
{
	// ... existing body unchanged, except:
	Settings.Profile = Kind == ERoadKind::ServiceRoad ? ResolveServiceRoadProfile() : ResolveProfile();
	return Settings;
}

bool ARoadNetworkActor::ConnectNodes(int32 FromIndex, int32 ToIndex, ERoadKind Kind)
{
	return Facade->ConnectNodes(FromIndex, ToIndex, Kind);
}

void ARoadNetworkActor::UpdateGhost(int32 FromNodeIndex, const FRoadSnapResult& Snap, bool bValid,
	ERoadKind Kind)
{
	// ... existing guards unchanged ...
	Presenter->UpdateGhost(Network, FromNodeIndex, Snap, bValid, MakeGhostSurfaceSettings(Kind));
}
```

`BuildGhostBuffers` keeps its taxiway ghost - it is the test seam for "a preview leaves the
real network unchanged" and has no kind of its own. Pass `ERoadKind::Taxiway` explicitly at
its `MakeGhostSurfaceSettings` call so the choice is visible rather than defaulted.

- [ ] **Step 5: Carry the kind through the road tool**

In `Public/Tool/RoadDrawTool.h`: both states and the tool take the kind.

```cpp
class AIRSIDE_API FRoadIdleState : public IRoadDrawState
{
public:
	explicit FRoadIdleState(ERoadKind InKind = ERoadKind::Taxiway) : Kind(InKind) {}
	// ... existing overrides ...
private:
	/** Which cross-section a click lays. Carried by the STATE as well as the tool because a
	 *  state builds its successor and the successor must lay the same kind. */
	ERoadKind Kind = ERoadKind::Taxiway;
};

class AIRSIDE_API FRoadChainingState : public IRoadDrawState
{
public:
	FRoadChainingState(int32 InFrom, bool bInCreated, ERoadKind InKind)
		: From(InFrom), bCreated(bInCreated), Kind(InKind) {}
	// ...
private:
	int32 From = INDEX_NONE;
	bool bCreated = false;
	ERoadKind Kind = ERoadKind::Taxiway;
};

class AIRSIDE_API FRoadDrawTool : public IBuildTool
{
public:
	/**
	 * ONE TOOL, TWO REGISTRY ENTRIES. Drawing a road and drawing a taxiway are the same
	 * gesture with the same states, snap chain and removal rules; only the cross-section
	 * differs. A second class would be a copy of ~200 lines that must agree with this one
	 * for ever, which is the duplication CLAUDE.md's "lists that must agree are ONE list"
	 * exists to prevent - here applied to behaviour rather than to a table.
	 */
	explicit FRoadDrawTool(ERoadKind InKind = ERoadKind::Taxiway);

	// ...
private:
	ERoadKind Kind = ERoadKind::Taxiway;
};
```

In `Private/Tool/RoadDrawTool.cpp`: the constructor seeds `State` with the kind;
`GetDisplayName` returns "Road" for `ServiceRoad` and "Taxiway" otherwise (the registry's
`Name` must equal `GetDisplayName()` - `Airside.Tool.BuildSession` asserts it); every
`ConnectNodes` and `UpdateGhost` call passes `Kind`; every `MakeUnique<FRoadIdleState>()`
becomes `MakeUnique<FRoadIdleState>(Kind)` and every `MakeUnique<FRoadChainingState>(N, b)`
becomes `MakeUnique<FRoadChainingState>(N, b, Kind)`.

```cpp
FRoadDrawTool::FRoadDrawTool(ERoadKind InKind)
	: Kind(InKind)
{
	State = MakeUnique<FRoadIdleState>(Kind);
}

FText FRoadDrawTool::GetDisplayName() const
{
	// MUST MATCH the registry's Name - Airside.Tool.BuildSession asserts the two cannot
	// drift, which is the class of bug the registry exists to make impossible elsewhere.
	return Kind == ERoadKind::ServiceRoad
		? LOCTEXT("RoadTool", "Road")
		: LOCTEXT("TaxiwayTool", "Taxiway");
}
```

- [ ] **Step 6: Register key 9**

In `Private/Tool/BuildSession.cpp`, after the `EKeys::Eight` entry:

```cpp
		// NINE: the same FRoadDrawTool, laying the service-road cross-section. One tool, two
		// entries - see FRoadDrawTool's own comment for why this is not a second class.
		{ EKeys::Nine,  LOCTEXT("Road",      "Road"),      [] { return MakeUnique<FRoadDrawTool>(ERoadKind::ServiceRoad); } },
```

and change the taxiway entry to say so explicitly:

```cpp
		{ EKeys::One,   LOCTEXT("Taxiway",   "Taxiway"),   [] { return MakeUnique<FRoadDrawTool>(ERoadKind::Taxiway); } },
```

Update the table's header comment: it currently lists the tools and their keys, and a list
that must agree with the table below it is exactly what this file is about.

- [ ] **Step 7: Run the tests**

Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside.Tool`
Expected: `Airside.Tool.ServiceRoadLaid` passes and `Airside.Tool.BuildSession` still does
(its tool count moves from 7 to 8 - update the expected number there if it is asserted as a
literal; assert against `ToolRegistry().Num()` rather than a number if it is not already).

- [ ] **Step 8: Build and commit**

```bash
git add Plugins/Airside/Source/Airside Plugins/Airside/Source/AirsideTests
git commit -m "feat(airside): key 9 lays a service road - ERoadKind through the tool seam"
```

---

### Task 3: Crossings hold, and dead ends are reported

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Private/Build/RoadGuidelineBuilder.cpp` (census only)
- Test: `Plugins/Airside/Source/AirsideTests/Private/RoadCrossingTest.cpp` (new)

**Interfaces:**
- Consumes: Task 1's `MakeServiceRoadTransient`; `FRoadNetworkSolver::SolveAll`;
  `FRoadGuidelineBuilder::Build`; `FTrafficMask::Allows`; `URoadProfile::MakeTransient`.
- Produces: one new `UE_LOG(LogAirside, Log, ...)` line in the builder's census block,
  counting arm ends whose class reaches nothing through their junction.

- [ ] **Step 1: Write the failing test**

Create `Plugins/Airside/Source/AirsideTests/Private/RoadCrossingTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Model/RoadTraffic.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** A taxiway east-west and a service road north-south, crossing at the origin. */
	void LayCrossing(URoadNetwork& Net, bool bDrawFarSideOfRoad)
	{
		URoadProfile* Taxiway = URoadProfile::MakeTransient(2300.0, 1500.0, 230.0);
		URoadProfile* Road = URoadProfile::MakeServiceRoadTransient();
		Net.DefaultProfile = Taxiway;

		const FRoadNodeId West   = Net.AddNode(FVector2D(-10000.0, 0.0));
		const FRoadNodeId Centre = Net.AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId East   = Net.AddNode(FVector2D(10000.0, 0.0));
		const FRoadNodeId South  = Net.AddNode(FVector2D(0.0, -10000.0));

		Net.AddStraightSegment(West, Centre, Taxiway);
		Net.AddStraightSegment(Centre, East, Taxiway);
		Net.AddStraightSegment(South, Centre, Road);
		if (bDrawFarSideOfRoad)
		{
			const FRoadNodeId North = Net.AddNode(FVector2D(0.0, 10000.0));
			Net.AddStraightSegment(Centre, North, Road);
		}
	}

	/** Every live derived edge's mask, so a class's reachability can be counted. */
	int32 CountEdgesAllowing(const URoadNetwork& Net, ETraversalClass Class)
	{
		int32 Count = 0;
		for (const FGuidelineEdge& Edge : Net.GetGuidelineEdges())
		{
			Count += (Edge.bAlive && Edge.AllowedTraffic.Allows(Class)) ? 1 : 0;
		}
		return Count;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadCrossesTaxiwayTest,
	"Airside.Build.RoadCrossesTaxiway",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadCrossesTaxiwayTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	LayCrossing(*Net, /*bDrawFarSideOfRoad=*/true);
	FRoadGuidelineBuilder::Build(*Net, FRoadNetworkSolver::SolveAll(*Net));

	// THE WHOLE CROSSING RULE, and it is already in the builder: Turn.AllowedTraffic is
	// FromMask & ToMask (RoadGuidelineBuilder.cpp), so a turn between arms of different
	// classes keeps only Emergency. This test exists because that behaviour is now LOAD
	// BEARING for the fuel slice and nothing pinned it.
	int32 VehicleTurns = 0, AircraftTurns = 0, MixedTurns = 0;
	for (const FGuidelineEdge& Edge : Net->GetGuidelineEdges())
	{
		// A TURN PATH carries no DerivedFrom - that is how it is told from a segment's own
		// guideline (see the builder's comment where DerivedFrom is deliberately left unset).
		if (!Edge.bAlive || !Edge.bDerived || Edge.DerivedFrom.IsSet()) { continue; }
		const bool bVehicle = Edge.AllowedTraffic.Allows(ETraversalClass::GroundVehicle);
		const bool bAircraft = Edge.AllowedTraffic.Allows(ETraversalClass::Aircraft);
		VehicleTurns += (bVehicle && !bAircraft) ? 1 : 0;
		AircraftTurns += (bAircraft && !bVehicle) ? 1 : 0;
		MixedTurns += (bVehicle && bAircraft) ? 1 : 0;
	}

	TestTrue(TEXT("vehicles get road-to-road turns"), VehicleTurns > 0);
	TestTrue(TEXT("aircraft get taxiway-to-taxiway turns"), AircraftTurns > 0);

	// THE SAFETY PROPERTY. A single mixed turn is an aircraft admitted onto a service road,
	// or a truck onto a taxiway, and neither is recoverable by any later rule.
	TestEqual(TEXT("and NO turn admits both - no road-to-taxiway path for anybody"),
		MixedTurns, 0);

	TestTrue(TEXT("the road is line a vehicle can use"),
		CountEdgesAllowing(*Net, ETraversalClass::GroundVehicle) > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadEndsAgainstTaxiwayTest,
	"Airside.Build.RoadEndsAgainstTaxiway",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadEndsAgainstTaxiwayTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	LayCrossing(*Net, /*bDrawFarSideOfRoad=*/false);
	FRoadGuidelineBuilder::Build(*Net, FRoadNetworkSolver::SolveAll(*Net));

	// A road dead-ending against a taxiway's side: the only other arm of its class is
	// itself, so there is nothing to turn INTO and the junction derives no vehicle turn.
	int32 VehicleTurns = 0;
	for (const FGuidelineEdge& Edge : Net->GetGuidelineEdges())
	{
		if (!Edge.bAlive || !Edge.bDerived || Edge.DerivedFrom.IsSet()) { continue; }
		VehicleTurns += Edge.AllowedTraffic.Allows(ETraversalClass::GroundVehicle) ? 1 : 0;
	}
	TestEqual(TEXT("no vehicle path through the junction"), VehicleTurns, 0);

	// NOT REFUSED, AND THE AIRCRAFT SIDE IS UNAFFECTED. The player may be about to draw the
	// far side; meanwhile the taxiway must be exactly the taxiway it was.
	int32 AircraftTurns = 0;
	for (const FGuidelineEdge& Edge : Net->GetGuidelineEdges())
	{
		if (!Edge.bAlive || !Edge.bDerived || Edge.DerivedFrom.IsSet()) { continue; }
		AircraftTurns += Edge.AllowedTraffic.Allows(ETraversalClass::Aircraft) ? 1 : 0;
	}
	TestTrue(TEXT("aircraft still turn through it"), AircraftTurns > 0);
	return true;
}

#endif
```

- [ ] **Step 2: Run both and see where they stand**

Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside.Build.Road`
Expected: both PASS with no production change - the masks already behave. **If
`RoadCrossesTaxiway` fails on `MixedTurns`, stop and report it**: the spec's "needs no new
code" claim is then wrong and the whole crossing design needs revisiting before anything
else in this plan is built.

- [ ] **Step 3: Add the census line**

In `Private/Build/RoadGuidelineBuilder.cpp`, inside the existing census block (the one whose
comment begins "THE CENSUS"), after the per-edge loop and before the `UE_LOG`:

```cpp
		// ARM ENDS THAT LEAD NOWHERE FOR THEIR OWN CLASS. A road drawn up to a taxiway's
		// side, or up to a runway, makes a junction with turn paths for everybody EXCEPT
		// the class that drew it - because AllowedTraffic is the intersection of the two
		// arms' masks and there is no second arm of its own class to intersect with.
		//
		// COUNTED AND NOT REFUSED, the same treatment the tool gives a half-drawn crossing:
		// the player may be about to draw the far side, and a build tool that refused the
		// first of two clicks would make a crossing impossible to draw at all. The line is
		// what turns "the truck says no route" into "look at the junction you left open".
		int32 DeadEnds = 0;
		for (const FGuidelineNode& Node : Network.GetGuidelineNodes())
		{
			if (!Node.bAlive || !Node.bDerived || Node.Incident.Num() != 1) { continue; }
			const FGuidelineEdge* Only = Network.GetGuidelineEdge(Node.Incident[0]);
			// Only a SEGMENT's own guideline end counts. A turn path with one end is
			// impossible, and an anchor lead-in ending at a stand is not a dead end - it is
			// the point of the stand.
			DeadEnds += (Only != nullptr && Only->DerivedFrom.IsSet()) ? 1 : 0;
		}
```

and extend the log line, keeping every existing field:

```cpp
		UE_LOG(LogAirside, Log,
			TEXT("Guidelines: %d nodes (%d holding-position), %d edges (%d hand-authored, %d turn paths), "
				 "%d holding-position mark(s) on file, %d arm end(s) with no through path for their class"),
			NodesAlive, HoldingPosition, EdgesAlive, Authored, TurnPaths,
			Network.GetHoldingPositionMarks().Num(), DeadEnds);
```

- [ ] **Step 4: Re-run and confirm still green**

Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside.Build`
Expected: every `Airside.Build.*` test passes. `Airside.Probe.StarterMapRoutes` writes the
new field.

- [ ] **Step 5: Commit** (Live Coding covers this - it is a function body only.)

```bash
git add Plugins/Airside/Source/Airside/Private/Build/RoadGuidelineBuilder.cpp `
        Plugins/Airside/Source/AirsideTests/Private/RoadCrossingTest.cpp
git commit -m "test(airside): pin the crossing class masks; census counts arms with no through path"
```

---

### Task 4: The depot entity - a pose that is a vehicle's, not an aircraft's

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Model/RoadEntity.h`
- Modify: `Plugins/Airside/Source/Airside/Public/Model/RoadNetwork.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Model/RoadNetwork.cpp`
- Modify: `Plugins/Airside/Source/Airside/Public/Entities/EntityDefinition.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Entities/EntityDefinition.cpp`
- Modify: `Plugins/Airside/Source/Airside/Private/Build/AnchorLink.cpp`
- Test: `Plugins/Airside/Source/AirsideTests/Private/FuelDepotAnchorTest.cpp` (new)

**Interfaces:**
- Consumes: `FEntityInstance`, `URoadNetwork::PlaceEntity`, `FAnchorLink::Build`,
  `TraversalForRole`, `UEntityDefinition::RefreshResolvedAnchors`.
- Produces:
  - `FEntityInstance::PoseRole` (`UPROPERTY() EServiceRole`, default `Aircraft`)
  - `UEntityDefinition::PoseRole` (`UPROPERTY(EditAnywhere) EServiceRole`, default `Aircraft`)
  - `UEntityDefinition::FootprintExtent` (`UPROPERTY(EditAnywhere) FVector2D`, default zero)
  - `URoadNetwork::PlaceEntity(..., double DesignWingspan = 0.0, EServiceRole PoseRole = EServiceRole::Aircraft)`
  - `static void UEntityDefinition::BuildFuelDepot(UEntityDefinition*)` (`UFUNCTION(BlueprintCallable)`)
  - `static UEntityDefinition* UEntityDefinition::MakeFuelDepotTransient()`

- [ ] **Step 1: Write the failing test**

Create `Plugins/Airside/Source/AirsideTests/Private/FuelDepotAnchorTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Build/AnchorLink.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadTraffic.h"
#include "Model/RouteSearch.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** A straight east-west guideline at y, admitting exactly one class. */
	FGuidelineEdgeId LayLine(URoadNetwork& Net, double Y, ETraversalClass Class,
		FGuidelineNodeId& OutWest, FGuidelineNodeId& OutEast)
	{
		OutWest = Net.AddGuidelineNode(FVector2D(-10000.0, Y));
		OutEast = Net.AddGuidelineNode(FVector2D(10000.0, Y));
		FGuidelineEdge Edge;
		Edge.A = OutWest;
		Edge.B = OutEast;
		Edge.Control = FVector2D(0.0, Y);
		Edge.AllowedTraffic = FTrafficMask::Only(Class);
		Edge.AllowedTraffic.Add(ETraversalClass::Emergency);
		Edge.Direction = EGuidelineDir::Bidirectional;
		Edge.Width = 600.0;
		Edge.bDerived = true;
		return Net.AddGuidelineEdge(MoveTemp(Edge));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDepotJoinsRoadTest,
	"Airside.Entities.DepotJoinsRoad",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDepotJoinsRoadTest::RunTest(const FString& Parameters)
{
	UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
	if (!TestNotNull(TEXT("a depot definition"), Depot)) { return false; }
	TestEqual(TEXT("its pose is a vehicle's, not an aircraft's"),
		static_cast<int32>(Depot->PoseRole), static_cast<int32>(EServiceRole::Fuel));

	// ON A ROAD. The pose lead-in leaves along heading + 180 (see FAnchorLink), so a depot
	// at +90 casts down -Y at a line below it - the same arithmetic a stand uses, which is
	// why the depot needs no special placement rule.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		FGuidelineNodeId West, East;
		LayLine(*Net, 0.0, ETraversalClass::GroundVehicle, West, East);

		const FEntityInstanceId Placed = Net->PlaceEntity(Depot, Depot->Anchors,
			FVector2D(0.0, 4000.0), UE_DOUBLE_PI * 0.5, /*DesignWingspan=*/0.0, Depot->PoseRole);
		const FEntityInstance* Instance = Net->GetEntity(Placed);
		if (!TestNotNull(TEXT("the depot resolves"), Instance)) { return false; }
		TestEqual(TEXT("the instance captured the pose role"),
			static_cast<int32>(Instance->PoseRole), static_cast<int32>(EServiceRole::Fuel));

		const FGuidelineNodeId Pose = Instance->PoseNode;
		TestEqual(TEXT("it starts an island"), Net->GetGuidelineNode(Pose)->Incident.Num(), 0);

		TestTrue(TEXT("the pose lead-in joins the road"), FAnchorLink::Build(*Net) >= 1);
		TestTrue(TEXT("and the pose node now has line on it"),
			Net->GetGuidelineNode(Pose)->Incident.Num() > 0);

		// THE POINT: a truck can be routed from the depot. Before PoseRole the lead-in was
		// cast as an aircraft, found no aircraft guideline, and logged "joins nothing" on
		// every rebuild for ever.
		FRouteQuery Query;
		Query.Start = Pose;
		Query.Goal = East;
		Query.Class = ETraversalClass::GroundVehicle;
		TestTrue(TEXT("a vehicle routes off the depot"), RouteSearch::Find(*Net, Query).IsValid());
	}

	// OFF ANY ROAD - a depot facing a TAXIWAY. Its class is refused by the edge's mask, so
	// it stays unjoined and is counted, which is the census warning the player reads.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		FGuidelineNodeId West, East;
		LayLine(*Net, 0.0, ETraversalClass::Aircraft, West, East);

		const FEntityInstanceId Placed = Net->PlaceEntity(Depot, Depot->Anchors,
			FVector2D(0.0, 4000.0), UE_DOUBLE_PI * 0.5, 0.0, Depot->PoseRole);
		TestEqual(TEXT("a depot facing a taxiway joins nothing"), FAnchorLink::Build(*Net), 0);
		TestEqual(TEXT("and its pose node is still an island"),
			Net->GetGuidelineNode(Net->GetEntity(Placed)->PoseNode)->Incident.Num(), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandFuelAnchorJoinsRoadTest,
	"Airside.Entities.StandFuelAnchorJoinsRoad",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandFuelAnchorJoinsRoadTest::RunTest(const FString& Parameters)
{
	// The stand's HydrantPit is at local (-1200, +700) with LocalHeading -90 degrees
	// (UEntityDefinition::BuildCodeCStand), so with the stand at heading 0 the anchor casts
	// at -90: straight down -Y. A road below it is what it has been waiting for since the
	// anchor was authored - until this slice there was nothing of its class to reach, which
	// is why it logs unjoined today.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	FGuidelineNodeId TaxiWest, TaxiEast, RoadWest, RoadEast;
	LayLine(*Net, 6000.0, ETraversalClass::Aircraft, TaxiWest, TaxiEast);
	LayLine(*Net, -6000.0, ETraversalClass::GroundVehicle, RoadWest, RoadEast);

	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
	const FEntityInstanceId Placed = Net->PlaceEntity(Stand, Stand->Anchors,
		FVector2D(0.0, 0.0), UE_DOUBLE_PI, /*DesignWingspan=*/3600.0, Stand->PoseRole);

	FAnchorLink::Build(*Net);

	const TArray<FName> FuelIds = Net->GetAnchorIdsForRole(Placed, EServiceRole::Fuel);
	if (!TestEqual(TEXT("the stand has one fuel anchor"), FuelIds.Num(), 1)) { return false; }

	const FResolvedAnchor* Fuel = Net->FindResolvedAnchor(Placed, FuelIds[0]);
	if (!TestNotNull(TEXT("it resolved"), Fuel)) { return false; }
	TestTrue(TEXT("and now joins the road"),
		Net->GetGuidelineNode(Fuel->Node)->Incident.Num() > 0);

	// A truck can reach it. This is the OUT plan's goal, so a stand whose fuel anchor is an
	// island is a stand no fuel service can ever serve.
	FRouteQuery Query;
	Query.Start = RoadWest;
	Query.Goal = Fuel->Node;
	Query.Class = ETraversalClass::GroundVehicle;
	TestTrue(TEXT("a truck routes to the hydrant"), RouteSearch::Find(*Net, Query).IsValid());
	return true;
}

#endif
```

Note the stand is placed at heading `PI` here, not 0: `PlaceEntity`'s pose lead-in leaves at
heading + 180, so `PI` casts the AIRCRAFT lead-in at 0... **verify the arithmetic when you
write the fixture** by asserting the pose node also joins, and adjust the two line
Y-coordinates until both the pose node and the fuel anchor join. The assertion that matters
is the fuel anchor's, and the test must fail if it does not join.

- [ ] **Step 2: Run it and watch it fail**

Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside.Entities`
Expected: compile failure - `MakeFuelDepotTransient` and `PoseRole` do not exist.

- [ ] **Step 3: Add `PoseRole` to the instance and to `PlaceEntity`**

In `Public/Model/RoadEntity.h`, in `FEntityInstance`, immediately after `PoseNode`:

```cpp
	/**
	 * What the pose node is FOR: an aircraft's stop mark, or a service vehicle's home.
	 *
	 * CAPTURED AT PLACEMENT, exactly like DesignWingspan and FResolvedAnchor::Role, and for
	 * exactly the same reason: Model/ must not dereference the Entities layer (see the top
	 * of this file), so the one field placement needs afterwards is copied in rather than
	 * read live from UEntityDefinition::PoseRole.
	 *
	 * FAnchorLink reads it to decide which CLASS of guideline the pose lead-in may join. A
	 * depot's home is on a road; a stand's stop mark is on a taxiway. Before this existed
	 * the lead-in was cast as Aircraft unconditionally, so a depot's pose node found no
	 * aircraft guideline and logged "joins nothing" on every rebuild for ever.
	 *
	 * An instance saved before this field existed loads as Aircraft, which is correct for
	 * every entity that could have been placed then - they were all stands.
	 */
	UPROPERTY() EServiceRole PoseRole = EServiceRole::Aircraft;
```

In `Public/Model/RoadNetwork.h`, widen `PlaceEntity`'s declaration and extend its doc
comment (do not insert between comment and declaration - edit the comment in place):

```cpp
	FEntityInstanceId PlaceEntity(UEntityDefinition* Definition,
		TConstArrayView<FEntityAnchor> Anchors, const FVector2D& Position, double Heading,
		double DesignWingspan = 0.0, EServiceRole PoseRole = EServiceRole::Aircraft);
```

Append to that comment:

```
	 * PoseRole travels the same way and for the same reason as DesignWingspan: the caller
	 * reads UEntityDefinition::PoseRole, which this layer may not. Defaulted to Aircraft so
	 * every existing caller - all of them stands - keeps compiling and keeps meaning what
	 * it meant.
```

In `Private/Model/RoadNetwork.cpp`, `PlaceEntity` writes `Instance.PoseRole = PoseRole;`
beside its existing `Instance.DesignWingspan = DesignWingspan;`.

- [ ] **Step 4: Add `PoseRole`, `FootprintExtent` and the depot builder**

In `Public/Entities/EntityDefinition.h`, after `AvailableServices`:

```cpp
	/**
	 * What this installation's OWN pose is for - a stand's aircraft stop mark, a depot's
	 * truck bay.
	 *
	 * NOT AN ANCHOR, and deliberately (see FEntityInstance::PoseNode): an anchor is a
	 * FIXTURE dug into the concrete, and there is nothing at a stop mark but paint. But the
	 * pose still has to be REACHED, and by something - so it says which class of guideline
	 * its lead-in may join, through TraversalForRole.
	 *
	 * Aircraft by default, which is what every definition authored before this field existed
	 * meant, and is why a stand needs no edit.
	 */
	UPROPERTY(EditAnywhere) EServiceRole PoseRole = EServiceRole::Aircraft;

	/**
	 * Plan-view half-extents of the installation itself, uu, in its own local space.
	 *
	 * FOR THE PLACEMENT PREVIEW, and nothing else this slice. Zero draws nothing but the
	 * pose mark, which is what a STAND wants: a stand's extent is its design aircraft's, and
	 * a second box round it would be a second opinion about how big the thing is.
	 *
	 * A BOX AND NOT FEntityFootprint. That struct is aircraft-shaped - nose, wingspan,
	 * tailplane - and a fuel depot has none of those; filling it in for a building would be
	 * authored numbers nothing could read correctly.
	 */
	UPROPERTY(EditAnywhere) FVector2D FootprintExtent = FVector2D::ZeroVector;
```

and, after `BuildCodeCStand`:

```cpp
	/**
	 * Fill Definition with the fuel depot layout: a box on a service road, one truck.
	 *
	 * SCAFFOLDING, and named as such by the fuel-service spec (§0.1). M4 replaces this with
	 * a UBuildingInstance carrying a road anchor node, add-on modules, a fleet and an
	 * inventory. What survives the replacement is the ROAD CONNECTION - a pose whose lead-in
	 * joins a GroundVehicle guideline - which is why that part is a general mechanism here
	 * and the truck count is a bare number.
	 *
	 * NO ANCHORS, deliberately. The spec's first draft gave the depot a Fuel anchor as well
	 * as a pose; two lead-ins from one small building into one road is a duplicate painted
	 * line, and the pose is the node the truck is actually dispatched from and back to.
	 *
	 * Shared by MakeFuelDepotTransient and the commandlet that authors DA_FuelDepot, so the
	 * tested layout and the shipped one are the same numbers.
	 */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	static void BuildFuelDepot(UEntityDefinition* Definition);

	/** BuildFuelDepot plus a NewObject, for tests and the debug gallery. */
	static UEntityDefinition* MakeFuelDepotTransient();

	/** How many trucks this installation can have out at once. Read only from a definition
	 *  whose PoseRole is a service role; 0 on a stand and meaningless there.
	 *
	 *  SCAFFOLDING (spec §0.1): M3's UJobBoard bids by ETA over a real fleet, and this
	 *  becomes the fleet's size rather than a number the service counts against itself. */
	UPROPERTY(EditAnywhere, meta = (ClampMin = "0")) int32 Trucks = 0;
```

In `Private/Entities/EntityDefinition.cpp`:

```cpp
UEntityDefinition* UEntityDefinition::MakeFuelDepotTransient()
{
	UEntityDefinition* Definition = NewObject<UEntityDefinition>(GetTransientPackage());
	BuildFuelDepot(Definition);
	return Definition;
}

void UEntityDefinition::BuildFuelDepot(UEntityDefinition* Definition)
{
	if (Definition == nullptr)
	{
		return;
	}

	Definition->Anchors.Reset();

	// ORIGIN is the truck bay - where a truck stands when it is home, and the node it is
	// dispatched from. +X faces the road, so the pose lead-in (heading + 180, see
	// FAnchorLink) casts back out of the depot at whatever the player aimed it at, exactly
	// as a stand's does.
	Definition->PoseRole = EServiceRole::Fuel;

	// 12 m by 8 m: a tank, a pump and room to turn a bowser round. A placeholder box, and
	// the only geometry the depot has this slice.
	Definition->FootprintExtent = FVector2D(600.0, 400.0);

	// ONE truck. The number the fuel service counts trucks-out against; M3's job board
	// replaces the counting, not the number.
	Definition->Trucks = 1;

	// What this installation can provide. Fuel and nothing else, which is the whole slice.
	Definition->AvailableServices = { EServiceRole::Fuel };

	// NO DesignAircraft: nothing parks here, so there is no envelope to draw and no code
	// letter to size a sweep by. FAnchorLink falls back to Code C's 2500 uu radius for the
	// lead-in curve, which is generous for a van and costs nothing.
}
```

- [ ] **Step 5: Make `FAnchorLink` cast the pose by its role**

In `Private/Build/AnchorLink.cpp`, in the pose-link block, replace the two class lines and
extend the comment above them:

```cpp
			// The ray leaves along the entity's heading PLUS 180: +X faces the terminal (or
			// the depot's own building), so the lead-in runs back out of it to the movement
			// area. Cast the other way and every stand would try to join a guideline inside
			// the building.
			//
			// WHICH CLASS OF LINE IT MAY JOIN comes from the instance's PoseRole. This was
			// Aircraft unconditionally, which is right for a stand and silently wrong for
			// anything else: a fuel depot's pose found no aircraft guideline, joined
			// nothing, and logged a warning on every rebuild for ever.
			const double Out = Instance.Heading + UE_DOUBLE_PI;

			FPendingLink Link;
			Link.Node = Instance.PoseNode;
			Link.At = Pose->Position;
			Link.Dir = FVector2D(FMath::Cos(Out), FMath::Sin(Out));
			Link.Class = TraversalForRole(Instance.PoseRole);

			// A wingspan limit on a line no wing uses would be a limit nothing could ever
			// bind - see FProfileGuideline::MaxWingspan, where 0 means unlimited.
			Link.MaxWingspan = Link.Class == ETraversalClass::Aircraft ? StandWingspan : 0.0;
			Link.Radius = StandRadius;
			Pending.Add(Link);
```

Also widen the unjoined warning's class word, which currently reads "aircraft" or "vehicle"
off a two-way test - leave it as is if it already covers both; it does.

- [ ] **Step 6: Refresh the snapshot on load**

In `Private/Entities/EntityDefinition.cpp`, `RefreshResolvedAnchors` already brings
`FResolvedAnchor::LocalHeading` and `::Role` up to date. It cannot write `FEntityInstance::PoseRole`
(that is not an anchor), so add a sibling in the same function - it is the one place both
layers are known at once:

```cpp
	// The INSTANCE's own pose role, for the same reason the anchors' snapshots are
	// refreshed here: an entity placed and saved before FEntityInstance::PoseRole existed
	// loads with the UPROPERTY default (Aircraft), and nothing else ever corrects it. That
	// is right for every entity that COULD have been saved then - they were all stands -
	// but a depot placed and saved between this field landing and a definition being
	// re-authored would be stuck routing aircraft to its truck bay.
	//
	// Needs a URoadNetwork mutator, because Model/ owns the array. See SetEntityPoseRole.
```

Add `bool URoadNetwork::SetEntityPoseRole(FEntityInstanceId Entity, EServiceRole Role)` beside
`RefreshResolvedAnchor`, with the same "a pure data write, taking values rather than a
UEntityDefinition" justification, and call it from `RefreshResolvedAnchors` when the
instance's value differs from its definition's. Count the change into the function's return.

- [ ] **Step 7: Run the tests**

Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside.Entities`
Expected: `Airside.Entities.DepotJoinsRoad` and `Airside.Entities.StandFuelAnchorJoinsRoad`
pass. Then run the whole suite: `./Tools/Run-AirsideTests.ps1` - `Airside.Build.AnchorLink`,
`Airside.Model.RoadEntity` and `Airside.Build.LeadInSweep` all touch this code.

- [ ] **Step 8: Build and commit**

```bash
git add Plugins/Airside/Source/Airside Plugins/Airside/Source/AirsideTests
git commit -m "feat(airside): an entity's pose says which class may reach it; DA_FuelDepot layout"
```

---

### Task 5: Placing the depot - `FStandPlaceTool` parameterised

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Tool/RoadEditTarget.h`
- Modify: `Plugins/Airside/Source/Airside/Public/Tool/StandPlaceTool.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Tool/StandPlaceTool.cpp`
- Modify: `Plugins/Airside/Source/Airside/Private/Tool/StandPreview.cpp`
- Modify: `Plugins/Airside/Source/Airside/Private/Tool/BuildSession.cpp`
- Modify: `Plugins/Airside/Source/Airside/Public/Present/RoadEditFacade.h` / `.cpp`
- Modify: `Plugins/Airside/Source/Airside/Private/Present/RoadEditFacadeSurfaces.cpp`
- Modify: `Plugins/Airside/Source/Airside/Public/Present/RoadNetworkActor.h` / `.cpp`
- Modify: `Plugins/Airside/Source/Airside/Public/Content/AirsideContent.h`
- Test: `Plugins/Airside/Source/AirsideTests/Private/FuelDepotPlaceToolTest.cpp` (new)

**Interfaces:**
- Consumes: Task 4's `MakeFuelDepotTransient`, `PoseRole`, `Trucks`, `FootprintExtent`;
  `IRoadEditTarget::PlaceStand/GetStandDefinition`; `StandPreview::Describe`.
- Produces:
  - `enum class EPlaceableEntity : uint8 { Stand, FuelDepot };` in `Tool/RoadEditTarget.h`
  - `virtual int32 IRoadEditTarget::PlaceEntity(FVector2D Where, double Heading, EPlaceableEntity Kind) = 0;`
    plus non-virtual `int32 PlaceStand(FVector2D, double)` forwarding with `Stand`
  - `virtual const UEntityDefinition* IRoadEditTarget::GetEntityDefinition(EPlaceableEntity Kind) const = 0;`
    plus non-virtual `const UEntityDefinition* GetStandDefinition() const`
  - `explicit FStandPlaceTool(EPlaceableEntity Kind = EPlaceableEntity::Stand)`
  - `ARoadNetworkActor::FuelDepotDefinition` + `UEntityDefinition* ResolveFuelDepotDefinition() const`
  - `UAirsideContent::DefaultFuelDepot` (`TSoftObjectPtr<UEntityDefinition>`)
  - registry index 8: key `EKeys::Zero`, name "Fuel depot"

- [ ] **Step 1: Write the failing test**

Create `Plugins/Airside/Source/AirsideTests/Private/FuelDepotPlaceToolTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Entities/EntityDefinition.h"
#include "InputCoreTypes.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Tool/BuildSession.h"
#include "Tool/StandPlaceTool.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFuelDepotPlaceToolTest,
	"Airside.Tool.FuelDepotPlaced",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFuelDepotPlaceToolTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("a world"), World)) { return false; }
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };

	ARoadNetworkActor* Actor = World->SpawnActor<ARoadNetworkActor>();
	if (!TestNotNull(TEXT("the actor"), Actor)) { return false; }
	Actor->FuelDepotDefinition = UEntityDefinition::MakeFuelDepotTransient();
	Actor->StandDefinition = UEntityDefinition::MakeStandTransient();

	FStandPlaceTool DepotTool(EPlaceableEntity::FuelDepot);
	TestEqual(TEXT("the depot tool names itself"),
		DepotTool.GetDisplayName().ToString(), FString(TEXT("Fuel depot")));

	FToolContext ToolContext;
	ToolContext.Target = Actor;
	ToolContext.Cursor = FVector2D(3000.0, 3000.0);
	DepotTool.OnClick(ToolContext);

	const TArray<FEntityInstance>& Entities = Actor->Network->GetEntities();
	if (!TestEqual(TEXT("one entity placed"), Entities.Num(), 1)) { return false; }

	// THE POINT: the tool placed the DEPOT definition, and the instance carries the depot's
	// pose role - so FAnchorLink will cast its lead-in at a road (Task 4).
	TestEqual(TEXT("it is the depot"), Entities[0].Definition.Get(),
		Actor->FuelDepotDefinition.Get());
	TestEqual(TEXT("and its pose is a vehicle's"),
		static_cast<int32>(Entities[0].PoseRole), static_cast<int32>(EServiceRole::Fuel));
	TestEqual(TEXT("a depot has no anchors - the pose IS its road connection"),
		Entities[0].ResolvedAnchors.Num(), 0);

	// The same tool with the other kind still places a stand: one class, two entries.
	FStandPlaceTool StandTool(EPlaceableEntity::Stand);
	ToolContext.Cursor = FVector2D(-3000.0, -3000.0);
	StandTool.OnClick(ToolContext);
	if (!TestEqual(TEXT("two entities now"), Actor->Network->GetEntities().Num(), 2)) { return false; }
	TestEqual(TEXT("the second is a stand"), Actor->Network->GetEntities()[1].Definition.Get(),
		Actor->StandDefinition.Get());

	// Registry: key 0, name "Fuel depot", display name agreeing.
	bool bFound = false;
	for (const FToolRegistration& Entry : ToolRegistry())
	{
		if (Entry.Key == EKeys::Zero)
		{
			bFound = true;
			TestEqual(TEXT("key 0 is the depot tool"), Entry.Name.ToString(), FString(TEXT("Fuel depot")));
			TestEqual(TEXT("and its display name agrees"),
				Entry.Make()->GetDisplayName().ToString(), Entry.Name.ToString());
		}
	}
	TestTrue(TEXT("key 0 is registered"), bFound);
	return true;
}

#endif
```

- [ ] **Step 2: Run it and watch it fail**

Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside.Tool.FuelDepotPlaced`
Expected: compile failure - `EPlaceableEntity` undeclared.

- [ ] **Step 3: Widen the seam**

In `Public/Tool/RoadEditTarget.h`, beside `ERoadKind`:

```cpp
/**
 * Which authored installation a placement gesture drops.
 *
 * On the seam as an ENUM, not a UEntityDefinition*, for the reason ERoadKind is: resolving
 * a kind to an asset is the facade's job (ARoadNetworkActor::Resolve*Definition), in one
 * place, so the preview and the placement cannot resolve different objects - which is
 * exactly what GetStandDefinition's own comment already warns about.
 */
enum class EPlaceableEntity : uint8
{
	Stand,
	FuelDepot
};
```

Replace the stands block:

```cpp
	// --- Entities ----------------------------------------------------------------------

	virtual int32 PlaceEntity(FVector2D Where, double Heading, EPlaceableEntity Kind) = 0;

	/** A stand - what every caller before the fuel slice meant. A non-virtual overload, so
	 *  implementers override one signature. */
	int32 PlaceStand(FVector2D Where, double Heading)
	{
		return PlaceEntity(Where, Heading, EPlaceableEntity::Stand);
	}

	virtual bool DeleteEntity(int32 EntityIndex) = 0;
	virtual int32 FindEntityAt(FVector2D Where, double Radius) const = 0;

	/** The definition of Kind, read-only: a tool previews what would be placed, never
	 *  authors it. RESOLVED, the same as PlaceEntity places from - see
	 *  ARoadNetworkActor::GetEntityDefinition. */
	virtual const UEntityDefinition* GetEntityDefinition(EPlaceableEntity Kind) const = 0;

	/** The stand's, for every caller written before there was a second kind. */
	const UEntityDefinition* GetStandDefinition() const
	{
		return GetEntityDefinition(EPlaceableEntity::Stand);
	}
```

- [ ] **Step 4: Implement on the facade and the actor**

`URoadEditFacade`: rename `PlaceStand` to `PlaceEntity(FVector2D, double, EPlaceableEntity)`,
carrying every existing log line and comment. The body changes at four points:

```cpp
int32 URoadEditFacade::PlaceEntity(FVector2D Where, double Heading, EPlaceableEntity Kind)
{
	ARoadNetworkActor& Owner = Actor();
	UEntityDefinition* Definition = Owner.ResolveEntityDefinition(Kind);
	if (Definition == nullptr)
	{
		// The message names the ASSET AND THE SCRIPT that authors it, which is what turned
		// "the stand tool does nothing" into a one-line fix the first time.
		UE_LOG(LogRoadMesh, Warning,
			Kind == EPlaceableEntity::FuelDepot
				? TEXT("PlaceEntity refused: no FuelDepotDefinition. Author DA_FuelDepot with "
					   "Tools/Python/build_stand_asset.py, or set one on the actor.")
				: TEXT("PlaceEntity refused: no StandDefinition. Author DA_Stand_CodeC with "
					   "Tools/Python/build_stand_asset.py, or set one on the actor."));
		return INDEX_NONE;
	}

	// ... HasUsableAnchorIds check unchanged, with "PlaceStand:" in its message changed to
	// "PlaceEntity:" and *Definition->GetName() naming which asset ...

	URoadNetwork& Net = EnsureNetwork();
	FRoadEditScope Edit(HistoryForEdit(), &Net,
		Kind == EPlaceableEntity::FuelDepot ? TEXT("place fuel depot") : TEXT("place stand"));

	const double DesignWingspan =
		Definition->DesignAircraft != nullptr ? Definition->DesignAircraft->Footprint.Wingspan : 0.0;
	const FEntityInstanceId Placed = Net.PlaceEntity(Definition, Definition->Anchors, Where,
		Heading, DesignWingspan, Definition->PoseRole);
	// ... unchanged ...
}
```

`URoadEditFacade::GetEntityDefinition(Kind)` forwards to `Actor().ResolveEntityDefinition(Kind)`,
keeping the existing "RESOLVED, not the raw field" comment.

`ARoadNetworkActor`: add the property and the resolvers.

```cpp
	/**
	 * What the fuel depot tool places. Unset falls back to the content set's DefaultFuelDepot.
	 *
	 * Beside StandDefinition rather than in a map keyed by EPlaceableEntity: two kinds, and
	 * two asset pickers in the Details panel are easier to author than a map, for no loss
	 * until a third arrives.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside")
	TObjectPtr<UEntityDefinition> FuelDepotDefinition;

	// ... in the public resolver block, beside ResolveStandDefinition:
	UEntityDefinition* ResolveFuelDepotDefinition() const;

	/** ResolveStandDefinition or ResolveFuelDepotDefinition, by kind. THE ONE PLACE the
	 *  mapping lives, so the tool's preview and the facade's placement cannot pick
	 *  differently - which is what GetStandDefinition's comment has always warned about. */
	UEntityDefinition* ResolveEntityDefinition(EPlaceableEntity Kind) const;
```

```cpp
UEntityDefinition* ARoadNetworkActor::ResolveFuelDepotDefinition() const
{
	if (FuelDepotDefinition != nullptr) { return FuelDepotDefinition; }
	const UAirsideContent* Content = UAirsideSettings::GetContent();
	return Content != nullptr ? Content->DefaultFuelDepot.LoadSynchronous() : nullptr;
}

UEntityDefinition* ARoadNetworkActor::ResolveEntityDefinition(EPlaceableEntity Kind) const
{
	return Kind == EPlaceableEntity::FuelDepot ? ResolveFuelDepotDefinition() : ResolveStandDefinition();
}
```

In `Public/Content/AirsideContent.h`, beside `DefaultStand`:

```cpp
	/** What the fuel depot tool places. See UEntityDefinition::BuildFuelDepot; scaffolding
	 *  that M4's UBuildingInstance replaces (fuel-service spec §0.1). */
	UPROPERTY(EditAnywhere, Category = "Airside|Defaults")
	TSoftObjectPtr<UEntityDefinition> DefaultFuelDepot;
```

- [ ] **Step 5: Parameterise the tool and the preview**

`FStandPlaceTool` gains `explicit FStandPlaceTool(EPlaceableEntity InKind = EPlaceableEntity::Stand)`
and a private `EPlaceableEntity Kind`. Its class comment gains the "one class, two entries"
paragraph. Three bodies change:

```cpp
FText FStandPlaceTool::GetDisplayName() const
{
	// MUST MATCH the registry's Name - Airside.Tool.BuildSession asserts they cannot drift.
	return Kind == EPlaceableEntity::FuelDepot
		? LOCTEXT("FuelDepotTool", "Fuel depot")
		: LOCTEXT("StandTool", "Stand");
}
```

`OnClick` calls `Context.Target->PlaceEntity(Context.Cursor, LastHeading, Kind)`;
`PreviewPose` asks `Context.Target->GetEntityDefinition(Kind)`; the remove-modifier label
becomes `Kind == EPlaceableEntity::FuelDepot ? TEXT("remove fuel depot") : TEXT("remove stand")`.
The "in use by aircraft" label stays as it is - a depot's pose node is never a stand claim
(`ClaimGoalNode` reserves only for `ETraversalClass::Aircraft`), so the branch simply never
fires for a depot and needs no guard.

`StandPreview::Describe` draws the box, after the design-aircraft block and before the
fixture loop:

```cpp
	// THE INSTALLATION'S OWN BOX, for something that is not an aeroplane. A stand leaves
	// this at zero, because its extent is its design aircraft's and a second rectangle round
	// it would be a second opinion about how big the thing is.
	if (Definition->FootprintExtent.X > 0.0 && Definition->FootprintExtent.Y > 0.0)
	{
		const FVector2D& E = Definition->FootprintExtent;
		const FVector2D Corners[4] = {
			ToWorld(FVector2D(+E.X, +E.Y)), ToWorld(FVector2D(+E.X, -E.Y)),
			ToWorld(FVector2D(-E.X, -E.Y)), ToWorld(FVector2D(-E.X, +E.Y)) };
		for (int32 Index = 0; Index < 4; ++Index)
		{
			Sink.Line(Corners[Index], Corners[(Index + 1) % 4], EPreviewStyle::Snap);
		}
	}
```

Change the null branch's label from `"no stand definition"` to `"no definition to place"`:
the function now serves two kinds and naming one of them would be wrong half the time.

- [ ] **Step 6: Register key 0**

In `Private/Tool/BuildSession.cpp`, after the key 9 entry:

```cpp
		// ZERO, after nine, because it is the next key along a keyboard's top row and every
		// other number is spoken for. One FStandPlaceTool, two entries - see that class.
		{ EKeys::Zero,  LOCTEXT("FuelDepot", "Fuel depot"), [] { return MakeUnique<FStandPlaceTool>(EPlaceableEntity::FuelDepot); } },
```

and make the stand entry explicit: `MakeUnique<FStandPlaceTool>(EPlaceableEntity::Stand)`.
Update the table's header comment again.

- [ ] **Step 7: Run the tests**

Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside.Tool`
Then the whole suite - `Airside.Tool.StandPlaceTool` and `Airside.Present.*` call
`PlaceStand` and must still compile and pass through the forwarder.

- [ ] **Step 8: Build and commit**

```bash
git add Plugins/Airside/Source/Airside Plugins/Airside/Source/AirsideTests
git commit -m "feat(airside): key 0 places a fuel depot - FStandPlaceTool takes its definition by kind"
```

---

### Task 6: The truck - a vehicle airframe and a vehicle view

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Content/AirsideSettings.h` / `.cpp`
- Modify: `Plugins/Airside/Source/Airside/Public/Content/AirsideContent.h`
- Modify: `Plugins/Airside/Source/Airside/Public/Present/RoadAgentActor.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Present/RoadAgentActor.cpp`
- Modify: `Plugins/Airside/Source/Airside/Private/Present/AirsideTraffic.cpp`
- Test: `Plugins/Airside/Source/AirsideTests/Private/VehicleAgentTest.cpp` (new)
- Test: `Plugins/Airside/Source/AirsideTests/Private/TruckCrossingTest.cpp` (new)

**Interfaces:**
- Consumes: `FAirframe`, `FGroundPerformance`, `FTrafficRules::VehicleFootprint`,
  `UAirsideTraffic::SpawnView`, `ARoadAgentActor`.
- Produces:
  - `static FAirframe UAirsideSettings::ResolveDefaultVehicle()`
  - `UAirsideContent::VehicleMesh` (`TSoftObjectPtr<UStaticMesh>`)
  - `void ARoadAgentActor::SetVehicleBody(UStaticMesh* Mesh, const FVector& BoxSizeUu)`
  - `bool ARoadAgentActor::HasVehicleBodyForTest() const`

- [ ] **Step 1: Write the failing test**

Create `Plugins/Airside/Source/AirsideTests/Private/VehicleAgentTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Content/AirsideSettings.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadTraffic.h"
#include "Model/RouteSearch.h"
#include "Present/AirsideTraffic.h"
#include "Present/RoadAgentActor.h"
#include "Present/RoadNetworkActor.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVehicleAgentTest,
	"Airside.Present.VehicleAgentView",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVehicleAgentTest::RunTest(const FString& Parameters)
{
	// The performance bundle first, world-free: a truck that cannot move is a truck that
	// freezes on the line with nothing to say why - FGroundPerformance::IsSet is what every
	// caller checks, so it is what this must satisfy.
	const FAirframe Van = UAirsideSettings::ResolveDefaultVehicle();
	TestTrue(TEXT("a vehicle can move about an airport"), Van.Ground.IsSet());
	TestEqual(TEXT("and says what it is"), Van.TypeCode, FName(TEXT("FUEL")));
	TestEqual(TEXT("with no wing to fit through a turn"), Van.Wingspan, 0.0);

	// A vehicle NEVER FLIES, so Climb and Approach are left at their defaults and nothing
	// may read them. Pinned because the alternative - filling them in - is authored numbers
	// nothing reads, which this codebase has shipped three times.
	TestFalse(TEXT("no landing figures: a truck declines to land"), Van.Approach.IsSet());

	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("a world"), World)) { return false; }
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };

	ARoadNetworkActor* Actor = World->SpawnActor<ARoadNetworkActor>();
	if (!TestNotNull(TEXT("the actor"), Actor)) { return false; }

	URoadNetwork& Net = *Actor->Network;
	const FGuidelineNodeId A = Net.AddGuidelineNode(FVector2D(0.0, 0.0), /*bDerived=*/false);
	const FGuidelineNodeId B = Net.AddGuidelineNode(FVector2D(20000.0, 0.0), /*bDerived=*/false);
	FGuidelineEdge Edge;
	Edge.A = A;
	Edge.B = B;
	Edge.Control = FVector2D(10000.0, 0.0);
	Edge.AllowedTraffic = FTrafficMask::Only(ETraversalClass::GroundVehicle);
	Edge.Direction = EGuidelineDir::Bidirectional;
	Edge.bDerived = false;
	Net.AddGuidelineEdge(MoveTemp(Edge));

	FRouteQuery Query;
	Query.Start = A;
	Query.Goal = B;
	Query.Class = ETraversalClass::GroundVehicle;
	const FRoutePlan Plan = RouteSearch::Find(Net, Query);
	if (!TestTrue(TEXT("the road routes"), Plan.IsValid())) { return false; }

	if (!TestTrue(TEXT("the truck dispatches"),
		Actor->DispatchAgent(Plan, Van, ETraversalClass::GroundVehicle))) { return false; }

	const int32 Id = Actor->GetTraffic()->GetNewestAgentId();
	ARoadAgentActor* View = Actor->GetTraffic()->GetAgentView(Id);
	if (!TestNotNull(TEXT("a view was spawned for it"), View)) { return false; }

	// THE SEAM: a GroundVehicle agent is dressed as a vehicle, not as an aircraft. Without
	// this the truck is the aircraft placeholder cube at 4 m x 4 m x 2 m - which reads as a
	// second aeroplane rather than as a van, and is twice the vehicle footprint the traffic
	// model arbitrates it by.
	TestTrue(TEXT("and dressed as a vehicle"), View->HasVehicleBodyForTest());
	return true;
}

#endif
```

- [ ] **Step 2: Run it and watch it fail**

Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside.Present.VehicleAgentView`
Expected: compile failure - `ResolveDefaultVehicle` not a member.

- [ ] **Step 3: `ResolveDefaultVehicle`**

In `Public/Content/AirsideSettings.h`, after `ResolveDefaultAirframe`:

```cpp
	/**
	 * The performance a SERVICE VEHICLE moves with - the one place a truck's figures live.
	 *
	 * AN FAirframe FOR A THING WITH NO AIRFRAME, and that is scaffolding, named as such by
	 * the fuel-service spec (§0.1). FRouteFollower, FRoadAgent and the arbiter all take one
	 * FAirframe, so giving a truck anything else this slice would mean a second follower
	 * before there is a second kind of movement to justify it. M3 replaces this with a
	 * vehicle-shaped performance bundle when the fleet arrives.
	 *
	 * Only Ground is set. Climb, Approach and Engine stay at their struct defaults and are
	 * NEVER READ for a vehicle: a truck is dispatched with DispatchAgent, which arms a
	 * departure only when the route ends on a runway, and it never will - RouteSearch is
	 * asked with AvoidRunways::All. Filling them in would be authored numbers nothing reads.
	 *
	 * Wingspan 0 because 0 is UNLIMITED in the edge test (see FRouteQuery::Wingspan), which
	 * is the right answer rather than a lax one: a road guideline carries no wingspan limit
	 * either, so nothing on either side of that comparison means anything for a van.
	 */
	static FAirframe ResolveDefaultVehicle();
```

In `Private/Content/AirsideSettings.cpp`:

```cpp
FAirframe UAirsideSettings::ResolveDefaultVehicle()
{
	FAirframe Van;

	// A LIGHT COMMERCIAL VEHICLE, in the units FGroundRegime uses (uu/s and uu/s^2, and a uu
	// is a centimetre). 1 m/s^2 up, 2 m/s^2 braking, 10 m/s (36 km/h) flat out - an airside
	// speed limit, not a road one. The same figures FGroundRegime's own defaults carry,
	// written out here rather than left implicit because this is where a truck's performance
	// is DECIDED, and a reader must be able to see it without opening another header.
	Van.Ground.Taxi.Accel = 100.0;
	Van.Ground.Taxi.Decel = 200.0;
	Van.Ground.Taxi.SpeedCap = 1000.0;

	// Kept rolling to steer, like an aircraft, and for a different reason that lands in the
	// same place: a van CAN pivot, but a follower that let one stop dead mid-turn would
	// snap its heading round rather than swing it. 0.5 m/s.
	Van.Ground.MinTaxiSpeed = 50.0;

	// NINE TIMES an aircraft's 10 deg/s. A van on a 6 m road turns into a depot in its own
	// length; an aircraft's rate here would sweep it across the kerb and back.
	Van.Ground.MaxTurnRateDegPerSec = 90.0;

	// What the inspector SAYS this is - see FAirframe::TypeCode. FUEL rather than VAN
	// because the panel names the job the player can see, and this slice has exactly one.
	Van.TypeCode = TEXT("FUEL");

	return Van;
}
```

In `Public/Content/AirsideContent.h`, beside `AgentMesh`:

```cpp
	/**
	 * What a GROUND VEHICLE agent looks like. Null leaves the placeholder box.
	 *
	 * STATIC, not skeletal, and beside AgentMesh rather than replacing a branch inside it:
	 * a truck's wheels turn but nothing else does, and there is no rig yet. The box it falls
	 * back to is sized from FTrafficRules::VehicleFootprint, so what is on screen is the
	 * length the arbiter actually keeps clear - see ARoadAgentActor::SetVehicleBody.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Defaults")
	TSoftObjectPtr<UStaticMesh> VehicleMesh;
```

- [ ] **Step 4: `SetVehicleBody` on the view**

In `Public/Present/RoadAgentActor.h`, after `SetAirframe`:

```cpp
	/**
	 * Dress this view as a GROUND VEHICLE: Mesh if one was configured, else a box of
	 * BoxSizeUu.
	 *
	 * The sibling of SetAirframe, on the same actor rather than in a second class, because
	 * everything else about showing an agent - the pose, the motion, the no-collision rule,
	 * the outliner sprite - is identical and a second AActor would be a copy of it that must
	 * agree for ever.
	 *
	 * BoxSizeUu is the FULL size, X forward, so the caller passes the footprint the traffic
	 * arbiter actually reserves rather than a scale factor this class would have to know how
	 * to interpret.
	 */
	void SetVehicleBody(UStaticMesh* Mesh, const FVector& BoxSizeUu);

	/** True once SetVehicleBody has dressed this view. For Airside.Present.VehicleAgentView. */
	bool HasVehicleBodyForTest() const { return bIsVehicle; }
```

and, in the private section beside `bHasAirframe`:

```cpp
	/**
	 * The placeholder's world size in uu, so SetMotion's lift follows whatever the box was
	 * scaled to.
	 *
	 * A MEMBER RATHER THAN A CONSTANT because the box is no longer one size: an aircraft's
	 * stand-in is 4 m x 4 m x 2 m and a van's is its own footprint. The lift is half the
	 * HEIGHT, and reading it off a constant after the scale changed would sink a van into
	 * the road by exactly the difference.
	 */
	FVector PlaceholderSizeUu = FVector(400.0, 400.0, 200.0);

	/** False for an aircraft view. Only distinguishes what dressed it - the pose maths is
	 *  the same either way, and reads PlaceholderSizeUu, not this. */
	bool bIsVehicle = false;
```

In `Private/Present/RoadAgentActor.cpp`: keep `CubeUnits`, replace the constructor's
`SetRelativeScale3D` with one derived from `PlaceholderSizeUu`, change `SetMotion`'s lift to
`bHasAirframe ? 0.0 : PlaceholderSizeUu.Z * 0.5` (keeping its whole comment, extended to say
the size is now a member), and add:

```cpp
void ARoadAgentActor::SetVehicleBody(UStaticMesh* Mesh, const FVector& BoxSizeUu)
{
	bIsVehicle = true;

	// SIZED FIRST, so a null mesh still leaves a correctly-sized box rather than an
	// aircraft-sized one. The engine's unit cube is 100 uu, so the scale is the size over it.
	PlaceholderSizeUu = BoxSizeUu;
	if (Placeholder != nullptr)
	{
		Placeholder->SetRelativeScale3D(BoxSizeUu / CubeUnits);
	}

	if (Mesh == nullptr || Placeholder == nullptr)
	{
		// The box stands, deliberately, exactly as SetAirframe leaves the cube: a missing
		// asset must look like a placeholder rather than like an agent that failed to spawn.
		return;
	}

	// The mesh goes ON the placeholder component, not the skeletal root: a static mesh
	// cannot live in a skeletal one (see the header), and this is the component that
	// already exists for exactly this shape.
	Placeholder->EmptyOverrideMaterials();
	Placeholder->SetStaticMesh(Mesh);

	// SCALE 1 for a real asset. A truck modelled at its own size scaled to the footprint
	// would put the mesh and FTrafficRules::VehicleFootprint - which the arbiter reserves
	// line by - quietly out of step, the same trap SetAirframe records for the airframe.
	Placeholder->SetRelativeScale3D(FVector::OneVector);
	PlaceholderSizeUu = Mesh->GetBounds().BoxExtent * 2.0;
}
```

- [ ] **Step 5: Dress by class in `SpawnView`**

In `Private/Present/AirsideTraffic.cpp`, replace the content block inside `SpawnView`:

```cpp
	// THE MESH, not to be confused with the FAirframe performance struct. Pushed in, like
	// the pose: a view that fetched its own mesh by path was how a content move turned every
	// aircraft into a cube - see ARoadAgentActor::SetAirframe.
	//
	// BY THE AGENT'S CLASS, because an aircraft and a truck are different assets of
	// different kinds. Decided HERE rather than inside the view, so the view still knows
	// nothing about traffic classes and stays the dumb thing its header promises.
	const UAirsideContent* Content = UAirsideSettings::GetContent();
	if (Agent->Class == ETraversalClass::Aircraft)
	{
		if (Content != nullptr)
		{
			View->SetAirframe(Content->AgentMesh.LoadSynchronous(), Content->AgentAnimClass.LoadSynchronous());
		}
	}
	else
	{
		// THE BOX IS THE FOOTPRINT THE ARBITER RESERVES, so what the player sees stopping at
		// a junction is the length that actually stopped. Half the length across, which is a
		// van's proportions, and half again tall.
		const double Length = Model->Rules.FootprintFor(Agent->Class);
		View->SetVehicleBody(Content != nullptr ? Content->VehicleMesh.LoadSynchronous() : nullptr,
			FVector(Length, Length * 0.5, Length * 0.5));
	}
```

- [ ] **Step 6: Run the test**

Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside.Present`
Expected: `Airside.Present.VehicleAgentView` passes and every existing `Airside.Present.*`
still does - `AgentActorTest` and `AgentMotionTest` both assert the aircraft's lift.

- [ ] **Step 7: Write the crossing test - spec §8's `Traffic.TruckCrossesTaxiway`**

The one test that measures a truck against an aircraft rather than against the graph. It
belongs here rather than in Task 3 because it needs a dispatchable vehicle, which is what
this task produced.

Create `Plugins/Airside/Source/AirsideTests/Private/TruckCrossingTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Content/AirsideSettings.h"
#include "Misc/AutomationTest.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadTraffic.h"
#include "Model/RouteSearch.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTruckCrossesTaxiwayTest,
	"Airside.Traffic.TruckCrossesTaxiway",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTruckCrossesTaxiwayTest::RunTest(const FString& Parameters)
{
	// A hand-authored crossing: a taxiway east-west through the origin and a road
	// north-south through it, sharing the CENTRE node. Hand-authored rather than solved,
	// like every M2 traffic fixture, because what is under test is the arbiter and a
	// fixture that had to pave a junction first would fail for unrelated reasons.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());

	const FGuidelineNodeId Centre = Net->AddGuidelineNode(FVector2D(0.0, 0.0), false);
	const FGuidelineNodeId West   = Net->AddGuidelineNode(FVector2D(-20000.0, 0.0), false);
	const FGuidelineNodeId East   = Net->AddGuidelineNode(FVector2D(20000.0, 0.0), false);
	const FGuidelineNodeId South  = Net->AddGuidelineNode(FVector2D(0.0, -20000.0), false);
	const FGuidelineNodeId North  = Net->AddGuidelineNode(FVector2D(0.0, 20000.0), false);

	auto Link = [Net](FGuidelineNodeId A, FGuidelineNodeId B, ETraversalClass Class)
	{
		FGuidelineEdge Edge;
		Edge.A = A;
		Edge.B = B;
		Edge.Control = (Net->GetGuidelineNode(A)->Position + Net->GetGuidelineNode(B)->Position) * 0.5;
		Edge.AllowedTraffic = FTrafficMask::Only(Class);
		Edge.Direction = EGuidelineDir::Bidirectional;
		Edge.Width = 600.0;
		Edge.bDerived = false;
		Net->AddGuidelineEdge(MoveTemp(Edge));
	};
	Link(West, Centre, ETraversalClass::Aircraft);
	Link(Centre, East, ETraversalClass::Aircraft);
	Link(South, Centre, ETraversalClass::GroundVehicle);
	Link(Centre, North, ETraversalClass::GroundVehicle);

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());

	auto Route = [Net](FGuidelineNodeId Start, FGuidelineNodeId Goal, ETraversalClass Class)
	{
		FRouteQuery Query;
		Query.Start = Start;
		Query.Goal = Goal;
		Query.Class = Class;
		return RouteSearch::Find(*Net, Query);
	};

	const FRoutePlan AircraftPlan = Route(West, East, ETraversalClass::Aircraft);
	const FRoutePlan TruckPlan = Route(South, North, ETraversalClass::GroundVehicle);
	if (!TestTrue(TEXT("the aircraft routes along the taxiway"), AircraftPlan.IsValid())) { return false; }
	if (!TestTrue(TEXT("the truck routes across it"), TruckPlan.IsValid())) { return false; }

	const int32 Aircraft = Traffic->DispatchAgent(Net, AircraftPlan,
		UAirsideSettings::ResolveDefaultAirframe(), ETraversalClass::Aircraft, 0.0);
	const int32 Truck = Traffic->DispatchAgent(Net, TruckPlan,
		UAirsideSettings::ResolveDefaultVehicle(), ETraversalClass::GroundVehicle, 0.0);
	if (!TestTrue(TEXT("both are under way"), Aircraft != 0 && Truck != 0)) { return false; }

	// Drive them both and watch the closest they ever get. AIRCRAFT OVER VEHICLE is a fact
	// about the CLASSES (TraversalPriority, spec 5.4) and needs no authoring at the
	// junction - which is exactly the property worth measuring, because a crossing that
	// happened to work by timing would pass a test that only checked arrival.
	double Closest = TNumericLimits<double>::Max();
	bool bTruckWaited = false;
	for (int32 Step = 0; Step < 4000; ++Step)
	{
		Traffic->Advance(1.0 / 30.0, Net);
		const FRoadAgent* A = Traffic->FindAgent(Aircraft);
		const FRoadAgent* T = Traffic->FindAgent(Truck);
		if (A == nullptr || T == nullptr) { break; }

		Closest = FMath::Min(Closest,
			FVector2D::Distance(A->LastMotion.Position, T->LastMotion.Position));

		// A yield is the truck STOPPED while it still has road left - not the truck having
		// finished. WaitingOn names who refused it.
		bTruckWaited |= T->Phase == EAgentPhase::Taxiing
			&& T->LastMotion.GroundSpeed <= 0.0 && T->WaitingOn == Aircraft;
	}

	TestTrue(TEXT("the truck gave way to the aircraft at the crossing"), bTruckWaited);

	// MIN SEPARATION, measured rather than asserted by proxy. Half the aircraft footprint
	// plus half the vehicle's is the point at which the two bodies touch; anything at or
	// under that is a collision the arbiter was supposed to prevent.
	const double Touching = (Traffic->Rules.AircraftFootprint + Traffic->Rules.VehicleFootprint) * 0.5;
	TestTrue(FString::Printf(TEXT("they never touched: closest %.0f uu, touching at %.0f"),
		Closest, Touching), Closest > Touching);

	// AND THE TRUCK STILL GETS THERE. A yield that never released would pass every
	// assertion above and be exactly the starvation the resolver exists to prevent.
	const FRoadAgent* Arrived = Traffic->FindAgent(Truck);
	TestTrue(TEXT("the truck reaches the far side"),
		Arrived == nullptr || Arrived->Phase == EAgentPhase::Parked);
	return true;
}

#endif
```

Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside.Traffic.TruckCrossesTaxiway`
Expected: PASS with no production change. **If the truck never waits, stop and report it**:
either the fixture never brings the two together (widen the loop, or start the truck
further out so the timing overlaps) or class priority is not reaching a mixed-class node,
which is a finding that changes the slice.

- [ ] **Step 8: Build and commit**

```bash
git add Plugins/Airside/Source/Airside Plugins/Airside/Source/AirsideTests
git commit -m "feat(airside): a ground vehicle agent gets its own performance default and body"
```

---

### Task 7: `UFuelService` - the demand, the trip, the dwell

**Files:**
- Create: `Plugins/AirportOps/Source/AirportOps/Public/Model/FuelService.h`
- Create: `Plugins/AirportOps/Source/AirportOps/Private/Model/FuelService.cpp`
- Test: `Plugins/AirportOps/Source/AirportOpsTests/Private/FuelServiceTest.cpp` (new)

**Interfaces:**
- Consumes: `UGroundTraffic::{DispatchAgent, RedirectAgent, RetireAgent, FindAgent, GetSimSeconds}`;
  `URoadNetwork::{FindEntityIndexByPoseNode, EntityIdAt, GetEntity, GetEntities,
  GetAnchorIdsForRole, FindResolvedAnchor, GetGuidelineRevision}`;
  `RouteSearch::Find`, `FRouteQuery`, `ERunwayAvoidance::All`;
  `UAirsideSettings::ResolveDefaultVehicle()` (Task 6); `FEntityInstance::PoseRole`,
  `UEntityDefinition::Trucks` (Task 4); `EAgentPhase`, `LogAirportOps`.
- Produces:
  - `enum class EFuelDemandState : uint8 { Needed, TruckEnRoute, Fuelling, Done, Unserviceable };`
  - `enum class EFuelRefusal : uint8 { None, NoDepot, NoRoad, StandUnjoined, NoRoute };`
  - `struct FFuelDemand` (USTRUCT) with the spec's §6 fields
  - `UCLASS() UFuelService : public UObject` with:
    - `UPROPERTY(EditAnywhere) double DwellSeconds = 40.0;`
    - `void OnAgentPhase(UGroundTraffic& Traffic, const URoadNetwork& Network, int32 AgentId, EAgentPhase From, EAgentPhase To);`
    - `void Tick(UGroundTraffic& Traffic, const URoadNetwork& Network);`
    - `FString DescribeAgent(int32 AgentId) const;`
    - `const TArray<FFuelDemand>& GetDemands() const;`

- [ ] **Step 1: Write the failing tests**

Create `Plugins/AirportOps/Source/AirportOpsTests/Private/FuelServiceTest.cpp`. Build the
fixture once and reuse it across three tests:

```cpp
#include "CoreMinimal.h"
#include "Content/AirsideSettings.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/FuelService.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadTraffic.h"
#include "Model/RouteSearch.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/**
	 * One stand, one depot, one service road joining them - world-free.
	 *
	 * Hand-authored guidelines rather than a solve, exactly as the M2 traffic fixtures do:
	 * what is under test is the SERVICE, and a fixture that had to lay pavement correctly
	 * first would fail for reasons that have nothing to do with fuel.
	 */
	struct FFuelFixture
	{
		URoadNetwork* Net = nullptr;
		UGroundTraffic* Traffic = nullptr;
		UFuelService* Service = nullptr;

		FEntityInstanceId Stand;
		FEntityInstanceId Depot;
		FGuidelineNodeId StandPose;
		FGuidelineNodeId StandFuel;
		FGuidelineNodeId DepotPose;
		FGuidelineNodeId TaxiwayEnd;

		/** bWithRoad false leaves the depot and the stand's hydrant unjoined. */
		void Build(bool bWithRoad, bool bWithDepot = true);

		/** Add the road guideline after the fact and re-run FAnchorLink - the player drawing
		 *  the missing road. Bumps the graph revision, which is what re-offers a refusal. */
		void JoinRoad();

		/** Land an aircraft at the stand the crude way: dispatch it along the taxiway to the
		 *  stand's pose node and run it to Parked. Returns its agent id. */
		int32 ParkAircraft();

		/** Advance both the traffic and the service by Seconds, in the steps a tick takes. */
		void Advance(double Seconds);
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFuelServiceTest, "AirportOps.Ops.FuelService",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFuelServiceTest::RunTest(const FString& Parameters)
{
	FFuelFixture Fixture;
	Fixture.Build(/*bWithRoad=*/true);
	const int32 Aircraft = Fixture.ParkAircraft();

	// 1. PARKED MAKES A DEMAND, and nothing else does. No button, no offer - the player's
	// only act was to build the depot on a road (spec §2).
	Fixture.Advance(0.1);
	if (!TestEqual(TEXT("one demand"), Fixture.Service->GetDemands().Num(), 1)) { return false; }
	TestEqual(TEXT("for the aircraft that parked"),
		Fixture.Service->GetDemands()[0].AircraftId, Aircraft);

	// 2. A TRUCK GOES OUT on the next tick, and the demand names it.
	Fixture.Advance(0.1);
	const FFuelDemand& EnRoute = Fixture.Service->GetDemands()[0];
	if (!TestEqual(TEXT("a truck is en route"), static_cast<int32>(EnRoute.State),
		static_cast<int32>(EFuelDemandState::TruckEnRoute))) { return false; }
	TestTrue(TEXT("and it is a real agent"), EnRoute.TruckId != 0);
	TestEqual(TEXT("dispatched from the depot"), EnRoute.Depot, Fixture.Depot);
	TestEqual(TEXT("as a ground vehicle"),
		static_cast<int32>(Fixture.Traffic->FindAgent(EnRoute.TruckId)->Class),
		static_cast<int32>(ETraversalClass::GroundVehicle));

	// 3. IT REACHES THE HYDRANT AND FUELS. 20000 uu at 10 m/s is 20 s of driving; 60 gives
	// the arbiter room without being a timeout dressed as an assertion.
	Fixture.Advance(60.0);
	if (!TestEqual(TEXT("fuelling"), static_cast<int32>(Fixture.Service->GetDemands()[0].State),
		static_cast<int32>(EFuelDemandState::Fuelling))) { return false; }
	TestEqual(TEXT("the truck parked at the stand's fuel anchor"),
		Fixture.Traffic->FindAgent(EnRoute.TruckId)->GoalNode, Fixture.StandFuel);

	// 4. THE DWELL IS THE DWELL. Measured in UGroundTraffic's sim seconds - the same clock
	// the truck's own motion accrues on - so 40 s of dwell is 40 s of watching, not 0.55 as
	// it would be on the day-compressed USimClock.
	const double DwellStarted = Fixture.Traffic->GetSimSeconds();
	Fixture.Advance(Fixture.Service->DwellSeconds - 5.0);
	TestEqual(TEXT("still fuelling five seconds short"),
		static_cast<int32>(Fixture.Service->GetDemands()[0].State),
		static_cast<int32>(EFuelDemandState::Fuelling));
	Fixture.Advance(10.0);
	TestEqual(TEXT("done once the dwell is up"),
		static_cast<int32>(Fixture.Service->GetDemands()[0].State),
		static_cast<int32>(EFuelDemandState::Done));
	TestTrue(TEXT("and it took the dwell, not less"),
		Fixture.Traffic->GetSimSeconds() - DwellStarted >= Fixture.Service->DwellSeconds);

	// 5. HOME AND RETIRED. It does not fly away, so nothing else would ever remove it -
	// which is exactly what UGroundTraffic::RetireAgent exists for.
	const int32 TruckId = Fixture.Service->GetDemands()[0].TruckId;
	Fixture.Advance(60.0);
	TestNull(TEXT("the truck is retired at the depot"), Fixture.Traffic->FindAgent(TruckId));

	// 6. THE DEPOT'S COUNT IS FREE AGAIN: a second aircraft gets a truck.
	const int32 Second = Fixture.ParkAircraft();
	Fixture.Advance(0.2);
	bool bSecondServed = false;
	for (const FFuelDemand& Demand : Fixture.Service->GetDemands())
	{
		bSecondServed |= Demand.AircraftId == Second && Demand.TruckId != 0;
	}
	TestTrue(TEXT("the freed truck serves the next aircraft"), bSecondServed);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFuelServiceRefusalsTest, "AirportOps.Ops.FuelServiceRefusals",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFuelServiceRefusalsTest::RunTest(const FString& Parameters)
{
	// NO DEPOT. Checked FIRST so the reason names the thing nearest the player's hand
	// (spec §6): "no fuel depot" is a thing they can go and build.
	{
		FFuelFixture Fixture;
		Fixture.Build(/*bWithRoad=*/true, /*bWithDepot=*/false);
		Fixture.ParkAircraft();
		Fixture.Advance(0.2);
		if (!TestEqual(TEXT("one demand"), Fixture.Service->GetDemands().Num(), 1)) { return false; }
		TestEqual(TEXT("unserviceable"), static_cast<int32>(Fixture.Service->GetDemands()[0].State),
			static_cast<int32>(EFuelDemandState::Unserviceable));
		TestEqual(TEXT("because there is no depot"),
			static_cast<int32>(Fixture.Service->GetDemands()[0].Why),
			static_cast<int32>(EFuelRefusal::NoDepot));
	}

	// DEPOT OFF ANY ROAD, and the stand's hydrant unjoined too. NoRoad wins over
	// StandUnjoined by the spec's stated order.
	{
		FFuelFixture Fixture;
		Fixture.Build(/*bWithRoad=*/false);
		Fixture.ParkAircraft();
		Fixture.Advance(0.2);
		TestEqual(TEXT("depot not on a road"), static_cast<int32>(Fixture.Service->GetDemands()[0].Why),
			static_cast<int32>(EFuelRefusal::NoRoad));

		// A REBUILD RE-OFFERS IT. The player may have just drawn the road, and a terminal
		// state that never looked again would leave them staring at a truck that never comes.
		// The graph REVISION is what says so - see URoadNetwork::GetGuidelineRevision.
		Fixture.JoinRoad();
		Fixture.Advance(0.2);
		TestNotEqual(TEXT("the demand is live again"),
			static_cast<int32>(Fixture.Service->GetDemands()[0].State),
			static_cast<int32>(EFuelDemandState::Unserviceable));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFuelServiceAircraftLeavesTest, "AirportOps.Ops.FuelServiceAircraftLeaves",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFuelServiceAircraftLeavesTest::RunTest(const FString& Parameters)
{
	FFuelFixture Fixture;
	Fixture.Build(/*bWithRoad=*/true);
	const int32 Aircraft = Fixture.ParkAircraft();
	Fixture.Advance(0.2);

	const int32 TruckId = Fixture.Service->GetDemands()[0].TruckId;
	if (!TestTrue(TEXT("a truck went out"), TruckId != 0)) { return false; }

	// The aircraft goes mid-service. The truck must NOT be left standing at a hydrant
	// nobody is using, and the depot's count must come back - otherwise one departure
	// costs the airport a truck for the rest of the session.
	Fixture.Traffic->RetireAgent(Aircraft);
	Fixture.Advance(0.2);

	TestEqual(TEXT("the demand is dropped"), Fixture.Service->GetDemands().Num(), 0);
	if (!TestNotNull(TEXT("but the truck still exists"), Fixture.Traffic->FindAgent(TruckId))) { return false; }
	TestEqual(TEXT("and is heading home to the depot"),
		Fixture.Traffic->FindAgent(TruckId)->GoalNode, Fixture.DepotPose);

	Fixture.Advance(120.0);
	TestNull(TEXT("and is retired when it gets there"), Fixture.Traffic->FindAgent(TruckId));
	return true;
}

#endif
```

Write `FFuelFixture::Build`, `::ParkAircraft`, `::JoinRoad` and `::Advance` in the anonymous
namespace. `Build` lays: an aircraft guideline (the taxiway) from `TaxiwayEnd` to the stand's
pose; a `GroundVehicle` guideline (the road) when `bWithRoad`; places the stand with
`MakeStandTransient()` and the depot with `MakeFuelDepotTransient()`; then runs
`FAnchorLink::Build`. `JoinRoad` adds the road guideline afterwards and re-runs
`FAnchorLink::Build`, which bumps `GetGuidelineRevision`. `Advance` loops
`Traffic->Advance(Step, Net); Service->Tick(*Traffic, *Net);` at `Step = 1.0 / 30.0`.
`ParkAircraft` dispatches an aircraft along the taxiway to the stand pose with
`UAirsideSettings::ResolveDefaultAirframe()` and advances until its phase is `Parked`,
relaying every phase change into `Service->OnAgentPhase` - bind
`Traffic->OnAgentPhaseChanged` once in `Build` with a lambda that does exactly what
`UOpsRuntime` will do in Task 8.

- [ ] **Step 2: Run them and watch them fail**

Run: `./Tools/Run-AirsideTests.ps1 -Filter AirportOps.Ops`
Expected: compile failure - `Model/FuelService.h` not found.

- [ ] **Step 3: Write the header**

Create `Plugins/AirportOps/Source/AirportOps/Public/Model/FuelService.h`:

```cpp
#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"
#include "UObject/Object.h"
#include "FuelService.generated.h"

class UGroundTraffic;
class URoadNetwork;
enum class EAgentPhase : uint8;

/**
 * How far through being fuelled one parked aircraft is.
 *
 * AN ENUM, NEVER A SET OF BOOLS. "A truck is out" and "the dwell is running" can never both
 * be the current state, and the states are visited in one order, so the illegal combinations
 * stop being representable - the same rule EAgentPhase and ECrossingPhase follow.
 */
UENUM()
enum class EFuelDemandState : uint8
{
	/** Parked and asking. Every tick tries to find it a truck. */
	Needed,

	/** A truck is driving to the stand's hydrant. TruckId and Depot name which. */
	TruckEnRoute,

	/** The truck is at the hydrant, running down DwellEndsAt. */
	Fuelling,

	/** Fuelled. The truck is on its way home; the demand stays here so the aircraft's card
	 *  can say so and so a second truck is never sent. */
	Done,

	/**
	 * Nothing can serve it. TERMINAL FOR THIS GRAPH: a re-offer needs the airport to have
	 * CHANGED, which is what URoadNetwork::GetGuidelineRevision reports. Without that a
	 * demand refused once would be retried every tick for ever, logging as it went.
	 */
	Unserviceable
};

/**
 * WHY nothing can serve a demand - one cause, named for the thing the player would fix.
 *
 * Not a bare "no route", which is the least useful thing to say to somebody building an
 * airport (the same argument ERouteResult's header makes): "no fuel depot" is a building to
 * place, "depot not on a road" is a road to draw, and the two have nothing in common.
 */
UENUM()
enum class EFuelRefusal : uint8
{
	None,
	/** No live fuel depot anywhere on the airport. */
	NoDepot,
	/** Depots exist; not one of them has a road within its lead-in reach. */
	NoRoad,
	/** The stand's own fuel anchor joins nothing - no road within reach of the hydrant. */
	StandUnjoined,
	/** Everything is joined and the graph still does not connect the two. */
	NoRoute
};

/** One parked aircraft's fuel job. */
USTRUCT()
struct AIRPORTOPS_API FFuelDemand
{
	GENERATED_BODY()

	/** The parked agent. 0 is never issued, so 0 means "no aircraft". */
	UPROPERTY() int32 AircraftId = 0;

	/** Where it parked, so the hydrant can be found again after a rebuild. */
	UPROPERTY() FEntityInstanceId Stand;

	UPROPERTY() EFuelDemandState State = EFuelDemandState::Needed;

	/** The truck out for it, or 0. */
	UPROPERTY() int32 TruckId = 0;

	/** Whose truck - so the count can be freed against the right depot. */
	UPROPERTY() FEntityInstanceId Depot;

	/** UGroundTraffic::GetSimSeconds at which the dwell ends. See UFuelService's header for
	 *  why that clock and not USimClock. */
	UPROPERTY() double DwellEndsAt = 0.0;

	UPROPERTY() EFuelRefusal Why = EFuelRefusal::None;
};

/**
 * Every aircraft that parks demands fuel; this finds it a truck, waits out the dwell, and
 * sends the truck home. Spec 2026-09-07-fuel-service-slice §6.
 *
 * IN AirportOps AND NOT IN Airside, deliberately. Airside knows how a thing MOVES and must
 * never learn what it is FOR - so the road, the depot's pose, the vehicle's performance and
 * the truck's view are all over there, and demand, dwell and job state are here. That
 * boundary is what Check-Architecture.ps1 enforces in one direction; this class is the
 * other side of it.
 *
 * IN Model/ AND NOT Present/, which is what makes it testable with no world: it takes the
 * UGroundTraffic and the URoadNetwork PER CALL and holds neither, so it cannot outlive a
 * graph and a test drives it with NewObject fixtures. Dispatch goes through UGroundTraffic
 * rather than ARoadNetworkActor for the same reason - and the truck's VIEW still appears,
 * because UAirsideTraffic spawns one off the model's own phase broadcast.
 *
 * TIME COMES FROM UGroundTraffic::GetSimSeconds, NOT FROM USimClock. The clock is
 * day-compressed - at the default 1200 real seconds per game day a 40 s dwell would be 0.55
 * real seconds - while the truck's MOTION runs on the speed multiplier alone. A dwell timed
 * on the clock would be over before the truck had stopped rolling. USimClock's own header
 * says turnaround durations are authored in game time; this is the one place that reading
 * does not survive contact with a dwell the player watches, and the reason is recorded here
 * rather than argued again later.
 *
 * SCAFFOLDING, and named as such by the spec (§0.1): M3's UJobBoard replaces "nearest depot
 * with a truck free" with demands from a flight, depots bidding by ETA, multi-trip jobs and
 * stuck recovery. What survives is everything below it - the road, the anchor joins, the
 * vehicle agent and its view - which is why none of that is in this class.
 */
UCLASS()
class AIRPORTOPS_API UFuelService : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * How long a truck stays at the hydrant, in UGroundTraffic sim seconds.
	 *
	 * A PROPERTY AND NOT A CONSTANT, so it is a figure a designer changes rather than a
	 * recompile. Set from UScenario::FuelDwellSeconds at attach, exactly as USimClock's
	 * RealSecondsPerGameDay is - the default here is only what a bare NewObject gets.
	 */
	UPROPERTY(EditAnywhere, Category = "Fuel", meta = (ClampMin = "0.0"))
	double DwellSeconds = 40.0;

	/**
	 * Every phase change in the traffic model. The events this whole class is driven by:
	 * an aircraft reaching Parked at a stand makes a demand, a truck reaching Parked at the
	 * hydrant starts the dwell, a truck reaching Parked at home is retired, and an aircraft
	 * leaving drops its demand.
	 */
	void OnAgentPhase(UGroundTraffic& Traffic, const URoadNetwork& Network, int32 AgentId,
		EAgentPhase From, EAgentPhase To);

	/** One pass: offer every Needed demand a truck, run the dwells down, re-offer the
	 *  unserviceable ones when the graph has changed under them. */
	void Tick(UGroundTraffic& Traffic, const URoadNetwork& Network);

	/**
	 * The one line the inspector's aircraft card shows for this agent, or empty when it has
	 * no demand.
	 *
	 * A STRING BUILT HERE rather than an enum the panel switches on: it is presentation of
	 * two orthogonal model facts (the state and, for Unserviceable, the reason), and nothing
	 * branches on it - the same argument InspectFacts::StatusOf makes.
	 */
	FString DescribeAgent(int32 AgentId) const;

	const TArray<FFuelDemand>& GetDemands() const { return Demands; }

private:
	UPROPERTY() TArray<FFuelDemand> Demands;

	/**
	 * The guideline revision the last unserviceable pass was decided against.
	 *
	 * WHAT MAKES Unserviceable RE-OFFERABLE WITHOUT BEING RETRIED EVERY TICK. The revision
	 * is bumped by every guideline mutation (see URoadNetwork::GetGuidelineRevision), so a
	 * player drawing the missing road changes it and nothing else does. Not saved: it dates
	 * a graph within one session, and a loaded graph starts at zero with every demand gone
	 * anyway - agents never reach disk.
	 */
	uint32 LastRefusedRevision = 0;

	/** The demand for an aircraft, or null. Linear: there are tens of stands. */
	FFuelDemand* FindByAircraft(int32 AircraftId);
	const FFuelDemand* FindByAircraft(int32 AircraftId) const;
	FFuelDemand* FindByTruck(int32 TruckId);

	/**
	 * The stand's Fuel anchor node, or unset. By ROLE and then by id, never by index - see
	 * URoadNetwork::GetAnchorIdsForRole.
	 */
	static FGuidelineNodeId FuelAnchorOf(const URoadNetwork& Network, FEntityInstanceId Stand);

	/**
	 * Nearest depot with a truck free and a route, by ROUTE LENGTH and not by straight-line
	 * distance: a depot 200 m away across a runway is further than one 400 m away along the
	 * road, and the truck drives the road. Fills OutPlan with the winning route.
	 *
	 * Reports Why in the order the spec fixes (NoDepot, NoRoad, StandUnjoined, NoRoute) so
	 * the reason names the thing nearest the player's hand.
	 */
	FEntityInstanceId ChooseDepot(const URoadNetwork& Network, FGuidelineNodeId StandFuel,
		FRoutePlan& OutPlan, EFuelRefusal& OutWhy) const;

	/** How many trucks this depot has out right now, counted off Demands - never stored on
	 *  the entity, which would be a second source of truth to keep in step. */
	int32 TrucksOutFor(FEntityInstanceId Depot) const;

	/** Redirect the truck to its depot's pose node, or retire it where it stands if it
	 *  cannot get home. Used by Done and by an aircraft leaving mid-service. */
	void SendTruckHome(UGroundTraffic& Traffic, const URoadNetwork& Network, FFuelDemand& Demand);
};
```

`FRoutePlan` needs `#include "Model/RouteSearch.h"` in the header for `ChooseDepot`'s
out-parameter - add it beside `Model/RoadHandles.h`.

- [ ] **Step 4: Write the implementation**

Create `Plugins/AirportOps/Source/AirportOps/Private/Model/FuelService.cpp`. It includes
`AirportOpsLog.h`, `Content/AirsideSettings.h`, `Entities/EntityDefinition.h`,
`Model/GroundTraffic.h`, `Model/RoadAgent.h`, `Model/RoadEntity.h`, `Model/RoadNetwork.h`
and `Model/RouteSearch.h` - **and nothing from `Present/`**, which is what
`Check-Architecture.ps1` will fail the build over.

Points the implementation must get right, each of which the tests above pin:

- `OnAgentPhase`, `To == EAgentPhase::Parked`: read `Agent->GoalNode`. If
  `Network.FindEntityIndexByPoseNode(GoalNode)` names a live entity whose `PoseRole` is
  `Aircraft` **and** the agent's `Class` is `Aircraft`, add a `Needed` demand. An aircraft
  parked on a taxiway junction (the stand-death fallback) makes no demand - it has no
  entity - which is the spec's own rule and falls out of the same lookup.
- `OnAgentPhase`, truck reaching `Parked`: `FindByTruck`. If its goal is the stand's fuel
  anchor and the demand is `TruckEnRoute`, set `Fuelling` and
  `DwellEndsAt = Traffic.GetSimSeconds() + DwellSeconds`. If its goal is the depot's pose
  node, `Traffic.RetireAgent(TruckId)` and clear `TruckId` - which is what frees the count.
- `OnAgentPhase`, `To == EAgentPhase::Gone` for an AIRCRAFT with an open demand: send its
  truck home (`SendTruckHome`) and remove the demand. Note the truck's own retirement then
  arrives as a `Gone` for a `TruckId` no demand names any more, so `SendTruckHome` must
  either keep a home-bound truck in a demand or retire it on arrival by its own goal - **do
  the latter**: track home-bound trucks in a separate small `TArray<TPair<int32, FEntityInstanceId>> GoingHome`
  so the retire-at-home rule works for an orphaned truck too. Add that array with a WHY
  comment saying exactly this.
- `Tick`, `Needed`: `ChooseDepot`, then
  `Traffic.DispatchAgent(&Network, Plan, UAirsideSettings::ResolveDefaultVehicle(), ETraversalClass::GroundVehicle, /*ShutdownPauseSeconds=*/0.0)`.
  **Zero, with a comment**: the shutdown pause is an aircraft's post-arrival engine
  wind-down (see `ARoadNetworkActor::ShutdownPauseSeconds`), and a truck that idled ten
  seconds before its dwell even started would add that to every trip for nothing.
- `Tick`, `Fuelling` and `Traffic.GetSimSeconds() >= DwellEndsAt`: `SendTruckHome`, set `Done`.
- `Tick`, `Unserviceable` and `Network.GetGuidelineRevision() != LastRefusedRevision`: back
  to `Needed`. Write `LastRefusedRevision` whenever a demand is refused.
- `ChooseDepot` route query: `Class = GroundVehicle`, `Wingspan = 0.0`,
  `AvoidRunways = ERunwayAvoidance::All`, `Occupancy = &Traffic.GetOccupancy()` is **not**
  set - the depot choice is about the airport's shape, not about who is on it this instant,
  and a congestion-weighted length would make the chosen depot flicker between ticks.
  Comment that rejection.
- `SendTruckHome`: route the truck's current node to the depot's pose node and
  `Traffic.RedirectAgent`. `RedirectAgent` accepts a `Parked` agent, which is exactly the
  handover its own header describes. If the route fails, `RetireAgent` and log it -
  a truck stuck at a hydrant for ever is worse than one that vanishes with a line saying so.
- **One `UE_LOG(LogAirportOps, Log, ...)` per transition**, naming aircraft, stand, depot and
  truck ids, plus one `Warning` per refusal with its `Why`. That is the spec's §6 Outputs and
  §7's failure table, and it is what makes a PIE report readable off the log.

- [ ] **Step 5: Run the tests**

Run: `./Tools/Run-AirsideTests.ps1 -Filter AirportOps.Ops`
Expected: `3 test(s) run, 0 failed, 0 crashed`.

- [ ] **Step 6: Run the architecture lint and the whole suite**

Run: `./Tools/Check-Architecture.ps1` then `./Tools/Run-AirsideTests.ps1`
Expected: the lint exits 0 (no `Present/` include from `Model/`), and nothing else regressed.

- [ ] **Step 7: Build and commit**

```bash
git add Plugins/AirportOps/Source
git commit -m "feat(ops): UFuelService - every parked aircraft demands fuel, one truck answers"
```

---

### Task 8: Wiring - the runtime owns it, the inspector shows it

**Files:**
- Modify: `Plugins/AirportOps/Source/AirportOps/Public/Model/OpsDefinition.h`
- Modify: `Plugins/AirportOps/Source/AirportOps/Public/Present/OpsRuntime.h`
- Modify: `Plugins/AirportOps/Source/AirportOps/Private/Present/OpsRuntime.cpp`
- Modify: `Plugins/Airside/Source/Airside/Public/Model/InspectFacts.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Model/InspectFacts.cpp`
- Modify: `Source/AirportMgr/InspectorWidget.cpp`
- Test: `Plugins/AirportOps/Source/AirportOpsTests/Private/FuelServiceWiredTest.cpp` (new)

**Interfaces:**
- Consumes: Task 7's `UFuelService`; `UOpsRuntime::{Attach, Tick, OnAgentPhase}`;
  `ARoadNetworkActor::{GetTraffic, Network}`; `UAirsideTraffic::GetModel()`.
- Produces:
  - `UScenario::FuelDwellSeconds` (`UPROPERTY(EditAnywhere) double`, default 40)
  - `UFuelService* UOpsRuntime::GetFuelService() const`
  - `FAgentFacts::Fuel` (`FString`)
  - `FStandFacts::PoseRole` (`EServiceRole`)

- [ ] **Step 1: Write the failing test**

Create `Plugins/AirportOps/Source/AirportOpsTests/Private/FuelServiceWiredTest.cpp` - the
composition proof, with a real world, a real actor and a real tick loop:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFuelServiceWiredTest, "AirportOps.Present.FuelServiceWired",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
```

It spawns an `ARoadNetworkActor` in a `UWorld::CreateWorld` game world, assigns
`StandDefinition`, `FuelDepotDefinition` and `ServiceRoadProfile` by hand (no content set is
configured in automation), lays the taxiway and road guidelines, places the stand and the
depot, runs `RebuildMesh()`, `NewObject<UOpsRuntime>()` and `Attach(Actor)`, dispatches an
aircraft to the stand pose, then ticks BOTH `Actor->Tick(Step)` and `Runtime->Tick(Step)` in
a loop, asserting in order:

```cpp
	// THE COMPOSITION-LEVEL PROOF, and the only test that fails if the relay or the tick is
	// left unwired. Every piece below has its own unit test; what is measured here is that
	// they are joined - the same job AirportOps.Present.Runtime does for save/load.
	TestNotNull(TEXT("the runtime owns a fuel service"), Runtime->GetFuelService());
	// ... tick until the aircraft parks ...
	TestEqual(TEXT("parking made a demand"), Runtime->GetFuelService()->GetDemands().Num(), 1);
	// ... tick on ...
	TestTrue(TEXT("a GroundVehicle agent appeared"), bSawVehicleAgent);
	TestTrue(TEXT("with a view of its own"),
		Actor->GetTraffic()->GetAgentView(TruckId) != nullptr);
	// ... tick past the dwell and the trip home ...
	TestTrue(TEXT("and it went away again"), Actor->GetTraffic()->FindAgent(TruckId) == nullptr);

	// The inspector's line, through the same seam the panel reads.
	TestTrue(TEXT("the aircraft's card can say what fuelling is doing"),
		!Runtime->GetFuelService()->DescribeAgent(AircraftId).IsEmpty());
```

- [ ] **Step 2: Run it and watch it fail**

Run: `./Tools/Run-AirsideTests.ps1 -Filter AirportOps.Present.FuelServiceWired`
Expected: compile failure - `GetFuelService` not a member of `UOpsRuntime`.

- [ ] **Step 3: The scenario figure**

In `Public/Model/OpsDefinition.h`, in `UScenario`:

```cpp
	/**
	 * How long a fuel truck stays at the hydrant, in the sim seconds a truck MOVES in - see
	 * UFuelService::DwellSeconds, and its header for why that is not game time. Copied into
	 * UFuelService by UOpsRuntime at attach, exactly as RealSecondsPerGameDay is copied into
	 * USimClock.
	 */
	UPROPERTY(EditAnywhere, Category = "Scenario", meta = (ClampMin = "0.0"))
	double FuelDwellSeconds = 40.0;
```

- [ ] **Step 4: The runtime owns and drives it**

In `Public/Present/OpsRuntime.h`: forward-declare `class UFuelService;`, add
`UFuelService* GetFuelService() const { return FuelService; }` beside the other getters, and
`UPROPERTY() TObjectPtr<UFuelService> FuelService;` beside `Catalog`. Extend the class
comment's "It GROWS BY FORWARDING" paragraph to name the fuel service as the first such
subobject, so the next reader sees the pattern being followed rather than broken.

In `Private/Present/OpsRuntime.cpp`:

```cpp
	// Constructor, beside the others:
	FuelService = CreateDefaultSubobject<UFuelService>(TEXT("FuelService"));

	// Attach, after the scenario is resolved - beside the RealSecondsPerGameDay copy, so
	// the two designer figures are set from the same asset in the same breath:
	if (const UScenario* Scenario = UAirportOpsSettings::ResolveDefaultScenario())
	{
		Clock->RealSecondsPerGameDay = Scenario->RealSecondsPerGameDay;
		FuelService->DwellSeconds = Scenario->FuelDwellSeconds;
		UE_LOG(LogAirportOps, Log, TEXT("Scenario '%s': %.0f real s per game day, %.0f s fuel dwell"),
			*Scenario->GetName(), Scenario->RealSecondsPerGameDay, Scenario->FuelDwellSeconds);
	}
```

`Tick` gains, after the speed push:

```cpp
	// THE NETWORK IS READ FRESH, never cached: URoadEditFacade::ClearNetwork replaces the
	// actor's network OBJECT rather than draining it, so a pointer held across a clear is
	// stale - the same reason LoadFromSlot re-reads it.
	//
	// TICKED IN REAL FRAME TIME, unscaled: the service's own clock is
	// UGroundTraffic::GetSimSeconds, which the actor's tick has already advanced by the
	// speed multiplier. Scaling here as well would run the dwell at the square of the
	// player's speed setting.
	if (Target != nullptr && Target->Network != nullptr && Target->GetTraffic() != nullptr)
	{
		if (UGroundTraffic* Model = Target->GetTraffic()->GetModel())
		{
			FuelService->Tick(*Model, *Target->Network);
		}
	}
```

`OnAgentPhase` relays into the service **before** the bus, so a Blueprint subscriber sees a
world in which the service has already reacted:

```cpp
void UOpsRuntime::OnAgentPhase(int32 AgentId, EAgentPhase From, EAgentPhase To)
{
	// THE SERVICE FIRST, THEN THE BUS. A Blueprint listener that asked the fuel service what
	// an aircraft was doing would otherwise see the state from before the event it was
	// woken by - one frame stale, and only sometimes.
	if (Target != nullptr && Target->Network != nullptr && Target->GetTraffic() != nullptr)
	{
		if (UGroundTraffic* Model = Target->GetTraffic()->GetModel())
		{
			FuelService->OnAgentPhase(*Model, *Target->Network, AgentId, From, To);
		}
	}
	Events->NotifyAgentPhaseChanged(AgentId, From, To);
}
```

- [ ] **Step 5: The facts fields**

In `Public/Model/InspectFacts.h`, in `FAgentFacts`:

```cpp
	/**
	 * What fuelling is doing for this aircraft - "truck en route", "no fuel depot" - or
	 * empty when nothing is.
	 *
	 * FILLED BY AirportOps, NOT BY DescribeAgent, which leaves it empty. Airside must never
	 * learn what a truck is for (see UFuelService), so the FIELD is here - because the panel
	 * reads FAgentFacts and never FRoadAgent - and the SENTENCE comes from the layer that
	 * knows. The same seam M3's UFlight fills its airline and off-block time through.
	 */
	FString Fuel;
```

In `FStandFacts`:

```cpp
	/**
	 * What this entity's pose is FOR - see FEntityInstance::PoseRole. Aircraft is a stand;
	 * anything else is a service installation, and the panel titles and describes it
	 * differently.
	 *
	 * The role rather than a bIsDepot flag: a flag would need a second one the day a second
	 * kind of installation arrives, and the enum already exists and already says it.
	 */
	EServiceRole PoseRole = EServiceRole::Aircraft;
```

Amend `DescribeStand`'s doc comment: it now describes any entity, and `SizeClass` /
`DesignWingspan` are meaningless for one with no design aircraft. Fill `PoseRole` from the
instance in `Private/Model/InspectFacts.cpp`; leave every other field exactly as it is.

- [ ] **Step 6: The panel**

In `Source/AirportMgr/InspectorWidget.cpp`, in the aircraft branch, after `Status = F.Status;`:

```cpp
		// THE FUEL LINE, from the layer that knows what fuel is. Reached through the ops
		// subsystem rather than through Target, because the airport actor is Airside's and
		// must not carry a pointer to a service it is forbidden to know about.
		if (const UOpsRuntime* Runtime = UOpsRuntimeSubsystem::Get(GetWorld()))
		{
			if (const UFuelService* Fuel = Runtime->GetFuelService())
			{
				F.Fuel = Fuel->DescribeAgent(F.Id);
			}
		}
		if (!F.Fuel.IsEmpty())
		{
			Facts += FString::Printf(TEXT("\nFuel %s"), *F.Fuel);
		}
```

and in the entity branch, replace the fixed title and facts with a branch on `S.PoseRole`:

```cpp
		if (S.PoseRole == EServiceRole::Aircraft)
		{
			// ... the existing stand title and facts, unchanged ...
		}
		else
		{
			Title = FString::Printf(TEXT("Fuel depot %d"), S.Index);
			// bReachable is the pose node having line on it, which for a depot means a
			// service road within its lead-in reach. The message names the fix, not the
			// symptom - the road is the thing the player goes and draws.
			Facts = S.bReachable ? TEXT("On a service road") : TEXT("Fuel depot: not on a road");
			Status = S.bReachable ? TEXT("Ready") : TEXT("Cannot dispatch");
			bDepartEnabled = false;
		}
```

- [ ] **Step 7: Run the tests**

Run: `./Tools/Run-AirsideTests.ps1 -Filter AirportOps` then the whole suite.
Expected: `AirportOps.Present.FuelServiceWired` passes; `Airside.Model.InspectFacts` and
`AirportMgr.InspectorWidget` still do.

- [ ] **Step 8: Build and commit**

```bash
git add Plugins/AirportOps/Source Plugins/Airside/Source/Airside Source/AirportMgr
git commit -m "feat(ops): UOpsRuntime owns and ticks the fuel service; the panel shows its line"
```

---

### Task 9: The authored assets and the probe

**Files:**
- Create: `Tools/Python/build_road_profiles.py`
- Modify: `Tools/Python/build_stand_asset.py`
- Modify: `Plugins/Airside/Source/AirsideTests/Private/StarterMapProbeTest.cpp`
- Modify: `Config/DefaultAirside.ini` - no change expected; the new slots live inside
  `DA_AirsideContent`, which the editor repoints itself.

**Interfaces:**
- Consumes: `URoadProfile::FillServiceRoad` and `UEntityDefinition::BuildFuelDepot`, both
  `BlueprintCallable` from Tasks 1 and 4; `UAirsideContent::{ServiceRoadProfile, DefaultFuelDepot}`.
- Produces: `/Game/DA_RoadProfile_ServiceRoad`, `/Game/Entities/DA_FuelDepot`, both wired
  into `DA_AirsideContent`; new `PROBE` log lines.

- [ ] **Step 1: Write the profile authoring script**

Create `Tools/Python/build_road_profiles.py`, modelled on `build_stand_asset.py` - same
`MARKER:` prefix, same `replace_asset` / `data_asset_factory` helpers, same
"editor must be CLOSED" note in the docstring. It calls
`unreal.RoadProfile.fill_service_road(profile, 600.0, 60.0, 500.0)` and saves
`/Game/DA_RoadProfile_ServiceRoad`, then logs the band widths and the guideline's class so
the result is readable off the log without opening the asset.

Its docstring must say what the file says: NO LAYOUT IS DEFINED HERE - every number comes
from the C++ builder through Blueprint exposure, so the figures the tests exercise and the
figures the shipped asset carries are the same figures.

- [ ] **Step 2: Add the depot to the stand script**

In `Tools/Python/build_stand_asset.py`, add a `build_fuel_depot()` beside `build_stand()`:

```python
def build_fuel_depot():
    depot = replace_asset(
        "DA_FuelDepot", unreal.EntityDefinition,
        data_asset_factory(unreal.EntityDefinition))
    if depot is None:
        return None

    unreal.EntityDefinition.build_fuel_depot(depot)
    unreal.EditorAssetLibrary.save_asset("%s/DA_FuelDepot" % ASSET_DIR)

    # NO ANCHORS is the correct answer here, not a failure - the depot's POSE is its road
    # connection (fuel-service spec, amended). Logged so a zero is read as intended rather
    # than as a build that half ran.
    unreal.log("MARKER: DA_FuelDepot built, pose role %s, %d truck(s), %d anchors" % (
        depot.get_editor_property("pose_role"),
        depot.get_editor_property("trucks"),
        len(depot.get_editor_property("anchors"))))
    return depot
```

and call it at the bottom of the file. Update the module docstring's asset list, which
currently names three assets.

- [ ] **Step 3: Run both scripts**

The editor must be CLOSED - `create_asset` refuses under `-unattended` while a running
editor holds the `.uasset` open.

```
D:\Epic\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe `
  C:\repos\AirportMgr2\AirportMgr.uproject -run=pythonscript `
  -script=C:\repos\AirportMgr2\Tools\Python\build_road_profiles.py -unattended -nosplash -nopause
```
then the same for `build_stand_asset.py`.
Expected: `MARKER:` lines in `Saved/Logs/AirportMgr.log` naming both new assets, with no
`MARKER: create_asset returned None`.

- [ ] **Step 4: Wire the two into `DA_AirsideContent`**

Open the editor, open `Content/DA_AirsideContent`, set `ServiceRoadProfile` to
`DA_RoadProfile_ServiceRoad` and `DefaultFuelDepot` to `DA_FuelDepot`, and save. **By hand,
in the editor, deliberately**: these are asset references the editor maintains, which is the
entire reason `UAirsideContent` exists (see its header) - a script that wrote them would be
the string path this class was built to remove.

`VehicleMesh` is left NULL: there is no truck asset yet, and null is the supported state
that draws the box. Say so in the commit message.

- [ ] **Step 5: Extend the probe**

In `StarterMapProbeTest.cpp`, after the existing anchor-link census, add a per-entity pass:

```cpp
	// PER ENTITY, so a "the truck never comes" report is answered by a grep rather than by
	// a PIE session: which stands can be fuelled at all, and which depots can dispatch.
	int32 StandsWithJoinedFuel = 0, DepotsJoined = 0, DepotsTotal = 0, StandsTotal = 0;
	for (int32 Index = 0; Index < Net->GetEntities().Num(); ++Index)
	{
		const FEntityInstance& Instance = Net->GetEntities()[Index];
		if (!Instance.bAlive) { continue; }
		const FEntityInstanceId Id = Net->EntityIdAt(Index);
		const FGuidelineNode* Pose = Net->GetGuidelineNode(Instance.PoseNode);
		const bool bPoseJoined = Pose != nullptr && Pose->Incident.Num() > 0;

		if (Instance.PoseRole != EServiceRole::Aircraft)
		{
			++DepotsTotal;
			DepotsJoined += bPoseJoined ? 1 : 0;
			UE_LOG(LogM2MapProbe, Log, TEXT("PROBE depot %d at (%.0f, %.0f): pose %s a road"),
				Index, Instance.Position.X, Instance.Position.Y,
				bPoseJoined ? TEXT("joins") : TEXT("JOINS NO"));
			continue;
		}

		++StandsTotal;
		for (const FName FuelId : Net->GetAnchorIdsForRole(Id, EServiceRole::Fuel))
		{
			const FResolvedAnchor* Anchor = Net->FindResolvedAnchor(Id, FuelId);
			const FGuidelineNode* Node = Anchor ? Net->GetGuidelineNode(Anchor->Node) : nullptr;
			const bool bJoined = Node != nullptr && Node->Incident.Num() > 0;
			StandsWithJoinedFuel += bJoined ? 1 : 0;
			UE_LOG(LogM2MapProbe, Log, TEXT("PROBE stand %d anchor '%s' (Fuel): %s a road"),
				Index, *FuelId.ToString(), bJoined ? TEXT("joins") : TEXT("JOINS NO"));
		}
	}
	UE_LOG(LogM2MapProbe, Log,
		TEXT("PROBE fuel readiness: %d of %d stand fuel anchor(s) on a road, %d of %d depot(s) on a road"),
		StandsWithJoinedFuel, StandsTotal, DepotsJoined, DepotsTotal);
```

- [ ] **Step 6: Run the full suite and the lint**

Run: `./Tools/Check-Architecture.ps1` then `./Tools/Run-AirsideTests.ps1`
Expected: exit 0 on the lint; read the `N test(s) run, N failed, N crashed` line and confirm
0 failed and 0 crashed. Quote that line in the commit.

- [ ] **Step 7: Commit**

```bash
git add Tools/Python Content/DA_AirsideContent.uasset Content/DA_RoadProfile_ServiceRoad.uasset `
        Content/Entities/DA_FuelDepot.uasset `
        Plugins/Airside/Source/AirsideTests/Private/StarterMapProbeTest.cpp
git commit -m "feat(content): DA_RoadProfile_ServiceRoad and DA_FuelDepot; probe reports fuel readiness"
```

---

## Verification: watch it work

The tests prove the parts. This is the slice's own claim - the GDD's first fifteen minutes -
and it is not made until somebody has seen it.

- [ ] **Step 1: Build, open the editor, PIE on `M_Starter`.**

- [ ] **Step 2: Draw it.** Key 9, draw a service road from beside the depot site round to the
  starboard side of a stand, crossing the taxiway if the layout needs it. Key 0, place a fuel
  depot with its +X facing the road. Key 7, land an aircraft.

- [ ] **Step 3: Watch.** The aircraft parks; a box leaves the depot, drives the road, stops at
  the hydrant, waits, and drives home.

- [ ] **Step 4: Read the log**, `Saved/Logs/AirportMgr.log`:
  - `Anchor links: N of M lead-in(s) joined a guideline, 0 unjoined` - if the depot or the
    hydrant is unjoined this line says so before anything else does.
  - `Guidelines: ... N arm end(s) with no through path for their class` - a crossing you
    only drew half of.
  - `LogAirportOps` - one line per transition, naming aircraft, stand, depot and truck.

- [ ] **Step 5: Click the aircraft.** The card reads `Fuel truck en route`, then `Fuel fuelling`,
  then `Fuel done`. Click the depot: `Fuel depot N`, `On a service road`.

**What confirms the fix:** the truck completes a round trip, and the `LogAirportOps` lines
for it run `Needed -> TruckEnRoute -> Fuelling -> Done` with the truck retired at the depot.
Anything short of that is the result, and it goes in the PR as it happened.

---

## Unresolved questions

1. **The runway crossing hold for trucks (spec amendment 1).** I have kept the derived runway
   holding position on service-road arms, against the spec's §3 bullet, because
   `UpdateCrossing` arms the crossing hold at a bar and skipping road arms would put a truck
   on a live strip holding nothing. That is the safe reading of two contradictory bullets -
   confirm it is the one you meant.
2. **The depot has no anchors (spec amendment 3).** Its pose node is its road connection and
   the truck is dispatched from and to it. §4's "ONE anchor of role Fuel" is dropped. If the
   anchor was meant to be a dispensing point distinct from the truck bay, say so and it comes
   back as a second lead-in.
3. **The dwell's clock (spec amendment 4).** `UGroundTraffic::GetSimSeconds`, not
   `USimClock`. This makes 40 s of dwell 40 s of watching, and it means the dwell does NOT
   compress with the game day - which is a deliberate departure from `USimClock`'s stated
   "turnaround durations are authored in game time".
4. **Key 0 for the depot.** Nine is the road; zero is the next key along the top row and the
   only number left. If the bar is meant to grow past ten tools, the numbering needs a plan
   before a second building arrives.
5. **`VehicleMesh` ships null**, so the truck is a 5 m x 2.5 m x 2.5 m box. Is a placeholder
   asset wanted this slice, or is the box the intended look until M3's fleet?
