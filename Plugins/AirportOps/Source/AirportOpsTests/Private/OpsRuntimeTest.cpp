#include "CoreMinimal.h"
#include "Content/AirsideSettings.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Model/FlightBoard.h"
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
#include "Profiles/RoadProfile.h"

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

	// A RUNWAY, clear of the road above, so UFlightBoard::DefaultApproachFocus has a threshold
	// to find - issue #105's follow-up comment on this test asks for exactly that, plus the
	// offer schedule check below.
	URoadProfile* RunwayProfile = URoadProfile::MakeTransient(4500.0, 1500.0, 450.0);
	RunwayProfile->bContinuousThroughJunctions = true;
	Actor->MinimumRunwayLength = 100.0;   // short, deliberately - see MeshFreshnessTest's own comment
	if (!TestTrue(TEXT("a runway is placed"),
		Actor->PlaceRunway(FVector2D(0.0, -50000.0), FVector2D(6000.0, -50000.0), RunwayProfile)))
	{
		return false;
	}
	FVector2D ExpectedFocus;
	if (!TestTrue(TEXT("the runway gives DefaultApproachFocus a threshold to find"),
		UFlightBoard::DefaultApproachFocus(*Actor->Network, ExpectedFocus)))
	{
		return false;
	}

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

		// ATTACH'S OFFER CADENCE (issue #105, follow-up from #121 review). Content-independent:
		// it only asks whether SOME airline in the real catalog offers anything, not whether
		// this test's runway admits any particular one.
		if (TestTrue(TEXT("Attach armed the repeating offer schedule"), Runtime->HasOfferScheduledForTest()))
		{
			// ONE TICK, RIGHT THROUGH THE SCHEDULE'S OWN INTERVAL - not a guessed real-seconds
			// delta that might land short of it (or, converted back from a huge one, spend the
			// test iterating hundreds of avoidable fires). TimeScale() is exactly the
			// game-seconds-per-real-second USimClock::Advance divides by internally.
			const double RealSeconds = Runtime->OfferIntervalSecondsForTest() / Runtime->GetClock()->TimeScale() * 1.01;
			Runtime->Tick(RealSeconds);

			// THE FOCUS UPDATES REGARDLESS OF WHETHER THIS RUNWAY ADMITS ANY REAL FLEET
			// AIRCRAFT (UOpsRuntime::GenerateOffer sets it before the admissibility check) -
			// so this assertion, unlike an offer actually appearing, does not depend on this
			// test's short runway matching a real AircraftType's LandingFieldLength.
			TestEqual(TEXT("a runway-bearing network's board focus equals DefaultApproachFocus"),
				Runtime->GetFlightBoard()->ApproachFocus, ExpectedFocus);
		}

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

		// Asked of the LADDER rather than a literal, so adding a rung does not turn this
		// into a failing test that says nothing about the behaviour it guards. It used to
		// read 8.0 and duly failed the day x16 and x32 were added, which is a maintenance
		// cost with no diagnostic value.
		const TArrayView<const ESimSpeed> Ladder = UOpsRuntime::SpeedLadder();
		const double Fastest = USimClock::Multiplier(Ladder.Last());
		const double Slowest = USimClock::Multiplier(Ladder[0]);

		Runtime->StepSpeed(+Ladder.Num() + 2);
		TestEqual(TEXT("stepping past the top clamps at the fastest rung"),
			Actor->GetSimTimeScale(), Fastest, 1e-12);
		Runtime->StepSpeed(-Ladder.Num() - 4);
		TestEqual(TEXT("stepping past the bottom clamps at the slowest rung, never paused"),
			Actor->GetSimTimeScale(), Slowest, 1e-12);

		Runtime->Tick(1.0);
		TestTrue(TEXT("ticking the runtime advances the clock"), Runtime->GetClock()->Now() > 0.0);

		// Save, clear, load: the network comes back and the mesh is rebuilt, measured by
		// triangle count - the same probe MeshFreshnessTest uses. The facade defers its
		// rebuild, so the baseline and the cleared state are each rebuilt explicitly before
		// they are read; the LOAD path is the one under test and gets no such help.
		Actor->RebuildMesh();
		const int32 TrisBefore = Actor->GetPresenter()->SurfaceTriangleCountForTest();
		TestTrue(TEXT("the road produced a surface to measure"), TrisBefore > 0);
		const FString Slot = TEXT("AirportOpsTest_Runtime");
		if (!TestTrue(TEXT("save writes"), Runtime->SaveToSlot(Slot))) { return false; }

		Actor->ClearNetwork();
		Actor->RebuildMesh();
		TestEqual(TEXT("cleared network has no nodes"), Actor->Network->GetNodes().Num(), 0);
		TestEqual(TEXT("and no surface"), Actor->GetPresenter()->SurfaceTriangleCountForTest(), 0);

		if (!TestTrue(TEXT("load reads"), Runtime->LoadFromSlot(Slot))) { return false; }
		// 4, not 2: the road's own two PLUS the runway's two, added for issue #105's follow-up
		// comment above - see its own comment.
		TestEqual(TEXT("the nodes are back"), Actor->Network->GetNodes().Num(), 4);
		TestEqual(TEXT("and the surface mesh was rebuilt from them"), Actor->GetPresenter()->SurfaceTriangleCountForTest(), TrisBefore);
		TestEqual(TEXT("agents do not survive a load - they were never saved"), Actor->GetTraffic()->GetAgentCount(), 0);
		TestFalse(TEXT("a load is a new undo baseline"), Actor->CanUndo());
		TestFalse(TEXT("a missing slot is refused, not a crash"), Runtime->LoadFromSlot(TEXT("AirportOpsTest_NoSuchRuntimeSlot")));
	}
	return true;
}

#endif
