#include "CoreMinimal.h"
#include "Content/AirsideSettings.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Model/OpsEvents.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadTraffic.h"
#include "Model/RouteSearch.h"
#include "Model/SimClock.h"
#include "OpsEventsTestListener.h"
#include "Present/AirsideTraffic.h"
#include "Present/OpsRuntime.h"
#include "Present/RoadNetworkActor.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOpsRuntimeTest,
	"AirportOps.Present.Runtime",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOpsRuntimeTest::RunTest(const FString& Parameters)
{
	// THE COMPOSITION TEST FOR THE SEAM. Every piece below has its own unit test; this is
	// the one that fails if any of them is left unwired: Airside delegate -> runtime ->
	// bus, runtime speed -> actor scale, runtime save -> slot -> restore -> mesh rebuilt.
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("a world to spawn into"), World)) { return false; }
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };

	ARoadNetworkActor* Actor = World->SpawnActor<ARoadNetworkActor>();
	if (!TestNotNull(TEXT("actor spawned"), Actor)) { return false; }

	// A real road, so the mesh has triangles the load can be measured by.
	const int32 RoadA = Actor->PlaceNode(FVector2D(0.0, 30000.0));
	const int32 RoadB = Actor->PlaceNode(FVector2D(20000.0, 30000.0));
	if (!TestTrue(TEXT("a road segment exists"), Actor->ConnectNodes(RoadA, RoadB))) { return false; }
	if (!TestNotNull(TEXT("the actor has a network"), Actor->Network.Get())) { return false; }

	// And one authored guideline for the agent, as in AgentRedirectTest.
	{
		URoadNetwork& Net = *Actor->Network;
		const FGuidelineNodeId Start = Net.AddGuidelineNode(FVector2D(0.0, 0.0), /*bDerived*/ false);
		const FGuidelineNodeId End = Net.AddGuidelineNode(FVector2D(20000.0, 0.0), /*bDerived*/ false);
		FGuidelineEdge Edge;
		Edge.A = Start;
		Edge.B = End;
		Edge.Control = FVector2D(10000.0, 0.0);
		Edge.AllowedTraffic = FTrafficMask::All();
		Edge.Direction = EGuidelineDir::Bidirectional;
		Edge.bDerived = false;
		Net.AddGuidelineEdge(MoveTemp(Edge));

		FRouteQuery Query; Query.Start = Start; Query.Goal = End; Query.Class = ETraversalClass::GroundVehicle;
		const FRoutePlan Outbound = RouteSearch::Find(Net, Query);
		if (!TestTrue(TEXT("the leg routes"), Outbound.IsValid())) { return false; }

		UOpsRuntime* Runtime = NewObject<UOpsRuntime>();
		Runtime->Attach(Actor);

		UOpsEventsTestListener* L = NewObject<UOpsEventsTestListener>();
		Runtime->GetEvents()->OnAgentPhaseChanged.AddDynamic(L, &UOpsEventsTestListener::OnPhase);
		Runtime->GetEvents()->OnSpeedChanged.AddDynamic(L, &UOpsEventsTestListener::OnSpeed);

		Actor->DispatchAgent(Outbound, UAirsideSettings::ResolveDefaultAirframe());
		TestTrue(TEXT("a spawn on the Airside traffic reaches the ops bus as Gone -> Taxiing"),
			L->Seen.ContainsByPredicate([](const FString& S) { return S.StartsWith(TEXT("phase:")) && S.EndsWith(TEXT(":4->1")); }));

		Runtime->StepSpeed(+1);
		TestEqual(TEXT("stepping speed announces the new speed"), L->Seen.Last(), FString(TEXT("speed:2")));
		TestEqual(TEXT("and pushes the multiplier into the actor"), Actor->GetSimTimeScale(), 2.0, 1e-12);

		Runtime->TogglePause();
		TestEqual(TEXT("pause zeroes the actor's scale"), Actor->GetSimTimeScale(), 0.0, 1e-12);
		Runtime->TogglePause();
		TestEqual(TEXT("unpause restores the previous speed"), Actor->GetSimTimeScale(), 2.0, 1e-12);

		Runtime->StepSpeed(+5);
		TestEqual(TEXT("stepping past the top clamps at x8"), Actor->GetSimTimeScale(), 8.0, 1e-12);
		Runtime->StepSpeed(-9);
		TestEqual(TEXT("stepping past the bottom clamps at x1, never paused"), Actor->GetSimTimeScale(), 1.0, 1e-12);

		Runtime->Tick(1.0);
		TestTrue(TEXT("ticking the runtime advances the clock"), Runtime->GetClock()->Now() > 0.0);

		// Save, clear, load: the network comes back and the mesh is rebuilt, measured by
		// triangle count - the same probe MeshFreshnessTest uses. The facade defers its
		// rebuild, so the baseline and the cleared state are each rebuilt explicitly before
		// they are read; the LOAD path is the one under test and gets no such help.
		Actor->RebuildMesh();
		const int32 TrisBefore = Actor->SurfaceTriangleCountForTest();
		TestTrue(TEXT("the road produced a surface to measure"), TrisBefore > 0);
		const FString Slot = TEXT("AirportOpsTest_Runtime");
		if (!TestTrue(TEXT("save writes"), Runtime->SaveToSlot(Slot))) { return false; }

		Actor->ClearNetwork();
		Actor->RebuildMesh();
		TestEqual(TEXT("cleared network has no nodes"), Actor->Network->GetNodes().Num(), 0);
		TestEqual(TEXT("and no surface"), Actor->SurfaceTriangleCountForTest(), 0);

		if (!TestTrue(TEXT("load reads"), Runtime->LoadFromSlot(Slot))) { return false; }
		TestEqual(TEXT("the nodes are back"), Actor->Network->GetNodes().Num(), 2);
		TestEqual(TEXT("and the surface mesh was rebuilt from them"), Actor->SurfaceTriangleCountForTest(), TrisBefore);
		TestEqual(TEXT("agents do not survive a load - they were never saved"), Actor->GetTraffic()->GetAgentCount(), 0);
		TestFalse(TEXT("a load is a new undo baseline"), Actor->CanUndo());
		TestFalse(TEXT("a missing slot is refused, not a crash"), Runtime->LoadFromSlot(TEXT("AirportOpsTest_NoSuchRuntimeSlot")));
	}
	return true;
}

#endif
