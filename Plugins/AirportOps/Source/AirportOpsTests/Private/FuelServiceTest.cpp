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
#include "Model/RoutePolicy.h"
#include "Model/RoutePlanCache.h"
#include "Model/RouteSearch.h"
#include "Model/SimClock.h"
#include "Model/Vehicle.h"
#include "Profiles/RoadProfile.h"
#include "Solve/IcaoCode.h"
#include "Solve/StandBox.h"
#include "Present/AirsideTraffic.h"
#include "Present/OpsRuntime.h"
#include "Present/RoadNetworkActor.h"
#include "Testing/AirsideTestGraph.h"
#include "Testing/AirsideTestWorld.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace FuelServiceTest
{
	/**
	 * A stand of Letter, DRAWN at its letter's floor box with the stop mark on At, facing +X -
	 * the shape the plot tool commits. The OUTLINE is what UFuelService::VehicleFor reads the
	 * letter from, so a plop (which placement gives a Code C box whatever its template) would
	 * not be an A stand to the service at all.
	 */
	FEntityInstanceId PlaceDrawnStand(URoadNetwork& Net, EIcaoCode Letter, const FVector2D& At)
	{
		UEntityDefinition* Def = UEntityDefinition::MakeStandTransient(Letter);
		StandBox::FStandPose Pose;
		Pose.Position = At;
		Pose.Facing = FVector2D(1.0, 0.0);
		FEntityPlacement Placement;
		Placement.Definition = Def;
		Placement.Anchors = Def->Anchors;
		Placement.Position = At;
		Placement.Heading = 0.0;
		Placement.DesignWingspan = IcaoCode::DesignSpanForLetter(Letter);
		Placement.PoseRole = Def->PoseRole;
		Placement.Trucks = Def->Trucks;
		StandBox::BoxAt(Pose, Letter, IcaoCode::FloorEnvelopeForLetter(Letter), Placement.Outline);
		return Net.PlaceEntity(Placement);
	}
}

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

		/**
		 * Draw the first stand as this letter's floor box (FuelServiceTest::PlaceDrawnStand)
		 * rather than plop the shipping Code C stand. Unset by default: every older test here is
		 * about the Code C stand, which placement gives a Code C outline.
		 */
		TOptional<EIcaoCode> StandLetter;

		/**
		 * Lay the service road NORTH-SOUTH, FarRoadClearance beyond the first stand's far edge,
		 * with the depot east of it - instead of the east-west road to the south. Set before
		 * Build.
		 *
		 * WHAT A FAR-SIDE STAND WANTS (spec 2026-09-26: service vehicles enter only by the far
		 * edge). The south road still joins a stand's entries - the nearest point on it lies just
		 * beyond the far edge's half-plane - but the lead-in then loops round behind the stand
		 * and meets the lane from the wrong side, so the vehicle arrives facing the way it should
		 * leave (measured 2026-09-26 on a Code A stand: a 180 degree cusp at the lane, which the
		 * utility tow's trailer folds on and the router refuses). Off by default: every older
		 * test here predates the far edge and is about something else.
		 */
		bool bFarEdgeRoad = false;

		/** How far beyond the far edge the bFarEdgeRoad road runs - StandFixture.h's FarRoadX figure. */
		static constexpr double FarRoadClearance = 420.0;

		/** Where the bFarEdgeRoad road runs, x. Set by Build. */
		double FarRoadX = 0.0;

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
	// set this, and an unset vehicle table means a truck dispatched with zero speed and
	// acceleration, not the one every other caller of DispatchAgent gets. THE SAME RESOLVE,
	// through the same loop, so the fixture's letters get the vehicles the game's do.
	Service->ResolveVehicles([](EIcaoCode Letter) { return UAirsideSettings::ResolveStandDesignVehicle(Letter); });

	FGuidelineNodeId TaxiSouth, TaxiNorth;
	LayLine(*Net, FVector2D(-10000.0, -10000.0), FVector2D(-10000.0, 10000.0),
		ETraversalClass::Aircraft, TaxiSouth, TaxiNorth);
	TaxiwayFarEnd = TaxiSouth;

	if (bWithRoad && !bFarEdgeRoad)
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
		URoadProfile* Strip = TestProfiles::Runway();
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
	if (StandLetter.IsSet())
	{
		Stand = FuelServiceTest::PlaceDrawnStand(*Net, *StandLetter, FVector2D(0.0, 0.0));
	}
	else
	{
		Stand = Net->PlaceEntity(StandDef, StandDef->Anchors, FVector2D(0.0, 0.0), 0.0,
			/*DesignWingspan=*/3600.0, StandDef->PoseRole, StandDef->Trucks);
	}

	if (bWithRoad && bFarEdgeRoad)
	{
		// THE STAND'S OWN OUTLINE says where its far edge is: it faces +X, so the far edge is
		// the outline's largest x, whatever the letter.
		double FarEdge = -TNumericLimits<double>::Max();
		for (const FVector2D& Corner : Net->GetEntity(Stand)->Outline)
		{
			FarEdge = FMath::Max(FarEdge, Corner.X);
		}
		FarRoadX = FarEdge + FarRoadClearance;
		FGuidelineNodeId RoadSouth, RoadNorth;
		LayLine(*Net, FVector2D(FarRoadX, -10000.0), FVector2D(FarRoadX, 10000.0),
			ETraversalClass::GroundVehicle, RoadSouth, RoadNorth);
	}

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
		else if (bFarEdgeRoad)
		{
			// EAST of the far-edge road, facing +X, so its pose ray leaves toward -X and meets it
			// the same 4000 uu away the south-road depot sits from its road - and well south of
			// the stand, so the two lead-ins never share a stretch.
			Depot = Net->PlaceEntity(DepotDef, DepotDef->Anchors,
				FVector2D(FarRoadX + 4000.0, -6000.0),
				0.0, 0.0, DepotDef->PoseRole, DepotDef->Trucks);
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
	// #312: was a hand-built FRouteQuery that skipped AvoidRunways.
	const FRoutePlan Plan = TestGraph::Probe(*Net, TaxiwayFarEnd, Pose, ETraversalClass::Aircraft);
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

	// NO ROAD WIDE ENOUGH (spec 2026-09-23 §6). Everything is joined and connected, but the
	// truck does not fit the road - so the reason must say THAT, not "no road from depot",
	// which would send the player looking for a gap in a road that is there. A 20 m body
	// fits no lane on the fixture's airport; the real bowser fits them all.
	{
		FFuelFixture Fixture;
		Fixture.Build(/*bWithRoad=*/true);
		// BOTH halves of the fixture stand's letter entry: a stand DRAWN for the 20 m body too, so
		// the VehicleTooLarge guard (Sent larger than Design) does not answer first - this case
		// is about the road.
		FLetterFuelVehicles& Vehicles = Fixture.Service->VehiclesFor(EIcaoCode::C);
		Vehicles.Design.BodyWidth = 2000.0;
		Vehicles.Sent.BodyWidth = 2000.0;
		if (!TestTrue(TEXT("an aircraft parks"), Fixture.ParkAircraft() != 0)) { return false; }
		Fixture.Advance(0.2);

		if (!TestEqual(TEXT("one demand"), Fixture.Service->GetDemands().Num(), 1)) { return false; }
		TestEqual(TEXT("because no road is wide enough for the truck"),
			static_cast<int32>(Fixture.Service->GetDemands()[0].Why),
			static_cast<int32>(EFuelRefusal::TooNarrow));
		TestEqual(TEXT("and the card says the road is too narrow, not missing"),
			Fixture.Service->DescribeAgent(Fixture.Service->GetDemands()[0].AircraftId),
			FString(TEXT("no road wide enough for the fuel truck")));
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
	FFuelPerDemandRefusalRevisionTest, "AirportOps.Ops.FuelPerDemandRefusalRevision",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFuelPerDemandRefusalRevisionTest::RunTest(const FString& Parameters)
{
	// THE RACE ISSUE #193 NAMES. UFuelService::LastRefusedRevision used to be ONE field for
	// every demand's own fact. Demand A (Needed, processed first because it was ADDED first)
	// refuses for the first time inside a Tick pass that ALSO revisits Demand B, already
	// Unserviceable from an OLDER revision - A's write of the CURRENT revision into the
	// shared field lands before B is checked, so B compares the current revision against a
	// value its own refusal never set and stays stuck, with the re-offer log line
	// ("the airport changed; aircraft %d asks again") never firing.
	//
	// BOTH DEMANDS ARE NoDepot, built directly with AddDemandForTest rather than through two
	// independently-failing stands: NoDepot is decided from Network.GetEntities() alone (see
	// ChooseDepot), so it is deterministic with no depot at all on the fixture's airport, and
	// the test can pin exactly what each demand's history was and in what array order Tick
	// will visit them - the one thing two real stands refusing on their own schedules cannot
	// promise tick by tick.
	FFuelFixture Fixture;
	Fixture.Build(/*bWithRoad=*/true, /*bWithDepot=*/false);

	// THE STARTING REVISION, READ RATHER THAN ASSUMED ZERO: Build already laid the taxiway's
	// own guideline, which bumps it past zero before this test touches anything.
	const uint32 OriginalRevision = Fixture.Net->GetGuidelineRevision();

	// A: still Needed, added FIRST so Tick's loop reaches it before B.
	Fixture.Service->AddDemandForTest(/*AircraftId=*/1, EFuelDemandState::Needed,
		EFuelRefusal::None, /*RefusedAtRevision=*/OriginalRevision);

	// B: already refused at the ORIGINAL revision, added SECOND.
	Fixture.Service->AddDemandForTest(/*AircraftId=*/2, EFuelDemandState::Unserviceable,
		EFuelRefusal::NoDepot, /*RefusedAtRevision=*/OriginalRevision);

	// The player edits the airport - an inert node, far from anything, so the ONLY thing it
	// changes is the revision both demands are judged against. Still no depot, so neither
	// demand is actually fixable; the test is about whether B is even ASKED again.
	Fixture.Net->AddGuidelineNode(FVector2D(90000.0, 90000.0));
	const uint32 NewRevision = Fixture.Net->GetGuidelineRevision();
	if (!TestEqual(TEXT("setup: the edit bumped the guideline revision by exactly one"),
		NewRevision, OriginalRevision + 1)) { return false; }

	// ONE TICK. A refuses for the first time, at the new revision, and writes that fact; B is
	// checked in the very same pass, straight after.
	Fixture.Service->Tick(*Fixture.Traffic, *Fixture.Net, *Fixture.Clock);

	auto FindDemand = [&Fixture](int32 AircraftId) -> const FFuelDemand*
	{
		for (const FFuelDemand& Demand : Fixture.Service->GetDemands())
		{
			if (Demand.AircraftId == AircraftId) { return &Demand; }
		}
		return nullptr;
	};

	const FFuelDemand* DemandA = FindDemand(1);
	const FFuelDemand* DemandB = FindDemand(2);
	if (!TestNotNull(TEXT("A's demand survives the tick"), DemandA)) { return false; }
	if (!TestNotNull(TEXT("B's demand survives the tick"), DemandB)) { return false; }

	TestEqual(TEXT("A refuses for the first time, at the new revision"),
		static_cast<int32>(DemandA->State), static_cast<int32>(EFuelDemandState::Unserviceable));

	// THE DEFECT, DIRECTLY: B's OWN refusal was at revision 0, and the revision has moved to
	// 1 - it must be re-offered on this same tick, not left reading a fact A's refusal just
	// overwrote a moment before.
	TestNotEqual(TEXT("B is re-offered too - its own stale refusal is not masked by A's"),
		static_cast<int32>(DemandB->State), static_cast<int32>(EFuelDemandState::Unserviceable));
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
	FFuelBusyWaitSkipsChooseDepotTest, "AirportOps.Ops.FuelBusyWaitSkipsChooseDepot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFuelBusyWaitSkipsChooseDepotTest::RunTest(const FString& Parameters)
{
	// ISSUE #190, MEASURED DIRECTLY. A Needed demand behind a saturated fleet used to run
	// ChooseDepot - a walk of every depot, a route search, and an IsServiceNodeConnected BFS
	// - every single tick for as long as the fleet stayed busy, exactly the queue this
	// module's own FFuelQueuesOnABusyDepotTest drains. Same fixture: one depot, one truck,
	// two stands.
	FFuelFixture Fixture;
	Fixture.bSecondStand = true;
	Fixture.Build(/*bWithRoad=*/true);

	const int32 First = Fixture.ParkAircraft();
	if (!TestTrue(TEXT("an aircraft parked at the first stand"), First != 0)) { return false; }

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

	// ONE TICK TO DISCOVER IT IS BUSY. This is the call ChooseDepot must still make - finding
	// out costs exactly one walk, the same as any other Needed demand's first look.
	Fixture.Advance(1.0 / 30.0);
	const FFuelDemand* Waiting = SecondDemand();
	if (!TestNotNull(TEXT("the second demand survives its first tick"), Waiting)) { return false; }
	if (!TestEqual(TEXT("setup: it is Needed and busy, not Unserviceable"),
		static_cast<int32>(Waiting->State), static_cast<int32>(EFuelDemandState::Needed)))
	{
		return false;
	}

	// FROM HERE, NOTHING ABOUT THE AIRPORT OR THE FLEET CHANGES for a couple of seconds - the
	// first truck is still out (its 40 s dwell alone dwarfs this window). Every one of the
	// next 60 ticks is exactly the case the ticket names: a saturated fleet, checked again
	// for no reason.
	Fixture.Service->ResetChooseDepotCallCountForTest();
	for (int32 Index = 0; Index < 60; ++Index)
	{
		Fixture.Advance(1.0 / 30.0);
	}

	TestEqual(TEXT("60 idle ticks against an unchanged fleet and graph call ChooseDepot zero times"),
		Fixture.Service->GetChooseDepotCallCountForTest(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFuelChooseDepotCachesRouteFindsTest, "AirportOps.Ops.FuelChooseDepotCachesRouteFinds",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFuelChooseDepotCachesRouteFindsTest::RunTest(const FString& Parameters)
{
	// #301: FFuelBusyWaitSkipsChooseDepotTest above already proves the busy-wait keeps
	// ChooseDepot from running AT ALL while nothing changes. This proves the OTHER half - when
	// ChooseDepot DOES run again (a truck freeing up bumps FleetRevision every idle tick until
	// THIS depot's own route is asked for again), it does not re-run RouteSearch::Find for a
	// (depot, stand, truck) it has already answered on the same graph. ChooseDepotForTest calls
	// the walk directly, so N repeats do not need N contrived FleetRevision bumps to get there -
	// measured against RouteSearch::SearchCallCountForTest, a fact about the engine actually
	// running, not a caller-owned counter a cache that is never consulted could still satisfy.
	FFuelFixture Fixture;
	Fixture.Build(/*bWithRoad=*/true);
	const FName AnchorId = Fixture.Net->FirstAnchorIdForRole(Fixture.Stand, EServiceRole::Fuel);
	const FResolvedAnchor* Resolved = AnchorId.IsNone() ? nullptr : Fixture.Net->FindResolvedAnchor(Fixture.Stand, AnchorId);
	if (!TestNotNull(TEXT("the stand's fuel anchor resolves"), Resolved)) { return false; }
	const FGuidelineNodeId Hydrant = Resolved->Node;
	if (!TestTrue(TEXT("the hydrant is joined"), Hydrant.IsSet())) { return false; }

	RouteSearch::ResetSearchCallCountForTest();
	const FLetterFuelVehicles& Vehicles = Fixture.Service->VehiclesFor(EIcaoCode::C);
	const auto First = Fixture.Service->ChooseDepotForTest(*Fixture.Net, Hydrant, Vehicles);
	if (!TestTrue(TEXT("the one depot is chosen"), First.Depot.IsSet())) { return false; }
	TestEqual(TEXT("the first ask runs one Find"), RouteSearch::SearchCallCountForTest(), 1);

	for (int32 Idle = 0; Idle < 5; ++Idle)
	{
		Fixture.Service->ChooseDepotForTest(*Fixture.Net, Hydrant, Vehicles);
	}
	TestEqual(TEXT("5 more asks over an unchanged graph answer from the cache - no new Find"),
		RouteSearch::SearchCallCountForTest(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFuelBusyWaitReoffersOnceTest, "AirportOps.Ops.FuelBusyWaitReoffersOnce",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFuelBusyWaitReoffersOnceTest::RunTest(const FString& Parameters)
{
	// THE OTHER HALF OF #190's FIX: skipping ChooseDepot while busy is only correct if a
	// truck actually freeing up still reaches the waiting demand - and reaches it exactly
	// once, the tick FleetRevision moves, not on a retry every idle tick from then on.
	FFuelFixture Fixture;
	Fixture.bSecondStand = true;
	Fixture.Build(/*bWithRoad=*/true);

	const int32 First = Fixture.ParkAircraft();
	if (!TestTrue(TEXT("an aircraft parked at the first stand"), First != 0)) { return false; }

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

	// ESTABLISH BUSY, then start counting from a clean slate.
	Fixture.Advance(1.0 / 30.0);
	if (!TestNotNull(TEXT("setup: the second demand is busy"), SecondDemand())) { return false; }
	Fixture.Service->ResetChooseDepotCallCountForTest();

	// SAME BOUND AS FFuelQueuesOnABusyDepot: the first truck's whole round trip - drive out,
	// the 40 s dwell, drive home.
	const bool bServed = Fixture.AdvanceUntil([&SecondDemand]
	{
		const FFuelDemand* Demand = SecondDemand();
		return Demand != nullptr && Demand->TruckId != 0;
	}, 450.0);
	if (!TestTrue(TEXT("the truck coming home re-offers the waiting demand"), bServed)) { return false; }

	// THE DEFECT THIS WOULD CATCH: reverting the skip in Tick's Needed case turns this back
	// into "every idle tick along the way", which over a 450 s bound is thousands of calls,
	// not one.
	TestEqual(TEXT("ChooseDepot ran exactly once to do it - the free-up is caught the tick it "
		"happens, not retried on every idle tick beforehand"),
		Fixture.Service->GetChooseDepotCallCountForTest(), 1);
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

#if WITH_DEV_AUTOMATION_TESTS

// Review of 2026-09-24 item 3: a truck whose way OUT fitted can meet a corner on the way HOME
// that does not - the near-side turn is the tighter one. Retiring it at the stand costs the
// airport a truck per job; it drives home ungated instead, and the log says why.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFuelTruckGetsHomeWhenTooNarrowTest, "AirportOps.Ops.FuelTruckGetsHomeWhenTooNarrow",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFuelTruckGetsHomeWhenTooNarrowTest::RunTest(const FString& Parameters)
{
	FFuelFixture Fixture;
	Fixture.Build(/*bWithRoad=*/true);
	const int32 Aircraft = Fixture.ParkAircraft();
	if (!TestTrue(TEXT("an aircraft parked"), Aircraft != 0)) { return false; }
	Fixture.Advance(0.2);
	const int32 TruckId = Fixture.Service->GetDemands()[0].TruckId;
	if (!TestTrue(TEXT("a truck went out for it"), TruckId != 0)) { return false; }

	// Out it went; now nothing on the airport fits it on the way back. THE AGENT'S OWN VEHICLE,
	// since 2026-09-26: the road home is searched for the vehicle that is actually out, not a
	// service-wide field, so that is the one widened.
	const FRoadAgent* Out = Fixture.Traffic->FindAgent(TruckId);
	if (!TestTrue(TEXT("the truck is a vehicle"), Out != nullptr && Out->AsVehicle() != nullptr)) { return false; }
	FVehicle Wide = *Out->AsVehicle();
	Wide.BodyWidth = 2000.0;
	TestTrue(TEXT("its vehicle is widened"), FGroundTrafficTestAccess(*Fixture.Traffic).SetVehicle(TruckId, Wide));

	// THE WIDENING HAS TO REACH THE ROAD HOME, or the two assertions below pass on a truck that
	// simply fitted: SendTruckHome's own "does not fit ... driving it anyway" line, exactly once.
	AddExpectedMessagePlain(TEXT("does not fit the road home"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);
	Fixture.Traffic->RetireAgent(Aircraft);
	Fixture.Advance(0.2);

	TestNotNull(TEXT("the truck is not retired at the stand"), Fixture.Traffic->FindAgent(TruckId));
	TestEqual(TEXT("it drives home anyway"), Fixture.Service->TrucksGoingHomeForTest(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFuelTowNeverDrivenHomeIntoAFoldTest, "AirportOps.Ops.FuelTowNeverDrivenHomeIntoAFold",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFuelTowNeverDrivenHomeIntoAFoldTest::RunTest(const FString& Parameters)
{
	// THE UNGATED FALLBACK IS FOR A SCUFFED KERB, NOT A JACK-KNIFE (review of 9441ccf1): a truck
	// that does not fit the road home drives it anyway (FuelTruckGetsHomeWhenTooNarrow), UNLESS it
	// tows something that route folds - UFuelService::MayDriveUngated, the rule SendTruckHome asks.
	// On a hand-drawn road, because the fuel fixture's roads turn left and right and fold nothing:
	// three same-hand quarters (the turn of a dead-end balloon) fold the rig; two - a U - do not.
	const FVehicle Rig = UAirsideSettings::ResolveRigVehicle();
	const FVehicle Bowser = UAirsideSettings::ResolveDefaultVehicle();
	for (const int32 Quarters : { 3, 2 })
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		auto Node = [Net](const FVector2D& At) { return Net->AddGuidelineNode(At, /*bDerived=*/false); };
		auto Join = [Net](FGuidelineNodeId A, FGuidelineNodeId B, const FVector2D& Control)
		{
			FGuidelineEdge Edge;
			Edge.A = A;
			Edge.B = B;
			Edge.Control = Control;
			Edge.AllowedTraffic = FTrafficMask::All();
			Edge.Direction = EGuidelineDir::AToB;
			Edge.bDerived = false;
			Net->AddGuidelineEdge(MoveTemp(Edge));
		};
		const double Leg = 830.0;
		const FGuidelineNodeId Start = Node(FVector2D(-4000.0, 0.0));
		FGuidelineNodeId From = Node(FVector2D::ZeroVector);
		Join(Start, From, FVector2D(-2000.0, 0.0));
		FVector2D At = FVector2D::ZeroVector;
		FVector2D Heading(1.0, 0.0);
		for (int32 Quarter = 0; Quarter < Quarters; ++Quarter)
		{
			const FVector2D Side(-Heading.Y, Heading.X);
			const FVector2D Control = At + Heading * Leg;
			At = Control + Side * Leg;
			Heading = Side;
			const FGuidelineNodeId To = Node(At);
			Join(From, To, Control);
			From = To;
		}
		const FGuidelineNodeId Goal = Node(At + Heading * 4000.0);
		Join(From, Goal, At + Heading * 2000.0);
		const FRoutePlan Plan = RouteSearch::Find(*Net,
			FRouteQuery::For(ERouteErrand::GraphProbe, Start, Goal, 0.0, ETraversalClass::GroundVehicle));
		if (!TestTrue(TEXT("an ungated plan"), Plan.IsValid())) { continue; }

		FString Why;
		const bool bMay = UFuelService::MayDriveUngated(Plan, Rig, *Net, &Why);
		AddInfo(FString::Printf(TEXT("%d quarters: rig %s %s"), Quarters, bMay ? TEXT("may drive") : TEXT("may not:"), *Why));
		if (Quarters == 3)
		{
			TestFalse(TEXT("a rig is never sent home ungated on a road that folds its trailer"), bMay);
			TestTrue(TEXT("and the reason is the fold"), Why.StartsWith(TEXT("trailer folds at guideline node")));
		}
		else
		{
			TestTrue(TEXT("a road its trailer holds is driven ungated, as for any truck"), bMay);
		}
		TestTrue(TEXT("a rigid truck is always driven home ungated - nothing to fold"), UFuelService::MayDriveUngated(Plan, Bowser, *Net));
	}
	return true;
}

#endif

#if WITH_DEV_AUTOMATION_TESTS

// FAR-SIDE ENTRY TASK 7 (spec 2026-09-26 section 2): each stand is served by its OWN letter's
// design vehicle - the utility tow on A and B, the fuel truck from C - where one TruckVehicle
// used to go everywhere.

namespace FuelServiceTest
{
	/** The demand for Aircraft, or null. */
	const FFuelDemand* DemandFor(const UFuelService& Service, int32 Aircraft)
	{
		for (const FFuelDemand& Each : Service.GetDemands())
		{
			if (Each.AircraftId == Aircraft) { return &Each; }
		}
		return nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFuelTowServesCodeATest, "AirportOps.Fuel.TowServesCodeA",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFuelTowServesCodeATest::RunTest(const FString& Parameters)
{
	// END TO END ON A DRAWN CODE A STAND: the tow is sent, it is the WHOLE chain (cab plus
	// trailer, laid by StartDrive), and the aircraft leaves fuelled - no "UNFUELLED" line, and
	// no AddExpectedError, because nothing on the way is allowed to go wrong.
	//
	// NOT ASSERTED: the tow getting HOME. Measured 2026-09-26 on this fixture, its reverse off
	// the service point is refused ("turntable bent 13.4 deg (the lock engages within 3.0)") and
	// it stops there - a fact about the A template's approach leg and FTowReverseRun, not about
	// dispatch, and reported rather than pinned here (far-side-entry task 7 report).
	FFuelFixture Fixture;
	Fixture.StandLetter = EIcaoCode::A;
	Fixture.bFarEdgeRoad = true;
	Fixture.TurnaroundSeconds = 1800.0;
	Fixture.bWithRunway = true;
	Fixture.Build(/*bWithRoad=*/true);

	const FEntityInstance* Stand = Fixture.Net->GetEntity(Fixture.Stand);
	if (!TestNotNull(TEXT("the A stand is placed"), Stand)) { return false; }
	TestEqual(TEXT("and the service reads it as Code A"),
		static_cast<int32>(UFuelService::LetterOfStand(*Stand)), static_cast<int32>(EIcaoCode::A));

	const FVehicle Tow = UAirsideSettings::ResolveUtilityTowVehicle();
	if (!TestTrue(TEXT("the utility tow tows something - else this test measures a rigid truck"), Tow.HasTrailer()))
	{
		return false;
	}

	FLogLineSpy Spy(FName(TEXT("LogAirportOps")));
	GLog->AddOutputDevice(&Spy);

	const int32 Aircraft = Fixture.ParkAircraft();
	int32 TruckId = 0;
	FName SentType;
	bool bWholeChain = false;
	const bool bDeparted = Aircraft != 0 && Fixture.AdvanceUntil([&]
	{
		if (const FFuelDemand* Demand = FuelServiceTest::DemandFor(*Fixture.Service, Aircraft))
		{
			TruckId = Demand->TruckId != 0 ? Demand->TruckId : TruckId;
		}
		if (const FRoadAgent* Truck = TruckId != 0 ? Fixture.Traffic->FindAgent(TruckId) : nullptr)
		{
			SentType = Truck->TypeCode();
			const FVehicle* Vehicle = Truck->AsVehicle();
			bWholeChain |= Vehicle != nullptr && Vehicle->HasTrailer()
				&& Truck->TowAxles.Num() == Vehicle->Tow.Num();
		}
		const FRoadAgent* Plane = Fixture.Traffic->FindAgent(Aircraft);
		return Plane == nullptr || Plane->Phase != EAgentPhase::Parked;
	}, 900.0);

	GLog->RemoveOutputDevice(&Spy);

	if (!TestTrue(TEXT("an aircraft parked at the A stand"), Aircraft != 0)) { return false; }
	TestTrue(TEXT("a vehicle was dispatched"), TruckId != 0);
	TestEqual(TEXT("and it is the utility tow, A's design vehicle"), SentType, Tow.TypeCode);
	TestTrue(TEXT("dispatched as the whole chain - one laid axle per tow link"), bWholeChain);
	TestTrue(TEXT("and departed"), bDeparted);

	// FROM THE LOG, not the demand's state: the Done state and the departure that drops the
	// demand can fall in one Tick, so a per-step look at the state can miss Done entirely.
	bool bDepartLine = false;
	bool bFuelledLine = false;
	for (const FString& Line : Spy.CapturedLines)
	{
		bFuelledLine |= Line.Contains(FString::Printf(TEXT("aircraft %d fuelled at stand"), Aircraft));
		bDepartLine |= Line.Contains(TEXT("departs stand")) && Line.Contains(TEXT("after its turnaround"));
		TestFalse(*FString::Printf(TEXT("never UNFUELLED: %s"), *Line), Line.Contains(TEXT("UNFUELLED")));
	}
	TestTrue(TEXT("the aircraft was fuelled"), bFuelledLine);
	TestTrue(TEXT("the departure was logged as a normal one"), bDepartLine);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFuelTruckServesCodeCTest, "AirportOps.Fuel.TruckServesCodeC",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFuelTruckServesCodeCTest::RunTest(const FString& Parameters)
{
	FFuelFixture Fixture;
	Fixture.StandLetter = EIcaoCode::C;
	Fixture.bFarEdgeRoad = true;
	Fixture.Build(/*bWithRoad=*/true);
	const int32 Aircraft = Fixture.ParkAircraft();
	if (!TestTrue(TEXT("an aircraft parked at the C stand"), Aircraft != 0)) { return false; }
	Fixture.Advance(0.2);

	const FFuelDemand* Demand = FuelServiceTest::DemandFor(*Fixture.Service, Aircraft);
	const FRoadAgent* Truck = Demand != nullptr ? Fixture.Traffic->FindAgent(Demand->TruckId) : nullptr;
	if (!TestNotNull(TEXT("a vehicle is out for it"), Truck)) { return false; }

	const FVehicle Design = UAirsideSettings::ResolveStandDesignVehicle(EIcaoCode::C);
	TestEqual(TEXT("it is the fuel truck, C's design vehicle"), Truck->TypeCode(), Design.TypeCode);
	TestNotEqual(TEXT("and not the tow"), Truck->TypeCode(), UAirsideSettings::ResolveUtilityTowVehicle().TypeCode);

	// THE ROUTE CACHE KEYS ON THE VEHICLE'S FIGURES (RoutePlanCache::VehicleIdentity), so a tow
	// and a truck asked from the same depot to the same hydrant must never share an entry -
	// else the first one's plan would be driven by the other.
	TestNotEqual(TEXT("the tow and the truck never share a route cache entry"),
		RoutePlanCache::VehicleIdentity(UAirsideSettings::ResolveUtilityTowVehicle()),
		RoutePlanCache::VehicleIdentity(Design));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFuelVehicleTooLargeRefusedTest, "AirportOps.Fuel.VehicleTooLargeRefused",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFuelVehicleTooLargeRefusedTest::RunTest(const FString& Parameters)
{
	// A DEPOT THAT WOULD SEND THE TRUCK TO AN A STAND. Unreachable in the game today - fleets
	// are untyped, so Sent is always Design - and staged here by writing the table, which is
	// exactly where a typed fleet will one day put a different vehicle.
	FFuelFixture Fixture;
	Fixture.StandLetter = EIcaoCode::A;
	Fixture.bFarEdgeRoad = true;
	Fixture.Build(/*bWithRoad=*/true);
	Fixture.Service->VehiclesFor(EIcaoCode::A).Sent = UAirsideSettings::ResolveStandDesignVehicle(EIcaoCode::C);

	const int32 Aircraft = Fixture.ParkAircraft();
	if (!TestTrue(TEXT("an aircraft parked at the A stand"), Aircraft != 0)) { return false; }
	Fixture.Advance(0.2);

	const FFuelDemand* Demand = FuelServiceTest::DemandFor(*Fixture.Service, Aircraft);
	if (!TestNotNull(TEXT("it demanded fuel"), Demand)) { return false; }
	TestEqual(TEXT("unserviceable"),
		static_cast<int32>(Demand->State), static_cast<int32>(EFuelDemandState::Unserviceable));
	TestEqual(TEXT("because the vehicle is larger than the stand was drawn for"),
		static_cast<int32>(Demand->Why), static_cast<int32>(EFuelRefusal::VehicleTooLarge));
	TestEqual(TEXT("and the card names the vehicle, not the road"),
		Fixture.Service->DescribeAgent(Aircraft),
		FString(TEXT("the depot's vehicle is too large for this stand")));
	TestEqual(TEXT("no truck went out"), Demand->TruckId, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFuelRuntimeResolvesPerStandTest, "AirportOps.Fuel.RuntimeResolvesPerStand",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFuelRuntimeResolvesPerStandTest::RunTest(const FString& Parameters)
{
	// THE COMPOSITION SEAM: UOpsRuntime::Attach fills the table, the real tick loop dispatches.
	// An A stand and a C stand side by side, one depot each, one aircraft each - two DIFFERENT
	// vehicle types must come out. Fails if Attach resolves one fixed vehicle for every letter,
	// or if dispatch reads one entry whatever the stand.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("the actor"), Actor)) { return false; }
	Actor->PlaceNode(FVector2D(0.0, 40000.0));
	if (!TestNotNull(TEXT("a network"), Actor->Network.Get())) { return false; }
	URoadNetwork& Net = *Actor->Network;

	// THE WORLD-FREE FIXTURE'S bFarEdgeRoad GEOMETRY, for two stands: the taxiway to the west,
	// both stands facing +X off it (A at the origin, C 9000 north, clear of A's 5000 width), and
	// one north-south road beyond BOTH far edges - C's is the deeper - with a depot east of it
	// per stand, south of both, so neither aircraft has to wait for the other's truck.
	FGuidelineNodeId TaxiSouth, TaxiNorth, RoadSouth, RoadNorth;
	LayLine(Net, FVector2D(-10000.0, -10000.0), FVector2D(-10000.0, 20000.0),
		ETraversalClass::Aircraft, TaxiSouth, TaxiNorth);

	const FEntityInstanceId StandA = FuelServiceTest::PlaceDrawnStand(Net, EIcaoCode::A, FVector2D(0.0, 0.0));
	const FEntityInstanceId StandC = FuelServiceTest::PlaceDrawnStand(Net, EIcaoCode::C, FVector2D(0.0, 9000.0));
	double FarEdge = -TNumericLimits<double>::Max();
	for (const FEntityInstanceId Stand : { StandA, StandC })
	{
		for (const FVector2D& Corner : Net.GetEntity(Stand)->Outline)
		{
			FarEdge = FMath::Max(FarEdge, Corner.X);
		}
	}
	const double RoadX = FarEdge + FFuelFixture::FarRoadClearance;
	LayLine(Net, FVector2D(RoadX, -10000.0), FVector2D(RoadX, 20000.0),
		ETraversalClass::GroundVehicle, RoadSouth, RoadNorth);

	UEntityDefinition* DepotDef = UEntityDefinition::MakeFuelDepotTransient();
	for (const double Y : { -6000.0, -3000.0 })
	{
		Net.PlaceEntity(DepotDef, DepotDef->Anchors, FVector2D(RoadX + 4000.0, Y),
			0.0, 0.0, DepotDef->PoseRole, DepotDef->Trucks);
	}
	FAnchorLink::Build(Net, UAirsideSettings::ResolveLargestServiceVehicle());

	UOpsRuntime* Runtime = NewObject<UOpsRuntime>();
	if (!TestNotNull(TEXT("the runtime owns a fuel service"), Runtime->GetFuelService())) { return false; }
	Runtime->Attach(Actor);

	const FName Tow = UAirsideSettings::ResolveStandDesignVehicle(EIcaoCode::A).TypeCode;
	const FName Truck = UAirsideSettings::ResolveStandDesignVehicle(EIcaoCode::C).TypeCode;
	if (!TestNotEqual(TEXT("A and C have different design vehicles, or this proves nothing"), Tow, Truck))
	{
		return false;
	}

	constexpr float Step = 1.0f / 30.0f;
	TSet<FName> Sent;
	auto TickAndWatch = [&]()
	{
		Actor->Tick(Step);
		Runtime->Tick(Step);
		for (const FRoadAgent& Agent : Actor->GetTraffic()->GetModel()->GetAgents())
		{
			if (Agent.Class == ETraversalClass::GroundVehicle)
			{
				Sent.Add(Agent.TypeCode());
			}
		}
	};

	// ONE AIRCRAFT AT A TIME onto the shared taxiway, so the second never meets the first on
	// the lead-in: the second is dispatched once the first has parked and asked for fuel.
	for (const FEntityInstanceId Stand : { StandA, StandC })
	{
		const int32 DemandsBefore = Runtime->GetFuelService()->GetDemands().Num();
		const FRoutePlan Plan = TestGraph::Probe(Net, TaxiSouth, Net.GetEntity(Stand)->PoseNode, ETraversalClass::Aircraft);
		if (!TestTrue(TEXT("the aircraft routes to its stand"), Plan.IsValid())) { return false; }
		if (!TestTrue(TEXT("and dispatches"), Actor->DispatchAgent(Plan, UAirsideSettings::ResolveDefaultAirframe())))
		{
			return false;
		}
		for (int32 Tick = 0; Tick < 3600 && Runtime->GetFuelService()->GetDemands().Num() == DemandsBefore; ++Tick)
		{
			TickAndWatch();
		}
	}
	for (int32 Tick = 0; Tick < 600 && Sent.Num() < 2; ++Tick)
	{
		TickAndWatch();
	}

	TestTrue(TEXT("the A stand was sent the tow"), Sent.Contains(Tow));
	TestTrue(TEXT("the C stand was sent the truck"), Sent.Contains(Truck));
	TestEqual(TEXT("the runtime's table answers A's stand with the tow"),
		Runtime->GetFuelService()->VehicleFor(*Net.GetEntity(StandA)).TypeCode, Tow);
	TestEqual(TEXT("and C's with the truck"),
		Runtime->GetFuelService()->VehicleFor(*Net.GetEntity(StandC)).TypeCode, Truck);
	return true;
}

#endif
