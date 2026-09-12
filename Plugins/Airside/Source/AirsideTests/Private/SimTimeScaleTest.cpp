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

// ---------------------------------------------------------------------------------------
/**
 * A JITTERING FRAME RATE STILL MOVES THE AGENT IN EVEN STEPS.
 *
 * REPORTED FROM PLAY (2026-09-12): judder that got worse the closer the camera and the
 * faster the aircraft, and vanished in a slow turn. Every model test said the motion was
 * smooth, and every one of them was right - the model puts the aeroplane exactly where
 * Speed x DeltaSeconds says, and DeltaSeconds is real wall-clock that jitters. The display
 * does NOT jitter: vsync holds each frame for a whole number of refreshes. So equal display
 * slots were being given unequal distances, which is a velocity flicker - measured from
 * play at mean 5.1%, p95 16.8%, worst 36%.
 *
 * WHY THIS IS A PRESENT TEST AND NOT A MODEL ONE. Nothing in Model/ is wrong and nothing
 * there could have caught this: the defect is in what the model is HANDED, which is decided
 * in exactly one line of ARoadNetworkActor::Tick. Measured on distance per tick rather than
 * on the smoothed delta itself, because a delta that was even while Advance ignored it would
 * pass and the aircraft would still judder - the same trap SimTimeScale above documents.
 *
 * The deltas are a real 120 Hz frame rate with the jitter a real one has. An agent at a
 * constant speed must then cover near-identical ground every tick.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEvenStepTest,
	"Airside.Present.EvenStepsUnderAJitteringFrameRate",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FEvenStepTest::RunTest(const FString& Parameters)
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

	const FGuidelineNodeId Start = Net.AddGuidelineNode(FVector2D(0.0, 0.0), /*bDerived*/ false);
	const FGuidelineNodeId End = Net.AddGuidelineNode(FVector2D(400000.0, 0.0), /*bDerived*/ false);
	{
		FGuidelineEdge Edge;
		Edge.A = Start;
		Edge.B = End;
		Edge.Control = FVector2D(200000.0, 0.0);
		Edge.AllowedTraffic = FTrafficMask::All();
		Edge.Direction = EGuidelineDir::Bidirectional;
		Edge.bDerived = false;
		Net.AddGuidelineEdge(MoveTemp(Edge));
	}
	FRouteQuery Query; Query.Start = Start; Query.Goal = End; Query.Class = ETraversalClass::GroundVehicle;
	const FRoutePlan Plan = RouteSearch::Find(Net, Query);
	if (!TestTrue(TEXT("the leg routes"), Plan.IsValid())) { return false; }

	UAirsideTraffic* Traffic = Actor->GetTraffic();
	// Through the actor, the same way SimTimeScale above dispatches: it is the composition
	// under test here, not the model underneath it.
	if (!TestTrue(TEXT("dispatched"),
		Actor->DispatchAgent(Plan, UAirsideSettings::ResolveDefaultAirframe())))
	{
		return false;
	}

	// A real 120 Hz frame rate: 8.33 ms nominal, jittering the way the play trace did.
	const double Deltas[] = { 0.00833, 0.00791, 0.00874, 0.00812, 0.00902, 0.00768,
	                          0.00845, 0.00829, 0.00788, 0.00861 };

	// MEASURED FRAME TO FRAME, not as a spread over the run. An agent still on its
	// acceleration ramp covers a little more ground every tick for an entirely legitimate
	// reason, and a min/max over two hundred frames is dominated by that - the first version
	// of this test reported 25% and was measuring the ramp. Judder is the step changing
	// against the step BEFORE it, which a ramp does smoothly and jitter does not.
	const auto WorstStepChange = [&](double SmoothingRate)
	{
		Traffic->ClearAgents();
		Actor->SetDeltaSmoothingForTest(SmoothingRate);
		Actor->DispatchAgent(Plan, UAirsideSettings::ResolveDefaultAirframe());

		for (int32 Warm = 0; Warm < 1200; ++Warm)
		{
			Actor->Tick(static_cast<float>(Deltas[Warm % 10]));
		}

		FVector2D Previous = Traffic->LastAgentPositionForTest();
		double Last = 0.0;
		double Worst = 0.0;
		for (int32 Frame = 0; Frame < 200; ++Frame)
		{
			Actor->Tick(static_cast<float>(Deltas[Frame % 10]));
			const FVector2D At = Traffic->LastAgentPositionForTest();
			const double Step = FVector2D::Distance(At, Previous);
			Previous = At;
			if (Step > 0.5 && Last > 0.5)
			{
				Worst = FMath::Max(Worst, FMath::Abs(Step - Last) / Last);
			}
			Last = Step;
		}
		return Worst;
	};

	// 1.0 hands the raw delta straight through - the behaviour before this fix - and is the
	// control. Without it a fixture too gentle to jitter would pass whatever Tick did.
	const double Raw = WorstStepChange(1.0);
	const double Evened = WorstStepChange(0.1);

	AddInfo(FString::Printf(
		TEXT("worst frame-to-frame step change: raw delta %.2f%%, evened %.2f%%"),
		100.0 * Raw, 100.0 * Evened));

	TestTrue(*FString::Printf(
		TEXT("the raw delta puts its own jitter into the distance covered (%.1f%%)"), 100.0 * Raw),
		Raw > 0.10);
	TestTrue(*FString::Printf(
		TEXT("and evening it takes that out (%.2f%%, was %.1f%%)"), 100.0 * Evened, 100.0 * Raw),
		Evened < 0.01);
	return true;
}

#endif
