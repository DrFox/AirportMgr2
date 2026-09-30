#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "AirsideTestsLog.h"
#include "Misc/AutomationTest.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadAgent.h"
#include "Model/RoadNetwork.h"
#include "Model/TrafficOccupancy.h"
#include "Profiles/RoadProfile.h"
#include "Testing/AirsideTestGraph.h"

#if WITH_DEV_AUTOMATION_TESTS

// A RUNWAY CROSSING IS RELEASED ONLY WITH ITS EXIT CLEAR (PIE, 2026-09-30, M_Test_Small with two
// parallel runways): two taxi-in aircraft held short at the two bars of one crossing, opposite
// ways. The runway freed, agent 80 was granted the bar, committed, and stopped ON the strip short
// of the far bar node - "Agent 80 stops 0 uu short of node 52 held by agent 81" - while 81 held
// short "for runway segment 20 held by agent 80". Nothing asked whether the crossing's far side
// was free before the bar was granted, so the crosser owned the runway and the waiter owned its
// only way off it, and the resolver replanned 80 onto routes that all ran through 81, for ever.
//
// THE AGENT HELD AT THE BAR ON THE RUNWAY'S BEHALF (sentinel 99) is how both fixtures below get
// their aircraft to the bars first: a claim nobody drives, released by hand at the moment the
// PIE log's runway freed.

namespace
{
	constexpr int32 CrossingExitRunwayHolder = 99;

	/** The runway strip held OCCUPIED by the sentinel, the way a landing roll holds it. */
	void CrossingExitHoldStrip(UGroundTraffic& Traffic, FRoadSegmentId Strip)
	{
		FTrafficClaim Hold;
		Hold.AgentId = CrossingExitRunwayHolder;
		Hold.Resource = FTrafficResource::OfSurface(Strip);
		Hold.bOccupied = true;
		FTrafficClaim Blocker;
		Traffic.OccupancyForTest().TryClaim(Hold, Blocker);
	}

	/** Seconds an agent spent TAXIING, STOPPED, with its centre on the strip - the defect's shape. */
	void CrossingExitAccrueStoodOnStrip(const UGroundTraffic& Traffic, const URoadNetwork& Net,
		int32 Id, FRoadSegmentId Strip, double Dt, double& InOutSeconds)
	{
		const FRoadAgent* Agent = Traffic.FindAgent(Id);
		if (Agent != nullptr && Agent->Phase == EAgentPhase::Taxiing && Agent->Follower.Speed < 1.0
			&& Net.IsPointOnRunway(Agent->LastMotion.Position, Strip))
		{
			InOutSeconds += Dt;
		}
	}
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficCrossingExitClaimedBeforeReleaseTest,
	"Airside.Model.Traffic.CrossingExitClaimedBeforeRelease",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficCrossingExitClaimedBeforeReleaseTest::RunTest(const FString& Parameters)
{
	// THE ORDER THE PIE LOG SHOWS, on a crossing the two CAN share: A waits at the south bar H
	// while the runway is held; the runway frees, and at that moment B sets off towards the NORTH
	// side, arriving there while A is still accelerating across. The north side is a junction JN
	// a thousand uu past the far bar, so B comes in on its own branch and A leaves on another -
	// nothing forces a head-on, if B waits on its branch until A has gone.
	//
	//        NA            NB             A: S -> H -> X -> Far -> JN -> NA
	//          \          /               B: NB -> JN -> Far -> X -> H -> S
	//            \      /
	//               JN          (0, 4000)
	//               |
	//              Far  bar     (0, 3000)
	//   ============ X ==========  runway, half width 2250
	//               H   bar     (0, -3000)
	//               |
	//               S           (0, -20000)
	//
	// BEFORE THE RULE: A was granted the bar with nothing ahead of it claimed, committed, and B -
	// nearer its side than A was - reserved JN and Far and stopped with its nose on the far bar,
	// on A's only way off. A stopped on the strip, holding it; B held short for the runway A was
	// holding: the PIE cycle. Nobody in it can turn, so nobody ever finishes.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Runway = TestProfiles::Runway();
	const FRoadNodeId RA = Net->AddNode(FVector2D(-50000.0, 0.0));
	const FRoadNodeId RB = Net->AddNode(FVector2D(50000.0, 0.0));
	const FRoadSegmentId Strip = Net->AddStraightSegment(RA, RB, Runway);

	const FGuidelineNodeId S = TestGraph::Node(*Net, 0.0, -20000.0);
	const FGuidelineNodeId H = TestGraph::Node(*Net, 0.0, -3000.0);
	const FGuidelineNodeId X = TestGraph::Node(*Net, 0.0, 0.0);
	const FGuidelineNodeId Far = TestGraph::Node(*Net, 0.0, 3000.0);
	const FGuidelineNodeId JN = TestGraph::Node(*Net, 0.0, 4000.0);
	const FGuidelineNodeId NA = TestGraph::Node(*Net, -15000.0, 19000.0);
	// B'S START: 3000 uu out along its own branch - near enough that B, from rest, reaches the
	// north side before A (from rest at H) does. That is the race the rule has to win at the bar.
	const FVector2D NBAt = FVector2D(0.0, 4000.0) + FVector2D(1.0, 1.0).GetSafeNormal() * 3000.0;
	const FGuidelineNodeId NB = TestGraph::Node(*Net, NBAt.X, NBAt.Y);
	TestGraph::Join(*Net, S, H);
	TestGraph::Join(*Net, H, X);
	TestGraph::Join(*Net, X, Far);
	TestGraph::Join(*Net, Far, JN);
	TestGraph::Join(*Net, JN, NA);
	TestGraph::Join(*Net, JN, NB);
	Net->SetRunwayHoldingPositionForTest(H, Strip);
	Net->SetRunwayHoldingPositionForTest(Far, Strip);

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const FAirframe Airframe = TestAirframes::GroundOnly();
	CrossingExitHoldStrip(*Traffic, Strip);

	const int32 A = Traffic->DispatchAgent(Net, TestGraph::Probe(*Net, S, NA, ETraversalClass::Aircraft),
		Airframe, ETraversalClass::Aircraft, 1.0);
	if (!TestTrue(TEXT("A dispatched"), A > 0)) { return false; }

	// A REACHES THE BAR AND STOPS THERE - the runway is held.
	double StoppedFor = 0.0;
	TickUntil(*Traffic, *Net, 60.0, [&](int32)
	{
		const FRoadAgent* Q = Traffic->FindAgent(A);
		StoppedFor = (Q->Follower.Speed < 1.0 && Q->Follower.Travelled > 15000.0) ? StoppedFor + 0.05 : 0.0;
		return StoppedFor < 1.0;
	});
	if (!TestTrue(TEXT("A is holding short at H while the runway is held"), StoppedFor >= 1.0)) { return false; }

	// THE RUNWAY FREES, and B sets off.
	Traffic->OccupancyForTest().ReleaseAll(CrossingExitRunwayHolder);
	const int32 B = Traffic->DispatchAgent(Net, TestGraph::Probe(*Net, NB, S, ETraversalClass::Aircraft),
		Airframe, ETraversalClass::Aircraft, 1.0);
	if (!TestTrue(TEXT("B dispatched"), B > 0)) { return false; }

	const int32 DeadlockLinesBefore = Traffic->GetDeadlockLogLinesForTest();
	const int32 YieldsBefore = Traffic->GetYieldsForTest();
	double AStoodOnStrip = 0.0;
	double BStoodOnStrip = 0.0;
	TickUntil(*Traffic, *Net, 240.0, [&](int32)
	{
		CrossingExitAccrueStoodOnStrip(*Traffic, *Net, A, Strip, 0.05, AStoodOnStrip);
		CrossingExitAccrueStoodOnStrip(*Traffic, *Net, B, Strip, 0.05, BStoodOnStrip);
		return Traffic->FindAgent(A)->Phase != EAgentPhase::Parked || Traffic->FindAgent(B)->Phase != EAgentPhase::Parked;
	});

	const FRoadAgent* QA = Traffic->FindAgent(A);
	const FRoadAgent* QB = Traffic->FindAgent(B);
	UE_LOG(LogAirsideTests, Log,
		TEXT("CrossingExitClaimedBeforeRelease measured: A %s at %.0f uu (waiting on %d), B %s at %.0f uu (waiting on %d); ")
		TEXT("stood still on the strip A %.1f s, B %.1f s; deadlock lines %d, yields %d"),
		QA->Phase == EAgentPhase::Parked ? TEXT("parked") : TEXT("NOT parked"), QA->Follower.Travelled, QA->GetWaitingOn(),
		QB->Phase == EAgentPhase::Parked ? TEXT("parked") : TEXT("NOT parked"), QB->Follower.Travelled, QB->GetWaitingOn(),
		AStoodOnStrip, BStoodOnStrip,
		Traffic->GetDeadlockLogLinesForTest() - DeadlockLinesBefore, Traffic->GetYieldsForTest() - YieldsBefore);

	TestEqual(TEXT("A crossed and reached NA: it was released only with its exit claimed"), QA->Phase, EAgentPhase::Parked);
	TestEqual(TEXT("B crossed the other way after it, and reached S"), QB->Phase, EAgentPhase::Parked);
	TestTrue(FString::Printf(TEXT("nobody stood still on the runway (A %.1f s, B %.1f s) - the PIE crosser stopped on it for good"),
		AStoodOnStrip, BStoodOnStrip), AStoodOnStrip < 0.5 && BStoodOnStrip < 0.5);
	TestEqual(TEXT("and no deadlock had to be resolved: B waited on its own branch, off A's exit"),
		Traffic->GetDeadlockLogLinesForTest() - DeadlockLinesBefore, 0);
	TestFalse(TEXT("with the runway free afterwards"), Traffic->GetOccupancy().IsHeld(FTrafficResource::OfSurface(Strip), 0));
	return true;
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficCrossingHeadOnKeepsRunwayFreeTest,
	"Airside.Model.Traffic.CrossingHeadOnKeepsRunwayFree",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficCrossingHeadOnKeepsRunwayFreeTest::RunTest(const FString& Parameters)
{
	// THE PIE STATE ITSELF: both aircraft already holding short, at the two bars of one straight
	// crossing, opposite ways. On a single line that is a head-on no rule at the bar can untie -
	// each needs the ground the other stands on - so what is measured is WHERE it jams: at the
	// bars, with the runway free for landings and departures, not with one of them parked on the
	// centreline holding it. The jam itself is then the resolver's and the player's (an
	// all-aircraft deadlock is a layout alert); what must never happen is the runway going with it.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FCrossingFixture Crossing = FCrossingFixture::Build(*Net, /*bFarBar=*/true);
	const FTrafficResource Strip = FTrafficResource::OfSurface(Crossing.Strip);

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const FAirframe Airframe = TestAirframes::GroundOnly();
	CrossingExitHoldStrip(*Traffic, Crossing.Strip);

	const int32 A = Traffic->DispatchAgent(Net, TestGraph::Probe(*Net, Crossing.S, Crossing.N, ETraversalClass::Aircraft),
		Airframe, ETraversalClass::Aircraft, 1.0);
	const int32 B = Traffic->DispatchAgent(Net, TestGraph::Probe(*Net, Crossing.N, Crossing.S, ETraversalClass::Aircraft),
		Airframe, ETraversalClass::Aircraft, 1.0);
	if (!TestTrue(TEXT("both dispatched"), A > 0 && B > 0)) { return false; }

	// BOTH REACH THEIR BARS AND STOP - the runway is held.
	double StoppedFor = 0.0;
	TickUntil(*Traffic, *Net, 60.0, [&](int32)
	{
		const FRoadAgent* QA = Traffic->FindAgent(A);
		const FRoadAgent* QB = Traffic->FindAgent(B);
		const bool bBoth = QA->Follower.Speed < 1.0 && QA->Follower.Travelled > 15000.0
			&& QB->Follower.Speed < 1.0 && QB->Follower.Travelled > 15000.0;
		StoppedFor = bBoth ? StoppedFor + 0.05 : 0.0;
		return StoppedFor < 1.0;
	});
	if (!TestTrue(TEXT("both are holding short while the runway is held"), StoppedFor >= 1.0)) { return false; }

	// THE RUNWAY FREES. Spied from here on: the hold at the bar has to say why, once.
	FLogLineSpy Spy(TEXT("LogAirsideTraffic"));
	GLog->AddOutputDevice(&Spy);
	Traffic->OccupancyForTest().ReleaseAll(CrossingExitRunwayHolder);

	bool bEverArmed = false;
	double StoodOnStrip = 0.0;
	TickUntil(*Traffic, *Net, 30.0, [&](int32)
	{
		for (const int32 Id : { A, B })
		{
			bEverArmed = bEverArmed || Traffic->FindAgent(Id)->GetCrossingRunway().IsSet();
			CrossingExitAccrueStoodOnStrip(*Traffic, *Net, Id, Crossing.Strip, 0.05, StoodOnStrip);
		}
		return true;
	});
	GLog->RemoveOutputDevice(&Spy);

	int32 ExitLines = 0;
	FString FirstExitLine;
	for (const FString& Line : Spy.CapturedLines)
	{
		if (Line.Contains(TEXT("crossing exit")))
		{
			FirstExitLine = ExitLines == 0 ? Line : FirstExitLine;
			++ExitLines;
		}
	}
	const FRoadAgent* QA = Traffic->FindAgent(A);
	const FRoadAgent* QB = Traffic->FindAgent(B);
	UE_LOG(LogAirsideTests, Log,
		TEXT("CrossingHeadOnKeepsRunwayFree measured: A at %.0f uu, B at %.0f uu (bars at 17000); armed a crossing: %s; ")
		TEXT("stood still on the strip %.1f s; strip held: %s; %d exit-hold line(s), first: '%s'"),
		QA->Follower.Travelled, QB->Follower.Travelled, bEverArmed ? TEXT("yes") : TEXT("no"), StoodOnStrip,
		Traffic->GetOccupancy().IsHeld(Strip, 0) ? TEXT("yes") : TEXT("no"), ExitLines, *FirstExitLine);

	TestFalse(TEXT("neither aircraft was released onto the strip: each one's exit is the other's bar"), bEverArmed);
	TestTrue(FString::Printf(TEXT("nobody stood still on the runway (%.1f s)"), StoodOnStrip), StoodOnStrip < 0.5);
	TestFalse(TEXT("so the runway is FREE - a landing may be cleared past the jam"), Traffic->GetOccupancy().IsHeld(Strip, 0));
	TestTrue(TEXT("both still hold short at their bars, noses on the lines (centre half a footprint short of 17000)"),
		FMath::Abs(QA->Follower.Travelled - 16500.0) < 100.0 && FMath::Abs(QB->Follower.Travelled - 16500.0) < 100.0);
	// ON THE TRANSITION ONLY, one per waiter - not one per tick for 30 s.
	TestTrue(FString::Printf(TEXT("the hold at the bar names the held exit, once per waiter (%d line(s))"), ExitLines),
		ExitLines >= 1 && ExitLines <= 2);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
