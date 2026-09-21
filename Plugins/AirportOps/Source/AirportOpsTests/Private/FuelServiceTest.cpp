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
#include "Model/SimClock.h"
#include "Profiles/RoadProfile.h"

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

		/**
		 * The DAY-COMPRESSED clock, beside the traffic's own seconds - the pair UFuelService
		 * now takes. RealSecondsPerGameDay is left at its default so Advance(real seconds)
		 * moves game time 72x faster, exactly as a session does.
		 */
		USimClock* Clock = nullptr;

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

		/** A depot whose plot holds a shed and a tank and NO PUMP. Set before Build. */
		bool bDepotWithoutPump = false;

		/**
		 * Seconds of GAME time an aircraft dispatched by this fixture spends on stand, or 0
		 * to leave FAirframe's authored default alone.
		 *
		 * Set BEFORE parking, because the demand's deadline is computed from the agent's own
		 * airframe the moment it parks - which is exactly the path under test. Overriding it
		 * afterwards would test a number the production code never read.
		 */
		double TurnaroundSeconds = 0.0;

		/**
		 * Lay a runway north of the taxiway, and a guideline onto it.
		 *
		 * Off by default: the fuel tests are about demands and trucks, and every one of them
		 * predates an aircraft that could leave on its own. With no runway a departure is
		 * refused NoRunway, which those tests want - their aircraft are supposed to sit there.
		 * The turnaround tests need one, because what they assert is the aeroplane going.
		 */
		bool bWithRunway = false;

		/**
		 * How far south of the stands the service road runs.
		 *
		 * MOVED IN FROM -6000 ON 2026-09-17, because the stands' reach moved. A stand offers
		 * its declared ENTRIES now, all on its aft edge, and the furthest of them sits 8450 uu
		 * from a road at -6000 against a DefaultServiceLinkRadius of 6500 - so not one linked,
		 * and with nothing able to pass under a wing the starboard services had no route at
		 * all. At -4000 the furthest entry is 6450 away, inside the reach with 50 to spare.
		 *
		 * THE DEPOT-ONLY FIXTURE STILL MEANS WHAT IT SAYS: with RoadFromX at 9000 the nearest
		 * road point is 8000 uu from the nearer stand's entries, well outside the reach, while
		 * the depot's pose at (12000, -2000) is 2000 from the road. See
		 * Build_RoadReachesDepotOnly.
		 */
		double RoadY = -4000.0;

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

		/**
		 * The furthest any agent's body moved between two consecutive steps of this fixture, and
		 * which agent, and when.
		 *
		 * WATCHED BY THE FIXTURE rather than by one test, because a teleport is a defect in the
		 * HANDOVERS between motion phases and every test here drives a truck through all of them.
		 * Watching it in one place means the next phase added gets the same check for free.
		 *
		 * IT EXISTS BECAUSE A SYNTHETIC TEST MISSED THE REAL FLOW. Airside's own reverse test
		 * drives a FRESH agent down the way out and saw nothing; in the game the SAME agent is
		 * parked at the service point and REDIRECTED home, which is a different handover, and
		 * that is the one the player watched jump.
		 */
		double WorstJump = 0.0;
		int32 WorstJumpAgent = 0;
		double WorstJumpAt = 0.0;

		/** Where it went from and to, and which phase it was in on arrival - so the report
		 *  names the handover rather than only its size. */
		FVector2D WorstJumpFrom = FVector2D::ZeroVector;
		FVector2D WorstJumpTo = FVector2D::ZeroVector;
		int32 WorstJumpPhase = 0;
		int32 WorstJumpPhaseBefore = -1;

	private:
		void RelayPhases();
		void RunAnchorLinks();
		void WatchForJumps();

		struct FSeen { FVector2D At = FVector2D::ZeroVector; int32 Phase = -1; };
		TMap<int32, FSeen> LastSeen;
		double Watched = 0.0;
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
	Clock = NewObject<USimClock>(GetTransientPackage());

	// UOpsRuntime::Attach's job in production (#104) - a bare NewObject has no Present/ to
	// set this, and an unset TruckAirframe means a truck dispatched with zero speed and
	// acceleration, not the one every other caller of DispatchAgent gets.
	Service->TruckAirframe = UAirsideSettings::ResolveDefaultVehicle();

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

	if (bWithRunway)
	{
		// THE SAME RECIPE Airside.Model.Traffic.DepartAgent uses - PAVEMENT split at the
		// point the guideline meets it, and a wide continuous profile. No SetRunwayFacts: the
		// defaults admit the default airframe, and a fixture that authored facts would be
		// asserting admission rules this test says nothing about.
		//
		// NORTH of the taxiway's far end, so a departure taxis AWAY from the stand and the
		// leaving is unmistakable on the phase.
		URoadProfile* Strip = URoadProfile::MakeTransient(4500.0, 1500.0, 450.0);
		Strip->bContinuousThroughJunctions = true;
		const FRoadNodeId West = Net->AddNode(FVector2D(-50000.0, 20000.0));
		const FRoadNodeId Mid = Net->AddNode(FVector2D(-10000.0, 20000.0));
		const FRoadNodeId East = Net->AddNode(FVector2D(50000.0, 20000.0));
		Net->AddStraightSegment(West, Mid, Strip);
		Net->AddStraightSegment(Mid, East, Strip);

		// FROM THE TAXIWAY'S OWN NORTH NODE, not from a fresh one at the same place. LayLine
		// adds nodes, so a second call at (-10000, 10000) puts a SECOND node there joined to
		// nothing - the runway was then found and refused NoRoute, which is a graph with two
		// components and no way between them.
		const FGuidelineNodeId OnStrip = Net->AddGuidelineNode(FVector2D(-10000.0, 20000.0), false);
		FGuidelineEdge ToStrip;
		ToStrip.A = TaxiNorth;
		ToStrip.B = OnStrip;
		ToStrip.Control = FVector2D(-10000.0, 15000.0);
		ToStrip.AllowedTraffic = FTrafficMask::Only(ETraversalClass::Aircraft);
		ToStrip.AllowedTraffic.Add(ETraversalClass::Emergency);
		ToStrip.Direction = EGuidelineDir::Bidirectional;
		ToStrip.Width = 600.0;
		ToStrip.bDerived = true;
		Net->AddGuidelineEdge(MoveTemp(ToStrip));
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
		if (bDepotWithoutPump)
		{
			// A MODULAR depot whose plot holds a shed and a tank and no pump. Placed through
			// FEntityPlacement because that is the only path that carries modules at all -
			// the plotless signature above has none, and a depot with no modules is not a
			// depot with no pump (see UFuelService::HasWorkingPump).
			FEntityPlacement Placement;
			Placement.Definition = DepotDef;
			Placement.Anchors = DepotDef->Anchors;
			Placement.Position = FVector2D(12000.0, RoadY + 4000.0);
			Placement.Heading = UE_DOUBLE_PI * 0.5;
			Placement.PoseRole = DepotDef->PoseRole;
			Placement.Modules = { EDepotModule::Shed, EDepotModule::Tank };
			Depot = Net->PlaceEntity(Placement);
		}
		else
		{
			Depot = Net->PlaceEntity(DepotDef, DepotDef->Anchors,
				FVector2D(12000.0, RoadY + 4000.0),
				UE_DOUBLE_PI * 0.5, 0.0, DepotDef->PoseRole, DepotDef->Trucks);
		}
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
	FAnchorLink::Build(*Net, UAirsideSettings::ResolveLargestServiceVehicle());
}

void FFuelFixture::RelayPhases()
{
	// EXACTLY WHAT UOpsRuntime::OnAgentPhase WILL DO (Task 8). Bound here so the world-free
	// tests exercise the same relay the composition does, rather than calling OnAgentPhase
	// by hand at moments the test chose.
	UFuelService* Bound = Service;
	URoadNetwork* Graph = Net;
	UGroundTraffic* Model = Traffic;
	USimClock* Time = Clock;
	Traffic->OnAgentPhaseChanged.AddLambda(
		[Bound, Graph, Model, Time](int32 AgentId, EAgentPhase From, EAgentPhase To)
		{
			Bound->OnAgentPhase(*Model, *Graph, *Time, AgentId, From, To);
		});
}

void FFuelFixture::Build_RoadReachesDepotOnly()
{
	// A ROAD THE STAND CANNOT REACH, and the number moved because the stand's reach did.
	//
	// The hydrant used to cast a RAY down -Y from x = -1200, and a road starting at x = 5000
	// was simply not on it. A stand now offers its DECLARED ENTRIES, all on its aft edge, and
	// joins anything within DefaultServiceLinkRadius of one. The nearer stand's aft edge is at
	// x = 1020, so a road starting at x = 9000 is about 8000 uu from its closest entry against
	// a reach of 6500 - outside it, which is what this fixture needs. The depot's pose at
	// (12000, -2000) is 2000 uu from the road, so what this fixture means is unchanged.
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

	FAirframe Airframe = UAirsideSettings::ResolveDefaultAirframe();
	if (TurnaroundSeconds > 0.0)
	{
		Airframe.TurnaroundSeconds = TurnaroundSeconds;
	}

	const int32 Id = Traffic->DispatchAgent(Net, Plan,
		Airframe, ETraversalClass::Aircraft, /*Shutdown=*/0.0);

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

		// THE CLOCK MOVES TOO, in the order UOpsRuntime runs it: game time first, then the
		// service reads both it and the movement seconds the traffic just advanced.
		Clock->Advance(Step);
		Service->Tick(*Traffic, *Net, *Clock);

		// AFTER THE SERVICE, not before, and that is the whole point of watching here. The
		// service is what REDIRECTS a truck - it hands the agent a new route, which poses it -
		// so a jump introduced by a redirect only exists on this side of the call.
		Watched += Step;
		WatchForJumps();
	}
}

void FFuelFixture::WatchForJumps()
{
	if (Traffic == nullptr)
	{
		return;
	}

	for (const FRoadAgent& Agent : Traffic->GetAgents())
	{
		const FVector2D At = FVector2D(Agent.LastMotion.Position);
		if (const FSeen* Before = LastSeen.Find(Agent.Id))
		{
			const double Moved = FVector2D::Distance(Before->At, At);
			if (Moved > WorstJump)
			{
				WorstJump = Moved;
				WorstJumpAgent = Agent.Id;
				WorstJumpAt = Watched;
				WorstJumpFrom = Before->At;
				WorstJumpTo = At;
				WorstJumpPhaseBefore = Before->Phase;
				WorstJumpPhase = static_cast<int32>(Agent.Phase);
			}
		}
		LastSeen.Add(Agent.Id, FSeen{ At, static_cast<int32>(Agent.Phase) });
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

	// A DEPOT WITH NO PUMP. On a road, with a truck, and still unable to fuel - so the
	// reason must name the PUMP. Before NoPump existed this fell through to NoRoute and
	// said "no road from depot" about a depot sitting on a road, sending the player to look
	// at the one thing that was already right. That is the same misdirection the busy-depot
	// branch was added to stop, which is why this case is pinned here rather than trusted.
	{
		FFuelFixture Fixture;
		Fixture.bDepotWithoutPump = true;
		Fixture.Build(/*bWithRoad=*/true);
		if (!TestTrue(TEXT("an aircraft parks"), Fixture.ParkAircraft() != 0)) { return false; }
		Fixture.Advance(0.2);

		if (!TestEqual(TEXT("one demand"), Fixture.Service->GetDemands().Num(), 1)) { return false; }
		TestEqual(TEXT("because the depot has no pump"),
			static_cast<int32>(Fixture.Service->GetDemands()[0].Why),
			static_cast<int32>(EFuelRefusal::NoPump));
		TestEqual(TEXT("and the card names the pump, not the road"),
			Fixture.Service->DescribeAgent(Fixture.Service->GetDemands()[0].AircraftId),
			FString(TEXT("depot has no pump")));
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
		// THE WORDING IS THE ASSERTION, not just the enum: this string is what the offer card
		// puts in front of the player, and the stand-routing spec promised it name the
		// ENTRANCES rather than report a bare "not on a road" (which sent the player looking at
		// the stand's sides, where there is nothing to draw).
		TestEqual(TEXT("and the card names what the road has to reach"),
			Fixture.Service->DescribeAgent(Fixture.Service->GetDemands()[0].AircraftId),
			FString(TEXT("no road within reach of the stand's entrances")));
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
	// re-offered once the truck is home. The figure covers the first truck's drive out, its
	// 40 s dwell and its drive home, and is a bound rather than a wait.
	//
	// 450 s, RAISED FROM 300 ON 2026-09-17, and the round trip got genuinely longer rather
	// than the bound getting sloppy. The truck now BACKS OUT of the service point instead of
	// turning round on the spot and driving away forwards: 2529 uu of reverse leg at the 100
	// uu/s of FTrafficRules::ServiceReverseSpeed is 25 s where a forward pass took 5, and the
	// way out is 1658 uu longer besides, because the one-way cycle no longer lets the route
	// retrace the serve leg. Verified as a bound and not a stall before the number moved: the
	// same test passes at 1200 s, so the truck does get home.
	TestTrue(TEXT("and once the truck is home the second aircraft gets it"),
		Fixture.AdvanceUntil([&SecondDemand]
		{
			const FFuelDemand* Demand = SecondDemand();
			return Demand != nullptr && Demand->TruckId != 0;
		}, 450.0));

	// The card never said anything false along the way.
	if (const FFuelDemand* Served = SecondDemand())
	{
		TestEqual(TEXT("and was never marked unserviceable on the way"),
			static_cast<int32>(Served->Why), static_cast<int32>(EFuelRefusal::None));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFuelTurnaroundDepartsTest,
	"AirportOps.Model.TurnaroundDeparts",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFuelTurnaroundDepartsTest::RunTest(const FString& Parameters)
{
	FFuelFixture Fixture;

	// LONG ENOUGH THAT THE TRUCK IS STILL OUT WHEN IT EXPIRES, which is the first assertion
	// below and the reason this is not simply the authored 1800. The clock runs 72x real at
	// the default day, so 1800 game seconds is 25 real ones - comfortably inside the drive
	// out and the 40 s dwell, and therefore not a number that has to be tuned to stay true.
	Fixture.TurnaroundSeconds = 1800.0;
	Fixture.bWithRunway = true;
	Fixture.Build(/*bWithRoad=*/true);

	const int32 Aircraft = Fixture.ParkAircraft();
	if (!TestTrue(TEXT("an aircraft parked"), Aircraft != 0)) { return false; }

	auto Demand = [&Fixture, Aircraft]() -> const FFuelDemand*
	{
		for (const FFuelDemand& Each : Fixture.Service->GetDemands())
		{
			if (Each.AircraftId == Aircraft) { return &Each; }
		}
		return nullptr;
	};
	auto Phase = [&Fixture, Aircraft]
	{
		const FRoadAgent* Agent = Fixture.Traffic->FindAgent(Aircraft);
		return Agent != nullptr ? Agent->Phase : EAgentPhase::Gone;
	};

	if (!TestNotNull(TEXT("it demanded fuel"), Demand())) { return false; }
	const double Deadline = Demand()->TurnaroundEndsAt;

	// THE DEADLINE IS NOT A GUILLOTINE. Advanced until it has passed, then asserted that an
	// aircraft still being served has NOT been sent - a job it asked for is finished first,
	// which is what stops a truck being stranded at a hydrant nobody is at.
	TestTrue(TEXT("the turnaround runs out"),
		Fixture.AdvanceUntil([&Fixture, Deadline] { return Fixture.Clock->Now() >= Deadline; }, 120.0));

	if (const FFuelDemand* Now = Demand();
		TestNotNull(TEXT("and the demand is still open"), Now))
	{
		TestNotEqual(TEXT("because the truck has not finished"),
			static_cast<int32>(Now->State), static_cast<int32>(EFuelDemandState::Done));
		TestEqual(TEXT("so the aircraft is still on its stand"),
			static_cast<int32>(Phase()), static_cast<int32>(EAgentPhase::Parked));
	}

	// AND THEN IT GOES, on its own, with nothing pressing Depart. Measured on the PHASE,
	// which is what the player watches; asserting the demand was dropped would assert the
	// cause from its own effect, since leaving is what drops it.
	TestTrue(TEXT("once fuelled and out of time it departs by itself"),
		Fixture.AdvanceUntil([&Phase] { return Phase() != EAgentPhase::Parked; }, 300.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFuelUnserviceableStillDepartsTest,
	"AirportOps.Model.UnserviceableStillDeparts",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFuelUnserviceableStillDepartsTest::RunTest(const FString& Parameters)
{
	FFuelFixture Fixture;
	Fixture.TurnaroundSeconds = 1800.0;
	Fixture.bWithRunway = true;

	// NO DEPOT AT ALL, so the demand goes Unserviceable for a real reason rather than by
	// being written there - the same airport AirportOps.Model.FuelServiceRefusals uses.
	Fixture.Build(/*bWithRoad=*/true, /*bWithDepot=*/false);

	const int32 Aircraft = Fixture.ParkAircraft();
	if (!TestTrue(TEXT("an aircraft parked"), Aircraft != 0)) { return false; }

	TestTrue(TEXT("nothing can serve it"), Fixture.AdvanceUntil([&Fixture, Aircraft]
	{
		for (const FFuelDemand& Each : Fixture.Service->GetDemands())
		{
			if (Each.AircraftId == Aircraft)
			{
				return Each.State == EFuelDemandState::Unserviceable;
			}
		}
		return false;
	}, 60.0));

	// THE STAND FREES ITSELF. The whole reason the grace period IS the turnaround and not a
	// second figure: an airport with no depot costs the player a turnaround of throughput,
	// and does not silt up with aircraft that can never leave.
	TestTrue(TEXT("an aircraft nothing can serve leaves anyway once its turnaround is up"),
		Fixture.AdvanceUntil([&Fixture, Aircraft]
		{
			const FRoadAgent* Agent = Fixture.Traffic->FindAgent(Aircraft);
			return Agent == nullptr || Agent->Phase != EAgentPhase::Parked;
		}, 300.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTruckNeverTeleportsOnItsRoundTripTest, "AirportOps.Ops.TruckNeverTeleportsOnItsRoundTrip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTruckNeverTeleportsOnItsRoundTripTest::RunTest(const FString& Parameters)
{
	// THE WHOLE ROUND TRIP, WATCHED FRAME BY FRAME. Dispatch, the drive out, the dwell, the
	// REDIRECT home and the drive back - every handover between motion phases the game has,
	// in the order and by the caller the game uses.
	//
	// AIRSIDE'S OWN REVERSE TEST DOES NOT COVER THIS, and that gap is why the defect reached
	// PIE twice. It starts a FRESH agent on the way out, so it exercises arming and the
	// hand-back but never the REDIRECT: in the game the same agent is parked at the service
	// point when the service hands it a route home, and the log shows the reverse being armed
	// from inside that redirect's own posing step. A synthetic start cannot see it.
	//
	// A FIXTURE-WIDE WATCH rather than an assertion of this test's own, so every other test in
	// this file pays for it too and the next phase added is covered without being remembered.
	FFuelFixture Fixture;
	Fixture.Build(/*bWithRoad=*/true);
	Fixture.JoinRoad();
	const int32 Aircraft = Fixture.ParkAircraft();
	if (!TestTrue(TEXT("an aircraft parked and asked for fuel"), Aircraft != 0))
	{
		return false;
	}

	// UNTIL THE TRUCK IS HOME AND RETIRED, which is the last handover of the trip.
	const bool bDone = Fixture.AdvanceUntil([&Fixture]
		{
			const TArray<FFuelDemand>& Demands = Fixture.Service->GetDemands();
			return Demands.Num() > 0 && Demands[0].State == EFuelDemandState::Done;
		}, 600.0);

	AddInfo(FString::Printf(
		TEXT("round trip %s; furthest any body moved in one 1/30 s step was %.1f uu, by agent "
		     "%d at t=%.1f s, from (%.0f,%.0f) to (%.0f,%.0f), phase %d -> %d"),
		bDone ? TEXT("completed") : TEXT("DID NOT COMPLETE"),
		Fixture.WorstJump, Fixture.WorstJumpAgent, Fixture.WorstJumpAt,
		Fixture.WorstJumpFrom.X, Fixture.WorstJumpFrom.Y,
		Fixture.WorstJumpTo.X, Fixture.WorstJumpTo.Y,
		Fixture.WorstJumpPhaseBefore, Fixture.WorstJumpPhase));

	TestTrue(TEXT("the round trip completed, so the watch below saw all of it"), bDone);

	// 60 uu IN A THIRTIETH is 1800 uu/s, nearly twice the taxi cap, so ordinary motion cannot
	// reach it and a handover that re-poses the body cannot hide under it. The two already
	// found were a wheelbase (494 uu) and a whole reverse span (2529 uu).
	TestTrue(
		*FString::Printf(TEXT("no body ever teleports (worst %.1f uu, agent %d, t=%.1f s)"),
			Fixture.WorstJump, Fixture.WorstJumpAgent, Fixture.WorstJumpAt),
		Fixture.WorstJump < 60.0);

	return true;
}

#endif
