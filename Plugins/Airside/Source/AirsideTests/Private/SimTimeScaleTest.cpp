#include "CoreMinimal.h"
#include "Content/AirsideSettings.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadTraffic.h"
#include "Model/RouteSearch.h"
#include "Present/AirsideTraffic.h"
#include "Present/RoadNetworkActor.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSimTimeScaleTest,
	"Airside.Present.SimTimeScale",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSimTimeScaleTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("a world to spawn into"), World)) { return false; }
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };

	ARoadNetworkActor* Actor = World->SpawnActor<ARoadNetworkActor>();
	if (!TestNotNull(TEXT("actor spawned"), Actor)) { return false; }
	Actor->PlaceNode(FVector2D(-100000.0, -100000.0));
	if (!TestNotNull(TEXT("the actor has a network"), Actor->Network.Get())) { return false; }
	URoadNetwork& Net = *Actor->Network;

	// One long authored guideline; neither run below reaches its end. Same fixture shape as
	// AgentRedirectTest, inlined for the same unity-build reason.
	const FGuidelineNodeId Start = Net.AddGuidelineNode(FVector2D(0.0, 0.0), /*bDerived*/ false);
	const FGuidelineNodeId End = Net.AddGuidelineNode(FVector2D(200000.0, 0.0), /*bDerived*/ false);
	{
		FGuidelineEdge Edge;
		Edge.A = Start;
		Edge.B = End;
		Edge.Control = FVector2D(100000.0, 0.0);
		Edge.AllowedTraffic = FTrafficMask::All();
		Edge.Direction = EGuidelineDir::Bidirectional;
		Edge.bDerived = false;
		Net.AddGuidelineEdge(MoveTemp(Edge));
	}
	FRouteQuery Query; Query.Start = Start; Query.Goal = End; Query.Class = ETraversalClass::GroundVehicle;
	const FRoutePlan Outbound = RouteSearch::Find(Net, Query);
	if (!TestTrue(TEXT("the leg routes"), Outbound.IsValid())) { return false; }

	UAirsideTraffic* Traffic = Actor->GetTraffic();
	const FAirframe Airframe = UAirsideSettings::ResolveDefaultAirframe();

	// Two identical agents, one ticked at scale 1 for N frames and read; then the actor set
	// to scale 2 and the second agent ticked N frames. Distance covered must be ~2x. Measured
	// on position, not on a flag: a flag that said "scaled" while Advance ignored it would
	// pass and the clock would run at x8 with trucks at x1. Ninety frames at 30 Hz is three
	// seconds - past the spool-up and the acceleration ramp, so the ratio is of distances
	// mostly covered at the cap and lands near 2 rather than being dominated by the ramp.
	auto Run = [&](double Scale)
	{
		Actor->SetSimTimeScale(Scale);
		Actor->DispatchAgent(Outbound, Airframe);
		for (int32 I = 0; I < 90; ++I) { Actor->Tick(1.0f / 30.0f); }
		const double D = FVector2D::Distance(Traffic->LastAgentPositionForTest(), FVector2D(0.0, 0.0));
		Traffic->ClearAgents();
		return D;
	};
	const double D1 = Run(1.0);
	const double D2 = Run(2.0);

	TestTrue(TEXT("the agent moved at all at x1"), D1 > 100.0);
	TestTrue(TEXT("x2 covers more ground than x1 in the same real frames"), D2 > D1 * 1.5);
	TestEqual(TEXT("and the actor reports the scale it was given"), Actor->GetSimTimeScale(), 2.0, 1e-12);
	Actor->SetSimTimeScale(-3.0);
	TestEqual(TEXT("a negative scale clamps to zero rather than running time backwards"), Actor->GetSimTimeScale(), 0.0, 1e-12);
	return true;
}

#endif
