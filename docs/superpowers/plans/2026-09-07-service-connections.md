# Service Connections and the Stand Service Loop — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** A service road drawn ALONGSIDE a row of stands connects every stand, and a truck
reaches the hydrant without driving through the parked aeroplane.

**Architecture:** Three changes, in dependency order. (1) `UEntityDefinition` gains a
`ServiceLoop` polygon computed by the stand's own builder from the design aircraft's
footprint and the anchors. (2) `FGuidelineEdge` gains a `ServiceLoopOwner` so the derived
loop and its spurs can be told apart from the roads they must join. (3) `FAnchorLink` splits
on traversal class: `Aircraft` keeps the ray, everything else joins by PROXIMITY within a
level-authored radius — and a new `FServiceLoopBuild` puts each stand's loop and its anchor
spurs into the graph before the join runs.

**Tech Stack:** UE 5.8.2 C++, `Airside` plugin (`Model/`, `Solve/`, `Build/`, `Present/`,
`Entities/`), `AirportOps` plugin, UE automation tests, Python (`Tools/Python`) for content.

**Spec:** `docs/superpowers/specs/2026-09-07-service-connections-design.md` — read it first;
this plan argues from it and does not restate its reasoning.

## Global Constraints

- **UE 5.8.2, engine at `D:\Epic\UE_5.8`, project `C:\repos\AirportMgr2`.**
- **The editor must be CLOSED for every build.** Tasks 1 and 2 add three new `UPROPERTY`s
  (`UEntityDefinition::ServiceLoop`, `FGuidelineEdge::ServiceLoopOwner`,
  `ARoadNetworkActor::ServiceLinkRadius`) — Live Coding cannot patch those in.
- **A new test .cpp needs TWO builds.** The first reports `Succeeded` without compiling it.
  Build twice whenever a task adds a test file.
- Build line:
  `D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development -Project="C:\repos\AirportMgr2\AirportMgr.uproject" -WaitMutex`
- Test line: `./Tools/Run-AirsideTests.ps1` (narrow with `-Filter Airside.Build` etc.).
  **Never trust its exit code alone** — read the `N test(s) run, N failed, N crashed` line.
- Units are uu; 1 uu = 1 cm.
- `Solve/` may include nothing beyond `CoreMinimal.h`. `Model/` must not include
  `Build/ Tool/ Present/ Entities/`. `Build/` must not include `Present/ Tool/`.
- Comments explain WHY, and especially why an obvious alternative was rejected. Match the
  surrounding density; do not strip existing ones.
- Every `UE_LOG` that exists today survives every edit in this plan.
- Branch is `feature/service-connections`, already checked out. Commit after every task.

## File Structure

| File | Responsibility |
|---|---|
| `Plugins/Airside/Source/Airside/Public/Entities/EntityDefinition.h` (M) | `ServiceLoop` field; `BuildCodeCStand` takes the design aircraft |
| `.../Private/Entities/EntityDefinition.cpp` (M) | Computes the loop from footprint ∪ anchors + clearance |
| `.../Public/Model/RoadGuideline.h` (M) | `FGuidelineEdge::ServiceLoopOwner` |
| `.../Public/Model/RoadNetwork.h` + `.../Private/Model/RoadNetwork.cpp` (M) | `IsServiceNodeConnected` — "does this node lead anywhere off the stand's own loop" |
| `.../Public/Solve/GuidelineGeom.h` + `.../Private/Solve/GuidelineGeom.cpp` (M) | `NearestOnPolyline`, `NearestBetweenPolylines` |
| `.../Public/Build/ServiceLoopBuild.h` (C) + `.../Private/Build/ServiceLoopBuild.cpp` (C) | Derives each entity's loop and anchor spurs into the graph |
| `.../Public/Build/AnchorLink.h` + `.../Private/Build/AnchorLink.cpp` (M) | Class-split link rule; joins each loop to a road |
| `.../Public/Present/RoadSurfacePresenter.h` + `.cpp` (M) | Carries `ServiceLinkRadius` through to `FAnchorLink::Build` |
| `.../Public/Present/RoadNetworkActor.h` + `.cpp` (M) | The level-authored `ServiceLinkRadius` property |
| `Plugins/AirportOps/.../Private/Model/FuelService.cpp` (M) | "stand not on a road" now asks the graph, not the incident count |
| `.../AirsideTests/Private/ServiceLoopTest.cpp` (C) | `Entities.ServiceLoop*` — world-free, definition only |
| `.../AirsideTests/Private/ServiceLinkTest.cpp` (C) | `Build.*` and `Traffic.*` — the graph |
| `.../AirsideTests/Private/StarterMapProbeTest.cpp` (M) | Per-stand loop connectivity in the probe |
| `Tools/Python/build_stand_asset.py` (M) | Passes the A320 into `build_code_c_stand` |

---

### Task 1: The service loop on the definition

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Entities/EntityDefinition.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Entities/EntityDefinition.cpp`
- Modify: `Tools/Python/build_stand_asset.py`
- Test: `Plugins/Airside/Source/AirsideTests/Private/ServiceLoopTest.cpp` (create)

**Interfaces:**
- Consumes: `FEntityFootprint` (`Model/RoadEntity.h`), `UAircraftType::Footprint`,
  `RoadGeom::PointInPolygon`, `RoadGeom::SegmentsCross` (`Solve/RoadGeom.h`).
- Produces:
  - `TArray<FVector2D> UEntityDefinition::ServiceLoop` — local space, implicitly closed.
  - `static void UEntityDefinition::BuildCodeCStand(UEntityDefinition* Definition, UAircraftType* DesignAircraft)`
    — **signature change**, and it now sets `Definition->DesignAircraft`.

- [ ] **Step 1: Write the failing tests**

Create `Plugins/Airside/Source/AirsideTests/Private/ServiceLoopTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Entities/AircraftType.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadEntity.h"
#include "Solve/RoadGeom.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/**
	 * Nearest point on the implicitly-closed loop to Query. The spur a service anchor gets.
	 *
	 * The same rule FServiceLoopBuild uses in the graph, restated here rather than called,
	 * because this file must be able to judge a HAND-BUILT definition that the graph builder
	 * would never be given - see the far-side fixture below.
	 */
	FVector2D NearestOnLoop(const TArray<FVector2D>& Loop, const FVector2D& Query)
	{
		FVector2D Best = FVector2D::ZeroVector;
		double BestDistance = TNumericLimits<double>::Max();
		for (int32 At = 0; At < Loop.Num(); ++At)
		{
			const FVector2D& A = Loop[At];
			const FVector2D& B = Loop[(At + 1) % Loop.Num()];
			const double T = RoadGeom::ClosestPointOnSegment(A, B, Query);
			const FVector2D Point = FMath::Lerp(A, B, T);
			const double Distance = FVector2D::Distance(Point, Query);
			if (Distance < BestDistance)
			{
				BestDistance = Distance;
				Best = Point;
			}
		}
		return Best;
	}

	/**
	 * How many loop segments and anchor spurs cross the fuselage LENGTHWISE - the invariant
	 * of the design's section 5, MEASURED.
	 *
	 * The centreline as a finite segment from TailX to NoseX, never the footprint outline:
	 * forbidding the footprint would forbid passing UNDER A WING, which is normal and which
	 * the hydrant requires - HydrantPit is under the starboard wing root because that is
	 * where a hydrant pit is.
	 */
	int32 CountCentrelineCrossings(const UEntityDefinition& Definition)
	{
		if (Definition.DesignAircraft == nullptr || Definition.ServiceLoop.Num() < 3)
		{
			return 0;
		}

		const FEntityFootprint& Footprint = Definition.DesignAircraft->Footprint;
		const FVector2D Tail(Footprint.TailX, 0.0);
		const FVector2D Nose(Footprint.NoseX, 0.0);

		int32 Crossings = 0;
		for (int32 At = 0; At < Definition.ServiceLoop.Num(); ++At)
		{
			const FVector2D& A = Definition.ServiceLoop[At];
			const FVector2D& B = Definition.ServiceLoop[(At + 1) % Definition.ServiceLoop.Num()];
			Crossings += RoadGeom::SegmentsCross(A, B, Tail, Nose) ? 1 : 0;
		}
		for (const FEntityAnchor& Anchor : Definition.Anchors)
		{
			if (TraversalForRole(Anchor.Role) == ETraversalClass::Aircraft)
			{
				continue;
			}
			const FVector2D Spur = NearestOnLoop(Definition.ServiceLoop, Anchor.LocalPosition);
			Crossings += RoadGeom::SegmentsCross(Anchor.LocalPosition, Spur, Tail, Nose) ? 1 : 0;
		}
		return Crossings;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FServiceLoopEnclosesTheStandTest,
	"Airside.Entities.ServiceLoopEnclosesTheStand",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FServiceLoopEnclosesTheStandTest::RunTest(const FString& Parameters)
{
	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
	if (!TestNotNull(TEXT("a stand definition"), Stand)) { return false; }
	if (!TestNotNull(TEXT("with a design aircraft"), Stand->DesignAircraft.Get())) { return false; }

	// FOUR POINTS, NOT FIVE. The loop is closed implicitly and the array does not repeat
	// its first point - storing the repeat would be a value that must agree with another
	// value in the same array.
	if (!TestEqual(TEXT("the loop is a four-sided box"), Stand->ServiceLoop.Num(), 4))
	{
		return false;
	}

	const FEntityFootprint& Footprint = Stand->DesignAircraft->Footprint;
	const double HalfSpan = Footprint.Wingspan * 0.5;

	// EVERY WINGTIP CORNER. PointInPolygon, not a bounds comparison, because what the graph
	// will drive round is the POLYGON, and a bounds test would still pass if the builder
	// emitted the corners in an order that folds the box through itself.
	const FVector2D Corners[4] = {
		FVector2D(Footprint.NoseX,  HalfSpan), FVector2D(Footprint.NoseX, -HalfSpan),
		FVector2D(Footprint.TailX,  HalfSpan), FVector2D(Footprint.TailX, -HalfSpan) };
	for (const FVector2D& Corner : Corners)
	{
		TestTrue(TEXT("the aircraft's footprint is inside the loop"),
			RoadGeom::PointInPolygon(Stand->ServiceLoop, Corner));
	}

	// EVERY ANCHOR, which is why the box is a UNION and not the footprint alone: TugStand
	// waits at +1400, nine metres ahead of a nose that stops at +507.
	for (const FEntityAnchor& Anchor : Stand->Anchors)
	{
		TestTrue(TEXT("every anchor is inside the loop"),
			RoadGeom::PointInPolygon(Stand->ServiceLoop, Anchor.LocalPosition));
	}

	// THE CLEARANCE ITSELF, so a loop that merely touched the wingtips would fail. The
	// figures are the design's section 4, arithmetic from the A320 and the anchors.
	double MinX = TNumericLimits<double>::Max(), MaxX = -TNumericLimits<double>::Max();
	double MinY = TNumericLimits<double>::Max(), MaxY = -TNumericLimits<double>::Max();
	for (const FVector2D& Point : Stand->ServiceLoop)
	{
		MinX = FMath::Min(MinX, Point.X); MaxX = FMath::Max(MaxX, Point.X);
		MinY = FMath::Min(MinY, Point.Y); MaxY = FMath::Max(MaxY, Point.Y);
	}
	TestEqual(TEXT("3 m behind the tail"), MinX, -3550.0);
	TestEqual(TEXT("3 m ahead of the tug's box"), MaxX, 1700.0);
	TestEqual(TEXT("3 m outboard of the port wingtip"), MinY, -2090.0);
	TestEqual(TEXT("3 m outboard of the starboard wingtip"), MaxY, 2090.0);

	// A DEPOT HAS NONE, and empty is a supported state rather than an unfinished one.
	UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
	if (TestNotNull(TEXT("a depot definition"), Depot))
	{
		TestEqual(TEXT("a depot has no service loop - one pose, no anchors"),
			Depot->ServiceLoop.Num(), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FServiceLoopClearsTheAircraftTest,
	"Airside.Entities.ServiceLoopClearsTheAircraft",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FServiceLoopClearsTheAircraftTest::RunTest(const FString& Parameters)
{
	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
	if (!TestNotNull(TEXT("a stand definition"), Stand)) { return false; }

	TestEqual(TEXT("no loop segment and no spur crosses the fuselage lengthwise"),
		CountCentrelineCrossings(*Stand), 0);

	// THE VACUITY CHECK. A counter that could never report anything would pass the
	// assertion above on a stand whose lane ran straight through the aeroplane, so the same
	// counter is pointed at a layout that genuinely does cross.
	//
	// A ONE-SIDED loop, hand-built: the anchors are unchanged, but the lane runs only up the
	// starboard side, so the port-side GPU's spur has nowhere to go but across the fuselage.
	{
		UEntityDefinition* Bad = NewObject<UEntityDefinition>(GetTransientPackage());
		UEntityDefinition::BuildCodeCStand(Bad, Stand->DesignAircraft);
		Bad->ServiceLoop = {
			FVector2D(-3550.0,  500.0), FVector2D( 1700.0,  500.0),
			FVector2D( 1700.0, 2090.0), FVector2D(-3550.0, 2090.0) };

		TestTrue(TEXT("a lane only up the starboard side makes the port anchors cross"),
			CountCentrelineCrossings(*Bad) > 0);
	}
	return true;
}

#endif
```

- [ ] **Step 2: Run the tests to verify they fail**

Build (twice — new test file), then:
`./Tools/Run-AirsideTests.ps1 -Filter Airside.Entities`
Expected: compile error on `Stand->ServiceLoop` and on the two-argument `BuildCodeCStand`.
That IS the failure; the field does not exist yet.

- [ ] **Step 3: Add the field**

In `EntityDefinition.h`, immediately after the `Anchors` property (a doc comment touches its
declaration — put the whole block together):

```cpp
	/**
	 * A closed, INVISIBLE vehicle lane enclosing the parked aircraft and every anchor, in
	 * the entity's own local space. Empty means none.
	 *
	 * CLOSED IMPLICITLY: the last point joins the first, and the array does NOT repeat it.
	 * Storing the repeat would be a value that must agree with another value in the same
	 * array, which is exactly the drift FResolvedAnchor exists to remove.
	 *
	 * WHY A LOOP AT ALL, rather than joining each anchor straight to the road by proximity:
	 * the anchors sit AROUND the aeroplane - the hydrant pit under the starboard wing, fixed
	 * ground power off the port bow - so a straight spur from a road on one side to a box on
	 * the other crosses 37 m of fuselage, and nothing in the guideline graph has ever had an
	 * opinion about geometry crossing an aeroplane. Anchors spur to this; roads join this.
	 *
	 * COMPUTED by the builder that lays the anchors, never authored beside them. Four
	 * hand-typed corners would be a third authored thing that must agree with the aircraft
	 * AND with the anchors, and would drift from both. Deriving it at rebuild time was also
	 * rejected: that is a runtime algorithm's opinion with no override, and a second
	 * evaluator of the same geometry.
	 *
	 * INVISIBLE, and that is a decision rather than an omission: no marking builder, no
	 * material, no mesh. It exists only as guideline nodes and edges, and shows in the G
	 * overlay because everything in the graph does. It is a routing lane, not paint.
	 */
	UPROPERTY(EditAnywhere) TArray<FVector2D> ServiceLoop;
```

Change the `BuildCodeCStand` declaration, keeping its existing comment and appending to it:

```cpp
	/**
	 * Fill Definition with the Code C contact stand layout, replacing whatever it held.
	 *
	 * Shared by MakeStandTransient and the commandlet that authors the DA_Stand_CodeC data
	 * asset, so the tested layout and the shipped one are the same numbers rather than two
	 * transcriptions of them.
	 *
	 * TAKES THE DESIGN AIRCRAFT, and sets it. Both callers used to set DesignAircraft
	 * afterwards, which was harmless only for as long as nothing in the layout depended on
	 * it - and ServiceLoop does: a stand's geometry is laid out AROUND the aircraft it is
	 * sized for, so the builder has to know which one that is. A null aircraft is allowed
	 * and gives a loop round the anchors alone, which is what a definition with no envelope
	 * to clear actually wants.
	 */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	static void BuildCodeCStand(UEntityDefinition* Definition, UAircraftType* DesignAircraft);
```

- [ ] **Step 4: Compute the loop**

In `EntityDefinition.cpp`, change `MakeStandTransient` to build the aircraft first:

```cpp
UEntityDefinition* UEntityDefinition::MakeStandTransient()
{
	// A stand with no design aircraft draws no envelope, offers no service positions and -
	// since the service loop is laid out around the aeroplane - gets no lane either, which
	// in a test reads as "the feature is broken" rather than "the fixture is thin". Built
	// BEFORE the layout now, because the layout is measured from it.
	UAircraftType* A320 = NewObject<UAircraftType>(GetTransientPackage());
	UAircraftType::BuildA320(A320);

	UEntityDefinition* Definition = NewObject<UEntityDefinition>(GetTransientPackage());
	BuildCodeCStand(Definition, A320);

	return Definition;
}
```

In `BuildCodeCStand`, take the new parameter, assign it near the top:

```cpp
void UEntityDefinition::BuildCodeCStand(UEntityDefinition* Definition, UAircraftType* DesignAircraft)
{
	if (Definition == nullptr)
	{
		return;
	}

	Definition->Anchors.Reset();
	Definition->DesignAircraft = DesignAircraft;
```

and append, after `AvailableServices` is set (keep every existing comment):

```cpp
	// THE SERVICE LOOP: the closed lane the ground vehicles use, derived from what it has to
	// enclose rather than typed. See UEntityDefinition::ServiceLoop for why it is computed
	// here and not authored.
	//
	// The clearance is a CONSTANT, not a UPROPERTY, and deliberately: it is a fact about how
	// this stand type is laid out, decided at authoring time beside the anchors. A
	// level-authored version would be a knob that silently reshaped stands already placed.
	constexpr double LoopClearance = 300.0;

	// THE UNION of the design aircraft's footprint AND every anchor. A footprint-only box
	// leaves TugStand at +1400 outside it, because the tug waits nine metres ahead of the
	// nose - and an anchor outside the lane is an anchor whose spur has to cross it.
	double MinX = TNumericLimits<double>::Max(), MaxX = -TNumericLimits<double>::Max();
	double MinY = TNumericLimits<double>::Max(), MaxY = -TNumericLimits<double>::Max();
	auto Cover = [&MinX, &MaxX, &MinY, &MaxY](const FVector2D& Point)
	{
		MinX = FMath::Min(MinX, Point.X); MaxX = FMath::Max(MaxX, Point.X);
		MinY = FMath::Min(MinY, Point.Y); MaxY = FMath::Max(MaxY, Point.Y);
	};

	if (DesignAircraft != nullptr && DesignAircraft->Footprint.IsSet())
	{
		const FEntityFootprint& Footprint = DesignAircraft->Footprint;
		const double HalfSpan = Footprint.Wingspan * 0.5;
		Cover(FVector2D(Footprint.NoseX,  HalfSpan));
		Cover(FVector2D(Footprint.NoseX, -HalfSpan));
		Cover(FVector2D(Footprint.TailX,  HalfSpan));
		Cover(FVector2D(Footprint.TailX, -HalfSpan));
	}
	for (const FEntityAnchor& Anchor : Definition->Anchors)
	{
		Cover(Anchor.LocalPosition);
	}

	Definition->ServiceLoop.Reset();
	if (MinX <= MaxX)
	{
		MinX -= LoopClearance; MaxX += LoopClearance;
		MinY -= LoopClearance; MaxY += LoopClearance;

		// FOUR-SIDED AND CLOSED. An open U round the nose was rejected: a closed loop gives
		// entry from any side, including from behind the tail, and costs nothing because the
		// lane is invisible. Counter-clockwise, and the first point is never repeated.
		Definition->ServiceLoop = {
			FVector2D(MinX, MinY), FVector2D(MaxX, MinY),
			FVector2D(MaxX, MaxY), FVector2D(MinX, MaxY) };
	}
}
```

Add `#include "Solve/RoadGeom.h"`? No — none of the above needs it. Do add nothing new;
`Entities/AircraftType.h` is already reachable through `EntityDefinition.h`.

Also add to `BuildFuelDepot`, beside the existing "NO ANCHORS" comment:

```cpp
	// AND NO SERVICE LOOP. A loop encloses an aeroplane and the boxes around it; a depot has
	// one pose and nothing parked at it, and its pose IS its road connection (see the header).
	// Reset rather than left alone, for the same reason Anchors is.
	Definition->ServiceLoop.Reset();
```

- [ ] **Step 5: Update the other caller**

In `Tools/Python/build_stand_asset.py`, `build_stand`:

```python
    unreal.EntityDefinition.build_code_c_stand(stand, design_aircraft)
```

and delete the now-redundant `stand.set_editor_property("design_aircraft", design_aircraft)`
line, replacing it with nothing (the builder sets it). Then add, after the fixtures log:

```python
    loop = stand.get_editor_property("service_loop")
    unreal.log("MARKER: DA_Stand_CodeC service loop, %d corner(s)" % len(loop))
    for corner in loop:
        unreal.log("MARKER:   (%.0f, %.0f)" % (corner.x, corner.y))
```

Also update the module docstring's four-asset list line for the stand to read
`DA_Stand_CodeC  what the ground PROVIDES, its fixed plant, and the lane round it`.

- [ ] **Step 6: Run the tests to verify they pass**

Build (twice), then `./Tools/Run-AirsideTests.ps1 -Filter Airside.Entities`.
Expected: `Airside.Entities.ServiceLoopEnclosesTheStand` and
`Airside.Entities.ServiceLoopClearsTheAircraft` both pass; the pre-existing
`Airside.Entities.*` tests still pass. Read the `N test(s) run, N failed, N crashed` line.

- [ ] **Step 7: Commit**

```bash
git add -A
git commit -m "feat(entities): a stand carries a service loop, computed from the aeroplane it is sized for"
```

---

### Task 2: Telling the loop apart from the road

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Model/RoadGuideline.h`
- Modify: `Plugins/Airside/Source/Airside/Public/Model/RoadNetwork.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Model/RoadNetwork.cpp`
- Modify: `Plugins/Airside/Source/AirsideTests/Private/RoadEntityTest.cpp`

**Interfaces:**
- Consumes: `FEntityInstanceId` (`Model/RoadHandles.h`, already included by RoadGuideline.h).
- Produces:
  - `UPROPERTY() FEntityInstanceId FGuidelineEdge::ServiceLoopOwner;`
  - `bool URoadNetwork::IsServiceNodeConnected(FGuidelineNodeId Node) const;`

- [ ] **Step 1: Write the failing test**

Append to `Plugins/Airside/Source/AirsideTests/Private/RoadEntityTest.cpp`, before the
final `#endif`:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FServiceNodeConnectedTest,
	"Airside.Model.ServiceNodeConnected",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FServiceNodeConnectedTest::RunTest(const FString& Parameters)
{
	// THE QUESTION THIS ANSWERS, and why the incident count stopped answering it: once a
	// stand carries a service loop, its hydrant ALWAYS has a spur on it, so "has an edge"
	// is true for a stand in the middle of a field. What the player needs to know is
	// whether the lane reaches a road, which is a walk and not a count.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());

	FEntityInstanceId Owner;
	Owner.Index = 7;
	Owner.Generation = 1;

	const FGuidelineNodeId Anchor = Net->AddGuidelineNode(FVector2D(0.0, 0.0), /*bDerived=*/false);
	const FGuidelineNodeId LoopA  = Net->AddGuidelineNode(FVector2D(1000.0, 0.0));
	const FGuidelineNodeId LoopB  = Net->AddGuidelineNode(FVector2D(1000.0, 1000.0));
	const FGuidelineNodeId Road   = Net->AddGuidelineNode(FVector2D(9000.0, 1000.0));

	auto Join = [Net](FGuidelineNodeId A, FGuidelineNodeId B, FEntityInstanceId Owned)
	{
		FGuidelineEdge Edge;
		Edge.A = A;
		Edge.B = B;
		Edge.Control = (Net->GetGuidelineNode(A)->Position + Net->GetGuidelineNode(B)->Position) * 0.5;
		Edge.AllowedTraffic = FTrafficMask::Only(ETraversalClass::GroundVehicle);
		Edge.Direction = EGuidelineDir::Bidirectional;
		Edge.bDerived = true;
		Edge.ServiceLoopOwner = Owned;
		Net->AddGuidelineEdge(MoveTemp(Edge));
	};

	// The spur and one side of the lane, both owned. Nothing else.
	Join(Anchor, LoopA, Owner);
	Join(LoopA, LoopB, Owner);

	TestFalse(TEXT("an anchor whose only line is its own lane is not on a road"),
		Net->IsServiceNodeConnected(Anchor));

	// Now the lane reaches something that is not the lane.
	Join(LoopB, Road, FEntityInstanceId());

	TestTrue(TEXT("once the lane joins a road, the anchor is"),
		Net->IsServiceNodeConnected(Anchor));

	// A NODE WITH NO LANE AT ALL - a depot's pose - is answered by its own lead-in, without
	// the walk having anything to walk. This is the case the fuel service asks about most.
	const FGuidelineNodeId Pose = Net->AddGuidelineNode(FVector2D(-9000.0, 0.0), false);
	TestFalse(TEXT("an island is not on a road"), Net->IsServiceNodeConnected(Pose));
	Join(Pose, Road, FEntityInstanceId());
	TestTrue(TEXT("a bare lead-in is enough on its own"), Net->IsServiceNodeConnected(Pose));
	return true;
}
```

- [ ] **Step 2: Run it to verify it fails**

Build twice (the file gains a test, but the file already exists — one build is enough here;
build twice only when the .cpp is new). Then
`./Tools/Run-AirsideTests.ps1 -Filter Airside.Model`
Expected: compile error — `ServiceLoopOwner` and `IsServiceNodeConnected` do not exist.

- [ ] **Step 3: Add the field and the query**

In `RoadGuideline.h`, inside `FGuidelineEdge`, after `bDerived`:

```cpp
	/**
	 * The entity whose SERVICE LOOP or anchor spur this edge is. Unset for everything else,
	 * which is almost every edge.
	 *
	 * Provenance, exactly as DerivedFrom is for a road's own guidelines, and it is needed
	 * for the same reason turned inside out: a stand's lane is itself a vehicle guideline,
	 * so a link search that did not know which edges were the searcher's OWN would join a
	 * loop to itself, report every stand connected, and route no truck anywhere.
	 *
	 * The LINK from a loop to a road deliberately does NOT carry this. It is a lead-in like
	 * any other, and leaving it unowned is what lets IsServiceNodeConnected tell a lane that
	 * reaches a road from one that only reaches itself.
	 */
	UPROPERTY() FEntityInstanceId ServiceLoopOwner;
```

In `RoadNetwork.h`, in the guideline section next to `GetOutgoingGuidelines`:

```cpp
	/**
	 * True when Node has line on it that leads OFF the service geometry it belongs to.
	 *
	 * "Does this anchor have an edge" used to be the same question and stopped being it the
	 * moment stands grew service loops: a hydrant is always incident to its own spur, so the
	 * count is true for a stand in the middle of a field. This walks the edges marked
	 * FGuidelineEdge::ServiceLoopOwner - the lane and its spurs - and reports whether the
	 * component it reaches touches anything that is not one of them.
	 *
	 * A node with no service geometry at all is answered by its own first edge, so a depot's
	 * pose and a hand-drawn node both give the obvious answer without a walk.
	 */
	bool IsServiceNodeConnected(FGuidelineNodeId Node) const;
```

In `RoadNetwork.cpp`, next to `GetOutgoingGuidelines`:

```cpp
bool URoadNetwork::IsServiceNodeConnected(FGuidelineNodeId Node) const
{
	// Breadth-first over OWNED edges only. The frontier is tiny - a loop is four sides and
	// five spurs - so a queue of node ids costs nothing and terminates on the visited set.
	TSet<FGuidelineNodeId> Seen;
	TArray<FGuidelineNodeId> Frontier;
	Seen.Add(Node);
	Frontier.Add(Node);

	while (Frontier.Num() > 0)
	{
		const FGuidelineNodeId At = Frontier.Pop();
		const FGuidelineNode* Found = GetGuidelineNode(At);
		if (Found == nullptr)
		{
			continue;
		}

		for (const FGuidelineEdgeId& EdgeId : Found->Incident)
		{
			const FGuidelineEdge* Edge = GetGuidelineEdge(EdgeId);
			if (Edge == nullptr)
			{
				continue;
			}

			if (!Edge->ServiceLoopOwner.IsSet())
			{
				// Something that is not this stand's own lane. That is the whole question.
				return true;
			}

			const FGuidelineNodeId Other = Edge->A == At ? Edge->B : Edge->A;
			if (!Seen.Contains(Other))
			{
				Seen.Add(Other);
				Frontier.Add(Other);
			}
		}
	}
	return false;
}
```

- [ ] **Step 4: Run it to verify it passes**

Build, then `./Tools/Run-AirsideTests.ps1 -Filter Airside.Model`.
Expected: `Airside.Model.ServiceNodeConnected` passes; the rest of `Airside.Model` unchanged.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "feat(model): a guideline edge says whose service loop it is; IsServiceNodeConnected walks it"
```

---

### Task 3: Nearest point on a sampled polyline

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Solve/GuidelineGeom.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Solve/GuidelineGeom.cpp`
- Modify: `Plugins/Airside/Source/AirsideTests/Private/AnchorLinkTest.cpp` (the
  `Airside.Solve.GuidelineGeom` test lives there)

**Interfaces:**
- Consumes: `RoadGeom::ClosestPointOnSegment` — but `Solve/GuidelineGeom.cpp` may include
  `Solve/RoadGeom.h`, both being `Solve/`.
- Produces:
```cpp
	AIRSIDE_API double NearestOnPolyline(const TArray<FVector2D>& Points,
		const FVector2D& Query, int32& OutIndex, double& OutFraction);

	AIRSIDE_API double NearestBetweenPolylines(
		const TArray<FVector2D>& A, const TArray<FVector2D>& B,
		int32& OutAIndex, double& OutAFraction, int32& OutBIndex, double& OutBFraction);
```

- [ ] **Step 1: Write the failing test**

In `AnchorLinkTest.cpp`, inside `FGuidelineGeomTest::RunTest`, append before its `return true;`:

```cpp
	// NEAREST POINT ON A SAMPLED POLYLINE. The proximity half of the link rule measures
	// against the SAMPLES, like everything else that consumes this graph - see the header's
	// single-sampling note. A second evaluator here would let a link land somewhere the
	// overlay does not draw.
	{
		TArray<FVector2D> Line;
		Line.Add(FVector2D(0.0, 0.0));
		Line.Add(FVector2D(1000.0, 0.0));
		Line.Add(FVector2D(1000.0, 1000.0));

		int32 Index = -1;
		double Fraction = -1.0;
		const double Distance =
			GuidelineGeom::NearestOnPolyline(Line, FVector2D(400.0, 300.0), Index, Fraction);

		TestEqual(TEXT("the foot of the perpendicular, not an endpoint"), Distance, 300.0);
		TestEqual(TEXT("on the first span"), Index, 0);
		TestTrue(TEXT("four tenths along it"), FMath::IsNearlyEqual(Fraction, 0.4, 1e-9));

		// PAST THE END. Clamped to the segment, so a query beyond the last vertex is
		// answered by that vertex rather than by the infinite line.
		GuidelineGeom::NearestOnPolyline(Line, FVector2D(3000.0, 1000.0), Index, Fraction);
		TestEqual(TEXT("clamped to the far end's span"), Index, 1);
		TestTrue(TEXT("at its end"), FMath::IsNearlyEqual(Fraction, 1.0, 1e-9));
	}

	// CLOSEST APPROACH BETWEEN TWO POLYLINES. Both directions are tried, because for two
	// PARALLEL segments the closest pair contains no endpoint of the longer one - and a
	// service road drawn alongside a row of stands is exactly that case.
	{
		TArray<FVector2D> Loop;
		Loop.Add(FVector2D(-1000.0, 0.0));
		Loop.Add(FVector2D(1000.0, 0.0));

		TArray<FVector2D> Road;
		Road.Add(FVector2D(-9000.0, -500.0));
		Road.Add(FVector2D(9000.0, -500.0));

		int32 LoopIndex = -1, RoadIndex = -1;
		double LoopFraction = -1.0, RoadFraction = -1.0;
		const double Distance = GuidelineGeom::NearestBetweenPolylines(
			Loop, Road, LoopIndex, LoopFraction, RoadIndex, RoadFraction);

		TestEqual(TEXT("500 apart, whichever end you measure from"), Distance, 500.0);
		TestEqual(TEXT("on the loop's only span"), LoopIndex, 0);
		TestEqual(TEXT("and the road's only span"), RoadIndex, 0);
	}
```

- [ ] **Step 2: Run it to verify it fails**

`./Tools/Run-AirsideTests.ps1 -Filter Airside.Solve` — compile error, the functions do not
exist.

- [ ] **Step 3: Implement**

In `GuidelineGeom.h`, after `PolylineLength`:

```cpp
	/**
	 * Distance from Query to the nearest point on an already-sampled polyline, with the
	 * span index and the 0..1 fraction along that span written out.
	 *
	 * Against the SAMPLES, not against the curve, and deliberately: the samples are what the
	 * search costs, the overlay draws and a follower walks, and a link measured against a
	 * closed-form curve would be a second evaluator of the same geometry. Feed the two
	 * outputs to ParamAtSample to get the curve parameter a split needs.
	 *
	 * Returns a huge distance and leaves the outputs at 0 for a polyline with fewer than two
	 * points, which has no nearest point to report.
	 */
	AIRSIDE_API double NearestOnPolyline(const TArray<FVector2D>& Points,
		const FVector2D& Query, int32& OutIndex, double& OutFraction);

	/**
	 * Closest approach between two already-sampled polylines, with the span index and
	 * fraction of the closest point on EACH.
	 *
	 * BOTH DIRECTIONS ARE TRIED - every vertex of A against B, and every vertex of B against
	 * A - because the closest pair of points on two segments is an endpoint of one of them
	 * only when they are not parallel. A service road drawn ALONGSIDE a row of stands is
	 * parallel to the lane it must join, and a one-directional search measures the corner
	 * rather than the side.
	 */
	AIRSIDE_API double NearestBetweenPolylines(
		const TArray<FVector2D>& A, const TArray<FVector2D>& B,
		int32& OutAIndex, double& OutAFraction, int32& OutBIndex, double& OutBFraction);
```

In `GuidelineGeom.cpp` (add `#include "Solve/RoadGeom.h"` at the top):

```cpp
double GuidelineGeom::NearestOnPolyline(const TArray<FVector2D>& Points,
	const FVector2D& Query, int32& OutIndex, double& OutFraction)
{
	OutIndex = 0;
	OutFraction = 0.0;
	if (Points.Num() < 2)
	{
		return TNumericLimits<double>::Max();
	}

	double Best = TNumericLimits<double>::Max();
	for (int32 At = 1; At < Points.Num(); ++At)
	{
		const double T = RoadGeom::ClosestPointOnSegment(Points[At - 1], Points[At], Query);
		const double Distance = FVector2D::Distance(FMath::Lerp(Points[At - 1], Points[At], T), Query);
		if (Distance < Best)
		{
			Best = Distance;
			OutIndex = At - 1;
			OutFraction = T;
		}
	}
	return Best;
}

double GuidelineGeom::NearestBetweenPolylines(
	const TArray<FVector2D>& A, const TArray<FVector2D>& B,
	int32& OutAIndex, double& OutAFraction, int32& OutBIndex, double& OutBFraction)
{
	OutAIndex = 0; OutAFraction = 0.0;
	OutBIndex = 0; OutBFraction = 0.0;
	if (A.Num() < 2 || B.Num() < 2)
	{
		return TNumericLimits<double>::Max();
	}

	double Best = TNumericLimits<double>::Max();

	// The span a VERTEX sits on, expressed the way the outputs are: the last vertex is the
	// end of the last span, never the start of a span that does not exist.
	auto SpanOf = [](int32 Vertex, int32 Count, int32& OutSpan, double& OutFractionAt)
	{
		OutSpan = FMath::Clamp(Vertex, 0, Count - 2);
		OutFractionAt = Vertex >= Count - 1 ? 1.0 : 0.0;
	};

	for (int32 At = 0; At < A.Num(); ++At)
	{
		int32 Index = 0;
		double Fraction = 0.0;
		const double Distance = NearestOnPolyline(B, A[At], Index, Fraction);
		if (Distance < Best)
		{
			Best = Distance;
			SpanOf(At, A.Num(), OutAIndex, OutAFraction);
			OutBIndex = Index;
			OutBFraction = Fraction;
		}
	}

	for (int32 At = 0; At < B.Num(); ++At)
	{
		int32 Index = 0;
		double Fraction = 0.0;
		const double Distance = NearestOnPolyline(A, B[At], Index, Fraction);
		if (Distance < Best)
		{
			Best = Distance;
			OutAIndex = Index;
			OutAFraction = Fraction;
			SpanOf(At, B.Num(), OutBIndex, OutBFraction);
		}
	}
	return Best;
}
```

- [ ] **Step 4: Run it to verify it passes**

`./Tools/Run-AirsideTests.ps1 -Filter Airside.Solve` — `Airside.Solve.GuidelineGeom` passes.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "feat(solve): nearest point on a sampled guideline, and closest approach between two"
```

---

### Task 4: Deriving the loop and its spurs into the graph

**Files:**
- Create: `Plugins/Airside/Source/Airside/Public/Build/ServiceLoopBuild.h`
- Create: `Plugins/Airside/Source/Airside/Private/Build/ServiceLoopBuild.cpp`
- Create: `Plugins/Airside/Source/AirsideTests/Private/ServiceLinkTest.cpp`

**Interfaces:**
- Consumes: `UEntityDefinition::ServiceLoop`, `FGuidelineEdge::ServiceLoopOwner`,
  `GuidelineGeom::NearestOnPolyline`, `TraversalForRole`.
- Produces:
```cpp
struct AIRSIDE_API FServiceLoopBuild
{
	/** Width given to a lane nobody paints, uu. */
	static constexpr double LaneWidth = 400.0;

	struct FResult
	{
		/** Every node of every loop and spur now in the graph. */
		TSet<FGuidelineNodeId> Nodes;

		/** Per entity, the edges of its lane - the search's source, and never its target. */
		TMap<FEntityInstanceId, TArray<FGuidelineEdgeId>> Lanes;

		int32 LoopsBuilt = 0;
		int32 SpursBuilt = 0;
	};

	static FResult Build(URoadNetwork& Network);
};
```

- [ ] **Step 1: Write the failing test**

Create `Plugins/Airside/Source/AirsideTests/Private/ServiceLinkTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Build/AnchorLink.h"
#include "Build/ServiceLoopBuild.h"
#include "Entities/AircraftType.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadTraffic.h"
#include "Model/RouteSearch.h"
#include "Solve/GuidelineGeom.h"
#include "Solve/RoadGeom.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace ServiceLinkFixture
{
	/** A straight guideline admitting exactly one class, plus Emergency as derived ones do. */
	FGuidelineNodeId Lay(URoadNetwork& Net, const FVector2D& From, const FVector2D& To,
		ETraversalClass Class, FGuidelineNodeId& OutFar)
	{
		const FGuidelineNodeId Near = Net.AddGuidelineNode(From);
		OutFar = Net.AddGuidelineNode(To);

		FGuidelineEdge Edge;
		Edge.A = Near;
		Edge.B = OutFar;
		Edge.Control = (From + To) * 0.5;
		Edge.AllowedTraffic = FTrafficMask::Only(Class);
		Edge.AllowedTraffic.Add(ETraversalClass::Emergency);
		Edge.Direction = EGuidelineDir::Bidirectional;
		Edge.Width = 600.0;
		Edge.bDerived = true;
		Net.AddGuidelineEdge(MoveTemp(Edge));
		return Near;
	}

	FEntityInstanceId PlaceStand(URoadNetwork& Net, UEntityDefinition& Stand,
		const FVector2D& At, double Heading)
	{
		return Net.PlaceEntity(&Stand, Stand.Anchors, At, Heading,
			Stand.DesignAircraft != nullptr ? Stand.DesignAircraft->Footprint.Wingspan : 0.0,
			Stand.PoseRole, Stand.Trucks);
	}

	/** The node a named anchor resolved to, or an unset handle. */
	FGuidelineNodeId AnchorNode(const URoadNetwork& Net, FEntityInstanceId Entity, const TCHAR* Id)
	{
		const FResolvedAnchor* Found = Net.FindResolvedAnchor(Entity, FName(Id));
		return Found != nullptr ? Found->Node : FGuidelineNodeId();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FServiceLoopReachesTheGraphTest,
	"Airside.Build.ServiceLoopReachesTheGraph",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FServiceLoopReachesTheGraphTest::RunTest(const FString& Parameters)
{
	using namespace ServiceLinkFixture;

	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
	const FEntityInstanceId Placed = PlaceStand(*Net, *Stand, FVector2D::ZeroVector, 0.0);

	const FServiceLoopBuild::FResult First = FServiceLoopBuild::Build(*Net);
	TestEqual(TEXT("one stand, one lane"), First.LoopsBuilt, 1);
	TestEqual(TEXT("a spur for every service anchor"), First.SpursBuilt, 5);

	// EVERY SERVICE ANCHOR NOW HAS LINE ON IT, and the AIRCRAFT pose still does not - the
	// lane is for vehicles, and the stop mark's lead-in is not this builder's business.
	for (const FResolvedAnchor& Anchor : Net->GetEntity(Placed)->ResolvedAnchors)
	{
		const FGuidelineNode* Node = Net->GetGuidelineNode(Anchor.Node);
		if (TestNotNull(TEXT("the anchor resolves"), Node))
		{
			TestTrue(TEXT("and is spurred to the lane"), Node->Incident.Num() > 0);
		}
	}
	TestEqual(TEXT("the aircraft stop mark is untouched"),
		Net->GetGuidelineNode(Net->GetEntity(Placed)->PoseNode)->Incident.Num(), 0);

	// BUT IT IS NOT ON A ROAD. The lane is a vehicle guideline, so a check that only counted
	// edges would call this stand connected and no truck would ever route to it.
	TestFalse(TEXT("a lane with no road near it is not a connection"),
		Net->IsServiceNodeConnected(AnchorNode(*Net, Placed, TEXT("HydrantPit"))));

	// A SECOND PASS ADDS NOTHING. The graph is rebuilt on every road edit and this runs each
	// time; a builder that could not see its own previous output would stack a lane per pass.
	const FServiceLoopBuild::FResult Second = FServiceLoopBuild::Build(*Net);
	TestEqual(TEXT("a second pass builds no second lane"), Second.LoopsBuilt, 0);
	TestEqual(TEXT("and no second spur"), Second.SpursBuilt, 0);

	// A DEPOT HAS NO LANE, so nothing is built for it at all.
	{
		URoadNetwork* Bare = NewObject<URoadNetwork>(GetTransientPackage());
		UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
		Bare->PlaceEntity(Depot, Depot->Anchors, FVector2D::ZeroVector, 0.0, 0.0,
			Depot->PoseRole, Depot->Trucks);
		TestEqual(TEXT("a depot gets no lane"), FServiceLoopBuild::Build(*Bare).LoopsBuilt, 0);
	}
	return true;
}

#endif
```

- [ ] **Step 2: Run it to verify it fails**

Build twice (new .cpp), then `./Tools/Run-AirsideTests.ps1 -Filter Airside.Build`.
Expected: compile error — `Build/ServiceLoopBuild.h` does not exist.

- [ ] **Step 3: Write the header**

Create `Plugins/Airside/Source/Airside/Public/Build/ServiceLoopBuild.h`:

```cpp
#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"

class URoadNetwork;

/**
 * Puts each placed entity's SERVICE LOOP - and a spur from every service anchor to it - into
 * the guideline graph.
 *
 * WHY A LANE AT ALL is UEntityDefinition::ServiceLoop's business; this is only where it
 * reaches the graph. What matters here is that all of it is DERIVED, so it is swept and
 * remade by the ordinary rebuild, follows the stand when the stand moves, and goes when the
 * stand goes - the same lifecycle as any other derived edge, and the reason no instance
 * stores a loop and no level needs migrating.
 *
 * Runs BEFORE FAnchorLink, which then joins each lane to a road: the lane must exist before
 * anything can link it, and a service anchor spurred to the lane is already joined by the
 * time FAnchorLink asks, so it is skipped there rather than being cast at a road across the
 * aeroplane.
 *
 * IDEMPOTENT, because the two callers cannot promise the sweep ran in between (a test may
 * call FAnchorLink twice; the presenter always rebuilds first). An entity that already owns
 * live lane edges is left alone rather than given a second lane - detected from
 * FGuidelineEdge::ServiceLoopOwner, which is the same mark the link search uses to avoid
 * joining a lane to itself.
 */
struct AIRSIDE_API FServiceLoopBuild
{
	/**
	 * Physical width given to a lane nobody paints, uu.
	 *
	 * A number rather than a profile lookup because there is no surface here to read one
	 * from: the lane is invisible by design, and Width on a guideline drives marking geometry
	 * and clearance, neither of which this has. Four metres is a service road's lane, which
	 * is what a lane round a stand is.
	 */
	static constexpr double LaneWidth = 400.0;

	struct FResult
	{
		/**
		 * Every node of every lane and spur now in the graph.
		 *
		 * FAnchorLink excludes these as link TARGETS, the same way it excludes anchor nodes:
		 * a lane is a vehicle guideline, so without this a lane would join itself and a stand
		 * would read as connected while reaching no road.
		 */
		TSet<FGuidelineNodeId> Nodes;

		/** Per entity, the edges of its lane - what the link search measures FROM. */
		TMap<FEntityInstanceId, TArray<FGuidelineEdgeId>> Lanes;

		int32 LoopsBuilt = 0;
		int32 SpursBuilt = 0;
	};

	static FResult Build(URoadNetwork& Network);
};
```

- [ ] **Step 4: Write the implementation**

Create `Plugins/Airside/Source/Airside/Private/Build/ServiceLoopBuild.cpp`:

```cpp
#include "Build/ServiceLoopBuild.h"

#include "AirsideLog.h"
#include "Entities/EntityDefinition.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Solve/GuidelineGeom.h"

namespace
{
	/** Within this of an endpoint, join the endpoint rather than splitting off a stub.
	 *  The same figure FAnchorLink welds at, and for the same reason. */
	constexpr double LaneWeldTolerance = 10.0;

	/** A lane edge, or a spur. Both carry the owner; neither is ever a link target. */
	FGuidelineEdge MakeServiceEdge(FGuidelineNodeId A, FGuidelineNodeId B,
		const FVector2D& PositionA, const FVector2D& PositionB, FEntityInstanceId Owner)
	{
		FGuidelineEdge Edge;
		Edge.A = A;
		Edge.B = B;

		// Straight, spelled the way the builder spells it: the control ON the midpoint, which
		// is what GuidelineGeom::IsStraight tests for and what lets Sample short-circuit.
		Edge.Control = (PositionA + PositionB) * 0.5;

		// GroundVehicle, plus Emergency as every derived guideline carries. Not Aircraft: an
		// aeroplane routed round the lane would be driving round itself.
		Edge.AllowedTraffic = FTrafficMask::Only(ETraversalClass::GroundVehicle);
		Edge.AllowedTraffic.Add(ETraversalClass::Emergency);
		Edge.Direction = EGuidelineDir::Bidirectional;
		Edge.Width = FServiceLoopBuild::LaneWidth;

		// 0 is UNLIMITED (FProfileGuideline::MaxWingspan). A span limit on a line no wing
		// uses could never bind, and the class has already refused aircraft.
		Edge.MaxWingspan = 0.0;
		Edge.bDerived = true;
		Edge.ServiceLoopOwner = Owner;
		return Edge;
	}
}

FServiceLoopBuild::FResult FServiceLoopBuild::Build(URoadNetwork& Network)
{
	FResult Result;

	// WHAT IS ALREADY THERE, gathered before anything is added, so the idempotence check
	// cannot see this pass's own output. An owner whose entity has since died leaves its
	// edges here until the next sweep; they are still excluded as targets, which is right,
	// and are never reused, which is also right.
	{
		const TArray<FGuidelineEdge>& Edges = Network.GetGuidelineEdges();
		for (int32 Index = 0; Index < Edges.Num(); ++Index)
		{
			const FGuidelineEdge& Edge = Edges[Index];
			if (!Edge.bAlive || !Edge.ServiceLoopOwner.IsSet())
			{
				continue;
			}

			FGuidelineEdgeId Id;
			Id.Index = Index;
			Id.Generation = Edge.Generation;

			Result.Lanes.FindOrAdd(Edge.ServiceLoopOwner).Add(Id);
			Result.Nodes.Add(Edge.A);
			Result.Nodes.Add(Edge.B);
		}
	}

	// By index, like FAnchorLink::Build: nothing here adds or removes an ENTITY, so the
	// reference stays good, and the handle still has to be built by hand from the slot.
	const TArray<FEntityInstance>& Entities = Network.GetEntities();
	for (int32 Index = 0; Index < Entities.Num(); ++Index)
	{
		const FEntityInstance& Instance = Entities[Index];
		if (!Instance.bAlive || Instance.Definition == nullptr
			|| Instance.Definition->ServiceLoop.Num() < 3)
		{
			continue;
		}

		FEntityInstanceId EntityId;
		EntityId.Index = Index;
		EntityId.Generation = Instance.Generation;

		if (Result.Lanes.Contains(EntityId))
		{
			// Already laid, by a pass whose output was not swept. See the header.
			continue;
		}

		const TArray<FVector2D>& Local = Instance.Definition->ServiceLoop;
		const double Cosine = FMath::Cos(Instance.Heading);
		const double Sine = FMath::Sin(Instance.Heading);
		auto ToWorld = [&Instance, Cosine, Sine](const FVector2D& Point)
		{
			return Instance.Position
				+ FVector2D(Point.X * Cosine - Point.Y * Sine, Point.X * Sine + Point.Y * Cosine);
		};

		TArray<FGuidelineNodeId> Corners;
		Corners.Reserve(Local.Num());
		for (const FVector2D& Point : Local)
		{
			Corners.Add(Network.AddGuidelineNode(ToWorld(Point), /*bDerived=*/true));
		}

		TArray<FGuidelineEdgeId>& Lane = Result.Lanes.FindOrAdd(EntityId);
		for (int32 At = 0; At < Corners.Num(); ++At)
		{
			// CLOSED IMPLICITLY: the last corner joins the first, and the definition's array
			// does not repeat it - see UEntityDefinition::ServiceLoop.
			const FGuidelineNodeId A = Corners[At];
			const FGuidelineNodeId B = Corners[(At + 1) % Corners.Num()];

			// Read fresh: adding a node can reallocate the array the last read pointed into.
			const FVector2D PositionA = Network.GetGuidelineNode(A)->Position;
			const FVector2D PositionB = Network.GetGuidelineNode(B)->Position;

			Lane.Add(Network.AddGuidelineEdge(MakeServiceEdge(A, B, PositionA, PositionB, EntityId)));
			Result.Nodes.Add(A);
			Result.Nodes.Add(B);
		}
		++Result.LoopsBuilt;

		// SPURS. Each service anchor to its nearest point on the lane, which is short on
		// every anchor of a Code C stand and crosses no part of the aeroplane - see the
		// design's section 5 and Airside.Entities.ServiceLoopClearsTheAircraft, which
		// measures it on the definition rather than trusting this.
		for (const FResolvedAnchor& Resolved : Instance.ResolvedAnchors)
		{
			if (TraversalForRole(Resolved.Role) == ETraversalClass::Aircraft)
			{
				// An aircraft anchor is not something a truck drives to. There are none on a
				// Code C stand today, and a definition that grew one would want a painted
				// lead-in, not a spur onto the service lane.
				continue;
			}

			const FGuidelineNode* AnchorNode = Network.GetGuidelineNode(Resolved.Node);
			if (AnchorNode == nullptr || AnchorNode->Incident.Num() > 0)
			{
				// Already joined - by hand, or by a pass that survived. A second spur would
				// leave two lines into one painted box.
				continue;
			}
			const FVector2D At = AnchorNode->Position;

			// The nearest point across the lane's CURRENT edges: an earlier spur may have
			// split the very side this one is about to meet, and it must see the halves.
			FGuidelineEdgeId BestEdge;
			double BestDistance = TNumericLimits<double>::Max();
			double BestParam = 0.0;
			FVector2D BestPoint = FVector2D::ZeroVector;

			for (const FGuidelineEdgeId& EdgeId : Lane)
			{
				const FGuidelineEdge* Edge = Network.GetGuidelineEdge(EdgeId);
				const FGuidelineNode* EndA = Edge != nullptr ? Network.GetGuidelineNode(Edge->A) : nullptr;
				const FGuidelineNode* EndB = Edge != nullptr ? Network.GetGuidelineNode(Edge->B) : nullptr;
				if (EndA == nullptr || EndB == nullptr)
				{
					continue;
				}

				TArray<FVector2D> Points;
				GuidelineGeom::Sample(EndA->Position, Edge->Control, EndB->Position, Points);

				int32 Span = 0;
				double Fraction = 0.0;
				const double Distance = GuidelineGeom::NearestOnPolyline(Points, At, Span, Fraction);
				if (Distance >= BestDistance)
				{
					continue;
				}

				BestDistance = Distance;
				BestEdge = EdgeId;
				BestParam = GuidelineGeom::ParamAtSample(Span, Fraction, Points.Num());
				BestPoint = FMath::Lerp(Points[Span], Points[Span + 1], Fraction);
			}

			if (!BestEdge.IsSet())
			{
				continue;
			}

			// Copied before anything is removed: the pointer is into the slot array, and
			// adding the halves can reallocate it.
			const FGuidelineEdge Original = *Network.GetGuidelineEdge(BestEdge);
			const FVector2D PositionA = Network.GetGuidelineNode(Original.A)->Position;
			const FVector2D PositionB = Network.GetGuidelineNode(Original.B)->Position;

			FGuidelineNodeId Join;
			if (FVector2D::Distance(BestPoint, PositionA) <= LaneWeldTolerance)
			{
				Join = Original.A;
			}
			else if (FVector2D::Distance(BestPoint, PositionB) <= LaneWeldTolerance)
			{
				Join = Original.B;
			}
			else
			{
				FVector2D Mid, ControlLeft, ControlRight;
				GuidelineGeom::Split(PositionA, Original.Control, PositionB, BestParam,
					Mid, ControlLeft, ControlRight);

				Join = Network.AddGuidelineNode(Mid, /*bDerived=*/true);

				FGuidelineEdge Left = Original;
				Left.B = Join;
				Left.Control = ControlLeft;

				FGuidelineEdge Right = Original;
				Right.A = Join;
				Right.Control = ControlRight;

				Network.RemoveGuidelineEdge(BestEdge);
				Lane.Remove(BestEdge);
				Lane.Add(Network.AddGuidelineEdge(MoveTemp(Left)));
				Lane.Add(Network.AddGuidelineEdge(MoveTemp(Right)));
				Result.Nodes.Add(Join);
			}

			const FVector2D JoinAt = Network.GetGuidelineNode(Join)->Position;
			Lane.Add(Network.AddGuidelineEdge(
				MakeServiceEdge(Resolved.Node, Join, At, JoinAt, EntityId)));
			++Result.SpursBuilt;
		}
	}

	if (Result.LoopsBuilt > 0)
	{
		// One census line, beside the guideline builder's and FAnchorLink's. Zero is the
		// common idle rebuild and stays quiet.
		UE_LOG(LogAirside, Log, TEXT("Service loops: %d lane(s) laid, %d anchor spur(s)"),
			Result.LoopsBuilt, Result.SpursBuilt);
	}

	return Result;
}
```

Note: `Lane.Remove(BestEdge)` requires `FGuidelineEdgeId::operator==`, which exists.

- [ ] **Step 5: Run it to verify it passes**

Build twice, then `./Tools/Run-AirsideTests.ps1 -Filter Airside.Build`.
Expected: `Airside.Build.ServiceLoopReachesTheGraph` passes. `FServiceLoopBuild::Build` is
not yet called by anything else, so no other test changes.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat(build): a stand's service lane and its anchor spurs reach the guideline graph"
```

---

### Task 5: The connection rule splits on class

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Build/AnchorLink.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Build/AnchorLink.cpp`
- Modify: `Plugins/Airside/Source/AirsideTests/Private/ServiceLinkTest.cpp`

**Interfaces:**
- Produces:
```cpp
	static constexpr double DefaultServiceLinkRadius = 5000.0;
	static int32 FAnchorLink::Build(URoadNetwork& Network,
		double MaxLeadIn = DefaultMaxLeadIn,
		double ServiceLinkRadius = DefaultServiceLinkRadius);
```

- [ ] **Step 1: Write the failing tests**

Append to `ServiceLinkTest.cpp`, before `#endif`:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAircraftLeadInStillCastsARayTest,
	"Airside.Build.AircraftLeadInStillCastsARay",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAircraftLeadInStillCastsARayTest::RunTest(const FString& Parameters)
{
	using namespace ServiceLinkFixture;

	// THE TEST THAT FAILS IF THE PROXIMITY RULE LEAKS INTO THE AIRCRAFT ONE.
	//
	// A stand's lead-in IS the painted line, so it is cast along the stand's own heading and
	// a taxiway BEHIND the stand is behind the aircraft's tail. Nearest-guideline was
	// rejected for aircraft precisely because nearest is regularly the taxiway on the far
	// side of the terminal - see FAnchorLink's header. Ten metres behind is as near as it
	// gets, and it must still not join.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	FGuidelineNodeId East;
	Lay(*Net, FVector2D(-10000.0, 1000.0), FVector2D(10000.0, 1000.0),
		ETraversalClass::Aircraft, East);

	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();

	// Heading -90 puts the pose ray at +90: straight up +Y, away from a taxiway ten metres
	// BELOW the stand at y = +1000... so place the stand ABOVE it and face it away.
	const FEntityInstanceId Placed =
		PlaceStand(*Net, *Stand, FVector2D(0.0, 2000.0), UE_DOUBLE_PI * 0.5 * -1.0);

	FAnchorLink::Build(*Net);

	const FGuidelineNode* Pose = Net->GetGuidelineNode(Net->GetEntity(Placed)->PoseNode);
	if (TestNotNull(TEXT("the stop position resolves"), Pose))
	{
		TestEqual(TEXT("a taxiway 10 m behind a stand is still not joined"),
			Pose->Incident.Num(), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FServiceLinkJoinsFromAnyDirectionTest,
	"Airside.Build.ServiceLinkJoinsFromAnyDirection",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FServiceLinkJoinsFromAnyDirectionTest::RunTest(const FString& Parameters)
{
	using namespace ServiceLinkFixture;

	// THE WHOLE POINT OF THE CHANGE. A stand's five service anchors nearly all cast the same
	// way - three at -90, one at +90, one at 180 - so a road drawn the way a player draws
	// one, ALONGSIDE the stands, is parallel to every ray and served none of them. A vehicle
	// may genuinely arrive from any side, so a service link measures distance, not direction.
	//
	// The lane round a Code C stand at the origin, heading 0, is x in [-3550, +1700] and
	// y in [-2090, +2090]. Each road below sits GAP_NEAR beyond one of those sides.
	//
	// 4500 rather than exactly the 5000 uu radius: a boundary case measures the comparison
	// operator, not the rule, and would flip on a hundredth of a millimetre of rounding.
	constexpr double GapNear = 4500.0;
	constexpr double GapFar = 20000.0;

	struct FSide
	{
		const TCHAR* Name;
		FVector2D From;
		FVector2D To;
	};

	auto RoadsAt = [](double Gap) -> TArray<FSide>
	{
		return {
			{ TEXT("south"), FVector2D(-20000.0, -2090.0 - Gap), FVector2D(20000.0, -2090.0 - Gap) },
			{ TEXT("north"), FVector2D(-20000.0,  2090.0 + Gap), FVector2D(20000.0,  2090.0 + Gap) },
			{ TEXT("west"),  FVector2D(-3550.0 - Gap, -20000.0), FVector2D(-3550.0 - Gap, 20000.0) },
			{ TEXT("east"),  FVector2D( 1700.0 + Gap, -20000.0), FVector2D( 1700.0 + Gap, 20000.0) },
		};
	};

	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();

	for (const FSide& Side : RoadsAt(GapNear))
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		FGuidelineNodeId Far;
		const FGuidelineNodeId Near =
			Lay(*Net, Side.From, Side.To, ETraversalClass::GroundVehicle, Far);

		const FEntityInstanceId Placed = PlaceStand(*Net, *Stand, FVector2D::ZeroVector, 0.0);
		FAnchorLink::Build(*Net);

		const FGuidelineNodeId Hydrant = AnchorNode(*Net, Placed, TEXT("HydrantPit"));
		TestTrue(*FString::Printf(TEXT("a road to the %s reaches the lane"), Side.Name),
			Net->IsServiceNodeConnected(Hydrant));

		// AND A TRUCK CAN ACTUALLY GET THERE. Connectivity is the claim; a route is the
		// proof, and "the search found nothing" would otherwise read like a broken search.
		FRouteQuery Query;
		Query.Start = Near;
		Query.Goal = Hydrant;
		Query.Class = ETraversalClass::GroundVehicle;
		TestTrue(*FString::Printf(TEXT("and a truck routes from the %s to the hydrant"), Side.Name),
			RouteSearch::Find(*Net, Query).IsValid());
	}

	for (const FSide& Side : RoadsAt(GapFar))
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		FGuidelineNodeId Far;
		Lay(*Net, Side.From, Side.To, ETraversalClass::GroundVehicle, Far);

		const FEntityInstanceId Placed = PlaceStand(*Net, *Stand, FVector2D::ZeroVector, 0.0);
		FAnchorLink::Build(*Net);

		// THE SHORT RADIUS IS WHAT KEEPS THE REJECTED CASE REJECTED: 200 m would be the
		// service road on the other side of the terminal, and the link would cross it.
		TestFalse(*FString::Printf(TEXT("a road 200 m to the %s does not"), Side.Name),
			Net->IsServiceNodeConnected(AnchorNode(*Net, Placed, TEXT("HydrantPit"))));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FServiceLoopDoesNotJoinItselfTest,
	"Airside.Build.ServiceLoopDoesNotJoinItself",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FServiceLoopDoesNotJoinItselfTest::RunTest(const FString& Parameters)
{
	using namespace ServiceLinkFixture;

	// THE LANE IS ITSELF A VEHICLE GUIDELINE. A search that failed to exclude the searcher's
	// own geometry would find the nearest vehicle line about four metres away - the other
	// side of its own box - report every stand connected, and route no truck anywhere.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());

	// A TAXIWAY within reach, so the fixture is not merely an empty graph: the stand's own
	// pose must still join it, and the lane must still not.
	FGuidelineNodeId TaxiEast;
	const FGuidelineNodeId TaxiWest = Lay(*Net, FVector2D(-30000.0, 0.0), FVector2D(-10000.0, 0.0),
		ETraversalClass::Aircraft, TaxiEast);

	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
	const FEntityInstanceId Placed = PlaceStand(*Net, *Stand, FVector2D::ZeroVector, 0.0);

	FAnchorLink::Build(*Net);

	TestTrue(TEXT("the aircraft half still works - the pose joins the taxiway"),
		Net->GetGuidelineNode(Net->GetEntity(Placed)->PoseNode)->Incident.Num() > 0);

	for (const FResolvedAnchor& Anchor : Net->GetEntity(Placed)->ResolvedAnchors)
	{
		TestFalse(TEXT("no service anchor is on a road, because there is no road"),
			Net->IsServiceNodeConnected(Anchor.Node));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadAlongsideARowOfStandsTest,
	"Airside.Build.RoadAlongsideARowOfStands",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadAlongsideARowOfStandsTest::RunTest(const FString& Parameters)
{
	using namespace ServiceLinkFixture;

	// THE CASE THAT FAILED IN PIE ON 2026-09-07, and the one this whole design exists for:
	// one service road drawn alongside a row of stands, which is how a player draws one.
	// Measured then: 9 of 25 lead-ins joined, and the only service anchor that joined at any
	// stand was TugStand - the one anchor that casts ACROSS the road instead of along it.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());

	constexpr double RoadY = -6000.0;
	FGuidelineNodeId RoadEast;
	const FGuidelineNodeId RoadWest =
		Lay(*Net, FVector2D(-40000.0, RoadY), FVector2D(40000.0, RoadY),
			ETraversalClass::GroundVehicle, RoadEast);

	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();

	TArray<FEntityInstanceId> Row;
	for (int32 At = 0; At < 4; ++At)
	{
		// Six metres of clear ground between neighbouring lanes: 5250 uu of lane plus 600.
		Row.Add(PlaceStand(*Net, *Stand, FVector2D(-15000.0 + At * 8000.0, 0.0), 0.0));
	}

	FAnchorLink::Build(*Net);

	for (int32 At = 0; At < Row.Num(); ++At)
	{
		const FGuidelineNodeId Hydrant = AnchorNode(*Net, Row[At], TEXT("HydrantPit"));
		TestTrue(*FString::Printf(TEXT("stand %d's hydrant is on the road"), At),
			Net->IsServiceNodeConnected(Hydrant));

		FRouteQuery Query;
		Query.Start = RoadWest;
		Query.Goal = Hydrant;
		Query.Class = ETraversalClass::GroundVehicle;
		TestTrue(*FString::Printf(TEXT("and a truck routes to stand %d"), At),
			RouteSearch::Find(*Net, Query).IsValid());
	}

	// EACH STAND GETS ITS OWN CONNECTION. A road within reach of two stands joins BOTH, and
	// that is the normal case rather than a conflict.
	FRouteQuery BetweenStands;
	BetweenStands.Start = AnchorNode(*Net, Row[0], TEXT("HydrantPit"));
	BetweenStands.Goal = AnchorNode(*Net, Row.Last(), TEXT("HydrantPit"));
	BetweenStands.Class = ETraversalClass::GroundVehicle;
	TestTrue(TEXT("and a truck can go from the first stand to the last down the road"),
		RouteSearch::Find(*Net, BetweenStands).IsValid());
	return true;
}
```

- [ ] **Step 2: Run to verify they fail**

Build, then `./Tools/Run-AirsideTests.ps1 -Filter Airside.Build`.
Expected: `ServiceLinkJoinsFromAnyDirection`, `RoadAlongsideARowOfStands` FAIL (the lane is
built but never joined to a road); `AircraftLeadInStillCastsARay` and
`ServiceLoopDoesNotJoinItself` may already pass — that is fine, they are regression pins.

- [ ] **Step 3: Extend the header**

In `AnchorLink.h`, extend the class comment (do not delete a word of it) with:

```
 * THE RULE SPLITS ON TRAVERSAL CLASS, since 2026-09-07. The paragraph above is about an
 * AIRCRAFT lead-in, where the ray IS the painted line and "nearest" is regularly the
 * taxiway on the far side of the terminal. It never applied to a vehicle, which may
 * genuinely arrive from any side - and a Code C stand made that maximally wrong, because
 * four of its five service anchors cast along the row rather than across it, so a road
 * drawn ALONGSIDE the stands was parallel to every ray and served none of them. A service
 * link therefore joins the NEAREST guideline of its class in any direction, within a much
 * shorter reach: 50 m rather than 200, which is what keeps the rejected case rejected.
 *
 * Service anchors do not link here at all on a stand with a lane. FServiceLoopBuild has
 * already spurred each of them to the stand's service loop - a straight spur from a road on
 * one side to a box on the other would cross 37 m of fuselage - and it is the LANE that
 * links to the road.
```

and:

```cpp
	/**
	 * 50 m: how far a SERVICE connection may reach, in any direction.
	 *
	 * SHORT ON PURPOSE. A Code C stand is about 40 m deep and the gap from stand to service
	 * road is typically 10-30 m, so this reaches the road the player meant and cannot reach
	 * the far side of a terminal - which is the whole reason nearest-guideline was rejected
	 * for aircraft. The default only; the live figure is level-authored on
	 * ARoadNetworkActor::ServiceLinkRadius, because it is per-airport gameplay tuning rather
	 * than a content default.
	 */
	static constexpr double DefaultServiceLinkRadius = 5000.0;

	static int32 Build(URoadNetwork& Network, double MaxLeadIn = DefaultMaxLeadIn,
		double ServiceLinkRadius = DefaultServiceLinkRadius);
```

- [ ] **Step 4: Implement the split rule**

In `AnchorLink.cpp`:

1. Add `#include "Build/ServiceLoopBuild.h"` to the includes.

2. In `FPendingLink`, replace nothing and ADD:

```cpp
		/**
		 * How far this link may reach. The aircraft cap, or ServiceLinkRadius - see
		 * FAnchorLink's header for why the two differ by a factor of four.
		 */
		double Reach = FAnchorLink::DefaultMaxLeadIn;

		/** Set when this link is a stand's service LANE rather than a single node: the
		 *  edges to measure from, and the one that gets split to make the link's node. */
		TArray<FGuidelineEdgeId> Lane;
```

3. At the top of `Build`, before the entity walk:

```cpp
	// THE LANES FIRST. A service anchor spurred to its stand's loop is already joined by the
	// time this walk asks, so it is skipped below rather than cast at a road across the
	// aeroplane - and the lane itself becomes the thing that links.
	const FServiceLoopBuild::FResult Loops = FServiceLoopBuild::Build(Network);
```

4. Seed the exclusion set from it. Replace `TSet<FGuidelineNodeId> AnchorNodes;` with:

```cpp
	// EVERY NODE A LINK MUST NOT TARGET: anchor and pose nodes, as always, plus every node
	// of every service lane and spur. A lane is itself a vehicle guideline, so without the
	// second half a lane would join ITSELF - four metres away, across its own box - and
	// every stand would read as connected while reaching no road.
	TSet<FGuidelineNodeId> AnchorNodes = Loops.Nodes;
```

5. Set `Link.Reach` on each pending link as it is built:

```cpp
			Link.Reach = Link.Class == ETraversalClass::Aircraft ? MaxLeadIn : ServiceLinkRadius;
```

(both for the pose link and the anchor link).

6. After the per-entity anchor loop, add the lane's own pending link:

```cpp
		// THE LANE'S LINK TO A ROAD. One per stand: a lane within reach of two roads takes
		// the nearer, and a road within reach of two stands is joined by both, each getting
		// its own connection - which is the normal case, one service road serving a row.
		if (const TArray<FGuidelineEdgeId>* Lane = Loops.Lanes.Find(EntityId))
		{
			// ALREADY CONNECTED is asked of the GRAPH rather than remembered, because the
			// answer has to survive a pass that did not lay this lane - see
			// URoadNetwork::IsServiceNodeConnected.
			const FGuidelineEdge* Any = Lane->Num() > 0 ? Network.GetGuidelineEdge((*Lane)[0]) : nullptr;
			if (Any != nullptr && !Network.IsServiceNodeConnected(Any->A))
			{
				FPendingLink Link;
				Link.Lane = *Lane;
				Link.Class = ETraversalClass::GroundVehicle;
				Link.At = Network.GetGuidelineNode(Any->A)->Position;
				Link.MaxWingspan = 0.0;
				Link.Radius = StandRadius;
				Link.Reach = ServiceLinkRadius;
				Pending.Add(Link);
			}
		}
```

7. Make the join loop take a mutable link (`for (FPendingLink& Link : Pending)`) and split
the search. Replace the search body with:

```cpp
		FGuidelineEdgeId BestEdge;
		double BestDistance = Link.Reach;
		double BestParam = 0.0;

		// For a LANE link only: which of the lane's edges was closest, and where on it. The
		// link's own node does not exist yet - the lane gets split at this point to make it.
		FGuidelineEdgeId BestLaneEdge;
		double BestLaneParam = 0.0;

		const TArray<FGuidelineEdge>& Edges = Network.GetGuidelineEdges();
		for (int32 Index = 0; Index < Edges.Num(); ++Index)
		{
			... unchanged guards (bAlive, bDerived, A != B, AnchorNodes, AllowedTraffic, ends) ...

			TArray<FVector2D> Points;
			GuidelineGeom::Sample(EndA->Position, Edge.Control, EndB->Position, Points);

			FGuidelineEdgeId Id;
			Id.Index = Index;
			Id.Generation = Edge.Generation;

			if (Link.Lane.Num() > 0)
			{
				// LANE TO ROAD: closest approach between two polylines, so a road drawn
				// PARALLEL to the lane is measured side-to-side rather than corner-to-corner.
				for (const FGuidelineEdgeId& LaneId : Link.Lane)
				{
					const FGuidelineEdge* LaneEdge = Network.GetGuidelineEdge(LaneId);
					const FGuidelineNode* LaneA = LaneEdge ? Network.GetGuidelineNode(LaneEdge->A) : nullptr;
					const FGuidelineNode* LaneB = LaneEdge ? Network.GetGuidelineNode(LaneEdge->B) : nullptr;
					if (LaneA == nullptr || LaneB == nullptr)
					{
						continue;
					}

					TArray<FVector2D> LanePoints;
					GuidelineGeom::Sample(LaneA->Position, LaneEdge->Control, LaneB->Position, LanePoints);

					int32 LaneSpan = 0, RoadSpan = 0;
					double LaneFraction = 0.0, RoadFraction = 0.0;
					const double Distance = GuidelineGeom::NearestBetweenPolylines(
						LanePoints, Points, LaneSpan, LaneFraction, RoadSpan, RoadFraction);

					if (Distance <= LeadInWeldTolerance || Distance >= BestDistance)
					{
						continue;
					}

					BestDistance = Distance;
					BestEdge = Id;
					BestParam = GuidelineGeom::ParamAtSample(RoadSpan, RoadFraction, Points.Num());
					BestLaneEdge = LaneId;
					BestLaneParam = GuidelineGeom::ParamAtSample(LaneSpan, LaneFraction, LanePoints.Num());
				}
				continue;
			}

			if (Link.Class == ETraversalClass::Aircraft)
			{
				... the existing RayHitsSegment loop, unchanged, writing BestEdge/BestParam ...
				continue;
			}

			// PROXIMITY, ANY DIRECTION. A vehicle may arrive from any side, and an anchor's
			// authored heading has no representation on screen for a player to aim by.
			int32 Span = 0;
			double Fraction = 0.0;
			const double Distance = GuidelineGeom::NearestOnPolyline(Points, Link.At, Span, Fraction);
			if (Distance <= LeadInWeldTolerance || Distance >= BestDistance)
			{
				continue;
			}

			BestDistance = Distance;
			BestParam = GuidelineGeom::ParamAtSample(Span, Fraction, Points.Num());
			BestEdge = Id;
		}
```

8. Update the unjoined warning to name the rule that actually ran:

```cpp
			++Unjoined;
			UE_LOG(LogAirside, Warning,
				TEXT("%s at (%.0f, %.0f) joins nothing: no derived %s guideline within %.0f uu%s"),
				Link.Lane.Num() > 0 ? TEXT("Service lane") : TEXT("Anchor"),
				Link.At.X, Link.At.Y,
				Link.Class == ETraversalClass::Aircraft ? TEXT("aircraft") : TEXT("vehicle"),
				Link.Reach,
				Link.Class == ETraversalClass::Aircraft
					? *FString::Printf(TEXT(" along heading %.0f deg"),
						FMath::RadiansToDegrees(FMath::Atan2(Link.Dir.Y, Link.Dir.X)))
					: TEXT(" in any direction"));
			continue;
```

9. Immediately after `Found`/`EndA`/`EndB` are resolved and `Original`, `PositionA`,
`PositionB` are copied, and BEFORE `Corner` is used for `TaxiDir`, insert:

```cpp
		const FVector2D Corner =
			GuidelineGeom::Eval(PositionA, Original.Control, PositionB, BestParam);

		if (Link.Lane.Num() > 0)
		{
			// THE LANE IS SPLIT TO MAKE THE LINK'S OWN END. Entry in the middle of a side,
			// not at a corner: a corner-to-road connector would run diagonally across ground
			// the lane exists to keep clear, and the split costs one node on an invisible
			// lane.
			const FGuidelineEdge* LaneEdge = Network.GetGuidelineEdge(BestLaneEdge);
			const FGuidelineNode* LaneA = LaneEdge ? Network.GetGuidelineNode(LaneEdge->A) : nullptr;
			const FGuidelineNode* LaneB = LaneEdge ? Network.GetGuidelineNode(LaneEdge->B) : nullptr;
			if (LaneEdge == nullptr || LaneA == nullptr || LaneB == nullptr)
			{
				continue;
			}

			const FGuidelineEdge LaneOriginal = *LaneEdge;
			const FVector2D LanePositionA = LaneA->Position;
			const FVector2D LanePositionB = LaneB->Position;

			FVector2D LaneMid, LaneControlLeft, LaneControlRight;
			GuidelineGeom::Split(LanePositionA, LaneOriginal.Control, LanePositionB, BestLaneParam,
				LaneMid, LaneControlLeft, LaneControlRight);

			if (FVector2D::Distance(LaneMid, LanePositionA) <= LeadInWeldTolerance)
			{
				Link.Node = LaneOriginal.A;
			}
			else if (FVector2D::Distance(LaneMid, LanePositionB) <= LeadInWeldTolerance)
			{
				Link.Node = LaneOriginal.B;
			}
			else
			{
				Link.Node = Network.AddGuidelineNode(LaneMid, /*bDerived=*/true);

				FGuidelineEdge Left = LaneOriginal;
				Left.B = Link.Node;
				Left.Control = LaneControlLeft;

				FGuidelineEdge Right = LaneOriginal;
				Right.A = Link.Node;
				Right.Control = LaneControlRight;

				Network.RemoveGuidelineEdge(BestLaneEdge);
				Network.AddGuidelineEdge(MoveTemp(Left));
				Network.AddGuidelineEdge(MoveTemp(Right));
			}
			Link.At = Network.GetGuidelineNode(Link.Node)->Position;
		}

		if (Link.Class != ETraversalClass::Aircraft)
		{
			// The direction the join actually needs. A service link has no ray of its own -
			// it was found by distance - but everything below (the fillet, the two sweeps)
			// is written in terms of a direction of arrival, and this is it.
			const FVector2D Toward = Corner - Link.At;
			if (!Toward.IsNearlyZero())
			{
				Link.Dir = Toward.GetSafeNormal();
			}
		}
```

and delete the old `const FVector2D Corner = ...` line that followed, keeping `TaxiDir` and
everything after it untouched.

**Careful:** `Link.At` is re-read after the lane split, so `LeadRoom` below measures from the
lane, which is correct. The `Original`/`PositionA`/`PositionB`/`Corner` values were captured
BEFORE the lane split and the lane split never touches the road edge, so they stay valid.

- [ ] **Step 5: Run to verify they pass**

Build, then `./Tools/Run-AirsideTests.ps1 -Filter Airside`.
Expected: the four new `Airside.Build.*` tests pass, and `Airside.Build.AnchorLink`,
`Airside.Build.LeadInSweep`, `Airside.Entities.DepotJoinsRoad`,
`Airside.Entities.StandFuelAnchorJoinsRoad` still pass.

**If `Airside.Entities.StandFuelAnchorJoinsRoad` fails on "an aircraft cannot be routed to
the hydrant"** — that is a real regression to fix, not a fixture to edit: the lane and the
spurs must be `GroundVehicle` only.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat(build): a service connection joins by proximity, and the stand's lane is what joins"
```

---

### Task 6: The radius is level-authored

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Present/RoadSurfacePresenter.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Present/RoadSurfacePresenter.cpp`
- Modify: `Plugins/Airside/Source/Airside/Public/Present/RoadNetworkActor.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Present/RoadNetworkActor.cpp`
- Modify: `Plugins/Airside/Source/AirsideTests/Private/TrafficForwardersTest.cpp`

**Interfaces:**
- Consumes: `FAnchorLink::DefaultServiceLinkRadius`.
- Produces: `UPROPERTY(EditAnywhere) double ARoadNetworkActor::ServiceLinkRadius;` and
  `double URoadSurfacePresenter::FSurfaceSettings::ServiceLinkRadius`.

- [ ] **Step 1: Write the failing test**

Append to `TrafficForwardersTest.cpp`, before `#endif` (it already spawns a world and an
actor, which is what this seam needs):

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FServiceLinkRadiusIsLevelAuthoredTest,
	"Airside.Present.ServiceLinkRadiusIsLevelAuthored",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FServiceLinkRadiusIsLevelAuthoredTest::RunTest(const FString& Parameters)
{
	// THE SEAM, and the only test that fails if the property is left unwired. A figure the
	// designer sets on the level that never reaches FAnchorLink is exactly the failure mode
	// CLAUDE.md's "check where a list is CONSUMED" is about - a knob with nothing on the
	// other end of it.
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("a world"), World)) { return false; }
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };

	ARoadNetworkActor* Actor = World->SpawnActor<ARoadNetworkActor>();
	if (!TestNotNull(TEXT("the actor"), Actor)) { return false; }

	TestEqual(TEXT("the default is FAnchorLink's"),
		Actor->ServiceLinkRadius, FAnchorLink::DefaultServiceLinkRadius);

	Actor->ServiceLinkRadius = 777.0;
	TestEqual(TEXT("and it reaches the presenter's settings unchanged"),
		Actor->MakeSurfaceSettingsForTest().ServiceLinkRadius, 777.0);
	return true;
}
```

This needs `#include "Build/AnchorLink.h"`, `"Engine/World.h"`, `"Misc/ScopeExit.h"` in that
file if not already present, and a test accessor on the actor — `MakeSurfaceSettings` is
currently public or private; check and, if private, add beside it:

```cpp
	/** MakeSurfaceSettings for the seam test. The settings struct is the ONLY route from a
	 *  level-authored figure to the build, so the test reads it rather than the build. */
	URoadSurfacePresenter::FSurfaceSettings MakeSurfaceSettingsForTest() { return MakeSurfaceSettings(); }
```

- [ ] **Step 2: Run to verify it fails**

Build, `./Tools/Run-AirsideTests.ps1 -Filter Airside.Present` — compile error, no such
property.

- [ ] **Step 3: Wire it**

`RoadSurfacePresenter.h`: add `#include "Build/AnchorLink.h"` (Present/ may include Build/),
and inside `FSurfaceSettings`:

```cpp
		/** How far a SERVICE connection may reach, uu. See FAnchorLink::DefaultServiceLinkRadius. */
		double ServiceLinkRadius = FAnchorLink::DefaultServiceLinkRadius;
```

`RoadSurfacePresenter.cpp`, at the `FAnchorLink::Build` call:

```cpp
	// The service radius comes down from the LEVEL - see ARoadNetworkActor::ServiceLinkRadius.
	// The aircraft cap stays FAnchorLink's own default: 200 m is a fact about a painted
	// lead-in, not a per-airport tuning figure.
	FAnchorLink::Build(Network, FAnchorLink::DefaultMaxLeadIn, Settings.ServiceLinkRadius);
```

`RoadNetworkActor.h`: add `#include "Build/AnchorLink.h"` and, beside `TrafficRules`:

```cpp
	/**
	 * How far a SERVICE connection may reach, in any direction, uu. 50 m by default.
	 *
	 * HERE AND NOT A CONSTANT, for the same reason TrafficRules is here: it is per-airport
	 * gameplay tuning a designer sets on the level, not a content default to fall back on
	 * (which is UAirsideSettings' business) and not a fact about a painted line (which is
	 * FAnchorLink::DefaultMaxLeadIn). An airport laid out with wider service margins raises
	 * it; one that wants stands to connect only to the road right beside them lowers it.
	 *
	 * Short by default on purpose - see FAnchorLink::DefaultServiceLinkRadius for why a long
	 * reach reintroduces exactly the failure the ray rule was protecting against.
	 *
	 * PUBLIC, like TrafficRules: the level authors it and the seam test reads it back.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Traffic")
	double ServiceLinkRadius = FAnchorLink::DefaultServiceLinkRadius;
```

`RoadNetworkActor.cpp`, in `MakeSurfaceSettings`, next to the other scalar copies:

```cpp
	Settings.ServiceLinkRadius = ServiceLinkRadius;
```

Do NOT add it to `MakeGhostSurfaceSettings` — that one is deliberately narrower, and its
comment says so.

- [ ] **Step 4: Run to verify it passes**

Build, `./Tools/Run-AirsideTests.ps1 -Filter Airside.Present`.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "feat(present): the service link radius is level-authored, beside TrafficRules"
```

---

### Task 7: What the player is told

**Files:**
- Modify: `Plugins/AirportOps/Source/AirportOps/Private/Model/FuelService.cpp`
- Modify: `Plugins/AirportOps/Source/AirportOpsTests/Private/FuelServiceTest.cpp`
- Modify: `Plugins/Airside/Source/AirsideTests/Private/StarterMapProbeTest.cpp`

**Interfaces:**
- Consumes: `URoadNetwork::IsServiceNodeConnected`.

- [ ] **Step 1: Move the fixture that the lane now reaches**

`FuelServiceTest.cpp`, `FFuelFixture::Build_RoadReachesDepotOnly`: replace the body's comment
and figure.

```cpp
void FFuelFixture::Build_RoadReachesDepotOnly()
{
	// A ROAD THE STAND CANNOT REACH, and the number moved on 2026-09-07 because the stand's
	// reach did. The hydrant used to cast a ray down -Y from x = -1200 and a road starting at
	// x = 5000 was simply not on it; a stand now offers its whole SERVICE LANE - a box out to
	// x = +1700, y = -2090 - and joins anything within 50 m of any of it in any direction. At
	// x = 5000 that leaves 51 m, which is a fixture one rounding away from testing the
	// opposite of what it says. The depot's pose at x = 12000 is untouched either way.
	RoadFromX = 9000.0;
	Build(/*bWithRoad=*/true);
}
```

- [ ] **Step 2: Run to see the real failure**

`./Tools/Run-AirsideTests.ps1 -Filter AirportOps`.
Expected: `AirportOps.Ops.FuelService*` fails on "the reason names the STAND, not the depot" —
the hydrant now always has a spur on it, so `bStandJoined` is true and the refusal is
`NoRoute`. That is the defect to fix in Step 3.

- [ ] **Step 3: Ask the graph, not the incident count**

`FuelService.cpp`, replacing the `bStandJoined` line and extending its comment:

```cpp
	// JOINED, NOT MERELY RESOLVED - and since 2026-09-07 not merely INCIDENT either. The
	// first version tested StandFuel.IsSet(), which is a fact about PLACEMENT: PlaceEntity
	// makes a node per anchor whether a lead-in ever reaches it or not, so it was true for
	// every Code C stand ever placed and StandUnjoined could not fire at all. Counting
	// incident edges fixed that and then stopped working for the same shape of reason the
	// moment stands grew SERVICE LANES: a hydrant is always spurred to its own lane, so the
	// count is true for a stand in the middle of a field. The question was never "does this
	// node have a line on it" but "does that line go anywhere", which is a walk -
	// URoadNetwork::IsServiceNodeConnected. Both wrong answers reported NoRoute - "no road
	// from depot" - and sent the player to look at the wrong end of the airport.
	const bool bStandJoined = StandFuel.IsSet() && Network.IsServiceNodeConnected(StandFuel);
```

Delete the now-unused `FuelNode` local if nothing else reads it; if the compiler says it is
still used, leave it and keep both.

And in the refusal log line further down, replace the hydrant clause:

```cpp
				UE_LOG(LogAirportOps, Warning,
					TEXT("Fuel: aircraft %d at stand %d cannot be served: %s. %d depot(s), %d on a "
						 "road; the stand's hydrant %s. Check the 'Anchor links:' line."),
					Demand.AircraftId, Demand.Stand.Index, RefusalText(Why), Depots, DepotsOnRoad,
					!Hydrant.IsSet() ? TEXT("has no node at all")
						: Network.IsServiceNodeConnected(Hydrant) ? TEXT("is on a road")
						: TEXT("is NOT on a road"));
```

Delete the `HydrantNode` local that fed the old clause.

- [ ] **Step 4: Report it in the probe**

`StarterMapProbeTest.cpp`, in the fuel-readiness block, replace the per-anchor `bJoined`:

```cpp
				const bool bJoined = Anchor != nullptr && Net->IsServiceNodeConnected(Anchor->Node);
```

and extend the log line so the lane is visible as its own fact:

```cpp
				UE_LOG(LogM2MapProbe, Log, TEXT("PROBE stand %d anchor '%s' (Fuel): %s a road"),
					Index, *FuelId.ToString(), bJoined ? TEXT("joins") : TEXT("JOINS NO"));
```

(unchanged text — the meaning is now right). Then add, immediately after the per-anchor loop
inside the same `for` over entities:

```cpp
			// THE LANE ITSELF, so "the truck never comes" can be read off one line: a stand
			// whose lane reaches no road is a stand no service road was drawn near, which is
			// a different repair from a stand with no hydrant.
			bool bLaneConnected = false;
			for (const FResolvedAnchor& Anchor : Instance.ResolvedAnchors)
			{
				bLaneConnected = bLaneConnected || Net->IsServiceNodeConnected(Anchor.Node);
			}
			UE_LOG(LogM2MapProbe, Log, TEXT("PROBE stand %d service lane: %s a road"),
				Index, bLaneConnected ? TEXT("reaches") : TEXT("REACHES NO"));
```

- [ ] **Step 5: Run everything**

Build, then `./Tools/Run-AirsideTests.ps1` (all three suites).
Expected: `N test(s) run, 0 failed, 0 crashed`. Read the line; do not trust the exit code.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "fix(ops): 'stand not on a road' asks whether the lane goes anywhere, not whether it exists"
```

---

### Task 8: The truck does not drive through the aeroplane

**Files:**
- Modify: `Plugins/Airside/Source/AirsideTests/Private/ServiceLinkTest.cpp`

- [ ] **Step 1: Write the failing test**

Append to `ServiceLinkTest.cpp`, before `#endif`:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTruckReachesHydrantWithoutCrossingTheAircraftTest,
	"Airside.Traffic.TruckReachesHydrantWithoutCrossingTheAircraft",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTruckReachesHydrantWithoutCrossingTheAircraftTest::RunTest(const FString& Parameters)
{
	using namespace ServiceLinkFixture;

	// THE INVARIANT, MEASURED ON A ROUTE. Airside.Entities.ServiceLoopClearsTheAircraft
	// measures the definition; this measures what the SEARCH will actually hand a driver,
	// which is the thing the player watches. A lane that cleared the aeroplane and a link
	// that did not would pass the first test and fail here.
	//
	// The road is on the PORT side and the hydrant is under the STARBOARD wing, which is the
	// arrangement that makes a straight spur cross 37 m of fuselage. That is exactly why the
	// lane exists, and why joining each anchor directly to the road was rejected.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());

	constexpr double RoadY = -6000.0;
	FGuidelineNodeId RoadEast;
	const FGuidelineNodeId RoadWest =
		Lay(*Net, FVector2D(-30000.0, RoadY), FVector2D(30000.0, RoadY),
			ETraversalClass::GroundVehicle, RoadEast);

	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
	if (!TestNotNull(TEXT("a design aircraft to clear"), Stand->DesignAircraft.Get())) { return false; }

	const FEntityInstanceId Placed = PlaceStand(*Net, *Stand, FVector2D::ZeroVector, 0.0);
	FAnchorLink::Build(*Net);

	FRouteQuery Query;
	Query.Start = RoadWest;
	Query.Goal = AnchorNode(*Net, Placed, TEXT("HydrantPit"));
	Query.Class = ETraversalClass::GroundVehicle;

	const FRoutePlan Plan = RouteSearch::Find(*Net, Query);
	if (!TestTrue(TEXT("a truck routes from the road to the hydrant"), Plan.IsValid())
		|| Plan.Polyline.Num() < 2)
	{
		// Returning rather than reading on: a refused plan has an EMPTY polyline and the
		// loop below would measure nothing while reporting success.
		return false;
	}

	// The parked aircraft's centreline, in world space. The stand is at the origin facing
	// +X, so local and world coincide - stated rather than assumed, because a fixture that
	// rotated the stand and forgot to rotate this would measure the wrong line.
	const FEntityFootprint& Footprint = Stand->DesignAircraft->Footprint;
	const FVector2D Tail(Footprint.TailX, 0.0);
	const FVector2D Nose(Footprint.NoseX, 0.0);

	int32 Crossings = 0;
	for (int32 At = 1; At < Plan.Polyline.Num(); ++At)
	{
		Crossings += RoadGeom::SegmentsCross(Plan.Polyline[At - 1], Plan.Polyline[At], Tail, Nose)
			? 1 : 0;
	}

	// NOT "does not intersect the footprint", deliberately: that would forbid passing under
	// a wing, which is normal and which the hydrant requires - the pit is under the
	// starboard wing root because that is where a hydrant pit is.
	TestEqual(TEXT("the truck's route never crosses the fuselage lengthwise"), Crossings, 0);

	// AND THE ROUTE IS NOT ABSURD. A plan that went round the airport would also cross
	// nothing; the lane is about 15 m from the road and the hydrant is on the far side of
	// the box, so anything past 200 m means the truck is not using the lane.
	TestTrue(TEXT("and it is a short journey, not a tour of the airport"),
		GuidelineGeom::PolylineLength(Plan.Polyline) < 20000.0);
	return true;
}
```

- [ ] **Step 2: Run it**

Build, `./Tools/Run-AirsideTests.ps1 -Filter Airside.Traffic`.
Expected: PASS. If `Crossings > 0` the lane or the link is crossing the aeroplane and the
implementation is wrong — do not relax the assertion.

- [ ] **Step 3: Commit**

```bash
git add -A
git commit -m "test(traffic): a truck's route to the hydrant never crosses the fuselage"
```

---

### Task 9: Re-author the content and verify against the level

**Files:**
- Modify: `Content/Entities/DA_Stand_CodeC.uasset` (regenerated)
- Modify: `docs/superpowers/specs/2026-09-07-service-connections-design.md` (status line only)

- [ ] **Step 1: Full build and full test run**

```
D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development `
  -Project="C:\repos\AirportMgr2\AirportMgr.uproject" -WaitMutex
./Tools/Run-AirsideTests.ps1
```

Read the `N test(s) run, N failed, N crashed` line and quote it in the commit.

- [ ] **Step 2: Re-author DA_Stand_CodeC**

The editor must be CLOSED. `DA_Stand_CodeC` references `DA_Aircraft_A320`, and a re-run
holds that reference open — if `create_asset` returns None for the aircraft, delete
`Content/Entities/DA_Aircraft_A320.uasset` and `DA_Aircraft_B738.uasset` from disk first (the
script's own docstring says so).

```
D:\Epic\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe `
  C:\repos\AirportMgr2\AirportMgr.uproject -run=pythonscript `
  -script=C:\repos\AirportMgr2\Tools\Python\build_stand_asset.py -unattended -nosplash -nopause
```

Then grep the log for `MARKER: DA_Stand_CodeC service loop` and confirm four corners at
`(-3550, -2090) (1700, -2090) (1700, 2090) (-3550, 2090)`.

- [ ] **Step 3: Read the probe against the player's own level**

```
./Tools/Run-AirsideTests.ps1 -Filter Airside.Probe
```

Then grep the run's log for `PROBE stand`, `service lane`, `Service loops:` and
`Anchor links:`. Record in the commit message:
- the `Service loops: N lane(s) laid, N anchor spur(s)` line,
- the `Anchor links: N of N lead-in(s) joined` line (compare against the 9-of-25 the design
  records for the same map),
- how many stands report `service lane: reaches a road`.

**This is the evidence the change worked.** A stand still reporting `REACHES NO road` is
either genuinely more than 50 m from any service road on that map — check its position in
the same log against the road nodes — or a defect.

- [ ] **Step 4: Mark the spec done**

Change the spec's `**Status:**` line to record that it was implemented, with the date and the
probe figures. Do not edit anything else in it.

- [ ] **Step 5: Commit and open the PR**

```bash
git add -A
git commit -m "chore(content): DA_Stand_CodeC carries its service lane; probe reports lane connectivity"
git push -u origin feature/service-connections
gh pr create --fill
```

Fill the PR template's build line, test line, and the probe figures from Step 3.

---

## Notes for the executor

- **The `Corner`/`Original` capture order in Task 5 step 9 matters.** The lane split adds and
  removes edges; `Original`, `PositionA`, `PositionB` and `Corner` are values captured from
  the ROAD edge before that, and the lane split never touches the road edge — but re-reading
  `Found` after the split would be a dangling pointer.
- **`Link.At` is deliberately rewritten** by the lane split, because `LeadRoom` below measures
  from the link's node to the corner and the node did not exist until then.
- **If the automation tree drops a test**, the run count is the only thing that catches it —
  see the `unreal-automation-test-tree-drops-bare-parent` note. Every leaf name added here is
  distinct and none is a bare parent of another.
- **Do not weaken either invariant.** The surface model's bitwise weld and the guideline
  graph's single sampling are untouched by this work; if a tolerance starts looking necessary
  in `GuidelineGeom`, something else is wrong.
