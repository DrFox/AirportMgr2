#include "CoreMinimal.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Entities/AircraftType.h"
#include "Misc/AutomationTest.h"
#include "Model/DeparturePlanner.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteSearch.h"
#include "Present/AirsideTraffic.h"
#include "Present/RoadAgentActor.h"
#include "Present/RoadNetworkActor.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** The departure-release fixture: a runway split at (0,0) and an authored guideline from
	 *  A (0,-20000) to B (0,0) ON the strip. Built onto whichever network is handed in. */
	struct FDepAgentGraph { FGuidelineNodeId A, B; };

	FDepAgentGraph DepAgentBuild(URoadNetwork& Net)
	{
		URoadProfile* Runway = URoadProfile::MakeTransient(4500.0, 1500.0, 450.0);
		Runway->bContinuousThroughJunctions = true;
		const FRoadNodeId RA = Net.AddNode(FVector2D(-50000.0, 0.0));
		const FRoadNodeId RM = Net.AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId RB = Net.AddNode(FVector2D(50000.0, 0.0));
		Net.AddStraightSegment(RA, RM, Runway);
		Net.AddStraightSegment(RM, RB, Runway);

		FDepAgentGraph G;
		G.A = Net.AddGuidelineNode(FVector2D(0.0, -20000.0), false);
		G.B = Net.AddGuidelineNode(FVector2D(0.0, 0.0), false);
		FGuidelineEdge Edge;
		Edge.A = G.A; Edge.B = G.B;
		Edge.Control = FVector2D(0.0, -10000.0);
		Edge.AllowedTraffic = FTrafficMask::All();
		Edge.Direction = EGuidelineDir::Bidirectional;
		Edge.bDerived = false;
		Net.AddGuidelineEdge(MoveTemp(Edge));
		return G;
	}

	FAirframe DepAgentPiper()
	{
		FAirframe A;
		A.Ground = UAircraftType::PiperMeridianGround();
		A.Climb = UAircraftType::PiperMeridianClimb();
		A.Approach = UAircraftType::PiperMeridianApproach();
		A.Engine = UAircraftType::PiperMeridianEngine();
		return A;
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
	FRouteQuery Q; Q.Start = G.B; Q.Goal = G.A; Q.Class = ETraversalClass::Aircraft;
	const int32 Id = Traffic->DispatchAgent(Net, RouteSearch::Find(*Net, Q), DepAgentPiper(), ETraversalClass::Aircraft, 1.0);
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
	TestEqual(TEXT("it is taxiing again"), P->Phase, EAgentPhase::Taxiing);
	TestTrue(TEXT("with a departure armed"), P->bDepartureArmed);
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
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("a world"), World)) { return false; }
	FWorldContext& Ctx = GEngine->CreateNewWorldContext(EWorldType::Game);
	Ctx.SetCurrentWorld(World);
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };

	ARoadNetworkActor* Actor = World->SpawnActor<ARoadNetworkActor>();
	if (!TestNotNull(TEXT("actor"), Actor)) { return false; }
	Actor->PlaceNode(FVector2D(-100000.0, -100000.0));
	const FDepAgentGraph G = DepAgentBuild(*Actor->Network);

	FRouteQuery Q; Q.Start = G.B; Q.Goal = G.A; Q.Class = ETraversalClass::Aircraft;
	if (!TestTrue(TEXT("dispatched through the actor"), Actor->DispatchAgent(RouteSearch::Find(*Actor->Network, Q), DepAgentPiper()))) { return false; }
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

#endif
