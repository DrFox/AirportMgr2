#include "CoreMinimal.h"
#include "Build/AnchorLink.h"
#include "Content/AirsideSettings.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/JobBoard.h"
#include "Model/GroundTraffic.h"
#include "Model/InspectFacts.h"
#include "Model/RoadAgent.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadTraffic.h"
#include "Model/RoutePolicy.h"
#include "Model/RouteSearch.h"
#include "Present/AirsideTraffic.h"
#include "Present/OpsRuntime.h"
#include "Present/RoadAgentActor.h"
#include "Present/RoadNetworkActor.h"
#include "Testing/AirsideTestGraph.h"
#include "Testing/AirsideTestWorld.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	void LayFuelLine(URoadNetwork& Net, const FVector2D& From, const FVector2D& To,
		ETraversalClass Class, FGuidelineNodeId& OutA, FGuidelineNodeId& OutB)
	{
		OutA = Net.AddGuidelineNode(From);
		OutB = Net.AddGuidelineNode(To);

		FGuidelineEdge Edge;
		Edge.A = OutA;
		Edge.B = OutB;
		Edge.Control = (From + To) * 0.5;
		Edge.AllowedTraffic = FTrafficMask::Only(Class);
		Edge.AllowedTraffic.Add(ETraversalClass::Emergency);
		Edge.Direction = EGuidelineDir::Bidirectional;
		Edge.Width = 600.0;
		Edge.bDerived = true;
		Net.AddGuidelineEdge(MoveTemp(Edge));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFuelServiceWiredTest, "AirportOps.Present.FuelServiceWired",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFuelServiceWiredTest::RunTest(const FString& Parameters)
{
	// THE COMPOSITION TEST FOR THE SEAM, and the only one that fails if the relay or the
	// tick is left unwired. Every piece below has its own world-free test; this is the one
	// that proves they are joined - the same job AirportOps.Present.Runtime does for
	// save/load, spawned in a real world with a real actor and a real tick loop.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("the actor"), Actor)) { return false; }

	// A node first: the actor's Network is built lazily by the first edit.
	Actor->PlaceNode(FVector2D(0.0, 40000.0));
	if (!TestNotNull(TEXT("a network"), Actor->Network.Get())) { return false; }

	URoadNetwork& Net = *Actor->Network;

	// The same geometry the world-free fixture uses - see AirportOps.Ops.FuelService for
	// where each ray goes and why.
	// -4000 SINCE 2026-09-17, the same figure and the same reason as the world-free fixture's
	// FFuelFixture::RoadY: a stand offers its aft-edge entries now, and the furthest of them
	// was 8450 uu from a road at -6000 against a reach of 6500.
	constexpr double RoadY = -4000.0;
	FGuidelineNodeId TaxiSouth, TaxiNorth, RoadWest, RoadEast;
	LayFuelLine(Net, FVector2D(-10000.0, -10000.0), FVector2D(-10000.0, 10000.0),
		ETraversalClass::Aircraft, TaxiSouth, TaxiNorth);

	UEntityDefinition* StandDef = UEntityDefinition::MakeStandTransient();
	UEntityDefinition* DepotDef = UEntityDefinition::MakeFuelDepotTransient();
	const FEntityInstanceId Stand = Net.PlaceEntity(StandDef, StandDef->Anchors,
		FVector2D(0.0, 0.0), 0.0, 3600.0, StandDef->PoseRole, StandDef->Trucks);

	// THE ROAD SOUTH, AND A SPUR UP THE STAND'S FAR EDGE - FFuelFixture::LaySouthRoad's shape and
	// reason (2026-09-27): a road running alongside a stand does not serve it, so the service road
	// meets the far edge. The spur leaves the road at a node the road shares, so a truck can turn.
	double FarEdge = -TNumericLimits<double>::Max();
	for (const FVector2D& Corner : Net.GetEntity(Stand)->Outline)
	{
		FarEdge = FMath::Max(FarEdge, Corner.X);
	}
	const double SpurX = FarEdge + 420.0;
	FGuidelineNodeId SpurFoot, SpurHead;
	LayFuelLine(Net, FVector2D(SpurX, RoadY), FVector2D(SpurX, 10000.0),
		ETraversalClass::GroundVehicle, SpurFoot, SpurHead);
	auto Join = [&Net](FGuidelineNodeId A, FGuidelineNodeId B)
	{
		FGuidelineEdge Edge;
		Edge.A = A;
		Edge.B = B;
		Edge.Control = (Net.GetGuidelineNode(A)->Position + Net.GetGuidelineNode(B)->Position) * 0.5;
		Edge.AllowedTraffic = FTrafficMask::Only(ETraversalClass::GroundVehicle);
		Edge.AllowedTraffic.Add(ETraversalClass::Emergency);
		Edge.Direction = EGuidelineDir::Bidirectional;
		Edge.Width = 600.0;
		Edge.bDerived = true;
		Net.AddGuidelineEdge(MoveTemp(Edge));
	};
	RoadWest = Net.AddGuidelineNode(FVector2D(-20000.0, RoadY));
	RoadEast = Net.AddGuidelineNode(FVector2D(20000.0, RoadY));
	Join(RoadWest, SpurFoot);
	Join(SpurFoot, RoadEast);
	const FEntityInstanceId Depot = Net.PlaceEntity(DepotDef, DepotDef->Anchors,
		FVector2D(12000.0, RoadY + 4000.0), UE_DOUBLE_PI * 0.5, 0.0, DepotDef->PoseRole,
		DepotDef->Trucks);
	FAnchorLink::Build(Net, UAirsideSettings::ResolveLargestServiceVehicle());

	UOpsRuntime* Runtime = NewObject<UOpsRuntime>();
	if (!TestNotNull(TEXT("the runtime owns a fuel service"), Runtime->GetJobBoard())) { return false; }
	Runtime->Attach(Actor);

	// Taxi an aircraft in to the stand.
	// #312: was a hand-built FRouteQuery that skipped AvoidRunways.
	const FRoutePlan Plan = TestGraph::Probe(Net, TaxiSouth, Net.GetEntity(Stand)->PoseNode, ETraversalClass::Aircraft);
	if (!TestTrue(TEXT("the aircraft routes to the stand"), Plan.IsValid())) { return false; }
	if (!TestTrue(TEXT("and dispatches"),
		Actor->DispatchAgent(Plan, UAirsideSettings::ResolveDefaultAirframe()))) { return false; }
	const int32 Aircraft = Actor->GetTraffic()->GetNewestAgentId();

	// BOTH TICKS, in the order the game runs them: the actor advances the traffic, the
	// runtime advances the clock and the service. Nothing calls the service by hand.
	constexpr float Step = 1.0f / 30.0f;
	int32 TruckId = 0;
	// THE DEADLINE (stage 3): the serve's end is found by the job board's one Clock.At, not by looking
	// every frame - so it must end on the FIRST step at or past StepEndsAt, never before, never later.
	double ServeDueAt = -1.0;
	double ServeLeftAt = -1.0;
	double NowBeforeLeaving = -1.0;
	double PreviousNow = 0.0;
	bool bSawVehicleAgent = false;
	bool bSawTruckWithAView = false;
	bool bSawFuelling = false;

	// 18000 TICKS AT A THIRTIETH IS 600 s, RAISED FROM 12000 ON 2026-09-17. The round trip got
	// longer because the truck now BACKS OUT of the service point rather than turning round on
	// the spot: 2529 uu of reverse leg at 100 uu/s is 25 s against a forward pass's 5, and the
	// one-way cycle stops the route retracing the serve leg on the way home. A ceiling, not a
	// wait - the loop breaks as soon as it has seen what it came for.
	for (int32 Tick = 0; Tick < 18000; ++Tick)
	{
		Actor->Tick(Step);
		Runtime->Tick(Step);

		const double Now = Runtime->GetClock()->Now();
		const FServiceVehicle* Serving = Runtime->GetJobBoard()->GetVehicles().FindByPredicate([](const FServiceVehicle& Vehicle)
			{ return Vehicle.State == EServiceVehicleState::Serving && Vehicle.CurrentJob != 0; });
		if (Serving != nullptr && ServeLeftAt < 0.0)
		{
			ServeDueAt = Serving->StepEndsAt;
		}
		else if (Serving == nullptr && ServeDueAt >= 0.0 && ServeLeftAt < 0.0)
		{
			ServeLeftAt = Now;
			NowBeforeLeaving = PreviousNow;
		}
		PreviousNow = Now;

		for (const FRoadAgent& Agent : Actor->GetTraffic()->GetModel()->GetAgents())
		{
			if (Agent.Class != ETraversalClass::GroundVehicle)
			{
				continue;
			}
			bSawVehicleAgent = true;
			TruckId = Agent.Id;
			bSawTruckWithAView |= Actor->GetTraffic()->GetAgentView(Agent.Id) != nullptr;
		}

		for (const FServiceJob& Demand : Runtime->GetJobBoard()->GetJobs())
		{
			bSawFuelling |= Demand.State == EServiceJobState::Serving;
		}

		if (bSawFuelling && bSawVehicleAgent && TruckId != 0
			&& Actor->GetTraffic()->GetModel()->FindAgent(TruckId) == nullptr)
		{
			break;
		}
	}

	// 1. The RELAY: an aircraft parking reached the service at all.
	TestEqual(TEXT("parking made a demand"), Runtime->GetJobBoard()->GetJobs().Num(), 1);
	TestEqual(TEXT("for the aircraft that parked"),
		Runtime->GetJobBoard()->GetJobs()[0].AircraftId, Aircraft);
	TestEqual(TEXT("at the stand it parked on"),
		Runtime->GetJobBoard()->GetJobs()[0].Stand, Stand);
	TestEqual(TEXT("served by the depot"),
		Runtime->GetJobBoard()->GetJobs()[0].LastDepot, Depot);

	// 2. The TICK: a GroundVehicle agent appeared, with a view of its own, and fuelled.
	TestTrue(TEXT("a ground vehicle agent appeared"), bSawVehicleAgent);
	TestTrue(TEXT("with a view of its own"), bSawTruckWithAView);
	TestTrue(TEXT("and it fuelled the aircraft"), bSawFuelling);
	if (TestTrue(TEXT("the serve ended"), ServeLeftAt >= 0.0))
	{
		TestTrue(FString::Printf(TEXT("not before it was due (left %.2f, due %.2f)"), ServeLeftAt, ServeDueAt), ServeLeftAt >= ServeDueAt);
		TestTrue(FString::Printf(TEXT("and on the first step past it - its deadline woke the board (step before %.2f, due %.2f)"),
			NowBeforeLeaving, ServeDueAt), NowBeforeLeaving < ServeDueAt);
	}

	// 3. And went away again. A truck does not fly off, so only the service retires it -
	// this is the end of the round trip the whole slice exists to produce.
	TestNull(TEXT("the truck is gone once it is home"),
		Actor->GetTraffic()->GetModel()->FindAgent(TruckId));
	TestEqual(TEXT("done"), static_cast<int32>(Runtime->GetJobBoard()->GetJobs()[0].State),
		static_cast<int32>(EServiceJobState::Done));

	// 4. THE PANEL'S SEAM. FAgentFacts::Fuel is filled by the ops layer, not by DescribeAgent
	// - so the field must be empty out of Airside and non-empty once the service is asked.
	FAgentFacts AirsideFacts;
	if (!TestTrue(TEXT("the aircraft describes"),
		InspectFacts::DescribeAgent(*Actor->GetTraffic()->GetModel(), &Net, Aircraft, AirsideFacts)))
	{
		return false;
	}
	TestTrue(TEXT("Airside leaves the fuel line empty - it must not know what fuel is"),
		AirsideFacts.Fuel.IsEmpty());
	TestEqual(TEXT("and the ops layer fills it"),
		Runtime->GetJobBoard()->DescribeAgent(Aircraft, 0.0, nullptr).EndsWith(TEXT("\u00B7 done")), true);

	// 5. And the depot's own card can tell itself from a stand.
	FStandFacts DepotFacts;
	if (!TestTrue(TEXT("the depot describes"),
		InspectFacts::DescribeStand(Actor->GetTraffic()->GetModel(), Net, Depot.Index, DepotFacts)))
	{
		return false;
	}
	TestEqual(TEXT("as a service installation, not a stand"),
		static_cast<int32>(DepotFacts.PoseRole), static_cast<int32>(EServiceRole::Fuel));
	TestTrue(TEXT("and it is on a road"), DepotFacts.bReachable);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFleetRuntimeSeedsTest, "AirportOps.Present.Fleet.AttachSeedsTheStarterFleetOnTheFirstDrain",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFleetRuntimeSeedsTest::RunTest(const FString& Parameters)
{
	// THE COMPOSITION BEHIND CouldServe'S "REAL VEHICLES ONLY" (#443): the starter fleet is no longer predicted beside its
	// seeding, so the seeding has to have happened before an offer is read. Attach marks every pass dirty, and the first
	// drain runs the "FleetSeed" pass - which seeds a depot that was already there, when nothing was announced, through the
	// fleet's door. Unwired, CouldServe would say "no fuel" for a starter depot until some unrelated event woke the pass.
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("the actor"), Actor)) { return false; }
	Actor->PlaceNode(FVector2D(0.0, 40000.0));
	if (!TestNotNull(TEXT("a network"), Actor->Network.Get())) { return false; }
	UEntityDefinition* DepotDef = UEntityDefinition::MakeFuelDepotTransient();
	const FEntityInstanceId Depot = Actor->Network->PlaceEntity(DepotDef, DepotDef->Anchors, FVector2D(12000.0, 0.0), 0.0, 0.0,
		DepotDef->PoseRole, 1);
	UOpsRuntime* Runtime = NewObject<UOpsRuntime>();
	Runtime->Attach(Actor);
	Runtime->Tick(0.0);
	TestTrue(TEXT("the first drain seeded the starter depot's fleet"), Runtime->GetJobBoard()->VehiclesAt(Depot) > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFleetPlacedDepotSeededByAnnouncementTest, "AirportOps.Present.Fleet.PlacedDepotIsSeededByTheAnnouncement",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFleetPlacedDepotSeededByAnnouncementTest::RunTest(const FString& Parameters)
{
	// THE SEAM THE "FleetSeed" PASS STANDS ON (#443): a depot placed AFTER the attach is seeded because the placement was
	// ANNOUNCED - FNetworkChangedEvent, published by the rebuild the placement committed - and not because a job board Step
	// walked the entities and found it (Step does not seed any more). Through the actor's live placement path, the one the
	// player's gesture takes. Unwire the subscription and the depot stays empty for the session: this goes red on the last line.
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("the actor"), Actor)) { return false; }
	Actor->FuelDepotDefinition = UEntityDefinition::MakeFuelDepotTransient();
	Actor->PlaceNode(FVector2D(0.0, 40000.0));
	if (!TestNotNull(TEXT("a network"), Actor->Network.Get())) { return false; }
	UOpsRuntime* Runtime = NewObject<UOpsRuntime>();
	Runtime->Attach(Actor);
	Runtime->Tick(1.0 / 30.0);
	TestEqual(TEXT("setup: an airport with no depot has no fleet"), Runtime->GetJobBoard()->GetVehicles().Num(), 0);

	const int32 Index = Actor->PlaceEntity(FVector2D(12000.0, 0.0), 0.0, EPlaceableEntity::FuelDepot);
	if (!TestTrue(TEXT("the depot is placed"), Index != INDEX_NONE)) { return false; }
	const FEntityInstanceId Depot = Actor->Network->EntityIdAt(Index);
	TestEqual(TEXT("nothing runs at the call site of the placement: the announcement is queued, and no vehicle exists yet"),
		Runtime->GetJobBoard()->VehiclesAt(Depot), 0);

	Runtime->Tick(1.0 / 30.0);
	TestTrue(TEXT("the drain that heard the announcement seeded the depot's starter fleet"), Runtime->GetJobBoard()->VehiclesAt(Depot) > 0);
	const int32 Seeded = Runtime->GetJobBoard()->VehiclesAt(Depot);
	for (int32 Tick = 0; Tick < 5; ++Tick) { Runtime->Tick(1.0 / 30.0); }
	TestEqual(TEXT("and only once: later drains add nothing to a depot that has been seen"), Runtime->GetJobBoard()->VehiclesAt(Depot), Seeded);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFleetUndoRedoSeedsAgainTest, "AirportOps.Present.Fleet.UndoThenRedoOfAStarterDepotSeedsItAgain",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFleetUndoRedoSeedsAgainTest::RunTest(const FString& Parameters)
{
	// AN UNDO RESTORES THE DEPOT'S EXACT {Index, Generation} (#487): Entities and EntityFreeList ride in the undo Memento, so place ->
	// undo -> redo hands the undone depot's id back. UJobBoard::SeededDepots still held that id from the first seeding, so the depot
	// came back with NO starter fleet for the rest of the session. (Bulldozing then placing was fine: RoadSlot::Remove bumps the
	// generation.) Withdrawing a removed depot's vehicles now forgets the depot, so the redo is seeded like any placement.
	// Through the actor's live placement and its undo history, the path the player's keys take.
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("the actor"), Actor)) { return false; }
	Actor->FuelDepotDefinition = UEntityDefinition::MakeFuelDepotTransient();
	Actor->PlaceNode(FVector2D(0.0, 40000.0));
	if (!TestNotNull(TEXT("a network"), Actor->Network.Get())) { return false; }
	UOpsRuntime* Runtime = NewObject<UOpsRuntime>();
	Runtime->Attach(Actor);
	Runtime->Tick(1.0 / 30.0);

	const int32 Index = Actor->PlaceEntity(FVector2D(12000.0, 0.0), 0.0, EPlaceableEntity::FuelDepot);
	if (!TestTrue(TEXT("the depot is placed"), Index != INDEX_NONE)) { return false; }
	const FEntityInstanceId Depot = Actor->Network->EntityIdAt(Index);
	Runtime->Tick(1.0 / 30.0);
	const int32 Seeded = Runtime->GetJobBoard()->VehiclesAt(Depot);
	if (!TestTrue(TEXT("setup: the placement seeded the starter fleet"), Seeded > 0)) { return false; }

	if (!TestTrue(TEXT("setup: the placement is undoable"), Actor->Undo())) { return false; }
	Runtime->Tick(1.0 / 30.0);
	TestNull(TEXT("the undone depot is gone"), Actor->Network->GetEntity(Depot));
	TestEqual(TEXT("and its vehicles went with it"), Runtime->GetJobBoard()->VehiclesAt(Depot), 0);

	if (!TestTrue(TEXT("setup: and the undo is redoable"), Actor->Redo())) { return false; }
	TestTrue(TEXT("the premise: the redo hands back the SAME {Index, Generation} - else this measures an ordinary placement"),
		Actor->Network->GetEntity(Depot) != nullptr);
	Runtime->Tick(1.0 / 30.0);
	TestEqual(TEXT("the redone depot has its starter fleet again, not the empty yard an id already seen produced"),
		Runtime->GetJobBoard()->VehiclesAt(Depot), Seeded);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFleetBulldozedStarterSeededAgainTest, "AirportOps.Present.Fleet.ABulldozedStarterDepotIsSeededAgainWhateverItHeld",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFleetBulldozedStarterSeededAgainTest::RunTest(const FString& Parameters)
{
	// THE DEPOT'S REMOVAL DECIDES, NOT WHAT IT HELD WHEN IT WENT (#487, PR #491 review). The forget of SeededDepots used to fire as each
	// vehicle of a removed depot was withdrawn, so bulldoze -> undo (which restores the exact {Index, Generation}) gave a PARTLY sold
	// starter depot its whole fleet back while a SOLD-OUT one - no vehicle to withdraw - stayed unseeded for good. Both cases, through the
	// actor's own bulldoze and undo and the runtime's own sale: they must agree, and agree on "seeded again".
	for (const bool bSellEverything : { false, true })
	{
		const FString Named = bSellEverything ? TEXT("sold out") : TEXT("partly sold");
		FAirsideTestWorld TestWorld;
		ARoadNetworkActor* Actor = TestWorld.Actor;
		if (!TestNotNull(TEXT("the actor"), Actor)) { return false; }
		Actor->FuelDepotDefinition = UEntityDefinition::MakeFuelDepotTransient();
		Actor->PlaceNode(FVector2D(0.0, 40000.0));
		if (!TestNotNull(TEXT("a network"), Actor->Network.Get())) { return false; }
		UOpsRuntime* Runtime = NewObject<UOpsRuntime>();
		Runtime->Attach(Actor);
		Runtime->Tick(1.0 / 30.0);

		const int32 Index = Actor->PlaceEntity(FVector2D(12000.0, 0.0), 0.0, EPlaceableEntity::FuelDepot);
		if (!TestTrue(*FString::Printf(TEXT("%s: the depot is placed"), *Named), Index != INDEX_NONE)) { return false; }
		const FEntityInstanceId Depot = Actor->Network->EntityIdAt(Index);
		Runtime->Tick(1.0 / 30.0);
		const int32 Starter = Runtime->GetJobBoard()->VehiclesAt(Depot);
		if (!TestTrue(*FString::Printf(TEXT("%s: setup: the placement seeded at least two vehicles"), *Named), Starter >= 2)) { return false; }

		TArray<int32> Ids;
		for (const FServiceVehicle& Vehicle : Runtime->GetJobBoard()->GetVehicles())
		{
			if (Vehicle.Home == Depot) { Ids.Add(Vehicle.Id); }
		}
		const int32 ToSell = bSellEverything ? Ids.Num() : 1;
		for (int32 Sale = 0; Sale < ToSell; ++Sale)
		{
			if (!TestTrue(*FString::Printf(TEXT("%s: setup: a starter vehicle sells"), *Named), Runtime->SellVehicle(Ids[Sale]).Succeeded())) { return false; }
		}
		Runtime->Tick(1.0 / 30.0);
		TestEqual(*FString::Printf(TEXT("%s: setup: what is left is what was not sold"), *Named), Runtime->GetJobBoard()->VehiclesAt(Depot), Starter - ToSell);

		if (!TestTrue(*FString::Printf(TEXT("%s: the depot is bulldozed"), *Named), Actor->DeleteEntity(Index))) { return false; }
		Runtime->Tick(1.0 / 30.0);
		TestEqual(*FString::Printf(TEXT("%s: nothing of it is left on the board"), *Named), Runtime->GetJobBoard()->VehiclesAt(Depot), 0);

		if (!TestTrue(*FString::Printf(TEXT("%s: the bulldoze is undone"), *Named), Actor->Undo())) { return false; }
		TestTrue(*FString::Printf(TEXT("%s: the premise: the undo hands back the SAME {Index, Generation} - else this measures an ordinary placement"), *Named),
			Actor->Network->GetEntity(Depot) != nullptr);
		Runtime->Tick(1.0 / 30.0);
		TestEqual(*FString::Printf(TEXT("%s: the restored depot is seeded again, its whole starter fleet"), *Named),
			Runtime->GetJobBoard()->VehiclesAt(Depot), Starter);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFleetLoadDoesNotReseedTest, "AirportOps.Present.Fleet.LoadDoesNotReseedADepotThatHasVehicles",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFleetLoadDoesNotReseedTest::RunTest(const FString& Parameters)
{
	// A LOAD ANNOUNCES THE NETWORK IT ADOPTED (FNetworkChangedEvent, once) AND MARKS EVERY PASS DIRTY, so the "FleetSeed" pass
	// runs after every load - and must find every depot seen. The restored vehicles' depots are (FServiceFleet::Restored), and
	// so are the depots whose fleet was sold (SeededDepots is saved). There are no player saves yet (owner ruling), but a
	// test snapshot is one, and a pass that seeded again would double the fleet on every load.
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("the actor"), Actor)) { return false; }
	Actor->PlaceNode(FVector2D(0.0, 40000.0));
	if (!TestNotNull(TEXT("a network"), Actor->Network.Get())) { return false; }
	UEntityDefinition* DepotDef = UEntityDefinition::MakeFuelDepotTransient();
	Actor->Network->PlaceEntity(DepotDef, DepotDef->Anchors, FVector2D(12000.0, 0.0), 0.0, 0.0, DepotDef->PoseRole, DepotDef->Trucks);
	Actor->RebuildMesh();
	UOpsRuntime* Runtime = NewObject<UOpsRuntime>();
	Runtime->Attach(Actor);
	Runtime->Tick(1.0 / 30.0);
	const int32 Seeded = Runtime->GetJobBoard()->GetVehicles().Num();
	if (!TestTrue(TEXT("setup: the depot was seeded"), Seeded > 0)) { return false; }

	const FString Slot = TEXT("AirportOpsTest_FleetLoadDoesNotReseed");
	if (!TestTrue(TEXT("setup: the airport saves"), Runtime->SaveToSlot(Slot))) { return false; }
	if (!TestTrue(TEXT("and loads"), Runtime->LoadFromSlot(Slot))) { return false; }
	Runtime->Tick(1.0 / 30.0);
	TestEqual(TEXT("a load restores the depot's vehicles and the pass it wakes seeds nothing beside them"),
		Runtime->GetJobBoard()->GetVehicles().Num(), Seeded);

	// THE OTHER HALF: a starter fleet the player sold stays sold across a load.
	TArray<int32> Ids;
	for (const FServiceVehicle& Vehicle : Runtime->GetJobBoard()->GetVehicles()) { Ids.Add(Vehicle.Id); }
	for (const int32 Id : Ids)
	{
		TestTrue(TEXT("setup: each idle starter vehicle sells"), Runtime->GetJobBoard()->Fleet().Withdraw(Id, EFleetReason::Sold, 0.0));
	}
	if (!TestTrue(TEXT("setup: the sold-out airport saves"), Runtime->SaveToSlot(Slot))) { return false; }
	if (!TestTrue(TEXT("and loads"), Runtime->LoadFromSlot(Slot))) { return false; }
	for (int32 Tick = 0; Tick < 3; ++Tick) { Runtime->Tick(1.0 / 30.0); }
	TestEqual(TEXT("a sold starter fleet stays sold: the load's pass finds the depot seen"), Runtime->GetJobBoard()->GetVehicles().Num(), 0);
	return true;
}

#endif
