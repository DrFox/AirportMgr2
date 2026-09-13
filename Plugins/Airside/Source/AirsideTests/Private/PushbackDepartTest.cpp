#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/DeparturePlanner.h"
#include "Model/GroundTraffic.h"
#include "Model/PushbackRun.h"
#include "Model/RoadAgent.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteSearch.h"
#include "Model/TrafficOccupancy.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// Prefixed against the UNITY build - these test files share one translation unit.

	/**
	 * A runway with a taxiway running south off it, and one more node beyond.
	 *
	 * A REAL RUNWAY IS NOT OPTIONAL: DeparturePlanner::PlanAny refuses "not on a runway"
	 * before any of the pushback logic is reached, so a graph of guidelines alone tests
	 * nothing at all. This is DepartAgentTest's own fixture with node C added past A, which
	 * is what lets ONE graph park an aeroplane facing either way:
	 *
	 *   B (0, 0)        on the runway - where a departure is heading
	 *   A (0, -20000)   where the aeroplane parks
	 *   C (0, -40000)   further out, so it can arrive at A from the SOUTH instead
	 *
	 * Taxi B -> A and the aeroplane parks facing south, with its way out 180 degrees behind
	 * it: a push. Taxi C -> A and it parks facing north, already pointing the way it will
	 * leave: no push. Same graph, same departure, opposite answers - which is the whole of
	 * what the straight-out rule measures.
	 */
	struct FPushbackGraph
	{
		FGuidelineNodeId A;
		FGuidelineNodeId B;
		FGuidelineNodeId C;
		FGuidelineEdgeId AB;
	};

	FPushbackGraph PushbackBuildGraph(URoadNetwork& Net)
	{
		URoadProfile* Runway = TestProfiles::Runway();
		const FRoadNodeId RA = Net.AddNode(FVector2D(-50000.0, 0.0));
		const FRoadNodeId RM = Net.AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId RB = Net.AddNode(FVector2D(50000.0, 0.0));
		Net.AddStraightSegment(RA, RM, Runway);
		Net.AddStraightSegment(RM, RB, Runway);

		FPushbackGraph G;
		G.B = TestGraph::Node(Net, 0.0, 0.0);
		G.A = TestGraph::Node(Net, 0.0, -20000.0);
		G.C = TestGraph::Node(Net, 0.0, -40000.0);

		// bDerived FALSE, as DepartAgentTest's own edge is: an authored line survives the
		// solve, and nothing here rebuilds the graph.
		TestGraph::FJoinOptions Options;
		Options.bDerived = false;
		G.AB = TestGraph::Join(Net, G.A, G.B, Options);
		TestGraph::Join(Net, G.C, G.A, Options);
		return G;
	}

	/** Taxis one aircraft From -> To and returns its id once it has parked, or 0. */
	int32 PushbackParkFacing(UGroundTraffic& Traffic, URoadNetwork& Net,
		FGuidelineNodeId From, FGuidelineNodeId To)
	{
		FRouteQuery Q;
		Q.Start = From;
		Q.Goal = To;
		Q.Class = ETraversalClass::Aircraft;

		const int32 Id = Traffic.DispatchAgent(&Net, RouteSearch::Find(Net, Q),
			TestAirframes::Piper(), ETraversalClass::Aircraft, /*ShutdownPauseSeconds*/ 0.0);
		if (Id <= 0)
		{
			return 0;
		}

		// THE PHASE SAYS IT ARRIVED, not a distance somebody computed - the shape
		// DepartAgentTest uses for the same wait.
		for (int32 I = 0; I < 20000 && Traffic.FindAgent(Id)->Phase != EAgentPhase::Parked; ++I)
		{
			Traffic.Advance(1.0 / 30.0, &Net);
		}
		return Traffic.FindAgent(Id)->Phase == EAgentPhase::Parked ? Id : 0;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPushbackClearanceTest,
	"Airside.Model.PushbackClearance",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPushbackClearanceTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const FPushbackGraph G = PushbackBuildGraph(*Net);

	// Taxied in from the runway, so it parks facing SOUTH with its way out behind it.
	const int32 Id = PushbackParkFacing(*Traffic, *Net, G.B, G.A);
	if (!TestTrue(TEXT("an aircraft parks facing away from its way out"), Id > 0))
	{
		return false;
	}

	// SOMEBODY ELSE IS STANDING ON THE GROUND THE PUSH NEEDS. A phantom holder is how
	// GroundTrafficTest plants a blocker, and it is the right tool here: what is under test is
	// the clearance arithmetic, not a second aircraft's behaviour.
	FTrafficClaim Sitting;
	Sitting.AgentId = 99;
	Sitting.Resource = FTrafficResource::OfNode(G.B);
	Sitting.bOccupied = true;
	FTrafficClaim Blocker;
	Traffic->OccupancyForTest().TryClaim(Sitting, Blocker);

	// A PUSH IS GRANTED WHOLE OR NOT AT ALL. A manoeuvring agent cannot replan - a stand's
	// lead-in is the only way off it - so a push that could be stopped half way would be a
	// phase able to block a taxiway indefinitely with nothing able to act on it.
	TestEqual(TEXT("a push whose ground is occupied is refused"),
		Traffic->DepartAgent(Id, *Net), EDepartureRefusal::PushbackBlocked);

	// AND NOTHING WAS HALF-STARTED. This is the assertion that would catch a DepartAgent which
	// armed the phase and then discovered the refusal: an aeroplane left Manoeuvring with a
	// blocked route would sit on its own lead-in for ever.
	TestEqual(TEXT("and the aeroplane is still parked, not half-pushed"),
		Traffic->FindAgent(Id)->Phase, EAgentPhase::Parked);

	// THE REFUSAL CLEARS ITSELF, unlike NoRoute or NotAdmitted. That is the whole reason it is
	// a separate value: a caller that treated it as permanent would give up on an aeroplane
	// that would have left seconds later.
	Traffic->OccupancyForTest().ReleaseAll(99);

	TestEqual(TEXT("and once the ground frees, the same aeroplane is cleared"),
		Traffic->DepartAgent(Id, *Net), EDepartureRefusal::None);
	TestEqual(TEXT("into the manoeuvre, NOT straight into a taxi"),
		Traffic->FindAgent(Id)->Phase, EAgentPhase::Manoeuvring);

	// A PUSHING AEROPLANE HOLDS ITS OWN LEAD-IN. FClaimPass::Run's first arm releases every
	// guideline claim for a phase that is not on a route; a manoeuvring agent that fell into
	// it would show the stand's own line free with an aeroplane on it, and something could be
	// cleared down the line it is being pushed along.
	//
	// MEASURED AS A HOLD rather than asserted as a phase check: a test that merely named this
	// contract would pass on an implementation that took the wrong arm and happened to claim
	// the ground some other way.
	Traffic->Advance(1.0 / 30.0, Net);
	TestTrue(TEXT("a pushing aeroplane holds the lead-in it is standing on"),
		Traffic->GetOccupancy().IsHeld(FTrafficResource::OfEdge(G.AB), /*ExcludingAgent*/ 0));

	// AND IT GETS OFF THE STAND. The push is bounded, so this must terminate: a manoeuvre that
	// never ended is the failure HasArrived's clamp to Plan.Length exists to prevent.
	int32 Ticks = 0;
	for (; Ticks < 20000 && Traffic->FindAgent(Id) != nullptr
		&& Traffic->FindAgent(Id)->Phase == EAgentPhase::Manoeuvring; ++Ticks)
	{
		Traffic->Advance(1.0 / 30.0, Net);
	}
	TestTrue(FString::Printf(TEXT("the push finishes (%d ticks)"), Ticks), Ticks < 20000);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPushbackStraightOutTest,
	"Airside.Model.PushbackStraightOut",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPushbackStraightOutTest::RunTest(const FString& Parameters)
{
	// AN AEROPLANE ALREADY POINTING THE WAY IT WILL LEAVE NEEDS NO PUSH, and the rule is a
	// MEASUREMENT of the ground ahead rather than a flag on the stand - which is why one
	// question answers a taxi-through stand, a taxiway a player happened to draw past a stand,
	// and a graph rebuilt since the aeroplane parked.
	//
	// THE SAME GRAPH AS THE CLEARANCE TEST, driven the other way round. That is what makes
	// this a contrast rather than a second unrelated fixture: nothing about the airport
	// differs, only which direction the aeroplane arrived from.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const FPushbackGraph G = PushbackBuildGraph(*Net);

	// Taxied in from the SOUTH, so it parks at A facing north - already pointing at the
	// runway it is about to depart from.
	const int32 Id = PushbackParkFacing(*Traffic, *Net, G.C, G.A);
	if (!TestTrue(TEXT("an aircraft parks facing its way out"), Id > 0))
	{
		return false;
	}

	const EDepartureRefusal Why = Traffic->DepartAgent(Id, *Net);
	if (!TestEqual(FString::Printf(TEXT("it is cleared to leave (%d)"), static_cast<int32>(Why)),
		Why, EDepartureRefusal::None))
	{
		return false;
	}

	TestEqual(TEXT("and drives straight out - it never enters the manoeuvre at all"),
		Traffic->FindAgent(Id)->Phase, EAgentPhase::Taxiing);

	return true;
}

#endif
