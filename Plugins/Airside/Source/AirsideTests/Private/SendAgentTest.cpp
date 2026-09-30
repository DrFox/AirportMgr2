#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Content/AirsideSettings.h"
#include "Misc/AutomationTest.h"
#include "Model/DeadlockResolver.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadAgent.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteSearch.h"
#include "Model/SendAgent.h"
#include "Model/TrafficOccupancy.h"
#include "Testing/AirsideTestGraph.h"

#if WITH_DEV_AUTOMATION_TESTS

// ISSUE #429 PART 2: THE CHOICE OF ROUTE CHANGE IS AIRSIDE'S. Part 1 made HOW a route changes one seam; these pin the
// operations that CHOOSE one - SendAgentTo's phase table, the deadlock resolver's per-agent step as the player's
// Unstick asks it (ReplanAroundBlocker), the bid's remaining drive, and the one definition of stuck - at the
// UGroundTraffic level, with real agents on a real claim table, so an operation that stopped calling the shared step
// would go red here and not only in a module above.

namespace SendAgentTest
{
	/**
	 * A fork - the route-change tests' own: A (0,0) east to B (3000,0), on to C (6000,0) and E (9000,0), and B north to
	 * D (3000,3000) and D on to C. Authored edges.
	 */
	struct FFork
	{
		URoadNetwork* Net = nullptr;
		FGuidelineNodeId A, B, C, D, E;
		FGuidelineEdgeId BC;
	};

	FFork BuildFork()
	{
		FFork F;
		F.Net = NewObject<URoadNetwork>(GetTransientPackage());
		URoadNetwork& Net = *F.Net;
		F.A = TestGraph::Node(Net, 0.0, 0.0);
		F.B = TestGraph::Node(Net, 3000.0, 0.0);
		F.C = TestGraph::Node(Net, 6000.0, 0.0);
		F.D = TestGraph::Node(Net, 3000.0, 3000.0);
		F.E = TestGraph::Node(Net, 9000.0, 0.0);
		TestGraph::FJoinOptions Authored;
		Authored.bDerived = false;
		TestGraph::Join(Net, F.A, F.B, Authored);
		F.BC = TestGraph::Join(Net, F.B, F.C, Authored);
		TestGraph::Join(Net, F.B, F.D, Authored);
		TestGraph::Join(Net, F.D, F.C, Authored);
		TestGraph::Join(Net, F.C, F.E, Authored);
		return F;
	}

	/** A van From -> To the short way, on a traffic of its own - so no case below waits on another's agent. */
	int32 Van(UGroundTraffic& Traffic, const FFork& F, FGuidelineNodeId From, FGuidelineNodeId To)
	{
		return Traffic.DispatchAgent(F.Net, TestGraph::Probe(*F.Net, From, To, ETraversalClass::GroundVehicle),
			TestAirframes::Van(), ETraversalClass::GroundVehicle, 0.0);
	}

	/** The query a vehicle's leg is sent with - AirportOps' own shape (UJobBoard::DriveVehicleTo): errand, rules, gate. */
	FRouteQuery VanLeg(const UGroundTraffic& Traffic, int32 AgentId, FGuidelineNodeId Goal)
	{
		FRouteQuery Query = FRouteQuery::For(ERouteErrand::VehicleToJob, FGuidelineNodeId(), Goal, 0.0,
			ETraversalClass::GroundVehicle);
		Query.WithRules(Traffic.Rules, Traffic.GetOccupancy(), AgentId);
		return Query;
	}

	/**
	 * THE DEADLOCK RING - Airside.Model.Traffic.DeadlockRing's own: four vans on a one-way square A -> B -> C -> D -> A
	 * of 750 uu lanes, each waiting on the next within a second, and one escape D -> X1 -> X2 -> X3 -> B round the
	 * outside. The footprint is that test's, for its reason: the escape only holds at 500 uu.
	 */
	struct FRing
	{
		URoadNetwork* Net = nullptr;
		UGroundTraffic* Traffic = nullptr;
		int32 V1 = 0, V2 = 0, V3 = 0, V4 = 0;
	};

	FRing BuildRing()
	{
		FRing R;
		R.Net = NewObject<URoadNetwork>(GetTransientPackage());
		URoadNetwork& Net = *R.Net;
		const FGuidelineNodeId A = TestGraph::Node(Net, 0.0, 0.0);
		const FGuidelineNodeId B = TestGraph::Node(Net, 750.0, 0.0);
		const FGuidelineNodeId C = TestGraph::Node(Net, 750.0, 750.0);
		const FGuidelineNodeId D = TestGraph::Node(Net, 0.0, 750.0);
		const FGuidelineNodeId X1 = TestGraph::Node(Net, -500.0, 1250.0);
		const FGuidelineNodeId X2 = TestGraph::Node(Net, 1500.0, 1250.0);
		const FGuidelineNodeId X3 = TestGraph::Node(Net, 1500.0, 0.0);
		TestGraph::Join(Net, A, B, { EGuidelineDir::AToB });
		TestGraph::Join(Net, B, C, { EGuidelineDir::AToB });
		TestGraph::Join(Net, C, D, { EGuidelineDir::AToB });
		TestGraph::Join(Net, D, A, { EGuidelineDir::AToB });
		TestGraph::Join(Net, D, X1, { EGuidelineDir::AToB });
		TestGraph::Join(Net, X1, X2, { EGuidelineDir::AToB });
		TestGraph::Join(Net, X2, X3, { EGuidelineDir::AToB });
		TestGraph::Join(Net, X3, B, { EGuidelineDir::AToB });
		R.Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
		R.Traffic->Rules.VehicleFootprint = 500.0;
		auto Dispatch = [&R, &Net](FGuidelineNodeId From, FGuidelineNodeId To)
		{
			return R.Traffic->DispatchAgent(&Net, TestGraph::Probe(Net, From, To, ETraversalClass::GroundVehicle),
				TestAirframes::Van(), ETraversalClass::GroundVehicle, 1.0);
		};
		R.V1 = Dispatch(A, C);
		R.V2 = Dispatch(B, D);
		R.V3 = Dispatch(C, A);
		R.V4 = Dispatch(D, B);
		return R;
	}

	/** The edges of AgentId's route, in order - what "the same route" means for a replan (ReplanAt's own test). */
	TArray<FGuidelineEdgeId> EdgesOf(const UGroundTraffic& Traffic, int32 AgentId)
	{
		TArray<FGuidelineEdgeId> Edges;
		if (const FRoadAgent* Agent = Traffic.FindAgent(AgentId))
		{
			for (const FRouteStep& Step : Agent->Follower.Plan.Steps)
			{
				Edges.Add(Step.Edge);
			}
		}
		return Edges;
	}
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSendAgentReplanAroundBlockerMatchesTheResolverTest,
	"Airside.Model.Traffic.ReplanAroundBlocker.MatchesTheResolver",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSendAgentReplanAroundBlockerMatchesTheResolverTest::RunTest(const FString& Parameters)
{
	using namespace SendAgentTest;
	// ONE HELD AGENT, TWO ASKERS, ONE ANSWER (#429's pin): the player's Unstick replans a held agent with the deadlock
	// resolver's own per-agent step, so the two choose the same splice and the same ban - the same route. Two identical
	// rings: in one the resolver breaks the jam as it always has; in the other, before the resolver's stall bound is
	// reached, the Unstick's ReplanAroundBlocker is asked of the agent the resolver chose. The Unstick used to copy the
	// ban from AirportOps with no upper bound; a copy that drifts in either ban or bound shows up here as a different
	// route, or none.
	FRing ByResolver = BuildRing();
	FRing ByUnstick = BuildRing();
	if (!TestTrue(TEXT("both rings dispatched"), ByResolver.V4 > 0 && ByUnstick.V4 > 0)) { return false; }

	TickUntil(*ByResolver.Traffic, *ByResolver.Net, 10.0, [&](int32) { return ByResolver.Traffic->GetLastResolvedAgentForTest() == 0; });
	const int32 Chosen = ByResolver.Traffic->GetLastResolvedAgentForTest();
	if (!TestEqual(TEXT("the resolver turned the highest id round the outside, as the ring always has"), Chosen, ByResolver.V4)) { return false; }
	const TArray<FGuidelineEdgeId> ResolverRoute = EdgesOf(*ByResolver.Traffic, Chosen);

	// Two seconds: every van stopped and waiting (within one), and the resolver not yet due (StallSeconds, 3 s).
	TickUntil(*ByUnstick.Traffic, *ByUnstick.Net, 2.0, [](int32) { return true; });
	if (!TestEqual(TEXT("precondition: the resolver has not acted in the second ring"), ByUnstick.Traffic->GetLastResolvedAgentForTest(), 0)) { return false; }
	const FRoadAgent* Held = ByUnstick.Traffic->FindAgent(Chosen);
	if (!TestTrue(TEXT("precondition: the same agent is held, refused a step"), Held != nullptr && Held->GetBlockedStep() >= 0
		&& Held->Follower.Plan.Steps.IsValidIndex(Held->GetBlockedStep()))) { return false; }
	const FGuidelineEdgeId Refused = Held->Follower.Plan.Steps[Held->GetBlockedStep()].Edge;

	TestEqual(TEXT("the Unstick's replan turns it round what holds it"),
		ByUnstick.Traffic->ReplanAroundBlocker(Chosen, *ByUnstick.Net), EBlockerReplan::Turned);
	const TArray<FGuidelineEdgeId> UnstickRoute = EdgesOf(*ByUnstick.Traffic, Chosen);
	AddInfo(FString::Printf(TEXT("resolver route %d step(s), Unstick route %d step(s)"), ResolverRoute.Num(), UnstickRoute.Num()));
	TestTrue(TEXT("onto the very route the resolver chose - the same splice and the same ban"), UnstickRoute == ResolverRoute);
	TestFalse(TEXT("which no longer takes the edge that refused it"), UnstickRoute.Contains(Refused));

	// AND WHERE THE RESOLVER WOULD NOT TURN IT, NEITHER DOES THE UNSTICK: the replan ended the wait (its refusal
	// cleared with the route it was on), so asked again at once the same van is held nowhere and is left alone.
	TestEqual(TEXT("a van no longer refused anything is not at a block, so it is not turned again"),
		ByUnstick.Traffic->ReplanAroundBlocker(Chosen, *ByUnstick.Net), EBlockerReplan::NotAtItsBlock);
	TestEqual(TEXT("nor an unknown one"), ByUnstick.Traffic->ReplanAroundBlocker(9999, *ByUnstick.Net), EBlockerReplan::NotAtItsBlock);
	return true;
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSendAgentReplanAroundBlockerBansWhatRefusedItTest,
	"Airside.Model.Traffic.ReplanAroundBlocker.BansWhatRefusedIt",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSendAgentReplanAroundBlockerBansWhatRefusedItTest::RunTest(const FString& Parameters)
{
	// THE BAN IS THE RESOLVER'S, NODE OR EDGE (#429): a node somebody stands on is a wall from every side, so the whole
	// node goes; anything else bans the refused step's edge. The ring above cannot tell the two apart - its escape
	// passes neither - so this does, on the fork: a van standing at B, refused at its first step (B -> C). Refused the
	// EDGE, it goes round by D. Refused the NODE C, there is no way round - every way to E passes C - and a copy that
	// banned only the edge (the shape AirportOps' copy could drift to) would send it by D into the very node that holds it.
	using namespace SendAgentTest;
	const FFork F = BuildFork();
	constexpr int32 Phantom = 4242;

	UGroundTraffic* ByEdge = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 EdgeVan = Van(*ByEdge, F, F.B, F.E);
	if (!TestTrue(TEXT("a van at B, going by BC"), EdgeVan > 0 && EdgesOf(*ByEdge, EdgeVan).Contains(F.BC))) { return false; }
	FGroundTrafficTestAccess(*ByEdge).ScriptWait(EdgeVan, FTrafficResource::OfEdge(F.BC), Phantom, 30.0, 0);
	TestEqual(TEXT("refused the edge: turned round it"), ByEdge->ReplanAroundBlocker(EdgeVan, *F.Net), EBlockerReplan::Turned);
	TestFalse(TEXT("by D - the edge is banned"), EdgesOf(*ByEdge, EdgeVan).Contains(F.BC));

	UGroundTraffic* ByNode = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 NodeVan = Van(*ByNode, F, F.B, F.E);
	if (!TestTrue(TEXT("a second van at B, going by BC"), NodeVan > 0)) { return false; }
	FGroundTrafficTestAccess(*ByNode).ScriptWait(NodeVan, FTrafficResource::OfNode(F.C), Phantom, 30.0, 0);
	TestEqual(TEXT("refused the NODE C: no way round, because the whole node is banned and every way to E passes it"),
		ByNode->ReplanAroundBlocker(NodeVan, *F.Net), EBlockerReplan::NoWayRound);
	TestTrue(TEXT("and it keeps the route it had"), EdgesOf(*ByNode, NodeVan).Contains(F.BC));
	return true;
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSendAgentIsStuckIsOneDefinitionTest,
	"Airside.Model.Traffic.IsStuckIsOneDefinition",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSendAgentIsStuckIsOneDefinitionTest::RunTest(const FString& Parameters)
{
	// ONE DEFINITION OF STUCK (#429). It was three - the stall clock's accrual rule, the resolver's waiter test
	// (`stall > StallSeconds && WaitingOn`) and the Unstick button's (`Stranded || stall >= Seconds`) - and the last
	// disagreed with the second on the bound and on whether anyone need be waited on. Now FindCycles asks
	// HasStalledFor and the inspector asks IsStuck; this holds both to one answer on the cases that told them apart.
	FTrafficRules Rules;
	FGuidelineNodeId Node;
	Node.Index = 7;
	auto Waiter = [&Node](int32 Id, int32 WaitsOn, double Stalled)
	{
		FRoadAgent Agent;
		Agent.Id = Id;
		Agent.Refuse(INDEX_NONE, FTrafficResource::OfNode(Node), TNumericLimits<double>::Max(), WaitsOn);
		Agent.AccrueStall(Stalled);
		return Agent;
	};
	auto CycleCount = [&Rules](const TArray<FRoadAgent>& Agents)
	{
		FDeadlockResolver::FCycleScratch Scratch;
		TArray<TArray<int32>> Cycles;
		FDeadlockResolver::FindCycles(Agents, Rules, Scratch, Cycles);
		return Cycles.Num();
	};

	// PAST THE BOUND: a waiter, stuck, and a cycle member.
	const TArray<FRoadAgent> Long = { Waiter(1, 2, Rules.StallSeconds * 2.0), Waiter(2, 1, Rules.StallSeconds * 2.0) };
	TestTrue(TEXT("a waiter past the bound has stalled for it"), Long[0].HasStalledFor(Rules.StallSeconds));
	TestTrue(TEXT("and is stuck at that bound"), Long[0].IsStuck(Rules.StallSeconds));
	TestEqual(TEXT("and the resolver counts the pair as a cycle"), CycleCount(Long), 1);
	TestFalse(TEXT("but not stuck at a bound it has not reached - the asker's threshold is the asker's"), Long[0].IsStuck(Rules.StallSeconds * 3.0));

	// EXACTLY AT THE BOUND: strictly longer, for both askers (the button's `>=` said stuck here, the resolver did not).
	const TArray<FRoadAgent> AtBound = { Waiter(1, 2, Rules.StallSeconds), Waiter(2, 1, Rules.StallSeconds) };
	TestFalse(TEXT("a wait exactly the bound long is not yet stuck"), AtBound[0].IsStuck(Rules.StallSeconds));
	TestEqual(TEXT("and the resolver agrees: no cycle yet"), CycleCount(AtBound), 0);

	// THE CLOCK WITHOUT A BLOCKER: nobody is waited on, so nobody is stuck behind anybody (the button's copy said stuck).
	FRoadAgent Cleared = Waiter(3, 4, Rules.StallSeconds * 2.0);
	Cleared.ClearArbitration();
	TestFalse(TEXT("a stall clock naming nobody is not a stalled waiter"), Cleared.HasStalledFor(Rules.StallSeconds));
	TestFalse(TEXT("nor stuck"), Cleared.IsStuck(Rules.StallSeconds));

	// STRANDED: stuck at any bound, with no clock at all - and never a wait-for edge.
	FRoadAgent Stranded;
	Stranded.Id = 5;
	Stranded.Phase = EAgentPhase::Stranded;
	TestTrue(TEXT("a stranded agent is stuck whatever the bound"), Stranded.IsStuck(1.0e6));
	TestFalse(TEXT("but it waits on nobody, so it is no resolver's waiter"), Stranded.HasStalledFor(0.0));
	return true;
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSendAgentRemainingDriveSecondsTest,
	"Airside.Model.Traffic.RemainingDriveSeconds",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSendAgentRemainingDriveSecondsTest::RunTest(const FString& Parameters)
{
	// THE BID'S ETA, AIRSIDE'S ANSWER (#429): what is left of the plan past Travelled, at the agent's own cruise. The bid
	// read the follower's plan and distance itself; this is that number, so it must BE that number.
	using namespace SendAgentTest;
	const FFork F = BuildFork();
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Id = Van(*Traffic, F, F.A, F.E);
	if (!TestTrue(TEXT("a van A -> E"), Id > 0)) { return false; }
	TickUntil(*Traffic, *F.Net, 2.0, [](int32) { return true; });
	const FRoadAgent* Agent = Traffic->FindAgent(Id);
	if (!TestNotNull(TEXT("on its way"), Agent)) { return false; }
	const double Cruise = FMath::Max(Agent->Chassis().Ground.Taxi.SpeedCap, 1.0);
	const double Expected = (Agent->Follower.Plan.Length - Agent->Follower.Travelled) / Cruise;
	AddInfo(FString::Printf(TEXT("%.0f uu left at %.0f uu/s: %.2f s"), Agent->Follower.Plan.Length - Agent->Follower.Travelled, Cruise, Expected));
	TestTrue(TEXT("it has some way still to go"), Expected > 0.0);
	TestEqual(TEXT("the drive left is the plan left at its own cruise"), Traffic->RemainingDriveSeconds(Id), Expected, 1.0e-9);
	TestEqual(TEXT("an unknown agent has nothing left to drive"), Traffic->RemainingDriveSeconds(9999), 0.0);
	return true;
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSendAgentByPhaseTest,
	"Airside.Model.Traffic.SendAgentTo.ByPhase",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSendAgentByPhaseTest::RunTest(const FString& Parameters)
{
	// SENDAGENTTO'S PHASE TABLE, ONE ROW EACH (#429). AirportOps' DriveVehicleTo and the stand re-offer used to pick
	// the verb themselves - RerouteAgent, ExtendRoute, RedirectAgent, RescueStranded - by reading the agent; the table is
	// here now, and every row is asked of a real agent on the fork, each on a traffic of its own.
	using namespace SendAgentTest;
	const FFork F = BuildFork();

	// TAXIING VAN: turned at the first node ahead that holds - B, the end of the step it is on - and still moving.
	{
		UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
		const int32 Id = Van(*Traffic, F, F.A, F.E);
		RunUntil(*Traffic, *F.Net, 20.0, [&]() { const FRoadAgent* A = Traffic->FindAgent(Id); return A != nullptr && A->Follower.Travelled > 300.0; });
		const FSendAgentResult Sent = Traffic->SendAgentTo(Id, F.D, VanLeg(*Traffic, Id, F.D), *F.Net);
		TestEqual(TEXT("a moving van is turned on the road"), Sent.Outcome, ESendOutcome::Turned);
		TestEqual(TEXT("at B, the node ahead"), Sent.TurnAt, F.B);
		const FRoadAgent* Agent = Traffic->FindAgent(Id);
		if (TestNotNull(TEXT("still there"), Agent))
		{
			TestEqual(TEXT("heading for D"), Agent->GoalNode, F.D);
			TestEqual(TEXT("without stopping"), Agent->Phase, EAgentPhase::Taxiing);
		}
		TestEqual(TEXT("asked again for the goal it now has: already going"),
			Traffic->SendAgentTo(Id, F.D, VanLeg(*Traffic, Id, F.D), *F.Net).Outcome, ESendOutcome::AlreadyGoing);
	}

	// PARKED VAN: redirected from where it stands.
	{
		UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
		const int32 Id = Van(*Traffic, F, F.B, F.C);
		const bool bParked = RunUntil(*Traffic, *F.Net, 60.0, [&]() { const FRoadAgent* A = Traffic->FindAgent(Id); return A != nullptr && A->Phase == EAgentPhase::Parked; });
		if (TestTrue(TEXT("a van parked at C"), bParked))
		{
			const FSendAgentResult Sent = Traffic->SendAgentTo(Id, F.E, VanLeg(*Traffic, Id, F.E), *F.Net);
			TestEqual(TEXT("a parked van is redirected from where it stands"), Sent.Outcome, ESendOutcome::Redirected);
			const FRoadAgent* Agent = Traffic->FindAgent(Id);
			TestTrue(TEXT("taxiing for E"), Agent != nullptr && Agent->Phase == EAgentPhase::Taxiing && Agent->GoalNode == F.E);
		}
	}

	// STRANDED VAN: rescued onto the pavement beside it.
	{
		UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
		const int32 Id = Van(*Traffic, F, F.A, F.E);
		TickUntil(*Traffic, *F.Net, 1.0, [](int32) { return true; });
		FGroundTrafficTestAccess(*Traffic).Strand(Id);
		TickUntil(*Traffic, *F.Net, 0.2, [](int32) { return true; });
		const FRoadAgent* Before = Traffic->FindAgent(Id);
		if (TestTrue(TEXT("a van stranded on AB"), Before != nullptr && Before->Phase == EAgentPhase::Stranded))
		{
			const FSendAgentResult Sent = Traffic->SendAgentTo(Id, F.E, VanLeg(*Traffic, Id, F.E), *F.Net);
			TestEqual(TEXT("a stranded van is rescued"), Sent.Outcome, ESendOutcome::Rescued);
			const FRoadAgent* Agent = Traffic->FindAgent(Id);
			TestTrue(TEXT("taxiing again"), Agent != nullptr && Agent->Phase == EAgentPhase::Taxiing);
		}
	}

	// TAXIING AIRCRAFT: extended from where its route ends (B), never restarted - the #435 re-offer's row.
	{
		UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
		const int32 Id = Traffic->DispatchAgent(F.Net, TestGraph::Probe(*F.Net, F.A, F.B, ETraversalClass::Aircraft),
			TestAirframes::GroundOnly(), ETraversalClass::Aircraft, 1.0);
		RunUntil(*Traffic, *F.Net, 20.0, [&]() { const FRoadAgent* A = Traffic->FindAgent(Id); return A != nullptr && A->Follower.Travelled > 300.0; });
		const FRoadAgent* Before = Traffic->FindAgent(Id);
		const double WasTravelled = Before != nullptr ? Before->Follower.Travelled : 0.0;
		const FSendAgentResult Sent = Traffic->SendAgentTo(Id, F.E,
			FRouteQuery::For(ERouteErrand::ArrivalTaxiIn, FGuidelineNodeId(), F.E, TestAirframes::GroundOnly().Wingspan, ETraversalClass::Aircraft), *F.Net);
		TestEqual(TEXT("a moving aircraft is turned on the move"), Sent.Outcome, ESendOutcome::Turned);
		TestEqual(TEXT("from where its route ended"), Sent.TurnAt, F.B);
		const FRoadAgent* Agent = Traffic->FindAgent(Id);
		if (TestNotNull(TEXT("still there"), Agent))
		{
			TestEqual(TEXT("heading for E"), Agent->GoalNode, F.E);
			TestEqual(TEXT("extended in place: the distance it has driven is kept, not restarted from rest"), Agent->Follower.Travelled, WasTravelled, 1.0e-6);
		}
	}

	TestEqual(TEXT("an unknown agent is sent nowhere"),
		NewObject<UGroundTraffic>(GetTransientPackage())->SendAgentTo(9999, F.E, FRouteQuery::For(ERouteErrand::VehicleToJob,
			FGuidelineNodeId(), F.E, 0.0, ETraversalClass::GroundVehicle), *F.Net).Outcome, ESendOutcome::NotSendable);
	return true;
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSendAgentUngatedFoldIsNotDrivenTest,
	"Airside.Model.Traffic.SendAgentTo.UngatedFoldIsNotDriven",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSendAgentUngatedFoldIsNotDrivenTest::RunTest(const FString& Parameters)
{
	// THE FOLD JUDGE, THROUGH THE COMPOSITION (#429 review). A tow parked at the mouth of a dead-end balloon - three
	// same-hand quarter turns, which fold the rig (AirportOps.Ops.FuelTowNeverDrivenHomeIntoAFold measures the road
	// itself, by calling the rule directly) - is sent on with ENarrowRoad::DriveAnyway. The gated search refuses the
	// road; the ungated retry finds it; and SendFromRest must judge that ungated route whole and NOT drive it: NoRoute,
	// the fold named for the caller. Without the judge the rig is redirected into its own jack-knife - the fuel tests
	// cannot see that, their fixture tows fold nothing. Two quarters (a U) is the control: the same call drives it.
	using namespace SendAgentTest;
	const FVehicle Rig = UAirsideSettings::ResolveRigVehicle();
	for (const int32 Quarters : { 3, 2 })
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		auto Node = [Net](const FVector2D& At) { return Net->AddGuidelineNode(At, /*bDerived=*/false); };
		auto Join = [Net](FGuidelineNodeId A, FGuidelineNodeId B, const FVector2D& Control)
		{
			FGuidelineEdge Edge;
			Edge.A = A;
			Edge.B = B;
			Edge.Control = Control;
			Edge.AllowedTraffic = FTrafficMask::All();
			Edge.Direction = EGuidelineDir::AToB;
			Edge.bDerived = false;
			Net->AddGuidelineEdge(MoveTemp(Edge));
		};
		// FuelServiceTest's balloon (830 uu quarters), with a lead-in the rig parks at the end of.
		const double Leg = 830.0;
		const FGuidelineNodeId Pre = Node(FVector2D(-8000.0, 0.0));
		const FGuidelineNodeId Start = Node(FVector2D(-4000.0, 0.0));
		Join(Pre, Start, FVector2D(-6000.0, 0.0));
		FGuidelineNodeId From = Node(FVector2D::ZeroVector);
		Join(Start, From, FVector2D(-2000.0, 0.0));
		FVector2D At = FVector2D::ZeroVector;
		FVector2D Heading(1.0, 0.0);
		for (int32 Quarter = 0; Quarter < Quarters; ++Quarter)
		{
			const FVector2D Side(-Heading.Y, Heading.X);
			const FVector2D Control = At + Heading * Leg;
			At = Control + Side * Leg;
			Heading = Side;
			const FGuidelineNodeId To = Node(At);
			Join(From, To, Control);
			From = To;
		}
		const FGuidelineNodeId Goal = Node(At + Heading * 4000.0);
		Join(From, Goal, At + Heading * 2000.0);

		UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
		const int32 Id = Traffic->DispatchAgent(Net, TestGraph::Probe(*Net, Pre, Start, ETraversalClass::GroundVehicle), Rig,
			ETraversalClass::GroundVehicle, 0.0);
		const bool bParked = RunUntil(*Traffic, *Net, 120.0,
			[&]() { const FRoadAgent* A = Traffic->FindAgent(Id); return A != nullptr && A->Phase == EAgentPhase::Parked; });
		if (!TestTrue(*FString::Printf(TEXT("%d quarters: the rig is parked at the balloon's mouth"), Quarters), Id > 0 && bParked)) { continue; }

		FRouteQuery Leg0 = FRouteQuery::For(ERouteErrand::VehicleToJob, FGuidelineNodeId(), Goal, 0.0, ETraversalClass::GroundVehicle);
		Leg0.WithRules(Traffic->Rules, Traffic->GetOccupancy(), Id);
		Leg0.WithVehicle(Rig);
		const FSendAgentResult Sent = Traffic->SendAgentTo(Id, Goal, Leg0, *Net, ENarrowRoad::DriveAnyway);
		AddInfo(FString::Printf(TEXT("%d quarters: outcome %d, narrow %s (%s), fold '%s'"), Quarters, static_cast<int32>(Sent.Outcome),
			Sent.bNarrow ? TEXT("yes") : TEXT("no"), *Sent.NarrowWhy, *Sent.FoldWhy));
		const FRoadAgent* Agent = Traffic->FindAgent(Id);
		if (Quarters == 3)
		{
			TestTrue(TEXT("the gate refused the balloon, so it was searched again ungated"), Sent.bNarrow);
			TestEqual(TEXT("and the ungated route, which folds the rig, is NOT driven"), Sent.Outcome, ESendOutcome::NoRoute);
			TestTrue(TEXT("the fold is named for the caller's line"), Sent.FoldWhy.StartsWith(TEXT("trailer folds at guideline node")));
			TestTrue(TEXT("and the rig stays parked where it was"), Agent != nullptr && Agent->Phase == EAgentPhase::Parked);
		}
		else
		{
			TestEqual(TEXT("control: a road its trailer holds is driven"), Sent.Outcome, ESendOutcome::Redirected);
			TestTrue(TEXT("with no fold named"), Sent.FoldWhy.IsEmpty());
		}
	}
	return true;
}

#endif
