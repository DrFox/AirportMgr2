#include "CoreMinimal.h"
#include "Build/AnchorLink.h"
#include "Content/AirsideSettings.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/FuelService.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadAgent.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadTraffic.h"
#include "Model/RouteSearch.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/**
	 * One stand, one depot, and a service road joining them - world-free.
	 *
	 * HAND-AUTHORED GUIDELINES rather than a solve, exactly as the M2 traffic fixtures do:
	 * what is under test is the SERVICE, and a fixture that had to lay pavement correctly
	 * first would fail for reasons that have nothing to do with fuel.
	 *
	 * The geometry, because the anchor rays decide it (see Airside.Entities.
	 * StandFuelAnchorJoinsRoad, which derives the same arithmetic):
	 *   - the stand sits at the origin facing +X, so its POSE ray leaves toward -X at a
	 *     north-south TAXIWAY to the west;
	 *   - its HydrantPit anchor is at local (-1200, +700) heading -90, so it casts down -Y
	 *     at an east-west ROAD to the south;
	 *   - the depot sits north of that road facing +Y, so its own pose ray leaves toward -Y
	 *     at the same road.
	 */
	struct FFuelFixture
	{
		URoadNetwork* Net = nullptr;
		UGroundTraffic* Traffic = nullptr;
		UFuelService* Service = nullptr;

		FEntityInstanceId Stand;
		FEntityInstanceId Depot;
		FGuidelineNodeId StandPose;
		FGuidelineNodeId DepotPose;
		FGuidelineNodeId TaxiwayFarEnd;

		/** A SECOND stand, for the queue case. Unset unless bSecondStand was set before Build. */
		FEntityInstanceId Stand2;
		FGuidelineNodeId StandPose2;

		/**
		 * Place a second stand east of the first, on the same road and the same taxiway.
		 *
		 * Off by default: every other test in this file is about ONE demand, and a second
		 * stand would put a second lane on the graph for them to trip over. The queue case
		 * needs two, because one depot with one truck cannot be busy for the aircraft that
		 * is using the truck.
		 */
		bool bSecondStand = false;

		double RoadY = -6000.0;

		/** West end of the road. Default reaches under the stand; see Build_RoadReachesDepotOnly. */
		double RoadFromX = -20000.0;

		void Build(bool bWithRoad, bool bWithDepot = true);

		/**
		 * Lay the road only EAST of the stand, so the depot's pose ray reaches it and the
		 * stand's hydrant ray - which leaves from x = -1200 - misses it entirely.
		 *
		 * The case StandUnjoined exists for, and which could not fire until 2026-09-07: the
		 * test for it read StandFuel.IsSet(), a fact about placement rather than about the
		 * airport, so an unjoined hydrant was reported as NoRoute instead.
		 */
		void Build_RoadReachesDepotOnly();
		void JoinRoad();
		int32 ParkAircraft();

		/** ParkAircraft, at a named stand's pose. */
		int32 ParkAircraftAt(FGuidelineNodeId Pose);
		void Advance(double Seconds);

		/**
		 * Advance until Predicate holds, or give up after MaxSeconds. Returns whether it held.
		 *
		 * A BOUND, NOT A WAIT, and the reason a flat Advance is wrong for a state machine:
		 * "advance 120 s and assert Fuelling" passed the drive AND the whole dwell and landed
		 * on Done - a test that fails for being too patient rather than for anything the code
		 * did. Advancing until the state arrives says what the test means and cannot drift
		 * when a figure is tuned.
		 */
		bool AdvanceUntil(TFunctionRef<bool()> Predicate, double MaxSeconds);

	private:
		void RelayPhases();
		void RunAnchorLinks();
	};

	void LayLine(URoadNetwork& Net, const FVector2D& From, const FVector2D& To,
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

void FFuelFixture::Build(bool bWithRoad, bool bWithDepot)
{
	Net = NewObject<URoadNetwork>(GetTransientPackage());
	Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	Service = NewObject<UFuelService>(GetTransientPackage());

	FGuidelineNodeId TaxiSouth, TaxiNorth;
	LayLine(*Net, FVector2D(-10000.0, -10000.0), FVector2D(-10000.0, 10000.0),
		ETraversalClass::Aircraft, TaxiSouth, TaxiNorth);
	TaxiwayFarEnd = TaxiSouth;

	if (bWithRoad)
	{
		FGuidelineNodeId RoadWest, RoadEast;
		LayLine(*Net, FVector2D(RoadFromX, RoadY), FVector2D(20000.0, RoadY),
			ETraversalClass::GroundVehicle, RoadWest, RoadEast);
	}

	UEntityDefinition* StandDef = UEntityDefinition::MakeStandTransient();
	Stand = Net->PlaceEntity(StandDef, StandDef->Anchors, FVector2D(0.0, 0.0), 0.0,
		/*DesignWingspan=*/3600.0, StandDef->PoseRole, StandDef->Trucks);

	if (bSecondStand)
	{
		// SIX THOUSAND EAST. A Code C lane is 5250 uu wide, so at x = 6000 the two lanes
		// clear each other by 750 uu and each is nearer the road (3910 uu) than the other -
		// and this stand's pose ray still reaches the taxiway 16 000 uu west, inside the
		// 20 000 uu aircraft cap.
		Stand2 = Net->PlaceEntity(StandDef, StandDef->Anchors, FVector2D(6000.0, 0.0), 0.0,
			/*DesignWingspan=*/3600.0, StandDef->PoseRole, StandDef->Trucks);
	}

	if (bWithDepot)
	{
		UEntityDefinition* DepotDef = UEntityDefinition::MakeFuelDepotTransient();

		// North of the road, facing +Y, so its pose ray leaves toward -Y and meets the road.
		// Well east of the stand so the two lead-ins never fight over the same stretch.
		Depot = Net->PlaceEntity(DepotDef, DepotDef->Anchors, FVector2D(12000.0, RoadY + 4000.0),
			UE_DOUBLE_PI * 0.5, 0.0, DepotDef->PoseRole, DepotDef->Trucks);
	}

	RunAnchorLinks();
	StandPose = Net->GetEntity(Stand)->PoseNode;
	if (Stand2.IsSet())
	{
		StandPose2 = Net->GetEntity(Stand2)->PoseNode;
	}
	if (Depot.IsSet())
	{
		DepotPose = Net->GetEntity(Depot)->PoseNode;
	}

	RelayPhases();
}

void FFuelFixture::RunAnchorLinks()
{
	// FAnchorLink lives in Airside's Build/, which this TEST module may include -
	// Check-Architecture's layer rules apply inside the plugin modules themselves, not to
	// their test modules. Called directly rather than through a solve, because this fixture
	// authors its guidelines by hand and has no pavement for a solve to work on.
	FAnchorLink::Build(*Net);
}

void FFuelFixture::RelayPhases()
{
	// EXACTLY WHAT UOpsRuntime::OnAgentPhase WILL DO (Task 8). Bound here so the world-free
	// tests exercise the same relay the composition does, rather than calling OnAgentPhase
	// by hand at moments the test chose.
	UFuelService* Bound = Service;
	URoadNetwork* Graph = Net;
	UGroundTraffic* Model = Traffic;
	Traffic->OnAgentPhaseChanged.AddLambda(
		[Bound, Graph, Model](int32 AgentId, EAgentPhase From, EAgentPhase To)
		{
			Bound->OnAgentPhase(*Model, *Graph, AgentId, From, To);
		});
}

void FFuelFixture::Build_RoadReachesDepotOnly()
{
	// A ROAD THE STAND CANNOT REACH, and the number moved because the stand's reach did.
	//
	// The hydrant used to cast a RAY down -Y from x = -1200, and a road starting at x = 5000
	// was simply not on it. A stand now offers its whole SERVICE LANE - a box out to
	// (+1700, -2090) - and joins anything within 50 m of any part of it in any direction. At
	// x = 5000 that leaves 51 m of margin, which is a fixture one rounding away from testing
	// the opposite of what it says. The depot's pose at x = 12000 is 40 m from the road
	// either way, so what this fixture means is unchanged.
	RoadFromX = 9000.0;
	Build(/*bWithRoad=*/true);
}

void FFuelFixture::JoinRoad()
{
	FGuidelineNodeId RoadWest, RoadEast;
	LayLine(*Net, FVector2D(-20000.0, RoadY), FVector2D(20000.0, RoadY),
		ETraversalClass::GroundVehicle, RoadWest, RoadEast);
	RunAnchorLinks();
}

int32 FFuelFixture::ParkAircraft()
{
	return ParkAircraftAt(StandPose);
}

int32 FFuelFixture::ParkAircraftAt(FGuidelineNodeId Pose)
{
	FRouteQuery Query;
	Query.Start = TaxiwayFarEnd;
	Query.Goal = Pose;
	Query.Class = ETraversalClass::Aircraft;
	const FRoutePlan Plan = RouteSearch::Find(*Net, Query);
	if (!Plan.IsValid())
	{
		return 0;
	}

	const int32 Id = Traffic->DispatchAgent(Net, Plan,
		UAirsideSettings::ResolveDefaultAirframe(), ETraversalClass::Aircraft, /*Shutdown=*/0.0);

	// Run it in. 120 s is generous for a 20 000 uu taxi and is a bound, not a wait.
	for (int32 Step = 0; Step < 3600; ++Step)
	{
		const FRoadAgent* Agent = Traffic->FindAgent(Id);
		if (Agent != nullptr && Agent->Phase == EAgentPhase::Parked)
		{
			break;
		}
		Advance(1.0 / 30.0);
	}
	return Id;
}

void FFuelFixture::Advance(double Seconds)
{
	// THE TWO TOGETHER, in the order UOpsRuntime runs them: the traffic moves and announces,
	// then the service reads the clock the traffic just advanced.
	constexpr double Step = 1.0 / 30.0;
	for (double Elapsed = 0.0; Elapsed < Seconds; Elapsed += Step)
	{
		Traffic->Advance(Step, Net);
		Service->Tick(*Traffic, *Net);
	}
}

bool FFuelFixture::AdvanceUntil(TFunctionRef<bool()> Predicate, double MaxSeconds)
{
	constexpr double Step = 1.0 / 30.0;
	for (double Elapsed = 0.0; Elapsed < MaxSeconds; Elapsed += Step)
	{
		if (Predicate())
		{
			return true;
		}
		Advance(Step);
	}
	return Predicate();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFuelServiceTest, "AirportOps.Ops.FuelService",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFuelServiceTest::RunTest(const FString& Parameters)
{
	FFuelFixture Fixture;
	Fixture.Build(/*bWithRoad=*/true);

	const int32 Aircraft = Fixture.ParkAircraft();
	if (!TestTrue(TEXT("an aircraft parked at the stand"), Aircraft != 0)) { return false; }

	// 1. PARKING MAKES A DEMAND, and nothing else does. No button and no offer - the player's
	// only act was to build a depot on a road that reaches the stand (spec §2).
	if (!TestEqual(TEXT("one demand"), Fixture.Service->GetDemands().Num(), 1)) { return false; }
	TestEqual(TEXT("for the aircraft that parked"),
		Fixture.Service->GetDemands()[0].AircraftId, Aircraft);
	TestEqual(TEXT("at the stand it parked on"),
		Fixture.Service->GetDemands()[0].Stand, Fixture.Stand);

	// 2. A TRUCK GOES OUT on the next tick, and the demand names it and its depot.
	Fixture.Advance(0.2);
	if (!TestEqual(TEXT("a truck is en route"),
		static_cast<int32>(Fixture.Service->GetDemands()[0].State),
		static_cast<int32>(EFuelDemandState::TruckEnRoute))) { return false; }

	const int32 TruckId = Fixture.Service->GetDemands()[0].TruckId;
	if (!TestTrue(TEXT("and it is a real agent"), TruckId != 0)) { return false; }
	TestEqual(TEXT("dispatched from the depot"),
		Fixture.Service->GetDemands()[0].Depot, Fixture.Depot);
	TestEqual(TEXT("as a ground vehicle"),
		static_cast<int32>(Fixture.Traffic->FindAgent(TruckId)->Class),
		static_cast<int32>(ETraversalClass::GroundVehicle));

	// 3. IT REACHES THE HYDRANT AND FUELS.
	if (!TestTrue(TEXT("the truck reaches the hydrant and starts fuelling"),
		Fixture.AdvanceUntil(
			[&Fixture] {
				return Fixture.Service->GetDemands().Num() == 1
					&& Fixture.Service->GetDemands()[0].State == EFuelDemandState::Fuelling;
			}, 240.0))) { return false; }

	// 4. THE DWELL IS THE DWELL. Measured in UGroundTraffic's sim seconds - the same clock
	// the truck's own motion accrues on - so 40 s of dwell is 40 s of watching, not the 0.55
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
	TestTrue(TEXT("and it took the whole dwell, not less"),
		Fixture.Traffic->GetSimSeconds() - DwellStarted >= Fixture.Service->DwellSeconds);

	// 5. HOME AND RETIRED. A truck does not fly away, so nothing else would ever remove it -
	// which is exactly what UGroundTraffic::RetireAgent exists for.
	Fixture.AdvanceUntil(
		[&Fixture, TruckId] { return Fixture.Traffic->FindAgent(TruckId) == nullptr; }, 300.0);
	TestNull(TEXT("the truck is retired at the depot"), Fixture.Traffic->FindAgent(TruckId));
	TestEqual(TEXT("and is no longer counted as going home"),
		Fixture.Service->TrucksGoingHomeForTest(), 0);

	// 6. THE DEPOT'S COUNT IS FREE AGAIN: the next aircraft gets a truck.
	//
	// The first aircraft goes first, and not only for tidiness: it is still PARKED on the
	// stand's pose node and holds it, so a second arrival cannot reach the stand at all. A
	// version of this that just parked another aeroplane measured nothing - the second never
	// arrived, so it never demanded fuel, and the assertion failed for a reason that had
	// nothing to do with the depot's count.
	Fixture.Traffic->RetireAgent(Aircraft);
	Fixture.Advance(0.2);
	TestEqual(TEXT("the fuelled aircraft's demand goes with it"),
		Fixture.Service->GetDemands().Num(), 0);

	const int32 Second = Fixture.ParkAircraft();
	if (!TestTrue(TEXT("a second aircraft parks at the freed stand"), Second != 0)) { return false; }
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
	// (spec §6): "no fuel depot" is a building they can go and place.
	{
		FFuelFixture Fixture;
		Fixture.Build(/*bWithRoad=*/true, /*bWithDepot=*/false);
		if (!TestTrue(TEXT("an aircraft parks"), Fixture.ParkAircraft() != 0)) { return false; }
		Fixture.Advance(0.2);

		if (!TestEqual(TEXT("one demand"), Fixture.Service->GetDemands().Num(), 1)) { return false; }
		TestEqual(TEXT("unserviceable"),
			static_cast<int32>(Fixture.Service->GetDemands()[0].State),
			static_cast<int32>(EFuelDemandState::Unserviceable));
		TestEqual(TEXT("because there is no depot"),
			static_cast<int32>(Fixture.Service->GetDemands()[0].Why),
			static_cast<int32>(EFuelRefusal::NoDepot));
		TestEqual(TEXT("and the card says so"),
			Fixture.Service->DescribeAgent(Fixture.Service->GetDemands()[0].AircraftId),
			FString(TEXT("no fuel depot")));
	}

	// DEPOT OFF ANY ROAD. The stand's hydrant is unjoined too, and NoRoad wins by the
	// spec's stated order - the depot is the thing nearest the player's hand.
	{
		FFuelFixture Fixture;
		Fixture.Build(/*bWithRoad=*/false);
		if (!TestTrue(TEXT("an aircraft parks"), Fixture.ParkAircraft() != 0)) { return false; }
		Fixture.Advance(0.2);

		if (!TestEqual(TEXT("one demand"), Fixture.Service->GetDemands().Num(), 1)) { return false; }
		TestEqual(TEXT("depot not on a road"),
			static_cast<int32>(Fixture.Service->GetDemands()[0].Why),
			static_cast<int32>(EFuelRefusal::NoRoad));

		// A REBUILD RE-OFFERS IT. The player may have just drawn the road, and a terminal
		// state that never looked again would leave them staring at a truck that never comes.
		// The graph REVISION is what says the airport changed - see
		// URoadNetwork::GetGuidelineRevision.
		Fixture.JoinRoad();
		Fixture.Advance(0.2);
		TestNotEqual(TEXT("the demand is live again once the road is drawn"),
			static_cast<int32>(Fixture.Service->GetDemands()[0].State),
			static_cast<int32>(EFuelDemandState::Unserviceable));
	}

	// THE DEPOT IS ON A ROAD AND THE STAND'S HYDRANT IS NOT.
	//
	// This case could not fire until 2026-09-07: the test read StandFuel.IsSet(), which is
	// true for every stand ever placed (PlaceEntity makes a node per anchor whether anything
	// reaches it or not), so an unjoined hydrant was reported as NoRoute - "no road from
	// depot" - and sent the player to look at the wrong end of the airport. Seen in PIE
	// before it was seen here, which is why it is pinned now.
	{
		FFuelFixture Fixture;
		Fixture.Build_RoadReachesDepotOnly();
		if (!TestTrue(TEXT("an aircraft parks"), Fixture.ParkAircraft() != 0)) { return false; }
		Fixture.Advance(0.2);

		if (!TestEqual(TEXT("one demand"), Fixture.Service->GetDemands().Num(), 1)) { return false; }
		TestEqual(TEXT("the reason names the STAND, not the depot"),
			static_cast<int32>(Fixture.Service->GetDemands()[0].Why),
			static_cast<int32>(EFuelRefusal::StandUnjoined));
		TestEqual(TEXT("and the card says so"),
			Fixture.Service->DescribeAgent(Fixture.Service->GetDemands()[0].AircraftId),
			FString(TEXT("stand not on a road")));
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
	if (!TestTrue(TEXT("an aircraft parked"), Aircraft != 0)) { return false; }
	Fixture.Advance(0.2);

	const int32 TruckId = Fixture.Service->GetDemands()[0].TruckId;
	if (!TestTrue(TEXT("a truck went out for it"), TruckId != 0)) { return false; }

	// The aircraft goes mid-service. The truck must NOT be left standing at a hydrant nobody
	// is using - that node would be held against every later job - and the depot's count must
	// come back, or one departure costs the airport a truck for the rest of the session.
	Fixture.Traffic->RetireAgent(Aircraft);
	Fixture.Advance(0.2);

	TestEqual(TEXT("the demand is dropped"), Fixture.Service->GetDemands().Num(), 0);
	if (!TestNotNull(TEXT("but the truck still exists"),
		Fixture.Traffic->FindAgent(TruckId))) { return false; }
	TestEqual(TEXT("and is on its way home"), Fixture.Service->TrucksGoingHomeForTest(), 1);
	TestEqual(TEXT("heading for the depot's own pose node"),
		Fixture.Traffic->FindAgent(TruckId)->GoalNode, Fixture.DepotPose);

	Fixture.AdvanceUntil(
		[&Fixture, TruckId] { return Fixture.Traffic->FindAgent(TruckId) == nullptr; }, 300.0);
	TestNull(TEXT("and is retired when it gets there"), Fixture.Traffic->FindAgent(TruckId));

	// THE COUNT IS BACK. Measured by serving the next aircraft rather than by reading an
	// internal counter: what matters is that the depot can dispatch again.
	const int32 Next = Fixture.ParkAircraft();
	if (!TestTrue(TEXT("another aircraft parks"), Next != 0)) { return false; }
	Fixture.Advance(0.2);

	if (!TestEqual(TEXT("it has a demand"), Fixture.Service->GetDemands().Num(), 1)) { return false; }
	TestTrue(TEXT("and the depot has a truck for it"),
		Fixture.Service->GetDemands()[0].TruckId != 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFuelQueuesOnABusyDepotTest, "AirportOps.Ops.FuelQueuesOnABusyDepot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFuelQueuesOnABusyDepotTest::RunTest(const FString& Parameters)
{
	// A DEPOT WITH ITS ONLY TRUCK OUT IS BUSY, NOT BROKEN - and until this test it was
	// reported as NoRoute, "no road from depot", which is a lie about the AIRPORT and it
	// stuck: Unserviceable is only re-offered when the guideline revision changes, and a
	// truck coming home changes no guideline. The second aircraft therefore waited for ever
	// while the player was sent to look at a road that was already there. Seen in PIE
	// 2026-09-08.
	//
	// ChooseDepot's busy branch always meant to leave the demand alone - "Busy, not broken.
	// Deliberately does NOT set a refusal" - but the else-chain below it assigned one
	// unconditionally, so the branch's intent never reached the caller. A comment describing
	// behaviour the code does not have; see CLAUDE.md.
	FFuelFixture Fixture;
	Fixture.bSecondStand = true;
	Fixture.Build(/*bWithRoad=*/true);

	const int32 First = Fixture.ParkAircraft();
	if (!TestTrue(TEXT("an aircraft parked at the first stand"), First != 0)) { return false; }

	// The one truck goes out for it.
	if (!TestTrue(TEXT("the depot dispatches its truck"),
		Fixture.AdvanceUntil([&Fixture]
		{
			const FFuelDemand* Demand = Fixture.Service->GetDemands().Num() > 0
				? &Fixture.Service->GetDemands()[0] : nullptr;
			return Demand != nullptr && Demand->TruckId != 0;
		}, 30.0)))
	{
		return false;
	}

	const int32 Second = Fixture.ParkAircraftAt(Fixture.StandPose2);
	if (!TestTrue(TEXT("a second aircraft parked at the second stand"), Second != 0)) { return false; }

	auto SecondDemand = [&Fixture, Second]() -> const FFuelDemand*
	{
		for (const FFuelDemand& Demand : Fixture.Service->GetDemands())
		{
			if (Demand.AircraftId == Second) { return &Demand; }
		}
		return nullptr;
	};

	if (!TestNotNull(TEXT("the second aircraft made a demand"), SecondDemand())) { return false; }

	// A tick or two is all it takes: the state machine offers a Needed demand every tick.
	Fixture.Advance(0.5);

	const FFuelDemand* Waiting = SecondDemand();
	if (!TestNotNull(TEXT("the second demand survives"), Waiting)) { return false; }

	// THE DEFECT, DIRECTLY. Not "the card reads oddly" - Unserviceable is TERMINAL until the
	// graph changes, so this state is the aircraft never being fuelled.
	TestEqual(TEXT("a demand waiting on a busy truck stays Needed, not Unserviceable"),
		static_cast<int32>(Waiting->State), static_cast<int32>(EFuelDemandState::Needed));
	TestEqual(TEXT("and carries no refusal, because nothing about the airport is wrong"),
		static_cast<int32>(Waiting->Why), static_cast<int32>(EFuelRefusal::None));

	// AND THE QUEUE ACTUALLY DRAINS, with NO edit to the airport. This is the half a
	// state-only assertion would miss: Needed is worth nothing if the demand is never
	// re-offered once the truck is home. 300 s covers the first truck's drive out, its
	// 40 s dwell and its drive home, and is a bound rather than a wait.
	TestTrue(TEXT("and once the truck is home the second aircraft gets it"),
		Fixture.AdvanceUntil([&SecondDemand]
		{
			const FFuelDemand* Demand = SecondDemand();
			return Demand != nullptr && Demand->TruckId != 0;
		}, 300.0));

	// The card never said anything false along the way.
	if (const FFuelDemand* Served = SecondDemand())
	{
		TestEqual(TEXT("and was never marked unserviceable on the way"),
			static_cast<int32>(Served->Why), static_cast<int32>(EFuelRefusal::None));
	}
	return true;
}

#endif
