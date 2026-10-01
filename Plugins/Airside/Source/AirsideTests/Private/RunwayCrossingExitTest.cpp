#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "AirsideTestsLog.h"
#include "Misc/AutomationTest.h"
#include "Model/TrafficClaims.h"
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
	// THE BODY CENTRE, NOT Follower.Travelled (#449): route distance is the nose gear's, and the claims are measured from the centre FClaimPass::CentreOf puts BodyCentreX - SteerAxleX from it - 316 uu aft on the Meridian. The default airframe was a hand copy with no footprint until #449, so the two coincided and this read one for the other.
	TestTrue(FString::Printf(TEXT("both still hold short at their bars, noses on the lines (centre half a footprint short of 17000: %.0f, %.0f)"),
			FClaimPass::CentreOf(*QA), FClaimPass::CentreOf(*QB)),
		FMath::Abs(FClaimPass::CentreOf(*QA) - 16500.0) < 100.0 && FMath::Abs(FClaimPass::CentreOf(*QB) - 16500.0) < 100.0);
	// ON THE TRANSITION ONLY, one per waiter - not one per tick for 30 s.
	TestTrue(FString::Printf(TEXT("the hold at the bar names the held exit, once per waiter (%d line(s))"), ExitLines),
		ExitLines >= 1 && ExitLines <= 2);
	return true;
}

// ---------------------------------------------------------------------------------------
namespace
{
	/** What one run of the off-the-cycle head-on ended with. */
	struct FCrossingExitHeadOnOutcome
	{
		bool bStaged = false;
		bool bAParked = false;
		bool bBParked = false;
		int32 Lines = 0;
		int32 LastResolved = 0;
		int32 A = 0;
		int32 B = 0;
	};

	/**
	 * THE PIE PING-PONG'S SHAPE (2026-09-30): a crosser A on the strip, a waiter B on the far bar,
	 * and each needing what the other holds. Staged with A ALREADY ON THE STRIP, as a just-vacated
	 * arrival is (HeadOnReplansRoundBarHolder stages it the same way): the exit rule keeps a
	 * crossing that STARTS at a bar from being released into this, but an aircraft that vacates
	 * onto a crossing never passed one.
	 *
	 *        N1                      B (dispatched first): N1 -> J -> F1 -> X1 -> H1 -> S1
	 *         |                      A: Z -> X1 -> F1 -> J -> N1, from the strip
	 *         J  (0, 3800)  <---- inside B's body; the BAIT X1 -> Q -> J reaches it round the side
	 *         |  \
	 *        F1 --\---------- F2    bars (y = 3000)
	 *   Z -- X1 ---- Q ====== X2 =  X1, Q and Z at y = CrossY, on the strip (half width 2250)
	 *        H1               H2    bars (y = -3000)
	 *         |               |
	 *        S1 ------------- S2
	 *
	 * B's WAY ROUND: F1 -> F2 -> X2 -> H2 -> S2 -> S1, touching nothing A stands on. At CrossY 0
	 * A's refusal at F1 stops it inside X1 -> F1, where it cannot turn, so B is the one asked.
	 *
	 * A SECOND VARIANT WAS TRIED AND DROPPED (2026-09-30): CrossY 2000 makes A a candidate, and its
	 * turn X1 -> Q -> J ends on B's body - the shape a "refuse replans that run into the cycle"
	 * rule was written for. It passed with and without that rule (A's tail is off the strip long
	 * before J, so B crosses and J is free), i.e. it measured nothing, and the rule went with it.
	 */
	FCrossingExitHeadOnOutcome CrossingExitHeadOnOffTheCycle(UObject* Outer)
	{
		// ONE VALUE, no longer a parameter: the second variant that passed a different one was dropped
		// (see above), and a parameter nothing varies reads as a knob someone is meant to turn.
		constexpr double CrossY = 0.0;
		FCrossingExitHeadOnOutcome Out;
		URoadNetwork* Net = NewObject<URoadNetwork>(Outer);
		URoadProfile* Runway = TestProfiles::Runway();
		const FRoadNodeId RA = Net->AddNode(FVector2D(-50000.0, 0.0));
		const FRoadNodeId RB = Net->AddNode(FVector2D(50000.0, 0.0));
		const FRoadSegmentId Strip = Net->AddStraightSegment(RA, RB, Runway);

		const FGuidelineNodeId S1 = TestGraph::Node(*Net, 0.0, -20000.0);
		const FGuidelineNodeId H1 = TestGraph::Node(*Net, 0.0, -3000.0);
		const FGuidelineNodeId Z = TestGraph::Node(*Net, -4000.0, CrossY);
		const FGuidelineNodeId X1 = TestGraph::Node(*Net, 0.0, CrossY);
		const FGuidelineNodeId F1 = TestGraph::Node(*Net, 0.0, 3000.0);
		const FGuidelineNodeId J = TestGraph::Node(*Net, 0.0, 3800.0);
		const FGuidelineNodeId N1 = TestGraph::Node(*Net, 0.0, 20000.0);
		const FGuidelineNodeId Q = TestGraph::Node(*Net, 6000.0, CrossY);
		const FGuidelineNodeId F2 = TestGraph::Node(*Net, 20000.0, 3000.0);
		const FGuidelineNodeId X2 = TestGraph::Node(*Net, 20000.0, 0.0);
		const FGuidelineNodeId H2 = TestGraph::Node(*Net, 20000.0, -3000.0);
		const FGuidelineNodeId S2 = TestGraph::Node(*Net, 20000.0, -20000.0);
		TestGraph::Join(*Net, S1, H1);
		TestGraph::Join(*Net, H1, X1);
		TestGraph::Join(*Net, Z, X1);
		TestGraph::Join(*Net, X1, F1);
		TestGraph::Join(*Net, F1, J);
		TestGraph::Join(*Net, J, N1);
		TestGraph::Join(*Net, X1, Q);
		TestGraph::Join(*Net, Q, J);
		TestGraph::Join(*Net, F1, F2);
		TestGraph::Join(*Net, F2, X2);
		TestGraph::Join(*Net, X2, H2);
		TestGraph::Join(*Net, H2, S2);
		TestGraph::Join(*Net, S2, S1);
		for (const FGuidelineNodeId Bar : { H1, F1, H2, F2 })
		{
			Net->SetRunwayHoldingPositionForTest(Bar, Strip);
		}

		UGroundTraffic* Traffic = NewObject<UGroundTraffic>(Outer);
		const FAirframe Airframe = TestAirframes::GroundOnly();
		CrossingExitHoldStrip(*Traffic, Strip);
		Out.B = Traffic->DispatchAgent(Net, TestGraph::Probe(*Net, N1, S1, ETraversalClass::Aircraft),
			Airframe, ETraversalClass::Aircraft, 1.0);
		if (Out.B <= 0) { return Out; }

		// B REACHES F1 AND HOLDS SHORT - the runway is held.
		double StoppedFor = 0.0;
		TickUntil(*Traffic, *Net, 60.0, [&](int32)
		{
			const FRoadAgent* QB = Traffic->FindAgent(Out.B);
			StoppedFor = (QB->Follower.Speed < 1.0 && QB->Follower.Travelled > 15000.0) ? StoppedFor + 0.05 : 0.0;
			return StoppedFor < 1.0;
		});
		if (StoppedFor < 1.0) { return Out; }

		// THE LANDING'S HANDOVER: the runway's holder becomes A, standing on the centreline at Z.
		Traffic->OccupancyForTest().ReleaseAll(CrossingExitRunwayHolder);
		Out.A = Traffic->DispatchAgent(Net, TestGraph::Probe(*Net, Z, N1, ETraversalClass::Aircraft),
			Airframe, ETraversalClass::Aircraft, 1.0);
		if (Out.A <= 0 || !FGroundTrafficTestAccess(*Traffic).BeginCrossing(Out.A, Strip)) { return Out; }
		Out.bStaged = true;

		const int32 LinesBefore = Traffic->GetDeadlockLogLinesForTest();
		TickUntil(*Traffic, *Net, 400.0, [&](int32)
		{
			return Traffic->FindAgent(Out.A)->Phase != EAgentPhase::Parked || Traffic->FindAgent(Out.B)->Phase != EAgentPhase::Parked;
		});

		const FRoadAgent* QA = Traffic->FindAgent(Out.A);
		const FRoadAgent* QB = Traffic->FindAgent(Out.B);
		Out.bAParked = QA->Phase == EAgentPhase::Parked;
		Out.bBParked = QB->Phase == EAgentPhase::Parked;
		Out.Lines = Traffic->GetDeadlockLogLinesForTest() - LinesBefore;
		Out.LastResolved = Traffic->GetLastResolvedAgentForTest();
		UE_LOG(LogAirsideTests, Log,
			TEXT("CrossingHeadOnReplansOffTheCycle (cross at y %.0f) measured: A %s at %.0f uu of %.0f, B %s at %.0f uu of %.0f; ")
			TEXT("deadlock lines %d, last resolved agent %d (A=%d, B=%d)"),
			CrossY, Out.bAParked ? TEXT("parked") : TEXT("NOT parked"), QA->Follower.Travelled, QA->Follower.Plan.Length,
			Out.bBParked ? TEXT("parked") : TEXT("NOT parked"), QB->Follower.Travelled, QB->Follower.Plan.Length,
			Out.Lines, Out.LastResolved, Out.A, Out.B);
		return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficCrossingHeadOnReplansOffTheCycleTest,
	"Airside.Model.Traffic.CrossingHeadOnReplansOffTheCycle",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficCrossingHeadOnReplansOffTheCycleTest::RunTest(const FString& Parameters)
{
	// THE WAITER TURNING AWAY FROM ITS BAR. A cannot turn here (it stops inside X1 -> F1), so B is
	// the only candidate, and its way round leaves F1 eastward - away from the runway. The bar at
	// F1 still reserved the runway for any route through the node, so B's replan was held at the
	// very bar it was turning away from, the cycle re-formed, B was replanned BACK onto the
	// crossing, and so on every retry window: measured before the fix, 76 deadlock lines in
	// 400 s, B replanned back and forth between its two routes, neither aircraft reaching its goal -
	// the PIE log's own alternation, and its flapping alert.
	const FCrossingExitHeadOnOutcome Out = CrossingExitHeadOnOffTheCycle(GetTransientPackage());
	if (!TestTrue(TEXT("staged: B holding at F1, A crossing at Z"), Out.bStaged)) { return false; }
	TestTrue(TEXT("A left the strip and reached N1 once B had gone"), Out.bAParked);
	TestTrue(TEXT("B went round by crossing 2 and reached S1"), Out.bBParked);
	TestEqual(TEXT("ONE resolution: a replan held at the bar it turns away from re-forms the cycle and logs again"),
		Out.Lines, 1);
	TestEqual(TEXT("and it was B's - A's only way off the strip runs through B's body"), Out.LastResolved, Out.B);
	return true;
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficAsymmetricBarToBarHoldsTest,
	"Airside.Model.Traffic.AsymmetricBarToBarHolds",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficAsymmetricBarToBarHoldsTest::RunTest(const FString& Parameters)
{
	// A HAND-DRAWN BAR-TO-BAR EDGE WHOSE MIDDLE IS OFF THE STRIP (review of dedf049f): the near bar
	// 3000 uu from the centreline, the far one 12000, so the edge's two vertices AND its midpoint
	// (4500) all miss a strip 2250 uu half wide. A "does the step lead onto the strip" test that
	// reads only those three points calls the near bar an exit bar: no runway reservation at it,
	// no exit chain - and the agent drives up to the asphalt with a landing on it, where only the
	// nose test arms the crossing, on the runway. The step's LINE crosses the strip; that is what
	// must be asked, by the same predicate UpdateCrossing arms with.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Runway = TestProfiles::Runway();
	const FRoadNodeId RA = Net->AddNode(FVector2D(-50000.0, 0.0));
	const FRoadNodeId RB = Net->AddNode(FVector2D(50000.0, 0.0));
	const FRoadSegmentId Strip = Net->AddStraightSegment(RA, RB, Runway);
	const FGuidelineNodeId S = TestGraph::Node(*Net, 0.0, -20000.0);
	const FGuidelineNodeId Hn = TestGraph::Node(*Net, 0.0, -3000.0);
	const FGuidelineNodeId Hf = TestGraph::Node(*Net, 0.0, 12000.0);
	const FGuidelineNodeId N = TestGraph::Node(*Net, 0.0, 30000.0);
	TestGraph::Join(*Net, S, Hn);
	TestGraph::Join(*Net, Hn, Hf);   // ONE edge across the runway; vertices and midpoint all off it.
	TestGraph::Join(*Net, Hf, N);
	Net->SetRunwayHoldingPositionForTest(Hn, Strip);
	Net->SetRunwayHoldingPositionForTest(Hf, Strip);

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	CrossingExitHoldStrip(*Traffic, Strip);
	const int32 Plane = Traffic->DispatchAgent(Net, TestGraph::Probe(*Net, S, N, ETraversalClass::Aircraft),
		TestAirframes::GroundOnly(), ETraversalClass::Aircraft, 1.0);
	if (!TestTrue(TEXT("dispatched"), Plane > 0)) { return false; }

	// THE RUNWAY IS HELD (a landing). The aircraft must stop with its nose on the near bar.
	bool bArmedWhileHeld = false;
	TickUntil(*Traffic, *Net, 60.0, [&](int32)
	{
		bArmedWhileHeld = bArmedWhileHeld || Traffic->FindAgent(Plane)->GetCrossingPhase() != ECrossingPhase::None;
		return true;
	});
	// THE BODY CENTRE, NOT Follower.Travelled (#449): route distance is the nose gear's, and the claims are measured from the centre FClaimPass::CentreOf puts BodyCentreX - SteerAxleX from it - 316 uu aft on the Meridian. The default airframe was a hand copy with no footprint until #449, so the two coincided and this read one for the other.
	const double HeldAt = FClaimPass::CentreOf(*Traffic->FindAgent(Plane));
	const int32 WaitingOn = Traffic->FindAgent(Plane)->GetWaitingOn();

	Traffic->OccupancyForTest().ReleaseAll(CrossingExitRunwayHolder);
	TickUntil(*Traffic, *Net, 120.0, [&](int32) { return Traffic->FindAgent(Plane)->Phase != EAgentPhase::Parked; });
	const bool bParked = Traffic->FindAgent(Plane)->Phase == EAgentPhase::Parked;

	UE_LOG(LogAirsideTests, Log,
		TEXT("AsymmetricBarToBarHolds measured: held at %.0f uu (nose on the near bar = 16500) waiting on %d; ")
		TEXT("armed a crossing while the runway was held: %s; parked after release: %s"),
		HeldAt, WaitingOn, bArmedWhileHeld ? TEXT("yes") : TEXT("no"), bParked ? TEXT("yes") : TEXT("no"));

	TestTrue(FString::Printf(TEXT("held with its nose on the near bar, off the runway (%.0f, want 16500)"), HeldAt),
		FMath::Abs(HeldAt - 16500.0) < 100.0);
	TestEqual(TEXT("held for the runway's holder - the bar reserved the strip"), WaitingOn, CrossingExitRunwayHolder);
	TestFalse(TEXT("and never armed a crossing onto a runway somebody else holds"), bArmedWhileHeld);
	TestTrue(TEXT("once the runway frees it crosses and parks"), bParked);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
