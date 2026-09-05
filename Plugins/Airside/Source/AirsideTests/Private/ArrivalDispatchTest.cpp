#include "CoreMinimal.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Entities/AircraftType.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/LandingRun.h"
#include "Model/RoadAgent.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FArrivalDispatchTest,
	"Airside.Present.ArrivalDispatch",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FArrivalDispatchTest::RunTest(const FString& Parameters)
{
	// THE TEST THAT SHOULD HAVE EXISTED BEFORE THE KEY WAS BOUND.
	//
	// FLandingRun passed in isolation and RunwayExitNodes passed in isolation, and pressing 7
	// did nothing, every time, for two sittings. Nothing exercised the thing that actually
	// runs when the key goes down - which is the COMPOSITION of those two with the route
	// search and the stand list, and which refused for a reason neither component could see.
	//
	// So this builds the airport the user actually drew - a runway, a taxiway joining it, and
	// stands on the taxiway - and asserts that ordering an arrival on it produces an aircraft.
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("a world to spawn into"), World))
	{
		return false;
	}
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);

	ON_SCOPE_EXIT
	{
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
	};

	ARoadNetworkActor* Actor = World->SpawnActor<ARoadNetworkActor>();
	if (!TestNotNull(TEXT("actor spawned"), Actor))
	{
		return false;
	}

	FGroundPerformance Ground = UAircraftType::PiperMeridianGround();

	// DISTINCTIVE, not authored: 1000 is the Piper's own Taxi.SpeedCap AND what a
	// default-constructed FGroundPerformance carries, so leaving the figure alone could not
	// tell "the follower now has the airframe's ground performance" apart from "the follower
	// never got it and is still running on the struct default" - which is the defect issue
	// #27 describes. 1234 belongs to neither, so only the real handover proves it.
	Ground.Taxi.SpeedCap = 1234.0;

	const FClimbPerformance Climb = UAircraftType::PiperMeridianClimb();
	const FApproachPerformance Approach = UAircraftType::PiperMeridianApproach();

	// THE RUNWAY IS SIZED FROM THE AIRCRAFT, not chosen. A strip shorter than the landing
	// distance is correctly refused, so a fixture that picked a length out of the air would
	// be testing the refusal or the acceptance depending on numbers nobody was watching.
	const double Needed =
		FLandingRun::RequiredLandingDistance(Ground, Climb, Approach) * FLandingRun::LandingMargin;
	const double RunwayLength = Needed * 1.5;

	Actor->PlaceNode(FVector2D(-100000.0, -100000.0));
	if (!TestNotNull(TEXT("the actor has a network"), Actor->Network.Get()))
	{
		return false;
	}
	URoadNetwork& Net = *Actor->Network;

	URoadProfile* Runway = URoadProfile::MakeTransient(4500.0, 1500.0, 450.0);
	Runway->bContinuousThroughJunctions = true;
	URoadProfile* Taxiway = URoadProfile::MakeTransient(2300.0, 1500.0, 230.0);

	// The runway, SPLIT at the exit - which is how a T-junction reaches the graph, and what
	// puts a guideline node on the runway centreline for the exit search to find.
	const FVector2D ThresholdAt(0.0, 0.0);
	const FVector2D ExitAt(RunwayLength * 0.8, 0.0);
	const FVector2D FarEnd(RunwayLength, 0.0);

	const FRoadNodeId Threshold = Net.AddNode(ThresholdAt);
	const FRoadNodeId Exit = Net.AddNode(ExitAt);
	const FRoadNodeId Far = Net.AddNode(FarEnd);
	Net.AddStraightSegment(Threshold, Exit, Runway);
	Net.AddStraightSegment(Exit, Far, Runway);

	// The taxiway off it, running south.
	const FRoadNodeId TaxiEnd = Net.AddNode(ExitAt + FVector2D(0.0, -20000.0));
	Net.AddStraightSegment(Exit, TaxiEnd, Taxiway);

	// THE STAND SITS BESIDE THE TAXIWAY AND FACES IT. Not on it: FAnchorLink casts the
	// lead-in FROM the stand along heading + 180 and links to the guideline it strikes, so a
	// stand on top of the taxiway has no ray to cast and a stand facing away casts into
	// nothing. Heading is RADIANS, which is the convention PlaceEntity takes throughout.
	//
	// Facing east (0), so the ray runs west and meets the taxiway 9000 uu away - the same
	// arrangement Airside.Build.LeadInSweep uses.
	const FVector2D StandAt = ExitAt + FVector2D(9000.0, -10000.0);

	// Derives the guideline graph and the anchor links, which is what the route search and
	// the exit search both read. Neither exists until something rebuilds.
	Actor->RebuildMesh();

	if (Actor->ResolveStandDefinitionForTest() == nullptr)
	{
		// Placing a stand needs an authored definition, and the content set is not loaded in
		// a bare automation run. Reported rather than skipped silently: a test that quietly
		// asserts nothing is worse than one that says why.
		AddInfo(TEXT("No stand definition available in this run - arrival dispatch not exercised."));
		return true;
	}

	const int32 Stand = Actor->PlaceStand(StandAt, 0.0);
	if (!TestTrue(TEXT("a stand is placed at the end of the taxiway"), Stand != INDEX_NONE))
	{
		return false;
	}
	Actor->RebuildMesh();

	// 1. THE EXIT IS FOUND. Measured before the dispatch, so a failure below can be told
	//    apart from a failure here - which is exactly what the first refusal message could
	//    not do, and why "pressing 7 does nothing" took two sittings to diagnose.
	{
		const TArray<FGuidelineNodeId> OnStrip =
			Net.RunwayExitNodes(ThresholdAt, FVector2D(1.0, 0.0), RunwayLength, 2250.0, 0.0);
		TestTrue(FString::Printf(
			TEXT("the taxiway junction puts guideline node(s) on the runway (%d found)"),
			OnStrip.Num()),
			OnStrip.Num() > 0);

		const TArray<FGuidelineNodeId> Usable =
			Net.RunwayExitNodes(ThresholdAt, FVector2D(1.0, 0.0), RunwayLength, 2250.0, Needed);
		TestTrue(FString::Printf(
			TEXT("and at least one of them is far enough down to be usable (%d of %d, past %.0f uu)"),
			Usable.Num(), OnStrip.Num(), Needed),
			Usable.Num() > 0);
	}

	// 2. THE MEASUREMENT. ORDERING AN ARRIVAL PRODUCES AN AIRCRAFT.
	//
	//    This is the assertion the feature was shipped without. Everything else about the
	//    landing can be perfect and this can still be false, which is the state it shipped in.
	//
	//    DispatchArrival now takes one FAirframe rather than four structs - see issue #29 -
	//    but this test still exercises it through the actor, because it is what needs the
	//    world: the plan itself is asserted world-free in Airside.Model.ArrivalPlanner.
	FAirframe Airframe;
	Airframe.Ground = Ground;
	Airframe.Climb = Climb;
	Airframe.Approach = Approach;

	const int32 Before = Actor->AgentCountForTest();
	const bool bDispatched = Actor->DispatchArrival(ThresholdAt, Airframe);

	TestTrue(TEXT("an arrival is accepted on a runway that has an exit to a stand"), bDispatched);
	TestEqual(TEXT("and an aircraft exists as a result"),
		Actor->AgentCountForTest(), Before + 1);

	// 3. THE FOLLOWER TAXIS ON THE AIRFRAME'S GROUND PERFORMANCE, NOT THE STRUCT DEFAULT -
	//    issue #27, and issue #28's own reason for existing: with FAirframe as ONE struct
	//    handed to FRoadAgent::StartArrival and read again from it at the VACATED handover
	//    (Airframe.Ground, never Follower.Ground), the two literally cannot disagree any
	//    more. That property is asserted world-free, on the STRUCT, in
	//    Airside.Model.RoadAgent - what THIS test can add on top is that the handover
	//    actually happens when Tick is what drives it, through the same Tick -> Traffic->
	//    Advance path PlayerTick uses every frame, not a direct call to FRoadAgent::Advance.
	//
	//    RUN TO COMPLETION, bounded rather than open-ended: an infinite loop over a defect
	//    that never resolves would hang the whole test run instead of failing one test.
	//    6000 * 0.1s = 600 simulated seconds, comfortably past the landing roll plus the
	//    taxi from the runway exit to the stand at even a cautious ground speed.
	{
		int32 Ticks = 0;
		while (Actor->LastAgentPhaseForTest() != EAgentPhase::Parked && Ticks < 6000)
		{
			Actor->Tick(0.1f);
			++Ticks;
		}

		TestEqual(FString::Printf(TEXT("the arrival parks within %d ticks (phase %d)"),
			Ticks, static_cast<int32>(Actor->LastAgentPhaseForTest())),
			Actor->LastAgentPhaseForTest(), EAgentPhase::Parked);
		TestEqual(TEXT("and it is still the only agent - parking does not spawn or drop one"),
			Actor->AgentCountForTest(), Before + 1);

		// THE 1234.0 FIXTURE. Read through the follower Tick actually drove, not the
		// Airframe the test itself constructed - this is what proves the handover in
		// FRoadAgent::Advance (Airframe.Ground copied into Follower.Ground at the VACATED
		// moment) reached the struct the taxi is actually driven by.
		TestEqual(TEXT("the parked follower taxied on the airframe's own SpeedCap, not the ")
			TEXT("FGroundPerformance struct default"),
			Actor->LastAgentTaxiSpeedCapForTest(), 1234.0);
	}

	// 4. A RUNWAY TOO SHORT TO STOP ON IS STILL REFUSED, and refused without spawning - an
	//    arrival that cannot be completed must leave nothing frozen on final.
	{
		ARoadNetworkActor* Small = World->SpawnActor<ARoadNetworkActor>();
		if (TestNotNull(TEXT("a second actor"), Small))
		{
			Small->PlaceNode(FVector2D(-100000.0, -100000.0));
			URoadNetwork& Tiny = *Small->Network;

			URoadProfile* Strip = URoadProfile::MakeTransient(4500.0, 1500.0, 450.0);
			Strip->bContinuousThroughJunctions = true;

			const FRoadNodeId A = Tiny.AddNode(FVector2D(0.0, 0.0));
			const FRoadNodeId B = Tiny.AddNode(FVector2D(Needed * 0.4, 0.0));
			Tiny.AddStraightSegment(A, B, Strip);
			Small->RebuildMesh();

			TestFalse(TEXT("a runway shorter than the landing distance is refused"),
				Small->DispatchArrival(FVector2D::ZeroVector, Airframe));
			TestEqual(TEXT("and nothing is left in the world"), Small->AgentCountForTest(), 0);
		}
	}

	// 5. A DEPARTURE REACHES GONE, AND THE AGENT COUNT RETURNS TO 0 - covering the other half
	//    of Tick -> Traffic->Advance that the arrival above cannot reach on its own: it only
	//    parks (nothing arms a departure for it), so this dispatches a SEPARATE agent with a
	//    plain DispatchAgent call on a two-point plan that ends on a runway, which is what
	//    arms one (see UAirsideTraffic::DispatchAgent's own "DOES THIS ROUTE END ON A RUNWAY"
	//    comment). Ticking it to Gone exercises the SetMotion call every surviving frame takes
	//    and the Gone-frame View->Destroy() + RemoveAt neither this test nor any other reaches.
	//
	//    ONE UNDIVIDED SEGMENT, deliberately, rather than a taxiway joining a runway at a
	//    junction: RoadGuidelineBuilder gives every segment END its own guideline node - even
	//    at a plain dead end, a node can sit well off the road node it was derived from once a
	//    fillet radius is in play - so hunting for "the taxiway's node" by position is not
	//    reliable. A single segment has exactly one derived guideline edge, and it is the
	//    plan in its entirety: no junction, no ambiguity about which end is which.
	{
		ARoadNetworkActor* Departing = World->SpawnActor<ARoadNetworkActor>();
		if (TestNotNull(TEXT("a third actor"), Departing))
		{
			Departing->PlaceNode(FVector2D(-100000.0, -100000.0));
			URoadNetwork& Net2 = *Departing->Network;

			URoadProfile* Runway2 = URoadProfile::MakeTransient(4500.0, 1500.0, 450.0);
			Runway2->bContinuousThroughJunctions = true;

			const FVector2D NearAt2(0.0, 0.0);
			const FVector2D FarAt2(RunwayLength * 2.0, 0.0);
			const FRoadNodeId NearNode2 = Net2.AddNode(NearAt2);
			const FRoadNodeId FarNode2 = Net2.AddNode(FarAt2);
			Net2.AddStraightSegment(NearNode2, FarNode2, Runway2);
			Departing->RebuildMesh();

			const TArray<FGuidelineEdge>& Edges2 = Net2.GetGuidelineEdges();
			if (TestEqual(TEXT("the lone runway segment derives exactly one guideline edge"),
				Edges2.Num(), 1))
			{
				// Wingspan 0: unconstrained, per FRouteQuery::Wingspan's own comment - what
				// this plan needs to prove is that it ends on the runway, not that it fits
				// under some edge's MaxWingspan.
				const FRoutePlan Plan = Departing->FindRoute(
					Edges2[0].A, Edges2[0].B, ETraversalClass::Aircraft, 0.0);
				if (TestTrue(TEXT("the runway's own centreline resolves to a route"),
					Plan.IsValid()))
				{
					TestEqual(TEXT("a route along one undivided segment is a two-point plan"),
						Plan.Polyline.Num(), 2);

					const int32 BeforeDeparture = Departing->AgentCountForTest();
					TestTrue(TEXT("a plain DispatchAgent onto a runway is accepted"),
						Departing->DispatchAgent(Plan, Airframe));
					TestEqual(TEXT("and an aircraft exists as a result"),
						Departing->AgentCountForTest(), BeforeDeparture + 1);

					int32 DepartTicks = 0;
					while (Departing->AgentCountForTest() > 0 && DepartTicks < 6000)
					{
						Departing->Tick(0.1f);
						++DepartTicks;
					}

					TestEqual(FString::Printf(
						TEXT("the departure clears and the agent is dropped within %d ticks"),
						DepartTicks),
						Departing->AgentCountForTest(), 0);
				}
			}
		}
	}

	return true;
}

#endif
