# The Flight Board and the Offer Inbox — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** An offer appears in an inbox; the player accepts it; a stand is reserved; at the
offer's ETA an aeroplane lands and flies the cycle that already works.

**Architecture:** Four world-free `Model/` objects in the AirportOps plugin (`UFlight`,
`UFlightBoard`, `UOfferGenerator`, `UStandAllocator`) hung off `UOpsRuntime` by forwarding,
exactly as `UFuelService` is. Capability is the EXISTING `AirsideCapability::Summarise` and
`ArrivalPlanner::Plan` asked speculatively, never a second evaluator. A stand is reserved by
a claim in the occupancy table the planner already honours. Presentation is MVVM viewmodels
in the game module driving a `UListView`.

**Tech Stack:** UE 5.8.2 C++; `ModelViewViewModel` (Beta) for the UI bindings; UMG;
`IMPLEMENT_SIMPLE_AUTOMATION_TEST` under `WITH_DEV_AUTOMATION_TESTS`.

**Spec:** `docs/superpowers/specs/2026-09-11-flight-board-and-offers-design.md` — read it
first. Every decision below is D1–D10 there.

## Global Constraints

- **Airside `Model/` includes nothing above it**, and AirportOps includes Airside, never the
  reverse. `Tools/Check-Architecture.ps1` fails the test run otherwise.
- **`Solve/` is `CoreMinimal.h` only.** Nothing in this plan belongs there.
- **A phase is an enum, never a set of bools** (`EFlightPhase`, D9).
- **Comments explain WHY**, and especially why an obvious alternative was rejected. Match the
  surrounding density; do not strip it.
- **Every `UE_LOG` is a feature.** `LogAirportOps` in the plugin, `LogRoadBuild` in the game
  module. Do not add a log line that describes a mechanism you have not written.
- **Name leaf tests distinctly.** UE's automation tree drops a bare-named test once a dotted
  child exists, and only the run count notices. `AirportOps.Model.Flight.Phases`, never a
  bare `AirportOps.Model.Flight` beside it.
- **A new test .cpp needs two builds**: the first reports `Result: Succeeded` without
  compiling it. Build twice before believing a new test file is green.
- **The editor must be CLOSED to build**, except in a git worktree (add
  `-NoHotReloadFromIDE` there, never on the checkout the editor has open).
- **Never trust the runner's exit code.** Read `N test(s) run, N failed, N crashed`.
- Build: `D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development -Project="C:\repos\AirportMgr2\AirportMgr.uproject" -WaitMutex`
- Tests: `./Tools/Run-AirsideTests.ps1` (add `-Filter AirportOps` to narrow).

## Spec refinement found while planning

D2 says the allocator "reserves by making exactly that claim". It cannot, as written:
`UGroundTraffic::GetOccupancy()` is const and `OccupancyForTest()` says in its own name that
nothing in production may call it. **Task 1 adds a named production API to Airside**
(`HoldStand` / `ReleaseHold`) and every later task goes through it. The spec's intent is
unchanged — one table, the claim the planner already honours — but the door is explicit.

## File Structure

| File | Responsibility |
|---|---|
| `Plugins/Airside/.../Public/Model/GroundTraffic.h` (modify) | `HoldStand` / `ReleaseHold` / `ReapplyHolds`: a stand held by something that is not an agent |
| `Plugins/AirportOps/.../Public/Model/AirlineDefinition.h` (create) | `UAirlineDefinition` — name, fleet, offer weight |
| `Plugins/AirportOps/.../Public/Model/Flight.h` (create) | `EFlightPhase`, `UFlight`, and the `EAgentPhase` mapping |
| `Plugins/AirportOps/.../Public/Model/StandAllocator.h` (create) | Choose and reserve a stand; release it |
| `Plugins/AirportOps/.../Public/Model/OfferGenerator.h` (create) | Capability filter, ETA, expiry |
| `Plugins/AirportOps/.../Public/Model/FlightBoard.h` (create) | Owns flights; accept/decline; schedules and dispatches; maps agent phases |
| `Plugins/AirportOps/.../Public/Present/OpsRuntime.h` (modify) | Two new subobjects, forwarded |
| `Plugins/AirportOps/.../Public/Model/OpsSave.h` (modify) | Snapshot v2 carries flights |
| `Source/AirportMgr/OfferViewModels.h` (create) | `UOfferViewModel`, `UOfferInboxViewModel` |
| `Source/AirportMgr/OfferInboxWidget.h` (create) | The `UListView` panel and its bottom-bar section |
| `Source/AirportMgr/RoadBuildController.cpp` (modify) | Key `7` creates a flight instead of dispatching |

---

### Task 1: Airside grows a named hold for a stand

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Model/GroundTraffic.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Model/GroundTraffic.cpp`
- Test: `Plugins/Airside/Source/AirsideTests/Private/StandHoldTest.cpp` (create)

**Interfaces:**
- Consumes: `FTrafficOccupancy::TryClaim`, `::Release`, `::IsHeld`; `FTrafficResource::OfNode`.
- Produces:
  - `bool UGroundTraffic::HoldStand(int32 HolderId, FGuidelineNodeId PoseNode)`
  - `void UGroundTraffic::ReleaseHold(int32 HolderId)`
  - `bool UGroundTraffic::IsStandHeld(FGuidelineNodeId PoseNode, int32 ExcludingHolder) const`

- [ ] **Step 1: Write the failing test**

Create `Plugins/Airside/Source/AirsideTests/Private/StandHoldTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadNetwork.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandHoldReservesTest,
	"Airside.Traffic.StandHoldReserves",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandHoldReservesTest::RunTest(const FString& Parameters)
{
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>();
	const FGuidelineNodeId Stand = FGuidelineNodeId(7);

	TestTrue(TEXT("a free stand is held once asked for"), Traffic->HoldStand(-1, Stand));
	TestTrue(TEXT("the hold is visible to anyone but the holder"),
		Traffic->IsStandHeld(Stand, 0));
	TestFalse(TEXT("the holder does not see its own hold"),
		Traffic->IsStandHeld(Stand, -1));
	TestFalse(TEXT("a second holder is refused the same stand"),
		Traffic->HoldStand(-2, Stand));

	Traffic->ReleaseHold(-1);
	TestFalse(TEXT("releasing frees it for the next holder"), Traffic->IsStandHeld(Stand, 0));
	TestTrue(TEXT("and the next holder gets it"), Traffic->HoldStand(-2, Stand));
	return true;
}

#endif
```

- [ ] **Step 2: Run it to verify it fails**

Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside.Traffic.StandHoldReserves`
Expected: compile error, `HoldStand` is not a member of `UGroundTraffic`. That is the
failure; a new test .cpp also needs the second build before it is seen at all.

- [ ] **Step 3: Add the API**

In `GroundTraffic.h`, in the public section beside `RetireAgent`:

```cpp
	/**
	 * Hold a stand for something that is not an agent yet - an accepted flight, hours before
	 * it is dispatched.
	 *
	 * THE SAME TABLE AND THE SAME CLAIM the planner already honours: ArrivalPlanner::
	 * ChooseStand skips a stand whose PoseNode is held, and FTrafficOccupancy::IsHeld counts
	 * a reservation (bOccupied false) as held. A separate reservation table would be a second
	 * source of truth and the two would drift the first time a stand was freed in one.
	 *
	 * HolderId is NOT an agent id. Agent ids are allocated NextAgentId++ from 1, so callers
	 * pass a NEGATIVE id (AirportOps passes the negative of the flight id) and the two spaces
	 * cannot collide without a registry to keep in step.
	 *
	 * Returns false if someone else already holds it, in which case nothing was changed.
	 */
	bool HoldStand(int32 HolderId, FGuidelineNodeId PoseNode);

	/** Give back every hold made by HolderId. Bodies are untouched: a hold is never occupied. */
	void ReleaseHold(int32 HolderId);

	/** Whether any holder but ExcludingHolder holds this stand. */
	bool IsStandHeld(FGuidelineNodeId PoseNode, int32 ExcludingHolder) const;
```

In `GroundTraffic.cpp`:

```cpp
bool UGroundTraffic::HoldStand(int32 HolderId, FGuidelineNodeId PoseNode)
{
	FTrafficClaim Claim;
	Claim.AgentId = HolderId;
	Claim.Resource = FTrafficResource::OfNode(PoseNode);
	Claim.bOccupied = false;  // A reservation. Nothing's body is at a stand hours before it lands.

	FTrafficClaim Blocker;
	return Occupancy.TryClaim(Claim, Blocker) == EClaimResult::Granted;
}

void UGroundTraffic::ReleaseHold(int32 HolderId)
{
	// ReleaseReservations and not ReleaseAll: the test is bOccupied, and a hold is never
	// occupied, so the two agree here - but ReleaseAll would also take a body if a caller
	// ever passed a real agent id by mistake, and that is the bug that does not announce
	// itself.
	Occupancy.ReleaseReservations(HolderId);
}

bool UGroundTraffic::IsStandHeld(FGuidelineNodeId PoseNode, int32 ExcludingHolder) const
{
	return Occupancy.IsHeld(FTrafficResource::OfNode(PoseNode), ExcludingHolder);
}
```

- [ ] **Step 4: Run it to verify it passes**

Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside.Traffic.StandHoldReserves`
Expected: `1 test(s) run, 0 failed, 0 crashed.`

- [ ] **Step 5: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Model/GroundTraffic.h `
  Plugins/Airside/Source/Airside/Private/Model/GroundTraffic.cpp `
  Plugins/Airside/Source/AirsideTests/Private/StandHoldTest.cpp
git commit -m "feat(model): a stand can be held by something that is not an agent yet"
```

---

### Task 2: `UAirlineDefinition`

**Files:**
- Create: `Plugins/AirportOps/Source/AirportOps/Public/Model/AirlineDefinition.h`
- Modify: `Config/DefaultGame.ini`
- Test: `Plugins/AirportOps/Source/AirportOpsTests/Private/AirlineDefinitionTest.cpp` (create)

**Interfaces:**
- Consumes: `UOpsDefinition` (base), `UOpsCatalog::All<T>()`, `UAircraftType`.
- Produces: `UAirlineDefinition` with `DisplayName` (`FText`), `Fleet`
  (`TArray<TObjectPtr<UAircraftType>>`), `OfferWeight` (`double`), `OffersPerDay` (`double`).

- [ ] **Step 1: Write the failing test**

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Entities/AircraftType.h"
#include "Model/AirlineDefinition.h"
#include "Model/OpsCatalog.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAirlineDefinitionCatalogTest,
	"AirportOps.Content.AirlineDefinition.ReachesTheCatalog",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAirlineDefinitionCatalogTest::RunTest(const FString& Parameters)
{
	UOpsCatalog* Catalog = NewObject<UOpsCatalog>();
	UAirlineDefinition* Airline = NewObject<UAirlineDefinition>();
	Airline->DisplayName = FText::FromString(TEXT("Meridian"));
	Airline->Fleet.Add(NewObject<UAircraftType>());
	Catalog->Add(Airline);

	const TArray<UAirlineDefinition*> Found = Catalog->All<UAirlineDefinition>();
	TestEqual(TEXT("the catalog returns the airline it was given"), Found.Num(), 1);
	TestEqual(TEXT("and its fleet survived"), Found.Num() > 0 ? Found[0]->Fleet.Num() : 0, 1);
	return true;
}

#endif
```

- [ ] **Step 2: Run it to verify it fails**

Run: `./Tools/Run-AirsideTests.ps1 -Filter AirportOps.Content.AirlineDefinition`
Expected: compile error, `AirlineDefinition.h` not found.

- [ ] **Step 3: Write the header**

```cpp
#pragma once

#include "CoreMinimal.h"
#include "Model/OpsDefinition.h"

#include "AirlineDefinition.generated.h"

class UAircraftType;

/**
 * One airline: who offers flights, in what, and how often.
 *
 * A DEFINITION ASSET and not a table in a scenario, because UOpsDefinition's own header
 * already names airlines as an intended subclass and because the fleet is a list of
 * UAircraftType assets - a content reference, which is exactly what a data asset is for.
 */
UCLASS(BlueprintType)
class AIRPORTOPS_API UAirlineDefinition : public UOpsDefinition
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "Airline") FText DisplayName;

	/** Every type this airline may send. An offer picks one of these that the airport admits. */
	UPROPERTY(EditAnywhere, Category = "Airline") TArray<TObjectPtr<UAircraftType>> Fleet;

	/** Relative weight against other airlines when an offer is generated. */
	UPROPERTY(EditAnywhere, Category = "Airline", meta = (ClampMin = "0.0"))
	double OfferWeight = 1.0;

	/** Offers per GAME day at full capability. The generator scales this; see UOfferGenerator. */
	UPROPERTY(EditAnywhere, Category = "Airline", meta = (ClampMin = "0.0"))
	double OffersPerDay = 6.0;
};
```

- [ ] **Step 4: Register the asset type**

`UOpsDefinition::GetPrimaryAssetId` derives the type from the class name minus its prefix, so
without this entry the catalog loads NOTHING and reports no airlines at all — with no error.
In `Config/DefaultGame.ini`, beside the existing `+PrimaryAssetTypesToScan` lines:

```ini
+PrimaryAssetTypesToScan=(PrimaryAssetType="AirlineDefinition",AssetBaseClass=/Script/AirportOps.AirlineDefinition,bHasBlueprintClasses=False,bIsEditorOnly=False,Directories=((Path="/Game/Entities")),SpecificAssets=,Rules=(Priority=-1,ChunkId=-1,bApplyRecursively=True,CookRule=AlwaysCook))
```

Copy the exact bracket shape from the line already there for `AircraftType` or `Scenario`;
if the existing entries use a different `Directories` path, match it rather than this one.

- [ ] **Step 5: Run it to verify it passes**

Run: `./Tools/Run-AirsideTests.ps1 -Filter AirportOps.Content.AirlineDefinition`
Expected: `1 test(s) run, 0 failed, 0 crashed.`

- [ ] **Step 6: Commit**

```bash
git add Plugins/AirportOps/Source/AirportOps/Public/Model/AirlineDefinition.h `
  Plugins/AirportOps/Source/AirportOpsTests/Private/AirlineDefinitionTest.cpp Config/DefaultGame.ini
git commit -m "feat(entities): an airline is a definition asset with a fleet"
```

---

### Task 3: `EFlightPhase` and `UFlight`

**Files:**
- Create: `Plugins/AirportOps/Source/AirportOps/Public/Model/Flight.h`
- Create: `Plugins/AirportOps/Source/AirportOps/Private/Model/Flight.cpp`
- Test: `Plugins/AirportOps/Source/AirportOpsTests/Private/FlightTest.cpp` (create)

**Interfaces:**
- Consumes: `EAgentPhase` (Airside), `FEntityInstanceId`, `UAircraftType`, `UAirlineDefinition`.
- Produces:
  - `enum class EFlightPhase : uint8`
  - `UFlight` with `Id`, `Airline`, `Type`, `Phase`, `ArrivesAt`, `OffBlockAt`, `ExpiresAt`,
    `Stand`, `AgentId`, `LandingFee`, `ParkingFee`
  - `int32 UFlight::HolderId() const` — the negative id used for a stand hold
  - `EFlightPhase FlightPhaseFromAgent(EAgentPhase To, EFlightPhase Current)`

- [ ] **Step 1: Write the failing tests**

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/Flight.h"
#include "Model/RoadAgent.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlightPhaseMappingTest,
	"AirportOps.Model.Flight.PhaseFromAgent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFlightPhaseMappingTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("an arriving agent is a landing flight"),
		FlightPhaseFromAgent(EAgentPhase::Arriving, EFlightPhase::Inbound), EFlightPhase::Landing);

	// The SAME agent phase on either side of the stand. This is the whole reason the mapping
	// takes the current flight phase: Taxiing alone cannot say which way the aeroplane is going.
	TestEqual(TEXT("taxiing before the stand is TaxiIn"),
		FlightPhaseFromAgent(EAgentPhase::Taxiing, EFlightPhase::Landing), EFlightPhase::TaxiIn);
	TestEqual(TEXT("taxiing after the turnaround is TaxiOut"),
		FlightPhaseFromAgent(EAgentPhase::Taxiing, EFlightPhase::Turnaround), EFlightPhase::TaxiOut);

	TestEqual(TEXT("parked is the turnaround"),
		FlightPhaseFromAgent(EAgentPhase::Parked, EFlightPhase::TaxiIn), EFlightPhase::Turnaround);
	TestEqual(TEXT("departing is departing"),
		FlightPhaseFromAgent(EAgentPhase::Departing, EFlightPhase::TaxiOut), EFlightPhase::Departing);
	TestEqual(TEXT("gone is departed"),
		FlightPhaseFromAgent(EAgentPhase::Gone, EFlightPhase::Departing), EFlightPhase::Departed);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlightHolderIdTest,
	"AirportOps.Model.Flight.HolderIdCannotCollideWithAnAgent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFlightHolderIdTest::RunTest(const FString& Parameters)
{
	UFlight* Flight = NewObject<UFlight>();
	Flight->Id = 1;
	// Agent ids are allocated NextAgentId++ from 1. A holder id must never be one of those.
	TestTrue(TEXT("a holder id is negative"), Flight->HolderId() < 0);
	TestEqual(TEXT("and is the negative of the flight id"), Flight->HolderId(), -1);
	return true;
}

#endif
```

- [ ] **Step 2: Run to verify they fail**

Run: `./Tools/Run-AirsideTests.ps1 -Filter AirportOps.Model.Flight`
Expected: compile error, `Model/Flight.h` not found.

- [ ] **Step 3: Write the header**

```cpp
#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"
#include "UObject/Object.h"

#include "Flight.generated.h"

class UAircraftType;
class UAirlineDefinition;
enum class EAgentPhase : uint8;

/**
 * Where one flight has got to.
 *
 * AN ENUM, NEVER A SET OF BOOLS: "offered" and "inbound" can never both be true, and the
 * states are visited in one order. Pushback, Diverted and Cancelled are deliberately absent -
 * see the spec's D9. A phase nothing can enter is a lie in an enum.
 */
UENUM()
enum class EFlightPhase : uint8
{
	/** In the inbox, undecided. Expires at ExpiresAt. */
	Offered,
	/** The player said yes. A stand is held and the arrival is on the clock. */
	Accepted,
	/** Between accept and the ETA. Nothing is in the world yet. */
	Inbound,
	Landing,
	TaxiIn,
	Turnaround,
	TaxiOut,
	Departing,
	Departed,
	/** The player said no. */
	Declined,
	/** Nobody said anything and the offer timed out. */
	Expired
};

/**
 * One flight, from the offer to the departure.
 *
 * A UObject and not a struct, because UListView::SetListItems takes UObject* and the inbox
 * binds a viewmodel per row; a struct would need an adapter object per row anyway.
 */
UCLASS()
class AIRPORTOPS_API UFlight : public UObject
{
	GENERATED_BODY()

public:
	/** Ids start at 1. HolderId() depends on that, so never renumber from 0. */
	UPROPERTY() int32 Id = 0;

	UPROPERTY() TObjectPtr<UAirlineDefinition> Airline = nullptr;
	UPROPERTY() TObjectPtr<UAircraftType> Type = nullptr;
	UPROPERTY() EFlightPhase Phase = EFlightPhase::Offered;

	/** USimClock::Now at which it lands. Saved, because the clock does NOT save its queue. */
	UPROPERTY() double ArrivesAt = 0.0;

	/** USimClock::Now at which it is due off the stand. Set from the type's TurnaroundSeconds. */
	UPROPERTY() double OffBlockAt = 0.0;

	/** USimClock::Now at which an unanswered offer lapses. */
	UPROPERTY() double ExpiresAt = 0.0;

	/**
	 * The stand HELD from Accept, and the stand actually parked on once it has landed.
	 *
	 * The two can differ: the hold guarantees A stand exists, and ArrivalPlanner then picks
	 * the nearest free one, which may be a different one if a nearer stand freed meanwhile.
	 * The board overwrites this at Parked from the agent's own GoalNode, so what is saved is
	 * always the stand the aeroplane is on.
	 */
	UPROPERTY() FEntityInstanceId Stand;

	/** The live agent, or INDEX_NONE before dispatch and after it is gone. */
	UPROPERTY() int32 AgentId = INDEX_NONE;

	/** Banked by the ledger in slice C. Computed now so that slice is a column and a post. */
	UPROPERTY() double LandingFee = 0.0;
	UPROPERTY() double ParkingFee = 0.0;

	/**
	 * The id this flight holds a stand under.
	 *
	 * NEGATIVE, because UGroundTraffic allocates agent ids NextAgentId++ from 1 and the
	 * occupancy table is mechanism, not policy - it never asks what an agent is. The sign is
	 * what keeps the two id spaces disjoint without a registry to keep in step.
	 */
	int32 HolderId() const { return -Id; }
};

/**
 * The flight phase an agent phase implies, given where the flight had got to.
 *
 * TAKES THE CURRENT PHASE because EAgentPhase::Taxiing happens twice - once to the stand and
 * once away from it - and the agent cannot tell them apart. Everything else is a plain map.
 */
AIRPORTOPS_API EFlightPhase FlightPhaseFromAgent(EAgentPhase To, EFlightPhase Current);
```

- [ ] **Step 4: Write the implementation**

`Flight.cpp`:

```cpp
#include "Model/Flight.h"

#include "Model/RoadAgent.h"

EFlightPhase FlightPhaseFromAgent(EAgentPhase To, EFlightPhase Current)
{
	switch (To)
	{
	case EAgentPhase::Arriving:
		return EFlightPhase::Landing;
	case EAgentPhase::Taxiing:
		// Parked or later means this is the taxi OUT. Anything earlier is the taxi in.
		return Current >= EFlightPhase::Turnaround ? EFlightPhase::TaxiOut : EFlightPhase::TaxiIn;
	case EAgentPhase::Parked:
		return EFlightPhase::Turnaround;
	case EAgentPhase::Departing:
		return EFlightPhase::Departing;
	case EAgentPhase::Gone:
		return EFlightPhase::Departed;
	default:
		return Current;
	}
}
```

The `>=` reads the declaration order of `EFlightPhase`, so the enum's order is load-bearing.
That is why `Turnaround` sits between `TaxiIn` and `TaxiOut` in the declaration and must stay
there; a comment to that effect goes on the enum if anyone reorders it.

- [ ] **Step 5: Run to verify they pass**

Run: `./Tools/Run-AirsideTests.ps1 -Filter AirportOps.Model.Flight`
Expected: `2 test(s) run, 0 failed, 0 crashed.`

- [ ] **Step 6: Commit**

```bash
git add Plugins/AirportOps/Source/AirportOps/Public/Model/Flight.h `
  Plugins/AirportOps/Source/AirportOps/Private/Model/Flight.cpp `
  Plugins/AirportOps/Source/AirportOpsTests/Private/FlightTest.cpp
git commit -m "feat(model): a flight is a thing, and an agent phase maps onto it"
```

---

### Task 4: `UStandAllocator`

**Files:**
- Create: `Plugins/AirportOps/Source/AirportOps/Public/Model/StandAllocator.h`
- Create: `Plugins/AirportOps/Source/AirportOps/Private/Model/StandAllocator.cpp`
- Test: `Plugins/AirportOps/Source/AirportOpsTests/Private/StandAllocatorTest.cpp` (create)

**Interfaces:**
- Consumes: Task 1's `UGroundTraffic::HoldStand`/`ReleaseHold`/`IsStandHeld`; `UFlight::HolderId`;
  `URoadNetwork::GetEntities`, `::EntityIdAt`, `::GetEntity`; `FEntityInstance::PoseNode`,
  `::DesignWingspan`, `::bAlive`.
- Produces:
  - `bool UStandAllocator::Reserve(UGroundTraffic&, const URoadNetwork&, UFlight&)`
  - `void UStandAllocator::Release(UGroundTraffic&, UFlight&)`
  - `void UStandAllocator::Reapply(UGroundTraffic&, const URoadNetwork&, const TArray<UFlight*>&)`

- [ ] **Step 1: Write the failing tests**

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Entities/AircraftType.h"
#include "Model/Flight.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadNetwork.h"
#include "Model/StandAllocator.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** A network with N stands, each sized for Wingspan, at increasing pose nodes. */
	URoadNetwork* NetworkWithStands(const TArray<double>& Wingspans);
	UFlight* FlightNeeding(double Wingspan, int32 Id);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandAllocatorSmallestFitTest,
	"AirportOps.Model.StandAllocator.TakesTheSmallestThatFits",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandAllocatorSmallestFitTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Network = NetworkWithStands({6000.0, 3600.0, 5200.0});
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>();
	UStandAllocator* Allocator = NewObject<UStandAllocator>();
	UFlight* Flight = FlightNeeding(3400.0, 1);

	TestTrue(TEXT("a fitting stand is reserved"), Allocator->Reserve(*Traffic, *Network, *Flight));
	const FEntityInstance* Chosen = Network->GetEntity(Flight->Stand);
	TestNotNull(TEXT("the reservation names a live stand"), Chosen);
	TestEqual(TEXT("the SMALLEST stand that admits it, not the first"),
		Chosen != nullptr ? Chosen->DesignWingspan : 0.0, 3600.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandAllocatorNeverDoubleBooksTest,
	"AirportOps.Model.StandAllocator.NeverDoubleBooks",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandAllocatorNeverDoubleBooksTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Network = NetworkWithStands({3600.0});
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>();
	UStandAllocator* Allocator = NewObject<UStandAllocator>();
	UFlight* First = FlightNeeding(3400.0, 1);
	UFlight* Second = FlightNeeding(3400.0, 2);

	TestTrue(TEXT("the first flight gets the only stand"),
		Allocator->Reserve(*Traffic, *Network, *First));
	TestFalse(TEXT("the second is refused rather than given the same stand"),
		Allocator->Reserve(*Traffic, *Network, *Second));

	Allocator->Release(*Traffic, *First);
	TestTrue(TEXT("and gets it once the first lets go"),
		Allocator->Reserve(*Traffic, *Network, *Second));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandAllocatorSurvivesRebuildTest,
	"AirportOps.Model.StandAllocator.SurvivesAGraphRebuild",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandAllocatorSurvivesRebuildTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Network = NetworkWithStands({3600.0});
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>();
	UStandAllocator* Allocator = NewObject<UStandAllocator>();
	UFlight* Flight = FlightNeeding(3400.0, 1);
	TestTrue(TEXT("reserved before the edit"), Allocator->Reserve(*Traffic, *Network, *Flight));

	// GroundTrafficRebuild drops EVERY node claim: "a set of resources ceasing to exist".
	// Without Reapply the player editing a taxiway silently un-reserves every stand.
	Traffic->OnGraphRebuilt(*Network);
	Allocator->Reapply(*Traffic, *Network, {Flight});

	UFlight* Rival = FlightNeeding(3400.0, 2);
	TestFalse(TEXT("the reservation still keeps a rival off the stand after a rebuild"),
		Allocator->Reserve(*Traffic, *Network, *Rival));
	return true;
}

#endif
```

The two helpers, written out (this is the shape `ServiceLinkTest.cpp`'s own `PlaceStand`
uses, checked 2026-09-11):

```cpp
namespace
{
	/** One stand per wingspan, spaced so their pose nodes differ. */
	URoadNetwork* NetworkWithStands(const TArray<double>& Wingspans)
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
		double X = 0.0;
		for (const double Wingspan : Wingspans)
		{
			// The design wingspan is PlaceEntity's OWN argument, so one definition stands in
			// for stands of several sizes. A UEntityDefinition per size would test nothing
			// extra and would drift from the content the game actually ships.
			Net->PlaceEntity(Stand, Stand->Anchors, FVector2D(X, 0.0), 0.0, Wingspan,
				Stand->PoseRole, Stand->Trucks);
			X += 20000.0;
		}
		return Net;
	}

	UFlight* FlightNeeding(double Wingspan, int32 Id)
	{
		UAircraftType* Type = NewObject<UAircraftType>(GetTransientPackage());
		Type->Footprint.Wingspan = Wingspan;
		UFlight* Flight = NewObject<UFlight>(GetTransientPackage());
		Flight->Id = Id;
		Flight->Type = Type;
		return Flight;
	}
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `./Tools/Run-AirsideTests.ps1 -Filter AirportOps.Model.StandAllocator`
Expected: compile error, `Model/StandAllocator.h` not found.

- [ ] **Step 3: Write the header**

```cpp
#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "StandAllocator.generated.h"

class UFlight;
class UGroundTraffic;
class URoadNetwork;

/**
 * Which stand a flight gets, and holding it so nobody else does.
 *
 * FIRST FIT BY SIZE, SMALLEST THAT ADMITS: a Code C stand is wasted on a Piper while a 737
 * waits for it, and the smallest-fit rule is the cheapest statement of "do not waste the big
 * one". Spec 3.4. The schedule grid supersedes this for contracted flights in M5.
 *
 * IT HOLDS, IT DOES NOT CHOOSE THE ARRIVAL'S STAND. ArrivalPlanner picks the stand the
 * aeroplane actually taxis to, from the nearest free one at the moment it lands. The hold
 * guarantees that A stand exists for this flight; UFlight::Stand's comment says why the two
 * can differ and who fixes it up.
 */
UCLASS()
class AIRPORTOPS_API UStandAllocator : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Hold the smallest live stand whose DesignWingspan admits this flight's type.
	 *
	 * Writes UFlight::Stand and returns true, or changes nothing and returns false when every
	 * fitting stand is already held - which is the refusal the inbox shows as "no stand free".
	 */
	bool Reserve(UGroundTraffic& Traffic, const URoadNetwork& Network, UFlight& Flight);

	/** Give the hold back. Safe to call on a flight that never had one. */
	void Release(UGroundTraffic& Traffic, UFlight& Flight);

	/**
	 * Re-make every hold after a graph rebuild.
	 *
	 * UGroundTraffic::OnGraphRebuilt goes through FTrafficOccupancy::ReleaseGuidelineClaims,
	 * which removes every Edge and Node claim because the resources themselves have ceased to
	 * exist. Holds are Node claims, so they go with them, and nothing announces it to a
	 * caller. The flight's saved truth is the stand ENTITY, so the node can be looked up
	 * again on the new graph.
	 */
	void Reapply(UGroundTraffic& Traffic, const URoadNetwork& Network,
		const TArray<UFlight*>& Held);
};
```

- [ ] **Step 4: Write the implementation**

```cpp
#include "Model/StandAllocator.h"

#include "AirportOpsLog.h"
#include "Entities/AircraftType.h"
#include "Model/Flight.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"

bool UStandAllocator::Reserve(UGroundTraffic& Traffic, const URoadNetwork& Network, UFlight& Flight)
{
	if (Flight.Type == nullptr)
	{
		return false;
	}
	const double Wingspan = Flight.Type->Footprint.Wingspan;

	FEntityInstanceId Best;
	double BestWingspan = TNumericLimits<double>::Max();
	const TArray<FEntityInstance>& Entities = Network.GetEntities();
	for (int32 Index = 0; Index < Entities.Num(); ++Index)
	{
		const FEntityInstance& Stand = Entities[Index];
		if (!Stand.bAlive || !Stand.PoseNode.IsSet() || Stand.DesignWingspan < Wingspan)
		{
			continue;
		}
		// Held is asked of the table, not of our own book-keeping: a stand with an aeroplane
		// ON it is held by that agent, and a stand promised to another flight by that
		// flight's holder id. One question covers both.
		if (Traffic.IsStandHeld(Stand.PoseNode, Flight.HolderId()))
		{
			continue;
		}
		if (Stand.DesignWingspan < BestWingspan)
		{
			BestWingspan = Stand.DesignWingspan;
			Best = Network.EntityIdAt(Index);
		}
	}

	if (!Best.IsSet())
	{
		return false;
	}
	const FEntityInstance* Chosen = Network.GetEntity(Best);
	if (Chosen == nullptr || !Traffic.HoldStand(Flight.HolderId(), Chosen->PoseNode))
	{
		return false;
	}
	Flight.Stand = Best;
	UE_LOG(LogAirportOps, Log, TEXT("Flight %d holds stand %d (%.0f uu stand for a %.0f uu span)"),
		Flight.Id, Best.Index, BestWingspan, Wingspan);
	return true;
}

void UStandAllocator::Release(UGroundTraffic& Traffic, UFlight& Flight)
{
	Traffic.ReleaseHold(Flight.HolderId());
}

void UStandAllocator::Reapply(UGroundTraffic& Traffic, const URoadNetwork& Network,
	const TArray<UFlight*>& Held)
{
	for (UFlight* Flight : Held)
	{
		if (Flight == nullptr || !Flight->Stand.IsSet())
		{
			continue;
		}
		const FEntityInstance* Stand = Network.GetEntity(Flight->Stand);
		if (Stand == nullptr || !Stand->PoseNode.IsSet())
		{
			// The stand was deleted under an accepted flight. Not this slice's problem to
			// solve, but it MUST say so: silence here is a flight that lands on a stand
			// nobody is holding.
			UE_LOG(LogAirportOps, Warning, TEXT("Flight %d held stand %d, which is gone"),
				Flight->Id, Flight->Stand.Index);
			continue;
		}
		Traffic.HoldStand(Flight->HolderId(), Stand->PoseNode);
	}
}
```

If `FEntityInstanceId` has no `Index` member, log whatever field it does expose — check
`Model/RoadHandles.h` and match the existing `%d` uses in `FuelService.cpp`, which already
log `Demand->Stand.Index`.

- [ ] **Step 5: Run to verify they pass**

Run: `./Tools/Run-AirsideTests.ps1 -Filter AirportOps.Model.StandAllocator`
Expected: `3 test(s) run, 0 failed, 0 crashed.`

- [ ] **Step 6: Commit**

```bash
git add Plugins/AirportOps/Source/AirportOps/Public/Model/StandAllocator.h `
  Plugins/AirportOps/Source/AirportOps/Private/Model/StandAllocator.cpp `
  Plugins/AirportOps/Source/AirportOpsTests/Private/StandAllocatorTest.cpp
git commit -m "feat(model): a flight holds the smallest stand that admits it"
```

---

### Task 5: `UOfferGenerator`

**Files:**
- Create: `Plugins/AirportOps/Source/AirportOps/Public/Model/OfferGenerator.h`
- Create: `Plugins/AirportOps/Source/AirportOps/Private/Model/OfferGenerator.cpp`
- Test: `Plugins/AirportOps/Source/AirportOpsTests/Private/OfferGeneratorTest.cpp` (create)

**Interfaces:**
- Consumes: `AirsideCapability::Summarise` → `FAirsideCapability` (`Runways[].Length`,
  `LongestRunway()`, `Stands[].DesignWingspan`); `UAirlineDefinition`; `UAircraftType::Requirements`
  (`LandingFieldLength`), `::Footprint.Wingspan`, `::TurnaroundSeconds`; `USimClock::Now`.
- Produces:
  - `UFlight* UOfferGenerator::MakeOffer(const FAirsideCapability&, UAirlineDefinition&, double Now, int32 NextId)`
  - `bool UOfferGenerator::AirportAdmits(const FAirsideCapability&, const UAircraftType&)`
  - `double UOfferGenerator::LeadTimeSeconds = 900.0`, `OfferLifeSeconds = 600.0`

- [ ] **Step 1: Write the failing tests**

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Entities/AircraftType.h"
#include "Model/AirlineDefinition.h"
#include "Model/AirsideCapability.h"
#include "Model/Flight.h"
#include "Model/OfferGenerator.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FAirsideCapability AirportWith(double RunwayLength, double StandWingspan)
	{
		FAirsideCapability Out;
		FRunwaySummary Runway;
		Runway.Length = RunwayLength;
		Out.Runways.Add(Runway);
		FStandSummary Stand;
		Stand.DesignWingspan = StandWingspan;
		Out.Stands.Add(Stand);
		return Out;
	}

	UAircraftType* TypeNeeding(double FieldLength, double Wingspan)
	{
		UAircraftType* Type = NewObject<UAircraftType>();
		Type->Requirements.LandingFieldLength = FieldLength;
		Type->Footprint.Wingspan = Wingspan;
		return Type;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOfferGeneratorShortRunwayTest,
	"AirportOps.Model.OfferGenerator.AShortRunwayIsNotOffered",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOfferGeneratorShortRunwayTest::RunTest(const FString& Parameters)
{
	const FAirsideCapability Airport = AirportWith(90000.0, 3600.0);
	TestFalse(TEXT("a type needing more runway than exists is not offered"),
		UOfferGenerator::AirportAdmits(Airport, *TypeNeeding(200000.0, 3000.0)));
	TestTrue(TEXT("a type that fits the runway and a stand is offered"),
		UOfferGenerator::AirportAdmits(Airport, *TypeNeeding(60000.0, 3000.0)));
	TestFalse(TEXT("a type wider than every stand is not offered, however long the runway"),
		UOfferGenerator::AirportAdmits(Airport, *TypeNeeding(60000.0, 6500.0)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOfferGeneratorEtaTest,
	"AirportOps.Model.OfferGenerator.AnOfferCarriesAnEtaAndAnExpiry",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOfferGeneratorEtaTest::RunTest(const FString& Parameters)
{
	UOfferGenerator* Generator = NewObject<UOfferGenerator>();
	UAirlineDefinition* Airline = NewObject<UAirlineDefinition>();
	UAircraftType* Type = TypeNeeding(60000.0, 3000.0);
	Type->TurnaroundSeconds = 1800.0;
	Airline->Fleet.Add(Type);

	UFlight* Offer = Generator->MakeOffer(AirportWith(90000.0, 3600.0), *Airline, 1000.0, 1);
	TestNotNull(TEXT("an admissible fleet produces an offer"), Offer);
	if (Offer == nullptr) { return false; }

	TestEqual(TEXT("it is Offered, not Accepted"), Offer->Phase, EFlightPhase::Offered);
	TestEqual(TEXT("the ETA is the lead time out"),
		Offer->ArrivesAt, 1000.0 + Generator->LeadTimeSeconds, 1e-9);
	TestEqual(TEXT("the offer lapses before it would have landed"),
		Offer->ExpiresAt, 1000.0 + Generator->OfferLifeSeconds, 1e-9);
	TestTrue(TEXT("the expiry is BEFORE the ETA, or an offer could lapse mid-approach"),
		Offer->ExpiresAt < Offer->ArrivesAt);
	return true;
}

#endif
```

- [ ] **Step 2: Run to verify they fail**

Run: `./Tools/Run-AirsideTests.ps1 -Filter AirportOps.Model.OfferGenerator`
Expected: compile error, `Model/OfferGenerator.h` not found.

- [ ] **Step 3: Write the header**

```cpp
#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "OfferGenerator.generated.h"

class UAircraftType;
class UAirlineDefinition;
class UFlight;
struct FAirsideCapability;

/**
 * Where offers come from.
 *
 * THE CHEAP HALF OF CAPABILITY. This asks FAirsideCapability - the longest runway and the
 * stands that exist - and nothing else, because it runs on a clock tick over every airline.
 * Whether a given offer can ACTUALLY be accepted is ArrivalPlanner::Plan's answer, asked once
 * when the player looks at the inbox: that one knows about occupancy and routes and costs a
 * search. Two questions, two costs, one evaluator each - see the spec's D1.
 */
UCLASS()
class AIRPORTOPS_API UOfferGenerator : public UObject
{
	GENERATED_BODY()

public:
	/** How far ahead of the offer an accepted flight lands, GAME seconds. */
	UPROPERTY(EditAnywhere, Category = "Offers", meta = (ClampMin = "0.0"))
	double LeadTimeSeconds = 900.0;

	/** How long an unanswered offer stands, GAME seconds. MUST be less than LeadTimeSeconds. */
	UPROPERTY(EditAnywhere, Category = "Offers", meta = (ClampMin = "0.0"))
	double OfferLifeSeconds = 600.0;

	/**
	 * Whether the airfield could take this type at all - runway length and a wide enough
	 * stand. Static: it reads its two arguments and nothing else, and the offer test wants
	 * to call it without a generator.
	 */
	static bool AirportAdmits(const FAirsideCapability& Airport, const UAircraftType& Type);

	/**
	 * One offer from this airline, or nullptr if nothing in its fleet fits the airport.
	 *
	 * NextId is the board's counter; the generator does not own numbering, because the board
	 * is what has to keep ids unique across a save.
	 */
	UFlight* MakeOffer(const FAirsideCapability& Airport, UAirlineDefinition& Airline,
		double Now, int32 NextId);
};
```

- [ ] **Step 4: Write the implementation**

```cpp
#include "Model/OfferGenerator.h"

#include "AirportOpsLog.h"
#include "Entities/AircraftType.h"
#include "Model/AirlineDefinition.h"
#include "Model/AirsideCapability.h"
#include "Model/Flight.h"

bool UOfferGenerator::AirportAdmits(const FAirsideCapability& Airport, const UAircraftType& Type)
{
	// 0 means "no published claim" in FRunwayRequirements, and no length refusal with it.
	const double Needed = Type.Requirements.LandingFieldLength;
	if (Needed > 0.0 && Airport.LongestRunway() < Needed)
	{
		return false;
	}
	for (const FStandSummary& Stand : Airport.Stands)
	{
		if (Stand.DesignWingspan >= Type.Footprint.Wingspan)
		{
			return true;
		}
	}
	return false;
}

UFlight* UOfferGenerator::MakeOffer(const FAirsideCapability& Airport,
	UAirlineDefinition& Airline, double Now, int32 NextId)
{
	TArray<UAircraftType*> Admissible;
	for (const TObjectPtr<UAircraftType>& Type : Airline.Fleet)
	{
		if (Type != nullptr && AirportAdmits(Airport, *Type))
		{
			Admissible.Add(Type);
		}
	}
	if (Admissible.Num() == 0)
	{
		return nullptr;
	}

	UAircraftType* Type = Admissible[FMath::RandHelper(Admissible.Num())];
	UFlight* Offer = NewObject<UFlight>(this);
	Offer->Id = NextId;
	Offer->Airline = &Airline;
	Offer->Type = Type;
	Offer->Phase = EFlightPhase::Offered;
	Offer->ArrivesAt = Now + LeadTimeSeconds;
	Offer->ExpiresAt = Now + OfferLifeSeconds;
	// Off-block is measured from the ETA, not from now: a turnaround starts when it parks.
	Offer->OffBlockAt = Offer->ArrivesAt + Type->TurnaroundSeconds;
	return Offer;
}
```

Fees (D8) are NOT set here — they are one line each when the ledger lands, and a number
nothing reads is a number that will be wrong by then. `UFlight` carries the fields; the
generator leaves them at zero.

- [ ] **Step 5: Run to verify they pass**

Run: `./Tools/Run-AirsideTests.ps1 -Filter AirportOps.Model.OfferGenerator`
Expected: `2 test(s) run, 0 failed, 0 crashed.`

- [ ] **Step 6: Commit**

```bash
git add Plugins/AirportOps/Source/AirportOps/Public/Model/OfferGenerator.h `
  Plugins/AirportOps/Source/AirportOps/Private/Model/OfferGenerator.cpp `
  Plugins/AirportOps/Source/AirportOpsTests/Private/OfferGeneratorTest.cpp
git commit -m "feat(model): offers come from what the airfield can actually take"
```

---

### Task 6: `UFlightBoard`

**Files:**
- Create: `Plugins/AirportOps/Source/AirportOps/Public/Model/FlightBoard.h`
- Create: `Plugins/AirportOps/Source/AirportOps/Private/Model/FlightBoard.cpp`
- Test: `Plugins/AirportOps/Source/AirportOpsTests/Private/FlightBoardTest.cpp` (create)

**Interfaces:**
- Consumes: Tasks 3–5; `USimClock::At`/`Cancel`/`Now`; `UGroundTraffic::FindAgent`;
  `FRoadAgent::GoalNode`; `URoadNetwork::FindEntityIndexByPoseNode`/`EntityIdAt`;
  `ArrivalPlanner::Plan`, `::DescribeRefusal`; `UAircraftType::Airframe()`.
- Produces:
  - `bool UFlightBoard::Accept(UGroundTraffic&, const URoadNetwork&, USimClock&, UFlight&)`
  - `void UFlightBoard::Decline(UFlight&)`
  - `void UFlightBoard::OnAgentPhase(UGroundTraffic&, const URoadNetwork&, int32, EAgentPhase, EAgentPhase)`
  - `void UFlightBoard::Tick(UGroundTraffic&, const URoadNetwork&, USimClock&)`
  - `EArrivalRefusal UFlightBoard::WhyNotAcceptable(UGroundTraffic&, const URoadNetwork&, const UFlight&) const`
  - `TArray<UFlight*> UFlightBoard::Offers() const`, `::Live() const`
  - `FSimpleMulticastDelegate UFlightBoard::OnChanged` — what the viewmodel binds to
  - `TFunction<bool(const FVector2D&, const FAirframe&)> UFlightBoard::Dispatcher` — the seam
    the composition test substitutes; `UOpsRuntime` points it at `ARoadNetworkActor::DispatchArrival`

- [ ] **Step 1: Write the failing tests**

```cpp
// Copy NetworkWithStands and FlightNeeding from Task 4's test file into this one. Two small
// fixtures beat a shared header that both files then have to agree about.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlightBoardAcceptReservesTest,
	"AirportOps.Model.FlightBoard.AcceptReservesAStand",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFlightBoardAcceptReservesTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NetworkWithStands({3600.0});   // ONE stand, and two flights want it
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>();
	USimClock* Clock = NewObject<USimClock>();
	UFlightBoard* Board = NewObject<UFlightBoard>();
	Board->Allocator = NewObject<UStandAllocator>();

	UFlight* First = FlightNeeding(3400.0, 1);
	UFlight* Second = FlightNeeding(3400.0, 2);
	First->ArrivesAt = Clock->Now() + 100.0;
	Second->ArrivesAt = Clock->Now() + 100.0;
	Board->AddOffer(First);
	Board->AddOffer(Second);

	TestTrue(TEXT("the first offer is accepted"), Board->Accept(*Traffic, *Net, *Clock, *First));
	TestEqual(TEXT("and becomes Accepted"), First->Phase, EFlightPhase::Accepted);
	TestTrue(TEXT("holding a named stand"), First->Stand.IsSet());

	TestFalse(TEXT("the second is REFUSED rather than given the same stand"),
		Board->Accept(*Traffic, *Net, *Clock, *Second));
	TestEqual(TEXT("and is left in the inbox for the player to see"),
		Second->Phase, EFlightPhase::Offered);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlightBoardDispatchesAtTheEtaTest,
	"AirportOps.Model.FlightBoard.DispatchesAtTheEta",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFlightBoardDispatchesAtTheEtaTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NetworkWithStands({3600.0});
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>();
	USimClock* Clock = NewObject<USimClock>();
	UFlightBoard* Board = NewObject<UFlightBoard>();
	Board->Allocator = NewObject<UStandAllocator>();

	UFlight* Flight = FlightNeeding(3400.0, 1);
	Flight->ArrivesAt = Clock->Now() + 100.0;
	Board->AddOffer(Flight);

	int32 Calls = 0;
	bool bHeldAtDispatch = true;
	Board->Dispatcher = [&](const FVector2D&, const FAirframe&)
	{
		++Calls;
		// THE HOLD MUST BE GONE BY NOW. ArrivalPlanner asks IsHeld excluding the AGENT, and
		// the hold is under the flight's negative id, so a hold still standing refuses this
		// arrival its own stand with NoFreeStand - an aeroplane that never appears.
		const FEntityInstance* Stand = Net->GetEntity(Flight->Stand);
		bHeldAtDispatch = Stand != nullptr && Traffic->IsStandHeld(Stand->PoseNode, 0);
		return true;
	};

	TestTrue(TEXT("accepted"), Board->Accept(*Traffic, *Net, *Clock, *Flight));
	// The clock compresses: at the default 1200 real seconds per game day, one real second is
	// 72 game seconds, so 10 is comfortably past an ETA 100 game seconds out.
	Clock->Advance(10.0);

	TestEqual(TEXT("the dispatcher ran exactly once, at the ETA"), Calls, 1);
	TestFalse(TEXT("the stand hold was released BEFORE the dispatch"), bHeldAtDispatch);
	TestEqual(TEXT("and the flight is landing"), Flight->Phase, EFlightPhase::Landing);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlightBoardFollowsTheAgentTest,
	"AirportOps.Model.FlightBoard.FollowsTheAgentPhases",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFlightBoardFollowsTheAgentTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NetworkWithStands({3600.0});
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>();
	UFlightBoard* Board = NewObject<UFlightBoard>();
	Board->Allocator = NewObject<UStandAllocator>();

	UFlight* Flight = FlightNeeding(3400.0, 1);
	Flight->AgentId = 5;
	Flight->Phase = EFlightPhase::Landing;
	Board->AddOffer(Flight);

	Board->OnAgentPhase(*Traffic, *Net, 5, EAgentPhase::Arriving, EAgentPhase::Taxiing);
	TestEqual(TEXT("taxiing before the stand is TaxiIn"), Flight->Phase, EFlightPhase::TaxiIn);

	Board->OnAgentPhase(*Traffic, *Net, 5, EAgentPhase::Taxiing, EAgentPhase::Parked);
	TestEqual(TEXT("parked is the turnaround"), Flight->Phase, EFlightPhase::Turnaround);

	// THE POINT OF THE TEST: the same agent phase, the other answer.
	Board->OnAgentPhase(*Traffic, *Net, 5, EAgentPhase::Parked, EAgentPhase::Taxiing);
	TestEqual(TEXT("taxiing after the turnaround is TaxiOut"), Flight->Phase, EFlightPhase::TaxiOut);

	Board->OnAgentPhase(*Traffic, *Net, 5, EAgentPhase::Taxiing, EAgentPhase::Departing);
	TestEqual(TEXT("departing"), Flight->Phase, EFlightPhase::Departing);

	Board->OnAgentPhase(*Traffic, *Net, 5, EAgentPhase::Departing, EAgentPhase::Gone);
	TestEqual(TEXT("gone is departed"), Flight->Phase, EFlightPhase::Departed);
	TestEqual(TEXT("and the agent handle is given back"), Flight->AgentId, INDEX_NONE);

	// An agent nobody owns - a fuel truck - must not move any flight.
	Board->OnAgentPhase(*Traffic, *Net, 99, EAgentPhase::Taxiing, EAgentPhase::Parked);
	TestEqual(TEXT("a truck's phase change moves no flight"), Flight->Phase, EFlightPhase::Departed);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlightBoardExpiresOffersTest,
	"AirportOps.Model.FlightBoard.ExpiresAnIgnoredOffer",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFlightBoardExpiresOffersTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NetworkWithStands({3600.0});
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>();
	USimClock* Clock = NewObject<USimClock>();
	UFlightBoard* Board = NewObject<UFlightBoard>();
	Board->Allocator = NewObject<UStandAllocator>();

	UFlight* Offer = FlightNeeding(3400.0, 1);
	Offer->ExpiresAt = Clock->Now() + 50.0;
	Board->AddOffer(Offer);
	TestEqual(TEXT("it is in the inbox to begin with"), Board->Offers().Num(), 1);

	Clock->Advance(1.0);   // 72 game seconds: past the expiry
	Board->Tick(*Traffic, *Net, *Clock);

	TestEqual(TEXT("an ignored offer lapses"), Offer->Phase, EFlightPhase::Expired);
	TestEqual(TEXT("and leaves the inbox"), Board->Offers().Num(), 0);
	return true;
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `./Tools/Run-AirsideTests.ps1 -Filter AirportOps.Model.FlightBoard`
Expected: compile error, `Model/FlightBoard.h` not found.

- [ ] **Step 3: Write the header**

```cpp
#pragma once

#include "CoreMinimal.h"
#include "Model/ArrivalPlanner.h"
#include "UObject/Object.h"

#include "FlightBoard.generated.h"

class UFlight;
class UGroundTraffic;
class UOfferGenerator;
class URoadNetwork;
class UStandAllocator;
class USimClock;
enum class EAgentPhase : uint8;

/**
 * Every live flight, and the only thing that dispatches an arrival.
 *
 * ONE DOOR. Key 7 and the inbox both come through here, because two doors onto arrival is
 * how this codebase has shipped three lists-that-must-agree bugs (see CLAUDE.md, "Check
 * where a list is CONSUMED"). ARoadNetworkActor::DispatchArrival is reached through the
 * Dispatcher seam below rather than called directly, so that the world-free tests can prove
 * the schedule fires without a UWorld.
 */
UCLASS()
class AIRPORTOPS_API UFlightBoard : public UObject
{
	GENERATED_BODY()

public:
	UFlightBoard();

	/**
	 * What actually puts an aeroplane in the world. Set by UOpsRuntime::Attach to call
	 * ARoadNetworkActor::DispatchArrival; substituted in tests.
	 *
	 * A TFunction and not an interface, because there is exactly one production
	 * implementation and it is a forwarder on an actor - an interface would be a class per
	 * call site for one call site.
	 */
	TFunction<bool(const FVector2D& Near, const FAirframe& Airframe)> Dispatcher;

	/** Raised whenever anything a viewmodel displays has changed. */
	FSimpleMulticastDelegate OnChanged;

	UPROPERTY() TObjectPtr<UStandAllocator> Allocator = nullptr;
	UPROPERTY() TObjectPtr<UOfferGenerator> Generator = nullptr;

	/** Where an arrival is aimed - the runway chosen by nearest threshold to this point. */
	UPROPERTY() FVector2D ApproachFocus = FVector2D::ZeroVector;

	void AddOffer(UFlight* Offer);

	/** Hold a stand and put the arrival on the clock. False if no stand admits it now. */
	bool Accept(UGroundTraffic& Traffic, const URoadNetwork& Network, USimClock& Clock,
		UFlight& Flight);

	void Decline(UFlight& Flight);

	/**
	 * Why this offer could not be accepted this instant, or EArrivalRefusal::None.
	 *
	 * The REAL ArrivalPlanner::Plan with the live occupancy, so the inbox's greyed-out reason
	 * is the same sentence the arrival itself would print. ArrivalPlanner::DescribeRefusal
	 * renders it.
	 */
	EArrivalRefusal WhyNotAcceptable(UGroundTraffic& Traffic, const URoadNetwork& Network,
		const UFlight& Flight) const;

	/** Expiry, and nothing else on the fast path. The ETA is the clock's job, not a poll. */
	void Tick(UGroundTraffic& Traffic, const URoadNetwork& Network, USimClock& Clock);

	void OnAgentPhase(UGroundTraffic& Traffic, const URoadNetwork& Network, int32 AgentId,
		EAgentPhase From, EAgentPhase To);

	/** Re-make every accepted flight's stand hold. See UStandAllocator::Reapply. */
	void OnGraphRebuilt(UGroundTraffic& Traffic, const URoadNetwork& Network);

	/** Re-arm the clock for every Accepted flight. Called after a load; see the class comment. */
	void RearmSchedules(UGroundTraffic& Traffic, const URoadNetwork& Network, USimClock& Clock);

	TArray<UFlight*> Offers() const;
	TArray<UFlight*> Live() const;
	int32 PendingOfferCount() const;

private:
	UPROPERTY() TArray<TObjectPtr<UFlight>> Flights;
	UPROPERTY() int32 NextFlightId = 1;

	/**
	 * Clock handles by flight id, so an accepted flight can be un-scheduled.
	 *
	 * NOT a UPROPERTY and NOT saved: USimClock deliberately does not save its queue either,
	 * and a handle restored against a queue that no longer holds it would cancel something
	 * else. UFlight::ArrivesAt is the saved truth; RearmSchedules rebuilds this map from it.
	 */
	TMap<int32, int32> ArrivalHandles;

	void DispatchNow(UGroundTraffic& Traffic, const URoadNetwork& Network, UFlight& Flight);
	UFlight* FindByAgent(int32 AgentId);
};
```

- [ ] **Step 4: Write the implementation**

The parts that must be exactly this, because each is a bug otherwise:

```cpp
bool UFlightBoard::Accept(UGroundTraffic& Traffic, const URoadNetwork& Network,
	USimClock& Clock, UFlight& Flight)
{
	if (Flight.Phase != EFlightPhase::Offered || Allocator == nullptr)
	{
		return false;
	}
	if (!Allocator->Reserve(Traffic, Network, Flight))
	{
		UE_LOG(LogAirportOps, Log, TEXT("Flight %d not accepted: no stand admits it"), Flight.Id);
		return false;
	}
	Flight.Phase = EFlightPhase::Accepted;

	// The handle is kept so a later Decline or a reload can cancel it. The lambda captures
	// ids and pointers the board owns, never the clock, because the clock is what calls it.
	const int32 Handle = Clock.At(Flight.ArrivesAt,
		[this, &Traffic, &Network, Id = Flight.Id]()
		{
			for (TObjectPtr<UFlight>& Each : Flights)
			{
				if (Each != nullptr && Each->Id == Id)
				{
					DispatchNow(Traffic, Network, *Each);
					break;
				}
			}
		});
	ArrivalHandles.Add(Flight.Id, Handle);

	UE_LOG(LogAirportOps, Log, TEXT("Flight %d accepted: stand %d held, landing at %.0f"),
		Flight.Id, Flight.Stand.Index, Flight.ArrivesAt);
	OnChanged.Broadcast();
	return true;
}

void UFlightBoard::DispatchNow(UGroundTraffic& Traffic, const URoadNetwork& Network, UFlight& Flight)
{
	if (Flight.Type == nullptr || !Dispatcher)
	{
		return;
	}
	// RELEASE BEFORE DISPATCH, and this order is the whole point: ArrivalPlanner asks
	// IsHeld excluding the AGENT, and the hold is under the FLIGHT's negative id, so a hold
	// still standing would make the planner refuse our own stand with NoFreeStand.
	if (Allocator != nullptr)
	{
		Allocator->Release(Traffic, Flight);
	}

	const bool bDispatched = Dispatcher(ApproachFocus, Flight.Type->Airframe());
	if (!bDispatched)
	{
		// The flight stays Accepted with no hold. Slice E's sequencer is what queues this
		// properly; for now it says so and the player sees it in the inbox.
		UE_LOG(LogAirportOps, Warning, TEXT("Flight %d could not be dispatched at its ETA"), Flight.Id);
		OnChanged.Broadcast();
		return;
	}
	Flight.AgentId = Traffic.GetNewestAgentId();
	Flight.Phase = EFlightPhase::Landing;
	UE_LOG(LogAirportOps, Log, TEXT("Flight %d dispatched as agent %d"), Flight.Id, Flight.AgentId);
	OnChanged.Broadcast();
}

void UFlightBoard::OnAgentPhase(UGroundTraffic& Traffic, const URoadNetwork& Network,
	int32 AgentId, EAgentPhase From, EAgentPhase To)
{
	UFlight* Flight = FindByAgent(AgentId);
	if (Flight == nullptr)
	{
		return;  // A truck, or a key-7 aeroplane from before this board existed.
	}
	Flight->Phase = FlightPhaseFromAgent(To, Flight->Phase);

	if (To == EAgentPhase::Parked)
	{
		// WHICH stand it actually got, which need not be the one held - see UFlight::Stand.
		// The agent's own GoalNode is the authority, exactly as UFuelService reads it.
		if (const FRoadAgent* Agent = Traffic.FindAgent(AgentId))
		{
			const int32 Index = Network.FindEntityIndexByPoseNode(Agent->GoalNode);
			if (Index != INDEX_NONE)
			{
				Flight->Stand = Network.EntityIdAt(Index);
			}
		}
	}
	if (To == EAgentPhase::Gone)
	{
		Flight->AgentId = INDEX_NONE;
	}
	OnChanged.Broadcast();
}

EArrivalRefusal UFlightBoard::WhyNotAcceptable(UGroundTraffic& Traffic,
	const URoadNetwork& Network, const UFlight& Flight) const
{
	if (Flight.Type == nullptr)
	{
		return EArrivalRefusal::NoRunway;
	}
	const FArrivalPlan Plan = ArrivalPlanner::Plan(Network, ApproachFocus,
		Flight.Type->Airframe(), &Traffic.GetOccupancy());
	return Plan.Why;
}
```

`RearmSchedules` walks `Flights`, and for every `Accepted` flight with `ArrivesAt` still in
the future calls `Clock.At` again and refills `ArrivalHandles`. A flight whose `ArrivesAt`
has already passed while the game was closed dispatches on the next tick rather than being
lost — log it, because a silently dropped flight is exactly the save bug this guards.

`OnGraphRebuilt` gathers every `Accepted` flight and calls `Allocator->Reapply`.

- [ ] **Step 5: Run to verify they pass**

Run: `./Tools/Run-AirsideTests.ps1 -Filter AirportOps.Model.FlightBoard`
Expected: `4 test(s) run, 0 failed, 0 crashed.`

- [ ] **Step 6: Commit**

```bash
git add Plugins/AirportOps/Source/AirportOps/Public/Model/FlightBoard.h `
  Plugins/AirportOps/Source/AirportOps/Private/Model/FlightBoard.cpp `
  Plugins/AirportOps/Source/AirportOpsTests/Private/FlightBoardTest.cpp
git commit -m "feat(ops): the board owns a flight from the offer to the departure"
```

---

### Task 7: Wire the board into `UOpsRuntime`, and key 7 through it

**Files:**
- Modify: `Plugins/AirportOps/Source/AirportOps/Public/Present/OpsRuntime.h`
- Modify: `Plugins/AirportOps/Source/AirportOps/Private/Present/OpsRuntime.cpp`
- Modify: `Source/AirportMgr/RoadBuildController.cpp` (the `aircraft.land` handler, near the
  `Land: nearest runway to the view focus` log at line 319)
- Test: `Plugins/AirportOps/Source/AirportOpsTests/Private/FlightBoardWiringTest.cpp` (create)

**Interfaces:**
- Consumes: everything above; `UOpsRuntime::Attach`, `::Tick`, `::OnAgentPhase`.
- Produces: `UFlightBoard* UOpsRuntime::GetFlightBoard() const`,
  `UOfferGenerator* UOpsRuntime::GetOfferGenerator() const`.

- [ ] **Step 1: Write the failing test**

A composition test, not a model test: spawn nothing, but assert the runtime hands the board a
dispatcher and drives it.

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlightBoardIsDrivenByTheRuntimeTest,
	"AirportOps.Present.FlightBoardIsDrivenByTheRuntime",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFlightBoardIsDrivenByTheRuntimeTest::RunTest(const FString& Parameters)
{
	UOpsRuntime* Runtime = NewObject<UOpsRuntime>();
	TestNotNull(TEXT("the runtime composes a flight board"), Runtime->GetFlightBoard());
	TestNotNull(TEXT("and an offer generator"), Runtime->GetOfferGenerator());
	TestNotNull(TEXT("the board has an allocator"), Runtime->GetFlightBoard()->Allocator);
	// The seam: unattached, there is no dispatcher, and the board must not pretend otherwise.
	TestFalse(TEXT("an unattached runtime leaves the dispatcher unset"),
		static_cast<bool>(Runtime->GetFlightBoard()->Dispatcher));
	return true;
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `./Tools/Run-AirsideTests.ps1 -Filter AirportOps.Present.FlightBoardIsDriven`
Expected: compile error, no `GetFlightBoard`.

- [ ] **Step 3: Compose the subobjects**

In `UOpsRuntime`'s constructor, beside the existing four:

```cpp
	FlightBoard = CreateDefaultSubobject<UFlightBoard>(TEXT("FlightBoard"));
	FlightBoard->Allocator = CreateDefaultSubobject<UStandAllocator>(TEXT("StandAllocator"));
	OfferGenerator = CreateDefaultSubobject<UOfferGenerator>(TEXT("OfferGenerator"));
	FlightBoard->Generator = OfferGenerator;
```

In `Attach`, after the traffic delegates are bound:

```cpp
	// The one production dispatcher. Weak, because the actor outlives nothing here and a
	// raw capture would keep a dead actor alive across a level change.
	TWeakObjectPtr<ARoadNetworkActor> WeakTarget = Target;
	FlightBoard->Dispatcher = [WeakTarget](const FVector2D& Near, const FAirframe& Airframe)
	{
		ARoadNetworkActor* Actor = WeakTarget.Get();
		return Actor != nullptr && Actor->DispatchArrival(Near, Airframe);
	};
```

In `Detach`, clear it: `FlightBoard->Dispatcher = nullptr;`.

In `Tick`, inside the existing `if (Target->Network != nullptr && ...)` block that already
ticks the fuel service:

```cpp
			FlightBoard->Tick(*Model, *Target->Network, *Clock);
```

In `OnAgentPhase`, beside the existing `FuelService->OnAgentPhase(...)`:

```cpp
			FlightBoard->OnAgentPhase(*Model, *Target->Network, AgentId, From, To);
```

- [ ] **Step 4: Route key 7 through the board**

In `RoadBuildController.cpp`, the `aircraft.land` handler currently calls `DispatchArrival`
directly. Replace that call with: make a `UFlight` for the chosen type with
`ArrivesAt = Clock->Now()`, add it to the board, `Accept` it, and let the scheduled callback
dispatch. Keep the existing `Land: nearest runway to the view focus` log line exactly as it
is — **every `UE_LOG` survives** — and set `FlightBoard->ApproachFocus` to the same focus
point it already computes, so the board aims at the runway the key chose.

If `Accept` returns false, log why through `ArrivalPlanner::DescribeRefusal` on
`WhyNotAcceptable`. The key currently appears to do nothing when the airport is full; after
this it says which.

- [ ] **Step 5: Run it to verify it passes**

Run: `./Tools/Run-AirsideTests.ps1 -Filter AirportOps.Present.FlightBoardIsDriven`
Expected: `1 test(s) run, 0 failed, 0 crashed.`

- [ ] **Step 6: Commit**

```bash
git add Plugins/AirportOps/Source/AirportOps/Public/Present/OpsRuntime.h `
  Plugins/AirportOps/Source/AirportOps/Private/Present/OpsRuntime.cpp `
  Source/AirportMgr/RoadBuildController.cpp `
  Plugins/AirportOps/Source/AirportOpsTests/Private/FlightBoardWiringTest.cpp
git commit -m "feat(ops): every arrival comes through the flight board, key 7 included"
```

---

### Task 8: The board survives a save

**Files:**
- Modify: `Plugins/AirportOps/Source/AirportOps/Public/Model/OpsSave.h`
- Modify: `Plugins/AirportOps/Source/AirportOps/Private/Model/OpsSave.cpp`
- Modify: `Plugins/AirportOps/Source/AirportOps/Private/Present/OpsRuntime.cpp` (Save/Load)
- Test: `Plugins/AirportOps/Source/AirportOpsTests/Private/FlightSaveTest.cpp` (create)

**Interfaces:**
- Consumes: `OpsSave::SerializeObject`/`DeserializeObject`, `FOpsSnapshot`.
- Produces: `FOpsSnapshot::Flights` (`TArray<uint8>`), `Version = 2`.

- [ ] **Step 1: Write the failing test**

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlightSurvivesASaveTest,
	"AirportOps.Model.FlightSave.AnInboundFlightStillArrives",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFlightSurvivesASaveTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NetworkWithStands({3600.0});
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>();
	USimClock* Clock = NewObject<USimClock>();
	UFlightBoard* Board = NewObject<UFlightBoard>();
	Board->Allocator = NewObject<UStandAllocator>();
	Board->Dispatcher = [](const FVector2D&, const FAirframe&) { return true; };

	UFlight* Flight = FlightNeeding(3400.0, 1);
	Flight->ArrivesAt = Clock->Now() + 100.0;
	Board->AddOffer(Flight);
	TestTrue(TEXT("accepted before the save"), Board->Accept(*Traffic, *Net, *Clock, *Flight));

	TArray<uint8> Bytes;
	OpsSave::SerializeObject(*Board, Bytes);

	// A FRESH board and a FRESH clock: this is a reload, not a copy. The clock deliberately
	// does not save its callback queue, so the restored flight has an ETA and nothing armed.
	UFlightBoard* Reloaded = NewObject<UFlightBoard>();
	Reloaded->Allocator = NewObject<UStandAllocator>();
	OpsSave::DeserializeObject(*Reloaded, Bytes);

	int32 Calls = 0;
	Reloaded->Dispatcher = [&Calls](const FVector2D&, const FAirframe&) { ++Calls; return true; };

	USimClock* FreshClock = NewObject<USimClock>();
	Reloaded->RearmSchedules(*Traffic, *Net, *FreshClock);
	FreshClock->Advance(10.0);

	TestEqual(TEXT("a reloaded inbound flight still arrives"), Calls, 1);
	return true;
}
```

**Known risk, check it before weakening the assertion:** `UFlight::Type` points at a
`UAircraftType` made with `NewObject` in the fixture, and an object reference serialises as a
path. If a transient type does not resolve on the way back in, `DispatchNow` returns early on
its null check and `Calls` stays 0 — which looks exactly like the bug the test is for. If
that happens, make the fixture's type a real content asset rather than lowering the bar; the
production path always has one.

- [ ] **Step 2: Run it to verify it fails**

Run: `./Tools/Run-AirsideTests.ps1 -Filter AirportOps.Model.FlightSave`
Expected: FAIL — the dispatcher is never called after the reload.

- [ ] **Step 3: Carry flights in the snapshot**

Add to `FOpsSnapshot`:

```cpp
	/** The flight board's non-Transient UPROPERTYs. Added at Version 2. */
	UPROPERTY() TArray<uint8> Flights;
```

Bump `Version` to 2, and in `OpsSave::Restore` treat a `Version == 1` snapshot as a game with
no flights rather than a failure — an old save must still load.

`Capture` and `Restore` gain a `UFlightBoard&` parameter; update both call sites in
`OpsRuntime.cpp`. After `Restore`, `UOpsRuntime` calls `FlightBoard->RearmSchedules(...)` and
then `FlightBoard->OnGraphRebuilt(...)` — the holds are node claims and the restored network
has just been rebuilt, so they must be re-made before anything can allocate against them.

- [ ] **Step 4: Run it to verify it passes**

Run: `./Tools/Run-AirsideTests.ps1 -Filter AirportOps.Model.FlightSave`
Expected: `1 test(s) run, 0 failed, 0 crashed.`

- [ ] **Step 5: Commit**

```bash
git add Plugins/AirportOps/Source/AirportOps/Public/Model/OpsSave.h `
  Plugins/AirportOps/Source/AirportOps/Private/Model/OpsSave.cpp `
  Plugins/AirportOps/Source/AirportOps/Private/Present/OpsRuntime.cpp `
  Plugins/AirportOps/Source/AirportOpsTests/Private/FlightSaveTest.cpp
git commit -m "feat(ops): an accepted flight survives a save and still arrives"
```

---

### Task 9: The viewmodels

**Files:**
- Modify: `AirportMgr.uproject` (enable `ModelViewViewModel`)
- Modify: `Source/AirportMgr/AirportMgr.Build.cs`
- Create: `Source/AirportMgr/OfferViewModels.h` / `.cpp`
- Test: `Source/AirportMgr/OfferViewModelsTest.cpp` (create)

**Interfaces:**
- Consumes: `UMVVMViewModelBase`, `UE_MVVM_SET_PROPERTY_VALUE`, `UFlightBoard::OnChanged`.
- Produces: `UOfferViewModel` (`Airline`, `TypeName`, `CodeLetter`, `Eta`, `bAcceptable`,
  `RefusalText`, `Flight`), `UOfferInboxViewModel` (`Offers`, `PendingCount`, `Accept`,
  `Decline`, `Refresh`).

- [ ] **Step 1: Enable the plugin**

In `AirportMgr.uproject`, beside the existing plugin entries:

```json
		{
			"Name": "ModelViewViewModel",
			"Enabled": true
		}
```

In `AirportMgr.Build.cs`, add `"ModelViewViewModel"` to `PrivateDependencyModuleNames` with a
comment saying what it is for — the file already comments why UMG and Slate are there, and a
Beta dependency deserves the same note.

- [ ] **Step 2: Write the failing test**

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOfferInboxCountBroadcastsTest,
	"AirportMgr.UI.OfferInbox.PendingCountBroadcasts",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOfferInboxCountBroadcastsTest::RunTest(const FString& Parameters)
{
	// Bind a delegate to the PendingCount field via AddFieldValueChangedDelegate, change the
	// count through the viewmodel, and assert the delegate fired.
	//
	// THE TEST EXISTS FOR ONE BUG: assigning the member directly instead of through
	// UE_MVVM_SET_PROPERTY_VALUE compiles, displays correctly on the first draw, and never
	// updates again. Nothing else catches that.
}
```

Use `UOfferInboxViewModel::FFieldNotificationClassDescriptor::PendingCount` as the field id —
the descriptor is generated for any `UPROPERTY(FieldNotify)`.

- [ ] **Step 3: Write the viewmodels**

```cpp
UCLASS()
class UOfferViewModel : public UMVVMViewModelBase
{
	GENERATED_BODY()

public:
	/** The flight this row shows. Weak: the board owns flights and retires them. */
	UPROPERTY() TWeakObjectPtr<UFlight> Flight;

	UPROPERTY(BlueprintReadOnly, FieldNotify, Getter, Category = "Offer") FText Airline;
	UPROPERTY(BlueprintReadOnly, FieldNotify, Getter, Category = "Offer") FText TypeName;
	UPROPERTY(BlueprintReadOnly, FieldNotify, Getter, Category = "Offer") FText Eta;
	UPROPERTY(BlueprintReadOnly, FieldNotify, Getter, Category = "Offer") bool bAcceptable = true;
	UPROPERTY(BlueprintReadOnly, FieldNotify, Getter, Category = "Offer") FText RefusalText;

	/** Pull every field from the flight and the board's refusal. Broadcasts what changed. */
	void Refresh(UFlightBoard& Board, UGroundTraffic& Traffic, const URoadNetwork& Network);
};
```

Every setter goes through `UE_MVVM_SET_PROPERTY_VALUE(Airline, NewValue)`, never a plain
assignment. `UOfferInboxViewModel` holds `TArray<TObjectPtr<UOfferViewModel>> Offers` and an
`int32 PendingCount`, rebuilds them on `UFlightBoard::OnChanged`, and exposes
`UFUNCTION() void Accept(UOfferViewModel*)` / `Decline(UOfferViewModel*)` that call the board.

**No viewmodel mutates the model.** Accept and Decline call `UFlightBoard`, which is the one
door for undo, save and tests.

- [ ] **Step 4: Run it to verify it passes**

Run: `./Tools/Run-AirsideTests.ps1 -Filter AirportMgr.UI.OfferInbox`
Expected: `1 test(s) run, 0 failed, 0 crashed.`

- [ ] **Step 5: Commit**

```bash
git add AirportMgr.uproject Source/AirportMgr/AirportMgr.Build.cs `
  Source/AirportMgr/OfferViewModels.h Source/AirportMgr/OfferViewModels.cpp `
  Source/AirportMgr/OfferViewModelsTest.cpp
git commit -m "feat(ui): the inbox has viewmodels, and the count broadcasts"
```

---

### Task 10: The inbox widget

**Files:**
- Create: `Source/AirportMgr/OfferInboxWidget.h` / `.cpp`
- Modify: `Source/AirportMgr/BuildBarWidget.h` / `.cpp` (an offers section and its badge)
- Test: `Source/AirportMgr/OfferInboxWidgetTest.cpp` (create)

**Interfaces:**
- Consumes: Task 9's viewmodels; `UListView`; `UMVVMViewListViewBaseClassExtension`.
- Produces: `UOfferInboxWidget` with `UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UListView> OfferList`.

- [ ] **Step 1: Write the failing test**

Widget tests DO need a world, unlike everything through Task 8. `InspectorWidgetTest.cpp`
already has the scaffolding; copy it exactly:

```cpp
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("a world"), World)) { return false; }
	FWorldContext& Ctx = GEngine->CreateNewWorldContext(EWorldType::Game);
	Ctx.SetCurrentWorld(World);
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };
```

Then: spawn an `ARoadNetworkActor`, build a board with two offers, construct the widget with
`CreateWidget<UOfferInboxWidget>(World)`, call its refresh with the same arguments
`NativeTick` passes, and assert the list holds two items and the badge text reads `2`.
`InspectorWidgetTest` calls `Refresh` directly for exactly this reason and says so in a
comment — follow that, and do not try to run a tick loop headlessly.

- [ ] **Step 2: Run it to verify it fails**

Run: `./Tools/Run-AirsideTests.ps1 -Filter AirportMgr.UI.OfferInboxWidget`
Expected: compile error, no `OfferInboxWidget.h`.

- [ ] **Step 3: Write the widget**

C++ base with `BindWidgetOptional` members, exactly as `UBuildBarWidget` does, so the
Blueprint can restyle without the C++ needing to know the layout. `SetListItems` takes the
inbox viewmodel's `Offers` array directly — they are `UObject*`s.

Document at the top, as `UBuildBarWidget` does, how to make the Widget Blueprint: parent it to
this class, name the widgets to match the `BindWidgetOptional` members, and set the entry
widget class on the `UListView`. Add the warning that **a renamed viewmodel field needs the
BP recompiled and resaved**, or the old binding runs against the new class.

- [ ] **Step 4: Run it to verify it passes**

Run: `./Tools/Run-AirsideTests.ps1 -Filter AirportMgr.UI.OfferInboxWidget`
Expected: `1 test(s) run, 0 failed, 0 crashed.`

- [ ] **Step 5: Commit**

```bash
git add Source/AirportMgr/OfferInboxWidget.h Source/AirportMgr/OfferInboxWidget.cpp `
  Source/AirportMgr/OfferInboxWidgetTest.cpp Source/AirportMgr/BuildBarWidget.h `
  Source/AirportMgr/BuildBarWidget.cpp
git commit -m "feat(ui): the offer inbox is a list on the bottom bar"
```

---

### Task 11: Build, test, and verify it in PIE

**Files:** none — this task is evidence.

- [ ] **Step 1: Full build, twice**

```
D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development `
  -Project="C:\repos\AirportMgr2\AirportMgr.uproject" -WaitMutex
```

Twice, because this plan adds eight new test .cpp files and the first build reports
`Result: Succeeded` without compiling them.

- [ ] **Step 2: Full test run**

```
./Tools/Run-AirsideTests.ps1
```

Read the `N test(s) run, N failed, N crashed` line, not the exit code. Expect the count to
have risen by at least 18 over the pre-branch figure; a count that did NOT rise means a test
name collided (see the Global Constraints note on bare names).

- [ ] **Step 3: Author two airline assets**

Two `UAirlineDefinition` assets under `Content/Entities`, one narrow-body operator with the
A320 in its fleet and one GA operator with the Piper. Confirm the catalog sees them: the
count appears in the startup log, and if it is zero the `PrimaryAssetTypesToScan` entry from
Task 2 is wrong or missing.

- [ ] **Step 4: PIE**

Instrument first, then ask for one session. What to look for, in order:

1. `Offers:` in `Saved/Logs/AirportMgr.log` — the generator is running at all.
2. Accept one in the inbox: `Flight N accepted: stand M held, landing at T`.
3. At the ETA: `Flight N dispatched as agent A`, then the existing arrival lines.
4. It parks, the fuel truck comes as it already does, and it leaves.
5. Accept offers until the stands run out — the next one greys out with a refusal sentence
   rather than being accepted and lost.
6. Save, quit to the menu, reload: an inbound flight still arrives.

- [ ] **Step 5: Mark the spec implemented**

Update the spec's Status line with the build line, the test line, and the log lines from the
PIE session — the same shape as the service-connections spec's status block.

- [ ] **Step 6: Commit and open the PR**

Fill in the PR template's build line, test line, and for the refactor section the `UE_LOG`
count before and after.

---

## Notes for the executor

- **The plan is not the code.** Every snippet here was checked against the headers on
  2026-09-11, but a plan that disagrees with a header is wrong and the header wins. This
  project has shipped plans with invented signatures; check before you transcribe.
- **`UFlight::Stand` means two things in sequence** — held, then parked on. Read its comment
  before changing anything that writes it.
- **Order matters in `DispatchNow`.** Release the hold, then dispatch. Reversed, the planner
  refuses the flight its own stand and the symptom is an aeroplane that never appears with a
  `NoFreeStand` in the log.
- **If a test needs a `UWorld`, it is in the wrong module.** Everything through Task 8 is
  world-free by construction.
