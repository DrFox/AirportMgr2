#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Model/DeparturePlanner.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoutePolicy.h"
#include "Model/RouteSearch.h"
#include "Present/AirsideTraffic.h"
#include "Present/RoadAgentActor.h"
#include "Present/RoadNetworkActor.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/**
	 * The departure-release fixture: a runway split at (0,0), a stand at A (0,-20000), and a
	 * JUNCTION at J (0,-10000) where the lead-in meets a taxiway running east-west.
	 *
	 * THE JUNCTION IS NOT DECORATION. A pushback reverses onto the arm of the junction the
	 * departure does NOT take, so a stand joined to its runway by a single edge has nowhere to
	 * be pushed and UGroundTraffic::DepartAgent refuses it - see PushbackPlanner::Plan, and the
	 * ruling that a stand nothing can leave is a layout problem rather than something to
	 * improvise around. This fixture used to be that single edge, and duly started refusing.
	 *
	 * A REAL AIRPORT NEVER HAS THE OTHER SHAPE: a stand's lead-in meets a taxiway, not a
	 * runway. The fixture is more realistic for the change, not less.
	 */
	struct FDepAgentGraph { FGuidelineNodeId A, B, J, E; };

	FDepAgentGraph DepAgentBuild(URoadNetwork& Net)
	{
		URoadProfile* Runway = TestProfiles::Runway();
		const FRoadNodeId RA = Net.AddNode(FVector2D(-50000.0, 0.0));
		const FRoadNodeId RM = Net.AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId RB = Net.AddNode(FVector2D(50000.0, 0.0));
		Net.AddStraightSegment(RA, RM, Runway);
		Net.AddStraightSegment(RM, RB, Runway);

		FDepAgentGraph G;
		G.A = Net.AddGuidelineNode(FVector2D(0.0, -20000.0), false);   // the stand
		G.J = Net.AddGuidelineNode(FVector2D(0.0, -10000.0), false);   // lead-in meets taxiway
		G.B = Net.AddGuidelineNode(FVector2D(0.0, 0.0), false);        // on the strip
		G.E = Net.AddGuidelineNode(FVector2D(20000.0, -10000.0), false); // the far arm

		auto Join = [&Net](FGuidelineNodeId From, FGuidelineNodeId To)
		{
			const FVector2D Mid =
				(Net.GetGuidelineNode(From)->Position + Net.GetGuidelineNode(To)->Position) * 0.5;

			FGuidelineEdge Edge;
			Edge.A = From;
			Edge.B = To;
			Edge.Control = Mid;
			Edge.AllowedTraffic = FTrafficMask::All();
			Edge.Direction = EGuidelineDir::Bidirectional;
			Edge.bDerived = false;
			Net.AddGuidelineEdge(MoveTemp(Edge));
		};

		Join(G.A, G.J);
		Join(G.J, G.B);
		Join(G.J, G.E);
		return G;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDepartAgentModelTest,
	"Airside.Model.Traffic.DepartAgent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDepartAgentModelTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FDepAgentGraph G = DepAgentBuild(*Net);
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());

	// Taxi B -> A: parks at A, off the runway, engine shut down after the pause.
	// #312: was a hand-built FRouteQuery that skipped AvoidRunways.
	const int32 Id = Traffic->DispatchAgent(Net, TestGraph::Probe(*Net, G.B, G.A, ETraversalClass::Aircraft), TestAirframes::Piper(), ETraversalClass::Aircraft, 1.0);
	if (!TestTrue(TEXT("dispatched"), Id > 0)) { return false; }

	TestEqual(TEXT("a taxiing agent may not depart"), Traffic->DepartAgent(Id, *Net), EDepartureRefusal::NotParked);
	TestEqual(TEXT("an unknown id is NotParked too - there is nothing parked by that id"), Traffic->DepartAgent(Id + 9, *Net), EDepartureRefusal::NotParked);

	for (int32 I = 0; I < 20000 && Traffic->FindAgent(Id)->Phase != EAgentPhase::Parked; ++I) { Traffic->Advance(1.0 / 30.0, Net); }
	if (!TestEqual(TEXT("parked"), Traffic->FindAgent(Id)->Phase, EAgentPhase::Parked)) { return false; }
	for (int32 I = 0; I < 300 && Traffic->FindAgent(Id)->bEngineRunning; ++I) { Traffic->Advance(0.1, Net); }
	if (!TestFalse(TEXT("engine shut down after the pause"), Traffic->FindAgent(Id)->bEngineRunning)) { return false; }

	const EDepartureRefusal Why = Traffic->DepartAgent(Id, *Net);
	if (!TestEqual(FString::Printf(TEXT("a parked agent departs (%d)"), static_cast<int32>(Why)), Why, EDepartureRefusal::None)) { return false; }
	const FRoadAgent* P = Traffic->FindAgent(Id);
	// MANOEUVRING FIRST, NOT TAXIING. This parked aeroplane's way out is 180 degrees behind
	// it, so it is pushed off its stand before it taxis at all - which is the whole of the
	// pushback feature. The assertion is UPDATED to the real sequence rather than relaxed:
	// going straight to Taxiing from Parked is now a defect, not an alternative.
	TestEqual(TEXT("it manoeuvres off the stand first"), P->Phase, EAgentPhase::Manoeuvring);
	TestTrue(TEXT("with a departure armed"), P->bDepartureArmed);

	// AND IT REACHES THE TAXI. Asserted rather than assumed: a manoeuvre that never handed
	// over would leave the aeroplane half off its stand for ever, and every assertion below
	// would still pass.
	for (int32 I = 0; I < 20000 && Traffic->FindAgent(Id) != nullptr
		&& Traffic->FindAgent(Id)->Phase == EAgentPhase::Manoeuvring; ++I)
	{
		Traffic->Advance(1.0 / 30.0, Net);
	}
	TestEqual(TEXT("and is taxiing once the push is over"),
		Traffic->FindAgent(Id)->Phase, EAgentPhase::Taxiing);
	TestTrue(TEXT("and the engine running - a redirect restarts it"), P->bEngineRunning);
	TestEqual(TEXT("departing twice is refused: it is no longer parked"), Traffic->DepartAgent(Id, *Net), EDepartureRefusal::NotParked);

	bool bDeparted = false;
	for (double T = 0.0; T < 300.0; T += 0.05)
	{
		Traffic->Advance(0.05, Net);
		const FRoadAgent* Now = Traffic->FindAgent(Id);
		if (Now == nullptr) { break; }
		bDeparted = bDeparted || Now->Phase == EAgentPhase::Departing;
	}
	TestTrue(TEXT("it rolled"), bDeparted);
	TestNull(TEXT("and it is gone"), Traffic->FindAgent(Id));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDepartAgentForwardersTest,
	"Airside.Present.DepartAgentForwarders",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDepartAgentForwardersTest::RunTest(const FString& Parameters)
{
	// THE SEAM TEST: the panel calls the actor; the actor must reach the model, and the
	// view must follow the agent out. A forwarder that was never wired compiles fine.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor"), Actor)) { return false; }
	Actor->PlaceNode(FVector2D(-100000.0, -100000.0));
	const FDepAgentGraph G = DepAgentBuild(*Actor->Network);
	// THE HAND-AUTHORED GRAPH IS THIS FIXTURE'S GRAPH. PlaceNode's rebuild derived (and stamped)
	// the network before DepAgentBuild wrote the runway and its lines straight onto it, so by
	// URoadNetwork::AreGuidelinesBehindRoad the graph is behind the road and both planners would
	// wait for a release that never comes (2026-09-27). Stamped current, because the lines this
	// test means ARE the ones it just authored - the model test above never derives at all.
	Actor->Network->MarkGuidelinesDerived();

	// #312: was a hand-built FRouteQuery that skipped AvoidRunways.
	if (!TestTrue(TEXT("dispatched through the actor"), Actor->DispatchAgent(TestGraph::Probe(*Actor->Network, G.B, G.A, ETraversalClass::Aircraft), TestAirframes::Piper()))) { return false; }
	const int32 Id = Actor->GetTraffic()->GetNewestAgentId();
	TestNotNull(TEXT("the actor can name the agent's view by id"), Actor->GetAgentView(Id));
	TestNull(TEXT("and returns null for an unknown id"), Actor->GetAgentView(Id + 9));

	TestEqual(TEXT("refused while taxiing, through the actor"), Actor->DepartAgent(Id), EDepartureRefusal::NotParked);
	for (int32 I = 0; I < 20000 && Actor->GetTraffic()->LastAgentPhaseForTest() != EAgentPhase::Parked; ++I) { Actor->Tick(1.0f / 30.0f); }
	TestEqual(TEXT("accepted once parked, through the actor"), Actor->DepartAgent(Id), EDepartureRefusal::None);

	for (int32 I = 0; I < 20000 && Actor->GetAgentCount() > 0; ++I) { Actor->Tick(1.0f / 30.0f); }
	TestEqual(TEXT("the agent departed and was dropped"), Actor->GetAgentCount(), 0);
	TestNull(TEXT("and its view went with it"), Actor->GetAgentView(Id));
	return true;
}

/**
 * A REFUSED DEPARTURE IS SAID ONCE, not every time it is asked.
 *
 * REPORTED FROM PLAY, 2026-09-27: an SR22 that could not take off wrote "DepartAgent 1:
 * Departure refused: the runway is 40366 uu; this aircraft's field length is 43000" 14,944
 * times in three minutes. UFuelService retries the departure every tick until it succeeds -
 * rightly, so lengthening the runway releases the aircraft - and quiets its OWN line on an
 * unchanged reason; the planner's verdict one level down was logged on every call.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDepartAgentRefusalSaidOnceTest,
	"Airside.Model.Traffic.DepartRefusalSaidOnce",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDepartAgentRefusalSaidOnceTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FDepAgentGraph G = DepAgentBuild(*Net);
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());

	// A take-off figure no strip here meets (the fixture's runway is 1 km), so every departure
	// is refused for the same reason. Dispatched as a taxi, not an arrival, so the arrival's
	// own can-it-leave check does not stop it reaching the stand.
	FAirframe CannotLeave = TestAirframes::Piper();
	CannotLeave.Requirements.TakeoffFieldLength = 1.0e7;
	const int32 Id = Traffic->DispatchAgent(Net, TestGraph::Probe(*Net, G.B, G.A, ETraversalClass::Aircraft),
		CannotLeave, ETraversalClass::Aircraft, 1.0);
	if (!TestTrue(TEXT("dispatched"), Id > 0)) { return false; }
	for (int32 I = 0; I < 20000 && Traffic->FindAgent(Id)->Phase != EAgentPhase::Parked; ++I) { Traffic->Advance(1.0 / 30.0, Net); }
	if (!TestEqual(TEXT("parked"), Traffic->FindAgent(Id)->Phase, EAgentPhase::Parked)) { return false; }

	FLogLineSpy Spy(FName(TEXT("LogAirsideTraffic")));
	GLog->AddOutputDevice(&Spy);
	int32 Refused = 0;
	for (int32 Try = 0; Try < 5; ++Try)
	{
		Refused += Traffic->DepartAgent(Id, *Net) != EDepartureRefusal::None ? 1 : 0;
	}
	GLog->RemoveOutputDevice(&Spy);

	TestEqual(TEXT("all five tries are refused"), Refused, 5);
	const FString Prefix = FString::Printf(TEXT("DepartAgent %d: Departure refused"), Id);
	const int32 Said = Spy.CapturedLines.FilterByPredicate(
		[&Prefix](const FString& Line) { return Line.StartsWith(Prefix); }).Num();
	TestEqual(FString::Printf(TEXT("but the refusal is logged once, not five times (%d)"), Said), Said, 1);
	return true;
}

#endif
