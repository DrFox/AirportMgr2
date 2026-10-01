#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/ArrivalPlanner.h"
#include "Model/DeparturePlanner.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadAgent.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteSearch.h"
#include "Model/TrafficOccupancy.h"
#include "Testing/AirsideTestGraph.h"
#include "Testing/AirsideTestWorld.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * OPS BATCH 3 PR E, the Airside half: a departure holding for a way to the runway used to plan one EVERY SUBSTEP
 * for as long as it held (FindNearestNode + DeparturePlanner::PlanAny), and a stand's held-or-not changed through
 * the claim pass with no counter a poller could key on. Both are counted here.
 */
namespace HeldTaxiOutGateTest
{
	/**
	 * PushbackDepartTest's field (a runway along y 0, a junction J with a far arm E, a stand A south of it),
	 * built again rather than reached across the unity build into that file's anonymous namespace.
	 */
	struct FGateField
	{
		URoadNetwork* Net = nullptr;
		FRoadSegmentId Runway;
		FGuidelineNodeId A, B, C, J, E;
	};

	FGateField Build()
	{
		FGateField F;
		F.Net = NewObject<URoadNetwork>(GetTransientPackage());
		URoadNetwork& Net = *F.Net;
		URoadProfile* Runway = TestProfiles::Runway();
		const FRoadNodeId RA = Net.AddNode(FVector2D(-50000.0, 0.0));
		const FRoadNodeId RM = Net.AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId RB = Net.AddNode(FVector2D(50000.0, 0.0));
		F.Runway = Net.AddStraightSegment(RA, RM, Runway);
		Net.AddStraightSegment(RM, RB, Runway);

		F.B = TestGraph::Node(Net, 0.0, 0.0);
		F.J = TestGraph::Node(Net, 0.0, -10000.0);
		F.A = TestGraph::Node(Net, 0.0, -20000.0);
		F.C = TestGraph::Node(Net, 0.0, -40000.0);
		F.E = TestGraph::Node(Net, 20000.0, -10000.0);
		TestGraph::FJoinOptions Options;
		Options.bDerived = false;
		TestGraph::Join(Net, F.A, F.J, Options);
		TestGraph::Join(Net, F.J, F.B, Options);
		TestGraph::Join(Net, F.J, F.E, Options);
		TestGraph::Join(Net, F.C, F.A, Options);
		return F;
	}

	/**
	 * One aeroplane holding for a way to the runway, with no line to the runway at all: parked facing south (taxied
	 * in from B), departed by pushing back onto E, taxiing out E -> J -> B, then - once past J - its route stranded
	 * and B, the only line onto the runway, deleted. A GRAPH EDIT, not a facts write: the refusal is a genuine NoRoute
	 * and the fix below is a genuine line drawn, so the test moves only what the gate reads (the review's I1 - an
	 * earlier version set the runway's mode straight onto URoadNetwork, the one write that moves no revision).
	 * Returns the agent id, or 0.
	 */
	int32 HoldWithNoWayOut(const FGateField& F, UGroundTraffic& Traffic)
	{
		const int32 Id = Traffic.DispatchAgent(F.Net, TestGraph::Probe(*F.Net, F.B, F.A, ETraversalClass::Aircraft),
			TestAirframes::Piper(), ETraversalClass::Aircraft, /*ShutdownPauseSeconds*/ 0.0);
		if (Id <= 0)
		{
			return 0;
		}
		for (int32 I = 0; I < 20000 && Traffic.FindAgent(Id)->Phase != EAgentPhase::Parked; ++I) { Traffic.Advance(1.0 / 30.0, F.Net); }
		if (Traffic.DepartAgent(Id, *F.Net) != EDepartureRefusal::None)
		{
			return 0;
		}
		auto PastJ = [&]()
		{
			const FRoadAgent* Agent = Traffic.FindAgent(Id);
			return Agent != nullptr && Agent->Phase == EAgentPhase::Taxiing && Agent->LastMotion.Position.X < 1000.0
				&& Agent->LastMotion.Position.Y > -9000.0;
		};
		for (int32 Tick = 0; Tick < 30 * 300 && !PastJ(); ++Tick) { Traffic.Advance(1.0 / 30.0, F.Net); }
		if (!PastJ() || !FGroundTrafficTestAccess(Traffic).Strand(Id))
		{
			return 0;
		}
		F.Net->RemoveGuidelineNode(F.B);
		// Into the hold: the stranded route ends, the taxi-complete guard holds, and the first replan is refused.
		for (int32 Tick = 0; Tick < 5; ++Tick) { Traffic.Advance(1.0 / 30.0, F.Net); }
		const FRoadAgent* Agent = Traffic.FindAgent(Id);
		return Agent != nullptr && Agent->IsHoldingForTaxiOut() ? Id : 0;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHeldTaxiOutAsksOncePerEditTest, "Airside.Model.Traffic.HeldTaxiOut.AsksOncePerEdit",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FHeldTaxiOutAsksOncePerEditTest::RunTest(const FString&)
{
	using namespace HeldTaxiOutGateTest;
	const FGateField F = Build();
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Before = Traffic->TaxiOutReplanAttemptsForTest();
	const int32 Id = HoldWithNoWayOut(F, *Traffic);
	if (!TestTrue(TEXT("it holds for a way out, with no line to the runway"), Id > 0)) { return false; }
	const int32 Refused = Traffic->TaxiOutReplanAttemptsForTest() - Before;
	TestEqual(TEXT("asked once on entering the hold, and refused"), Refused, 1);

	for (int32 Tick = 0; Tick < 100; ++Tick) { Traffic->Advance(1.0 / 30.0, F.Net); }
	TestTrue(TEXT("still holding"), Traffic->FindAgent(Id) != nullptr && Traffic->FindAgent(Id)->IsHoldingForTaxiOut());
	TestEqual(TEXT("100 quiet substeps: it asked once, not every substep"), Traffic->TaxiOutReplanAttemptsForTest() - Before, 1);

	// THE PLAYER'S FIX: a line to the runway again. The guideline revision moves because the graph did - the input
	// the gate reads - and it is asked again on the next substep, once, and goes.
	TestGraph::FJoinOptions Options;
	Options.bDerived = false;
	TestGraph::Join(*F.Net, F.J, TestGraph::Node(*F.Net, 0.0, 0.0), Options);
	Traffic->Advance(1.0 / 30.0, F.Net);
	TestEqual(TEXT("the edit: asked again, once"), Traffic->TaxiOutReplanAttemptsForTest() - Before, 2);
	const FRoadAgent* Agent = Traffic->FindAgent(Id);
	TestTrue(TEXT("and it has a way out: no longer holding"), Agent != nullptr && !Agent->IsHoldingForTaxiOut());
	TestTrue(TEXT("it is taxiing"), Agent != nullptr && Agent->Phase == EAgentPhase::Taxiing);
	for (int32 Tick = 0; Tick < 10; ++Tick) { Traffic->Advance(1.0 / 30.0, F.Net); }
	TestEqual(TEXT("and asks nothing more once it goes"), Traffic->TaxiOutReplanAttemptsForTest() - Before, 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHeldTaxiOutNewNetworkTest, "Airside.Model.Traffic.HeldTaxiOut.ANewNetworkAsksAgain",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FHeldTaxiOutNewNetworkTest::RunTest(const FString&)
{
	// A NEW NETWORK OBJECT AT THE SAME REVISION NUMBER: two deterministic Build()s with the same edit made to each
	// count their guideline revisions identically, so a refusal remembered against the first matches the second by
	// number. Only the gate's network identity tells them apart - without it the hold would never be asked again.
	using namespace HeldTaxiOutGateTest;
	const FGateField First = Build();
	const FGateField Second = Build();
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Id = HoldWithNoWayOut(First, *Traffic);
	if (!TestTrue(TEXT("holding on the first network"), Id > 0)) { return false; }
	Second.Net->RemoveGuidelineNode(Second.B);
	if (!TestEqual(TEXT("the two networks stand at the same guideline revision - the case under test"),
		Second.Net->GetGuidelineRevision(), First.Net->GetGuidelineRevision())) { return false; }

	const int32 Before = Traffic->TaxiOutReplanAttemptsForTest();
	Traffic->Advance(1.0 / 30.0, First.Net);
	TestEqual(TEXT("the same network, nothing moved: not asked"), Traffic->TaxiOutReplanAttemptsForTest() - Before, 0);
	Traffic->Advance(1.0 / 30.0, Second.Net);
	TestEqual(TEXT("the other network, same number: asked again"), Traffic->TaxiOutReplanAttemptsForTest() - Before, 1);
	Traffic->Advance(1.0 / 30.0, Second.Net);
	TestEqual(TEXT("and then remembered against it"), Traffic->TaxiOutReplanAttemptsForTest() - Before, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHeldTaxiOutBusyRunwayTest, "Airside.Model.Traffic.HeldTaxiOut.BusyRunwayIsNoRefusal",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FHeldTaxiOutBusyRunwayTest::RunTest(const FString&)
{
	// WHY THE GATE KEYS ON THE GRAPH ALONE (PR E, 2026-09-30): a held runway RANKS a departure's choice of runway, it
	// never refuses one - so a hold can never be waiting on a runway to free, and no runway-freed signal is an input of its
	// failure. If PlanAny ever starts refusing a busy strip, this goes red, and the gate must take the counter.
	using namespace HeldTaxiOutGateTest;
	const FGateField F = Build();
	FTrafficOccupancy Occupancy;
	for (const FTrafficResource& Surface : F.Net->RunwaySurfaces(F.Runway))
	{
		Occupancy.Assert(FTrafficClaim::Make(99, Surface, /*bOccupied*/ true, 2));
	}
	if (!TestTrue(TEXT("the whole strip is held"), Occupancy.IsAnyHeld(F.Net->RunwaySurfaces(F.Runway), 0, false))) { return false; }
	const FDeparturePlan Plan = DeparturePlanner::PlanAny(*F.Net, F.J, TestAirframes::Piper(), ETraversalClass::Aircraft, &Occupancy);
	TestTrue(FString::Printf(TEXT("a held runway is still planned to: %s"), *DeparturePlanner::Describe(Plan)), Plan.IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStandHoldsChurnTest, "Airside.Model.Traffic.StandHolds.ChurnIsCounted",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FStandHoldsChurnTest::RunTest(const FString&)
{
	// A BODY ON A STAND, put there and taken off by the claim pass - the one change to a stand's holder no revision
	// sees (PR D: "a stand freed by claim churn moves no revision"). The inspector's stand card keys on this count.
	const FTestAirport Field = FTestAirport::Build(TestAirframes::Piper());
	URoadNetwork* Net = Field.Net;
	const FGuidelineNodeId Pose = Net->GetEntity(Field.Stands[0])->PoseNode;
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	Traffic->Advance(0.05, Net);
	const uint32 Changes = Traffic->StandHoldChangeCount();
	const uint32 Revision = Traffic->OccupancyRevision();

	Traffic->OccupancyForTest().Assert(FTrafficClaim::Make(77, FTrafficResource::OfNode(Pose), /*bOccupied*/ true, 2));
	Traffic->Advance(0.05, Net);
	TestEqual(TEXT("a body arrives on the stand: counted"), Traffic->StandHoldChangeCount() - Changes, 1u);
	Traffic->Advance(0.05, Net);
	TestEqual(TEXT("and it standing there is no further change"), Traffic->StandHoldChangeCount() - Changes, 1u);
	Traffic->OccupancyForTest().ReleaseAll(77);
	Traffic->Advance(0.05, Net);
	TestEqual(TEXT("it leaves: counted"), Traffic->StandHoldChangeCount() - Changes, 2u);
	TestEqual(TEXT("and neither moved OccupancyRevision - which is why the count exists"), Traffic->OccupancyRevision(), Revision);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHeldTaxiOutRestartKeepsPoseAndEngineTest, "Airside.Model.Traffic.HeldTaxiOut.RestartKeepsPoseAndEngine",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FHeldTaxiOutRestartKeepsPoseAndEngineTest::RunTest(const FString&)
{
	// ISSUE #429: A TAXIING AEROPLANE THAT HELD FOR A WAY OUT RESTARTS THROUGH THE ROUTE-CHANGE SEAM, where it used to
	// call FRoadAgent::ResumeTaxiOut - RestartTaxi with its own heading, and its own copy of "keep the spool". Its
	// steps survive the move: from rest WHERE it holds, FACING the way it holds, its engine carried on rather than
	// started cold, and the goal moved to the new runway entry. One substep after the player's fix, so the agent has
	// had exactly one substep of motion from rest - a pose from anywhere else, or a propeller wound back to zero,
	// shows as more than a substep's worth.
	using namespace HeldTaxiOutGateTest;
	const FGateField F = Build();
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Id = HoldWithNoWayOut(F, *Traffic);
	if (!TestTrue(TEXT("it holds for a way out"), Id > 0)) { return false; }
	const FRoadAgent* Held = Traffic->FindAgent(Id);
	const FVector2D HeldAt = Held->LastMotion.Position;
	const double HeldHeading = Held->LastMotion.Heading;
	const FGuidelineNodeId HeldGoal = Held->GoalNode;
	if (!TestTrue(TEXT("precondition: its engine is running at speed while it holds"),
		Held->bEngineRunning && Held->GetEngineRPM() > 0.0)) { return false; }
	const double HeldRPM = Held->GetEngineRPM();

	TestGraph::FJoinOptions Options;
	Options.bDerived = false;
	TestGraph::Join(*F.Net, F.J, TestGraph::Node(*F.Net, 0.0, 0.0), Options);
	Traffic->Advance(1.0 / 30.0, F.Net);

	const FRoadAgent* Agent = Traffic->FindAgent(Id);
	if (!TestTrue(TEXT("it restarted: taxiing and no longer holding"),
		Agent != nullptr && Agent->Phase == EAgentPhase::Taxiing && !Agent->IsHoldingForTaxiOut())) { return false; }
	TestTrue(FString::Printf(TEXT("from where it held (%.1f uu away after one substep from rest)"),
		FVector2D::Distance(Agent->LastMotion.Position, HeldAt)),
		FVector2D::Distance(Agent->LastMotion.Position, HeldAt) < 20.0);
	TestTrue(FString::Printf(TEXT("facing the way it held (%.1f deg off)"),
		FMath::RadiansToDegrees(FMath::Abs(FMath::UnwindRadians(Agent->LastMotion.Heading - HeldHeading)))),
		FMath::Abs(FMath::UnwindRadians(Agent->LastMotion.Heading - HeldHeading)) < FMath::DegreesToRadians(5.0));
	TestTrue(FString::Printf(TEXT("its engine carried on, not started cold (%.0f RPM, was %.0f)"), Agent->GetEngineRPM(), HeldRPM),
		Agent->bEngineRunning && Agent->GetEngineRPM() >= HeldRPM - 1.0);
	TestTrue(TEXT("and its goal moved to the runway entry the new route ends at"),
		Agent->GoalNode.IsSet() && Agent->GoalNode != HeldGoal
		&& Agent->GoalNode == Agent->Follower.Plan.Steps.Last().To);
	return true;
}

/**
 * A HELD DEPARTURE IS NOT SENT TO A STAND BY A REBUILD (issue #444, #496's review). A taxi out that cannot be driven holds
 * DISARMED, and FPlanReResolver::ReResolvePlan's "the goal was a stand and the stand is gone" branch asked only
 * `!bDepartureArmed` - so a later rebuild that lost the end of the plan it held on took the departure for a taxi-in:
 * retargeted to a free stand, or armed to wait for one beside its taxi-out wait (the double wait two flags allowed). The
 * branch now skips an agent waiting for a taxi-out route; this drives it there for real.
 *
 * THE TAXI-COMPLETE GUARD'S HOLD, because its plan stays LIVE while it holds: a rebuild that loses the runway entry ahead of
 * a taxiing departure, with no way round, TRUNCATES its taxi out, and where the truncated route ends - far from the entry -
 * the guard disarms it and it holds, refused. (A hold on a STRANDED plan is skipped by the rebuild as not valid and never
 * reaches the branch.) Then the plan's own end node goes, with nothing within ResolveRadius of it and a free stand
 * reachable from the node before it - every condition of the branch but the departure's own wait.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHeldTaxiOutNotRetargetedTest, "Airside.Model.Traffic.HeldTaxiOut.RebuildDoesNotRetargetItToAStand",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FHeldTaxiOutNotRetargetedTest::RunTest(const FString&)
{
	const FAirframe Piper = TestAirframes::Piper();
	const FTestAirport Air = FTestAirport::Build(Piper, { .StandCount = 2 });
	URoadNetwork* Net = Air.Net;
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());

	// AN AEROPLANE ON A STAND, then sent off for the runway - and taxiing out, its push done.
	const int32 Id = Traffic->DispatchArrival(*Net, Air.Threshold - FVector2D(1000.0, 0.0), Piper, 0.0);
	if (!TestTrue(TEXT("an arrival is admitted"), Id > 0)) { return false; }
	if (!TestTrue(TEXT("and parks"), RunUntil(*Traffic, *Net, 900.0, [&]()
		{
			const FRoadAgent* A = Traffic->FindAgent(Id);
			return A != nullptr && A->Phase == EAgentPhase::Parked;
		}))) { return false; }
	if (!TestEqual(TEXT("it departs"), Traffic->DepartAgent(Id, *Net), EDepartureRefusal::None)) { return false; }
	if (!TestTrue(TEXT("and is taxiing out, armed"), RunUntil(*Traffic, *Net, 300.0, [&]()
		{
			const FRoadAgent* A = Traffic->FindAgent(Id);
			return A != nullptr && A->Phase == EAgentPhase::Taxiing && A->bDepartureArmed;
		}, 1.0 / 30.0))) { return false; }

	// THE RUNWAY'S LINES GO - every guideline node within 30 m of the centreline, the entry among them - while the taxi out
	// is still a step short of them: the rebuild cannot route round, and truncates the taxi out where the taxiway ends.
	TArray<FGuidelineNodeId> OnRunway;
	for (int32 Index = 0; Index < Net->GetGuidelineNodes().Num(); ++Index)
	{
		const FGuidelineNode& Node = Net->GetGuidelineNodes()[Index];
		if (Node.bAlive && FMath::Abs(Node.Position.Y) <= 3000.0)
		{
			OnRunway.Add(Net->GuidelineNodeIdAt(Index));
		}
	}
	for (const FGuidelineNodeId Node : OnRunway) { Net->RemoveGuidelineNode(Node); }
	Traffic->OnGraphRebuilt(*Net);

	// THE REFUSED HOLD: it taxis to where its route now ends, far from the entry; the guard disarms it, it holds, and the
	// retry finds no way to a runway from there.
	const int32 AskedBefore = Traffic->TaxiOutReplanAttemptsForTest();
	if (!TestTrue(TEXT("it reaches the truncated end and holds"), RunUntil(*Traffic, *Net, 300.0, [&]()
		{
			const FRoadAgent* A = Traffic->FindAgent(Id);
			return A != nullptr && A->IsHoldingForTaxiOut();
		}, 1.0 / 30.0))) { return false; }
	const FRoadAgent* Held = Traffic->FindAgent(Id);
	if (!TestTrue(TEXT("disarmed, its plan still live, and refused: asked, and the hold's line said"),
		!Held->bDepartureArmed && Held->Follower.Plan.IsValid() && Held->Follower.Plan.Steps.Num() > 0
		&& Traffic->TaxiOutReplanAttemptsForTest() > AskedBefore && Held->HasSaidWait())) { return false; }

	// THE PLAN'S END NODE GOES, with no node within ResolveRadius of where it was - so the plan's goal no longer resolves -
	// and a FREE STAND reachable from the node before it: the retarget branch's every other condition, held true here so that
	// only the guard can be what keeps it out.
	const FRoutePlan& Plan = Held->Follower.Plan;
	const FGuidelineNodeId PlanEnd = Plan.Steps.Last().To;
	const FGuidelineNode* PlanEndNode = Net->GetGuidelineNode(PlanEnd);
	if (!TestNotNull(TEXT("the plan's end node is live"), PlanEndNode)) { return false; }
	const FVector2D PlanEndAt = PlanEndNode->Position;
	const FGuidelineNodeId Before = UGroundTraffic::StepFromNode(Plan, Plan.Steps.Num() - 1);
	if (!TestTrue(TEXT("a free stand is reachable from the node before it"),
		ArrivalPlanner::ChooseStand(*Net, Before, Piper, &Traffic->GetOccupancy(), Id).IsSet())) { return false; }
	TestTrue(TEXT("its end node is deleted"), Net->RemoveGuidelineNode(PlanEnd));
	if (!TestFalse(TEXT("and nothing is within ResolveRadius of where it was"),
		RouteSearch::FindNearestNode(*Net, PlanEndAt, Held->Class, Traffic->Rules.ResolveRadius).IsSet())) { return false; }

	FLogLineSpy Spy(FName(TEXT("LogAirsideTraffic")));
	GLog->AddOutputDevice(&Spy);
	Traffic->OnGraphRebuilt(*Net);
	GLog->RemoveOutputDevice(&Spy);

	const FRoadAgent* After = Traffic->FindAgent(Id);
	if (!TestNotNull(TEXT("it is still there"), After)) { return false; }
	TestFalse(TEXT("no retarget line: it was not taken for a taxi-in whose stand had gone"),
		Spy.CapturedLines.ContainsByPredicate([](const FString& L) { return L.Contains(TEXT("its stand is gone")); }));
	TestTrue(TEXT("it still waits for a way to the runway - not for a stand"), After->IsWaitingFor(EAgentWait::ForTaxiOutRoute));
	for (const FEntityInstanceId Stand : Air.Stands)
	{
		TestTrue(TEXT("and its goal is no stand's pose"), After->GoalNode != Air.Pose(Stand));
	}

	// THE PLAYER'S FIX: a line from where it holds to the runway. It is asked again, finds it, and is a departure again.
	const int32 AskedAtFix = Traffic->TaxiOutReplanAttemptsForTest();
	TestGraph::FJoinOptions Options;
	Options.bDerived = false;
	TestGraph::Join(*Net, TestGraph::Node(*Net, After->LastMotion.Position.X, After->LastMotion.Position.Y),
		TestGraph::Node(*Net, After->LastMotion.Position.X, 0.0), Options);
	Traffic->Advance(1.0 / 30.0, Net);
	const FRoadAgent* Fixed = Traffic->FindAgent(Id);
	if (!TestNotNull(TEXT("it is there after the fix"), Fixed)) { return false; }
	TestTrue(TEXT("asked again after the fix"), Traffic->TaxiOutReplanAttemptsForTest() > AskedAtFix);
	TestTrue(TEXT("and re-armed for the runway"), Fixed->bDepartureArmed && !Fixed->IsWaitingFor(EAgentWait::ForTaxiOutRoute));
	return true;
}

/**
 * A HELD TAXI OUT'S RESTART IS CLAIMED BEFORE IT MOVES (issue #444, #496's review). The taxi-out replan ran between the claim
 * pass and the motion until #444, and a restart clears the agent's arbitration (FRoadAgent::ApplyRouteChange), so the step it
 * was restarted in ran its motion on a route no claim pass had seen. Somebody stands on the first node of the new route; the
 * restarted aeroplane must not move toward it.
 *
 * NOT AN ORDER PIN, and that is a measured result, not a gap: with the replan put back before the motion (2026-10-01) this
 * still passes, because a restart is from rest (ChangeRoute's Restart, speed 0) and the step it is made in moves the aeroplane
 * 0 uu - the next step's Arbitrate refuses it either way. #444's end-of-step retry is the one pass's place, not a motion fix.
 * What it pins is the behaviour at the composition: a held taxi out restarted toward a node somebody stands on is refused
 * there, by that agent, and stays put.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHeldTaxiOutRestartClaimedFirstTest, "Airside.Model.Traffic.HeldTaxiOut.RestartIsClaimedBeforeItMoves",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FHeldTaxiOutRestartClaimedFirstTest::RunTest(const FString&)
{
	using namespace HeldTaxiOutGateTest;
	const FGateField F = Build();
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Id = HoldWithNoWayOut(F, *Traffic);
	if (!TestTrue(TEXT("it holds for a way out"), Id > 0)) { return false; }
	const FRoadAgent* Held = Traffic->FindAgent(Id);
	const FVector2D HeldAt = Held->LastMotion.Position;

	// THE FIX, AND ITS FIRST NODE TAKEN: RestartKeepsPoseAndEngine's line from J to the runway - the replan starts at J, the
	// node nearest where it holds - and agent 99 standing on J (a phantom, GroundTrafficTest's way of planting one).
	TestGraph::FJoinOptions Options;
	Options.bDerived = false;
	TestGraph::Join(*F.Net, F.J, TestGraph::Node(*F.Net, 0.0, 0.0), Options);
	const FGuidelineNodeId Taken = F.J;
	FTrafficClaim Sitting;
	Sitting.AgentId = 99;
	Sitting.Resource = FTrafficResource::OfNode(Taken);
	Sitting.bOccupied = true;
	FTrafficClaim Blocker;
	Traffic->OccupancyForTest().TryClaim(Sitting, Blocker);

	// TWO SUBSTEPS: the one the replan happens in, and the first it could move in under either order.
	const int32 AskedBefore = Traffic->TaxiOutReplanAttemptsForTest();
	Traffic->Advance(1.0 / 30.0, F.Net);
	Traffic->Advance(1.0 / 30.0, F.Net);
	const FRoadAgent* Agent = Traffic->FindAgent(Id);
	if (!TestTrue(TEXT("it was replanned onto the new line, starting at J"),
		Agent != nullptr && Traffic->TaxiOutReplanAttemptsForTest() > AskedBefore && !Agent->IsHoldingForTaxiOut()
		&& Agent->Follower.Plan.Start == Taken)) { return false; }
	TestEqual(TEXT("the claim pass saw the new route and refused it at J"), Agent->GetWaitingOn(), 99);
	const double Moved = FVector2D::Distance(Agent->LastMotion.Position, HeldAt);
	TestTrue(FString::Printf(TEXT("and it has not moved toward J - %.4f uu"), Moved),
		Moved < 0.001);
	return true;
}

/**
 * A DEPARTURE WHOSE RUNWAY ENTRY GOES MID-PUSH HOLDS WHERE THE PUSH ENDS (issue #498). The push's re-resolve points the goal
 * at the push's own end, and the taxi out's re-resolve, finding no node where the entry was, replanned to that goal: the taxi
 * out became a drive to the last live node and back to where the push ended ("taxi-in replanned by the rebuild at step 2:
 * 10800 uu" on this fixture), disarmed and with no wait, so the aeroplane drove out and back and stopped there, no departure
 * armed and nothing to retry it. The held taxi out's contract (#174, #444) is the answer: the push runs out, the aeroplane
 * holds there for a way to the runway, and a line drawn to one re-arms it.
 *
 * TWO PHASES, because the bug is made in one and shows in the other: the rebuild's answer (the taxi out marked to be planned
 * again, and not extended), then what the aeroplane does with it once the push has run out.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHeldTaxiOutMidPushRunwayLossTest, "Airside.Model.Traffic.HeldTaxiOut.MidPushRunwayLossHolds",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FHeldTaxiOutMidPushRunwayLossTest::RunTest(const FString&)
{
	const FAirframe Piper = TestAirframes::Piper();
	const FTestAirport Air = FTestAirport::Build(Piper, { .StandCount = 2 });
	URoadNetwork* Net = Air.Net;
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());

	// AN AEROPLANE ON A STAND, sent off for the runway, and one second into its push.
	const int32 Id = Traffic->DispatchArrival(*Net, Air.Threshold - FVector2D(1000.0, 0.0), Piper, 0.0);
	if (!TestTrue(TEXT("an arrival is admitted"), Id > 0)) { return false; }
	if (!TestTrue(TEXT("and parks"), RunUntil(*Traffic, *Net, 900.0, [&]()
		{
			const FRoadAgent* A = Traffic->FindAgent(Id);
			return A != nullptr && A->Phase == EAgentPhase::Parked;
		}))) { return false; }
	if (!TestEqual(TEXT("it departs"), Traffic->DepartAgent(Id, *Net), EDepartureRefusal::None)) { return false; }
	for (int32 Tick = 0; Tick < 30; ++Tick) { Traffic->Advance(1.0 / 30.0, Net); }
	const FRoadAgent* Pushing = Traffic->FindAgent(Id);
	if (!TestTrue(TEXT("mid-push, armed, a taxi out waiting at the push's end"),
		Pushing != nullptr && Pushing->Phase == EAgentPhase::Manoeuvring && !Pushing->Pushback.HasArrived()
		&& Pushing->bDepartureArmed && Pushing->TaxiOutPlan.Polyline.Num() > 1)) { return false; }
	const FVector2D PushEnd = Pushing->TaxiOutPlan.Polyline[0];
	const double TaxiOutWas = Pushing->TaxiOutPlan.Length;

	// THE RUNWAY'S LINES GO - every guideline node within 30 m of the centreline, the entry among them.
	TArray<FGuidelineNodeId> OnRunway;
	for (int32 Index = 0; Index < Net->GetGuidelineNodes().Num(); ++Index)
	{
		const FGuidelineNode& Node = Net->GetGuidelineNodes()[Index];
		if (Node.bAlive && FMath::Abs(Node.Position.Y) <= 3000.0)
		{
			OnRunway.Add(Net->GuidelineNodeIdAt(Index));
		}
	}
	for (const FGuidelineNodeId Node : OnRunway) { Net->RemoveGuidelineNode(Node); }
	FLogLineSpy Spy(FName(TEXT("LogAirsideTraffic")));
	GLog->AddOutputDevice(&Spy);
	Traffic->OnGraphRebuilt(*Net);
	GLog->RemoveOutputDevice(&Spy);

	// PHASE 1, THE REBUILD: the taxi out is to be planned again, and it is not a route back to where the push ends.
	const FRoadAgent* Rebuilt = Traffic->FindAgent(Id);
	if (!TestNotNull(TEXT("it is still there"), Rebuilt)) { return false; }
	TestTrue(TEXT("still pushing"), Rebuilt->Phase == EAgentPhase::Manoeuvring && !Rebuilt->Pushback.HasArrived());
	TestTrue(TEXT("its taxi out is marked to be planned again where the push ends"), Rebuilt->IsWaitingFor(EAgentWait::ForTaxiOutRoute));
	TestFalse(TEXT("no replan line: the taxi out was not re-routed to the goal the push's re-resolve left"),
		Spy.CapturedLines.ContainsByPredicate([](const FString& L) { return L.Contains(TEXT("replanned by the rebuild")); }));
	TestTrue(TEXT("and the rebuild's line names the taxi out, not a taxi-in"),
		Spy.CapturedLines.ContainsByPredicate([](const FString& L) { return L.Contains(TEXT("'s taxi-out truncated by the rebuild")); }));
	const double EndsFromPushEnd = FVector2D::Distance(Rebuilt->TaxiOutPlan.Polyline.Last(), PushEnd);
	TestTrue(FString::Printf(TEXT("its taxi out is not extended back to the push's end (ends %.0f uu from it, %.0f uu long, was %.0f)"),
		EndsFromPushEnd, Rebuilt->TaxiOutPlan.Length, TaxiOutWas),
		EndsFromPushEnd > 1000.0 && Rebuilt->TaxiOutPlan.Length <= TaxiOutWas + 1.0);

	// AND A SECOND REBUILD, NOWHERE NEAR IT, STILL MID-PUSH (#502 review). With its taxi out waiting to be planned again,
	// the push's own re-resolve is the only one this rebuild makes for it, and the agent's goal is not the push's to move:
	// it is the departure's (DepartAgent). #501's push arm pointed it at the push's end by hand, and a push that wrote the
	// goal (ReResolvePlan's bOwnsGoal true for it) did the same - a held departure's card then named a node, not its runway.
	const FGuidelineNodeId GoalBefore = Rebuilt->GoalNode;
	TestGraph::Node(*Net, -90000.0, -90000.0);
	Traffic->OnGraphRebuilt(*Net);
	const FRoadAgent* Again = Traffic->FindAgent(Id);
	if (!TestTrue(TEXT("still mid-push after a second, unrelated rebuild"), Again != nullptr
		&& Again->Phase == EAgentPhase::Manoeuvring && !Again->Pushback.HasArrived())) { return false; }
	TestTrue(FString::Printf(TEXT("and the push's re-resolve leaves the departure's goal where it was (node %d, was %d)"),
		Again->GoalNode.Index, GoalBefore.Index), Again->GoalNode == GoalBefore);

	// PHASE 2, THE PUSH RUNS OUT: it holds there for a way to the runway, and never taxis - there is nowhere to taxi to.
	bool bEverTaxied = false;
	const int32 AskedBefore = Traffic->TaxiOutReplanAttemptsForTest();
	const bool bHeld = RunUntil(*Traffic, *Net, 300.0, [&]()
		{
			const FRoadAgent* A = Traffic->FindAgent(Id);
			bEverTaxied |= A != nullptr && A->Phase == EAgentPhase::Taxiing;
			return A != nullptr && A->IsHoldingForTaxiOut();
		}, 1.0 / 30.0);
	for (int32 Tick = 0; bHeld && Tick < 60; ++Tick)
	{
		Traffic->Advance(1.0 / 30.0, Net);
		bEverTaxied |= Traffic->FindAgent(Id)->Phase == EAgentPhase::Taxiing;
	}
	const FRoadAgent* Held = Traffic->FindAgent(Id);
	if (!TestTrue(TEXT("the push ran out and it holds for a way to the runway"), bHeld && Held != nullptr
		&& Held->IsHoldingForTaxiOut() && Held->IsWaitingFor(EAgentWait::ForTaxiOutRoute))) { return false; }
	TestFalse(TEXT("it never taxied"), bEverTaxied);
	TestTrue(FString::Printf(TEXT("it holds where the push ended (%.1f uu from it)"), FVector2D::Distance(Held->LastMotion.Position, PushEnd)),
		FVector2D::Distance(Held->LastMotion.Position, PushEnd) < 10.0);
	TestTrue(TEXT("asked for a way out, and refused - the hold is the retry's, not a dead end"),
		Traffic->TaxiOutReplanAttemptsForTest() > AskedBefore && Held->HasSaidWait());

	// THE PLAYER'S FIX: a line from the node nearest where it holds - the one the retry searches from - to the runway.
	const FGuidelineNodeId Near = RouteSearch::FindNearestNode(*Net, Held->LastMotion.Position, Held->Class, 3000.0);
	if (!TestTrue(TEXT("a taxi line is within reach of where it holds"), Near.IsSet())) { return false; }
	const FVector2D NearAt = Net->GetGuidelineNode(Near)->Position;
	TestGraph::FJoinOptions Options;
	Options.bDerived = false;
	TestGraph::Join(*Net, Near, TestGraph::Node(*Net, NearAt.X, 0.0), Options);
	Traffic->Advance(1.0 / 30.0, Net);
	const FRoadAgent* Fixed = Traffic->FindAgent(Id);
	if (!TestTrue(TEXT("re-armed for the runway, its wait over"),
		Fixed != nullptr && Fixed->bDepartureArmed && !Fixed->IsWaitingFor(EAgentWait::ForTaxiOutRoute))) { return false; }
	TestTrue(TEXT("and it leaves on the new line and rolls"), RunUntil(*Traffic, *Net, 300.0, [&]()
		{
			const FRoadAgent* A = Traffic->FindAgent(Id);
			return A != nullptr && A->Phase == EAgentPhase::Departing;
		}, 1.0 / 30.0));
	return true;
}

#endif
