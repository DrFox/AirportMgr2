#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/DeadlockResolver.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadAgent.h"
#include "Model/RoadNetwork.h"
#include "Model/TrafficClaims.h"

#if WITH_DEV_AUTOMATION_TESTS

// ONE DEFINITION OF A WAIT CYCLE (ops alerts spec 2026-09-29 §1): FDeadlockResolver::FindCycles is what
// Resolve acts on AND what the Deadlock alert reports, so the two cannot disagree about what a jam is.
// Staged exactly as Airside.Model.Traffic.DeadlockResolverStandalone stages it - Refuse + AccrueStall,
// no UGroundTraffic.

namespace
{
	/** Agents Id 1..N; each waits on Waits[i] (0 = on nobody) and has stalled for Stalled seconds. */
	TArray<FRoadAgent> DeadlockCyclesAgents(const TArray<int32>& Waits, double Stalled)
	{
		TArray<FRoadAgent> Agents;
		Agents.SetNum(Waits.Num());
		for (int32 Index = 0; Index < Waits.Num(); ++Index)
		{
			FGuidelineNodeId Node;
			Node.Index = 100 + Index;
			Agents[Index].Id = Index + 1;
			if (Waits[Index] != 0)
			{
				Agents[Index].Refuse(INDEX_NONE, FTrafficResource::OfNode(Node), TNumericLimits<double>::Max(), Waits[Index]);
				Agents[Index].AccrueStall(Stalled);
			}
		}
		return Agents;
	}

	TMap<int32, int32> DeadlockCyclesIndex(const TArray<FRoadAgent>& Agents)
	{
		TMap<int32, int32> Index;
		for (int32 At = 0; At < Agents.Num(); ++At)
		{
			Index.Add(Agents[At].Id, At);
		}
		return Index;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeadlockCyclesAllAircraftTest, "Airside.Model.Traffic.Deadlock.AllAircraftCycleReported",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FDeadlockCyclesAllAircraftTest::RunTest(const FString&)
{
	// 1 waits on 2, 2 on 1; 3 queues behind 1. Both members default to Aircraft.
	const TArray<FRoadAgent> Agents = DeadlockCyclesAgents({ 2, 1, 1 }, 10.0);
	FTrafficRules Rules;
	TArray<TArray<int32>> Cycles;
	FDeadlockResolver::AlertCycles(Agents, DeadlockCyclesIndex(Agents), Rules, Cycles);
	if (!TestEqual(TEXT("one all-aircraft cycle"), Cycles.Num(), 1)) { return false; }
	TArray<int32> Members = Cycles[0];
	Members.Sort();
	TestEqual(TEXT("its members are the two waiting on each other - not the one queued behind them"),
		Members, TArray<int32>{ 1, 2 });
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeadlockCyclesMixedTest, "Airside.Model.Traffic.Deadlock.MixedCycleIsNotAnAlert",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FDeadlockCyclesMixedTest::RunTest(const FString&)
{
	TArray<FRoadAgent> Agents = DeadlockCyclesAgents({ 2, 1 }, 10.0);
	Agents[1].Class = ETraversalClass::GroundVehicle;
	FTrafficRules Rules;
	FDeadlockResolver::FCycleScratch Scratch;
	TArray<TArray<int32>> Every;
	FDeadlockResolver::FindCycles(Agents, Rules, Scratch, Every);
	TestEqual(TEXT("the walk still finds the cycle - a van can go round, so the resolver acts on it"), Every.Num(), 1);
	TArray<TArray<int32>> Alerts;
	FDeadlockResolver::AlertCycles(Agents, DeadlockCyclesIndex(Agents), Rules, Alerts);
	TestEqual(TEXT("but only an ALL-aircraft cycle is a layout problem for the player"), Alerts.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeadlockCyclesThresholdTest, "Airside.Model.Traffic.Deadlock.ShortStallIsNoCycle",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FDeadlockCyclesThresholdTest::RunTest(const FString&)
{
	FTrafficRules Rules;
	const TArray<FRoadAgent> Agents = DeadlockCyclesAgents({ 2, 1 }, Rules.StallSeconds * 0.5);
	TArray<TArray<int32>> Cycles;
	FDeadlockResolver::AlertCycles(Agents, DeadlockCyclesIndex(Agents), Rules, Cycles);
	TestEqual(TEXT("two aircraft that have waited less than StallSeconds are traffic, not a jam"), Cycles.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeadlockCyclesQueueTest, "Airside.Model.Traffic.Deadlock.QueueIsNoCycle",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FDeadlockCyclesQueueTest::RunTest(const FString&)
{
	// 1 waits on 2, 2 on 3, and 3 waits on nobody (moving): a queue.
	const TArray<FRoadAgent> Agents = DeadlockCyclesAgents({ 2, 3, 0 }, 10.0);
	FTrafficRules Rules;
	FDeadlockResolver::FCycleScratch Scratch;
	TArray<TArray<int32>> Cycles;
	FDeadlockResolver::FindCycles(Agents, Rules, Scratch, Cycles);
	TestEqual(TEXT("a queue ending at a mover is no cycle"), Cycles.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeadlockCyclesTrafficTest, "Airside.Model.Traffic.Deadlock.QuietTrafficReportsNone",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FDeadlockCyclesTrafficTest::RunTest(const FString&)
{
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>();
	TArray<TArray<int32>> Cycles;
	Cycles.AddDefaulted();   // a stale entry must be cleared, not appended to
	Traffic->CurrentDeadlocks(Cycles);
	TestEqual(TEXT("an empty traffic model reports no deadlock, and the out array is reset"), Cycles.Num(), 0);
	return true;
}

// A CYCLE NOBODY IN CAN BE TURNED OUT OF IS AN ALERT, WHATEVER ITS MEMBERS ARE (issue #455, item 1).
//
// The alert used to be "every member is an aircraft", on the reasoning that a van in a cycle can go round. A truck
// backing along a bay's leg and an aeroplane being pushed off a stand cannot: they have no second line. A cycle through
// one is as much the player's to fix as an aircraft-only one, and the old filter dropped it for being made of vehicles.
// The controls are the cases that must stay quiet: a TAXIING van (replannable, so the resolver acts and the player need
// not) and a member nobody can find.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeadlockCyclesReversingTruckTest, "Airside.Model.Traffic.Deadlock.CycleThroughAReversingTruckIsAnAlert",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FDeadlockCyclesReversingTruckTest::RunTest(const FString&)
{
	FTrafficRules Rules;
	auto AlertsFor = [&Rules](TArray<FRoadAgent>& Agents)
	{
		TArray<TArray<int32>> Alerts;
		FDeadlockResolver::AlertCycles(Agents, DeadlockCyclesIndex(Agents), Rules, Alerts);
		return Alerts.Num();
	};

	// A reversing truck (1) and an aircraft (2), each waiting on the other.
	TArray<FRoadAgent> WithAircraft = DeadlockCyclesAgents({ 2, 1 }, 10.0);
	WithAircraft[0].Class = ETraversalClass::GroundVehicle;
	WithAircraft[0].Phase = EAgentPhase::Reversing;
	TestEqual(TEXT("a reversing truck and an aircraft waiting on each other are an alert (red while only all-aircraft cycles were)"),
		AlertsFor(WithAircraft), 1);

	// The same truck against a TAXIING van: the van can go round, so the resolver has a move and the player need not.
	TArray<FRoadAgent> WithVan = DeadlockCyclesAgents({ 2, 1 }, 10.0);
	WithVan[0].Class = ETraversalClass::GroundVehicle;
	WithVan[0].Phase = EAgentPhase::Reversing;
	WithVan[1].Class = ETraversalClass::GroundVehicle;
	TestEqual(TEXT("a reversing truck and a taxiing van are not: the van can turn"), AlertsFor(WithVan), 0);

	// Neither can: a truck backing and an aeroplane being pushed.
	TArray<FRoadAgent> BothFixed = DeadlockCyclesAgents({ 2, 1 }, 10.0);
	BothFixed[0].Class = ETraversalClass::GroundVehicle;
	BothFixed[0].Phase = EAgentPhase::Reversing;
	BothFixed[1].Phase = EAgentPhase::Manoeuvring;
	TestEqual(TEXT("a reversing truck and a pushed aeroplane are: nobody in the ring has a second line"), AlertsFor(BothFixed), 1);

	// THE SAME TRUCK, TAXIING, is the existing mixed case and stays no alert.
	TArray<FRoadAgent> Taxiing = DeadlockCyclesAgents({ 2, 1 }, 10.0);
	Taxiing[0].Class = ETraversalClass::GroundVehicle;
	TestEqual(TEXT("control: the same truck taxiing is a van that can go round, as MixedCycleIsNotAnAlert has it"), AlertsFor(Taxiing), 0);

	// AND ONE THE LOOKUP CANNOT FIND is still no alert: a miss must not promote a cycle.
	TArray<FRoadAgent> Lost = DeadlockCyclesAgents({ 2, 1 }, 10.0);
	Lost[0].Class = ETraversalClass::GroundVehicle;
	Lost[0].Phase = EAgentPhase::Reversing;
	TArray<TArray<int32>> LostAlerts;
	FDeadlockResolver::AlertCycles(Lost, TMap<int32, int32>(), Rules, LostAlerts);
	TestEqual(TEXT("control: with no id-to-agent map no member is found, so nothing is an alert"), LostAlerts.Num(), 0);
	return true;
}

// A RESERVATION CYCLE IS YIELDED BY A REPLANNABLE MEMBER ONLY (issue #455, review of #466).
//
// When every refusal in a cycle is against a reservation, the lowest-ranked, highest-id member gives up its
// reservations instead of anyone being replanned. Since #455 a reversing truck and a pushed aeroplane can be in such a
// cycle - and they hold their whole span or push as reservations from the first frame precisely so the manoeuvre is
// granted whole, so being made to let go of them is what the hold exists to prevent. The truck, a vehicle, ranks
// below the aeroplane and takes the highest id here: an unfiltered order picks it.
//
// STAGED BARE, as DeadlockResolverStandalone stages a ring. Van (1, taxiing), pushed aeroplane (2), reversing truck (3),
// each reserving a node the previous one waits on. The van is the ONLY replannable member: it must be the one to yield,
// and the other two must keep the reservation each holds. Then a ring of the truck and the aeroplane alone: nobody can
// yield, so nobody does - the cycle is left, seen once, on the retry cadence.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeadlockYieldSkipsFixedMembersTest, "Airside.Model.Traffic.Deadlock.ReservationYieldSkipsAReversingAndAPushedMember",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FDeadlockYieldSkipsFixedMembersTest::RunTest(const FString&)
{
	FGuidelineNodeId NodeA;
	NodeA.Index = 1;
	FGuidelineNodeId NodeB;
	NodeB.Index = 2;
	FGuidelineNodeId NodeC;
	NodeC.Index = 3;

	auto Reserve = [](FTrafficOccupancy& Occupancy, int32 Agent, FGuidelineNodeId Node)
	{
		FTrafficClaim Claim;
		Claim.AgentId = Agent;
		Claim.Resource = FTrafficResource::OfNode(Node);
		Claim.bOccupied = false;
		FTrafficClaim Blocker;
		Occupancy.TryClaim(Claim, Blocker);
	};
	auto Wait = [](FRoadAgent& Agent, int32 Id, FGuidelineNodeId On, int32 Blocker)
	{
		Agent.Id = Id;
		Agent.Refuse(INDEX_NONE, FTrafficResource::OfNode(On), TNumericLimits<double>::Max(), Blocker);
		Agent.AccrueStall(10.0);
	};
	auto Index = [](const TArray<FRoadAgent>& Agents)
	{
		TMap<int32, int32> Out;
		for (int32 At = 0; At < Agents.Num(); ++At) { Out.Add(Agents[At].Id, At); }
		return Out;
	};

	FTrafficRules Rules;
	FNodeReachCache Reach;
	FRunwayChainCache Chains;
	FPlanReResolver PlanReResolver;
	URoadNetwork* Network = NewObject<URoadNetwork>();

	// THREE: van (1) waits on A, held by the aeroplane (2); the aeroplane waits on B, held by the truck (3); the truck
	// waits on C, held by the van.
	{
		FTrafficOccupancy Occupancy;
		Reserve(Occupancy, 2, NodeA);
		Reserve(Occupancy, 3, NodeB);
		Reserve(Occupancy, 1, NodeC);
		TArray<FRoadAgent> Agents;
		Agents.SetNum(3);
		Agents[0].Class = ETraversalClass::GroundVehicle;
		Wait(Agents[0], 1, NodeA, 2);
		Agents[1].Phase = EAgentPhase::Manoeuvring;
		Wait(Agents[1], 2, NodeB, 3);
		Agents[2].Class = ETraversalClass::GroundVehicle;
		Agents[2].Phase = EAgentPhase::Reversing;
		Wait(Agents[2], 3, NodeC, 1);

		FDeadlockResolver Resolver;
		Resolver.Resolve(Agents, Index(Agents), FTrafficContext{ *Network, Rules, Occupancy, Reach, Chains, 100.0 }, PlanReResolver);
		TestEqual(TEXT("one cycle, settled by a yield"), Resolver.Yields, 1);
		TestEqual(TEXT("the VAN yields - the one replannable member (red while the truck, the lowest rank and highest id, was picked)"),
			Resolver.LastYieldedAgent, 1);
		TestFalse(TEXT("the van's reservation is gone"), Occupancy.FindClaim(1, FTrafficResource::OfNode(NodeC)) != nullptr);
		TestTrue(TEXT("the pushed aeroplane keeps the reservation it holds - its push is granted whole"),
			Occupancy.FindClaim(2, FTrafficResource::OfNode(NodeA)) != nullptr);
		TestTrue(TEXT("and the reversing truck keeps its span"), Occupancy.FindClaim(3, FTrafficResource::OfNode(NodeB)) != nullptr);
	}

	// TWO: the truck and the aeroplane alone. Nobody may yield.
	{
		FTrafficOccupancy Occupancy;
		Reserve(Occupancy, 2, NodeA);
		Reserve(Occupancy, 3, NodeB);
		TArray<FRoadAgent> Agents;
		Agents.SetNum(2);
		Agents[0].Phase = EAgentPhase::Manoeuvring;
		Wait(Agents[0], 2, NodeB, 3);
		Agents[1].Class = ETraversalClass::GroundVehicle;
		Agents[1].Phase = EAgentPhase::Reversing;
		Wait(Agents[1], 3, NodeA, 2);

		FDeadlockResolver Resolver;
		Resolver.Resolve(Agents, Index(Agents), FTrafficContext{ *Network, Rules, Occupancy, Reach, Chains, 100.0 }, PlanReResolver);
		TestEqual(TEXT("a ring of a pushed aeroplane and a reversing truck is not yielded: nobody in it may let go"), Resolver.Yields, 0);
		TestEqual(TEXT("it is seen"), Resolver.CyclesSeen.Num(), 1);
		TestTrue(TEXT("and both keep what they hold"),
			Occupancy.FindClaim(2, FTrafficResource::OfNode(NodeA)) != nullptr
			&& Occupancy.FindClaim(3, FTrafficResource::OfNode(NodeB)) != nullptr);
	}
	// TWO, GATE-REFUSED: a truck refused at its bay's reverse leg (Taxiing, refused at the reverse step, nothing armed) and a
	// pushed aeroplane. The truck MAY be the yielder, by intent: IsReplannable is a phase question and it is Taxiing, it has
	// not armed, so what it holds is the ground AHEAD of its nose - reservations the yield may give back and it will
	// win again - not a span it is already backing over. The aeroplane keeps its push. (The resolver will still never
	// TURN it: CanBeTurnedAtItsBlock refuses a block at a reverse step - a different question, asked of a different list.)
	{
		FTrafficOccupancy Occupancy;
		Reserve(Occupancy, 2, NodeA);
		Reserve(Occupancy, 3, NodeB);
		TArray<FRoadAgent> Agents;
		Agents.SetNum(2);
		Agents[0].Phase = EAgentPhase::Manoeuvring;
		Wait(Agents[0], 2, NodeB, 3);
		Agents[1].Class = ETraversalClass::GroundVehicle;
		Wait(Agents[1], 3, NodeA, 2);
		Agents[1].Follower.Plan.Steps.SetNum(2);
		Agents[1].Follower.Plan.Steps[1].bReverseLeg = true;
		Agents[1].Refuse(1, FTrafficResource::OfNode(NodeA), TNumericLimits<double>::Max(), 2);

		FDeadlockResolver Resolver;
		Resolver.Resolve(Agents, Index(Agents), FTrafficContext{ *Network, Rules, Occupancy, Reach, Chains, 100.0 }, PlanReResolver);
		TestEqual(TEXT("a gate-refused Taxiing truck may yield, by intent: it has not armed, and lets go only of ground ahead of it"),
			Resolver.LastYieldedAgent, 3);
		TestFalse(TEXT("its reservation is gone"), Occupancy.FindClaim(3, FTrafficResource::OfNode(NodeB)) != nullptr);
		TestTrue(TEXT("and the pushed aeroplane keeps its push"), Occupancy.FindClaim(2, FTrafficResource::OfNode(NodeA)) != nullptr);
	}
	return true;
}

// A RING OF TWO TRUCKS GATE-REFUSED AT THEIR BAYS IS AN ALERT (issue #455, re-review of #466).
//
// A truck refused the ground of the reverse leg it is about to arm (UGroundTraffic::GateReverseLeg) is Taxiing, so
// IsReplannable says yes - but the leg is the only line off its service point, so the resolver never turns it
// (CanReplanAtBlockedStep). The alert asked the phase question alone and counted it as a way out: two such trucks, each
// waiting on the other's ground (occupied, so not a yield), wait for ever and the player heard nothing but a log line
// every retry window. Both now ask CanBeTurnedAtItsBlock. The CONTROL is the same ring with the blocked steps ordinary
// taxi steps: two vans that can go round, no alert. And the resolver is run over the ring to show the two agree - it
// is neither resolved nor yielded, only seen.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeadlockCyclesGateRefusedRingTest, "Airside.Model.Traffic.Deadlock.RingOfTwoGateRefusedTrucksIsAnAlert",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FDeadlockCyclesGateRefusedRingTest::RunTest(const FString&)
{
	FGuidelineNodeId NodeA;
	NodeA.Index = 1;
	FGuidelineNodeId NodeB;
	NodeB.Index = 2;
	FTrafficRules Rules;

	// Two Taxiing trucks, each refused at step 1 of a two-step plan whose step 1 is (or is not) a reverse leg.
	auto Ring = [&](bool bReverseStep)
	{
		TArray<FRoadAgent> Agents = DeadlockCyclesAgents({ 2, 1 }, 10.0);
		for (int32 At = 0; At < 2; ++At)
		{
			Agents[At].Class = ETraversalClass::GroundVehicle;
			Agents[At].Follower.Plan.Steps.SetNum(2);
			Agents[At].Follower.Plan.Steps[1].bReverseLeg = bReverseStep;
			Agents[At].Refuse(1, FTrafficResource::OfNode(At == 0 ? NodeA : NodeB), TNumericLimits<double>::Max(), At == 0 ? 2 : 1);
		}
		return Agents;
	};

	TArray<FRoadAgent> Gated = Ring(true);
	TArray<TArray<int32>> Alerts;
	FDeadlockResolver::AlertCycles(Gated, DeadlockCyclesIndex(Gated), Rules, Alerts);
	TestEqual(TEXT("two trucks refused at their bays' reverse legs, each waiting on the other, are an alert (red while only IsReplannable was asked)"),
		Alerts.Num(), 1);

	TArray<FRoadAgent> Vans = Ring(false);
	Alerts.Reset();
	FDeadlockResolver::AlertCycles(Vans, DeadlockCyclesIndex(Vans), Rules, Alerts);
	TestEqual(TEXT("control: the same ring at ordinary taxi steps is two vans that can go round - no alert"), Alerts.Num(), 0);

	// THE RESOLVER AGREES: over the gated ring, with occupied claims (so it is a deadlock and not a yield), nobody is
	// replanned and nobody yields - the ring is seen and left, which is exactly what the alert has to tell the player.
	FTrafficOccupancy Occupancy;
	{
		FTrafficClaim Claim;
		FTrafficClaim Blocker;
		Claim.AgentId = 2;
		Claim.Resource = FTrafficResource::OfNode(NodeA);
		Claim.bOccupied = true;
		Occupancy.TryClaim(Claim, Blocker);
		Claim.AgentId = 1;
		Claim.Resource = FTrafficResource::OfNode(NodeB);
		Occupancy.TryClaim(Claim, Blocker);
	}
	FNodeReachCache Reach;
	FRunwayChainCache Chains;
	FPlanReResolver PlanReResolver;
	URoadNetwork* Network = NewObject<URoadNetwork>();
	FDeadlockResolver Resolver;
	Resolver.Resolve(Gated, DeadlockCyclesIndex(Gated), FTrafficContext{ *Network, Rules, Occupancy, Reach, Chains, 100.0 }, PlanReResolver);
	TestEqual(TEXT("the resolver replans nobody in it"), Resolver.LastResolvedAgent, 0);
	TestEqual(TEXT("and yields nobody: it is a deadlock, not reservations"), Resolver.Yields, 0);
	TestEqual(TEXT("it only sees it"), Resolver.CyclesSeen.Num(), 1);
	return true;
}

#endif
