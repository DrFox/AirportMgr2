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
	FAgentRedirectTest,
	"Airside.Present.AgentRedirectRetire",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAgentRedirectTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("a world to spawn into"), World)) { return false; }
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };

	ARoadNetworkActor* Actor = World->SpawnActor<ARoadNetworkActor>();
	if (!TestNotNull(TEXT("actor spawned"), Actor)) { return false; }
	Actor->PlaceNode(FVector2D(-100000.0, -100000.0));  // forces the network into existence
	if (!TestNotNull(TEXT("the actor has a network"), Actor->Network.Get())) { return false; }
	URoadNetwork& Net = *Actor->Network;

	// ONE AUTHORED GUIDELINE, no pavement. The redirect primitive is about the agent, not the
	// derivation, so the fixture is the smallest graph a route can exist on: two nodes and
	// a bidirectional edge. Authored (bDerived false) so a surface rebuild cannot sweep them.
	// Inlined rather than a shared helper: the tests module is a unity build and
	// RouteSearchTest already owns a Join() in its anonymous namespace.
	const FGuidelineNodeId Start = Net.AddGuidelineNode(FVector2D(0.0, 0.0), /*bDerived*/ false);
	const FGuidelineNodeId End = Net.AddGuidelineNode(FVector2D(20000.0, 0.0), /*bDerived*/ false);
	{
		FGuidelineEdge Edge;
		Edge.A = Start;
		Edge.B = End;
		Edge.Control = FVector2D(10000.0, 0.0);
		Edge.AllowedTraffic = FTrafficMask::All();
		Edge.Direction = EGuidelineDir::Bidirectional;
		Edge.bDerived = false;
		Net.AddGuidelineEdge(MoveTemp(Edge));
	}

	FRouteQuery Out; Out.Start = Start; Out.Goal = End; Out.Class = ETraversalClass::GroundVehicle;
	FRouteQuery Back; Back.Start = End; Back.Goal = Start; Back.Class = ETraversalClass::GroundVehicle;
	const FRoutePlan Outbound = RouteSearch::Find(Net, Out);
	const FRoutePlan Return = RouteSearch::Find(Net, Back);
	if (!TestTrue(TEXT("both legs route"), Outbound.IsValid() && Return.IsValid())) { return false; }

	UAirsideTraffic* Traffic = Actor->GetTraffic();
	TArray<TPair<EAgentPhase, EAgentPhase>> Transitions;
	Traffic->OnAgentPhaseChanged.AddLambda(
		[&Transitions](int32, EAgentPhase From, EAgentPhase To) { Transitions.Emplace(From, To); });

	const FAirframe Airframe = UAirsideSettings::ResolveDefaultAirframe();
	if (!TestTrue(TEXT("outbound dispatch accepted"), Actor->DispatchAgent(Outbound, Airframe))) { return false; }
	const int32 Id = Traffic->GetNewestAgentId();
	TestTrue(TEXT("a dispatched agent has a non-zero id"), Id > 0);

	auto TickUntil = [Actor](EAgentPhase Want, int32 MaxTicks)
	{
		for (int32 Tick = 0; Tick < MaxTicks; ++Tick)
		{
			Actor->Tick(1.0f / 30.0f);
			if (Actor->GetTraffic()->LastAgentPhaseForTest() == Want) { return true; }
		}
		return false;
	};
	if (!TestTrue(TEXT("the agent parks at the far end"), TickUntil(EAgentPhase::Parked, 20000))) { return false; }
	TestTrue(TEXT("and it is near the far end"),
		FVector2D::Distance(Traffic->LastAgentPositionForTest(), FVector2D(20000.0, 0.0)) < 1500.0);

	TestFalse(TEXT("an unknown id is refused"), Traffic->RedirectAgent(Id + 999, &Net, Return));
	if (!TestTrue(TEXT("a parked agent accepts a new plan"), Traffic->RedirectAgent(Id, &Net, Return))) { return false; }
	TestEqual(TEXT("redirect keeps the SAME agent"), Traffic->GetAgentCount(), 1);
	TestEqual(TEXT("and the same id"), Traffic->GetNewestAgentId(), Id);
	if (!TestTrue(TEXT("it parks again at the start"), TickUntil(EAgentPhase::Parked, 20000))) { return false; }
	TestTrue(TEXT("back where it began"),
		FVector2D::Distance(Traffic->LastAgentPositionForTest(), FVector2D(0.0, 0.0)) < 1500.0);

	TestTrue(TEXT("retire removes it"), Traffic->RetireAgent(Id));
	TestEqual(TEXT("no agents remain"), Traffic->GetAgentCount(), 0);
	TestFalse(TEXT("retiring twice is refused"), Traffic->RetireAgent(Id));

	auto Count = [&Transitions](EAgentPhase From, EAgentPhase To)
	{
		int32 N = 0;
		for (const TPair<EAgentPhase, EAgentPhase>& T : Transitions) { if (T.Key == From && T.Value == To) { ++N; } }
		return N;
	};
	TestEqual(TEXT("two arrivals at a stop were announced"), Count(EAgentPhase::Taxiing, EAgentPhase::Parked), 2);
	TestEqual(TEXT("the redirect was announced as Parked -> Taxiing"), Count(EAgentPhase::Parked, EAgentPhase::Taxiing), 1);
	TestEqual(TEXT("the retirement was announced as Parked -> Gone"), Count(EAgentPhase::Parked, EAgentPhase::Gone), 1);
	return true;
}

#endif
