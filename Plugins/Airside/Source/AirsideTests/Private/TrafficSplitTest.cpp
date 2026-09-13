#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadNetwork.h"
#include "Model/TrafficClaims.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------------------
// Issue #84: UGroundTraffic's claim pass, deadlock resolver and replan mechanism are now
// FClaimPass, FDeadlockResolver and FPlanReResolver - plain structs a test can construct
// and drive with NO UGroundTraffic at all. These three tests are that seam: each fails if
// the extraction left a struct silently dependent on something only UGroundTraffic
// provides. Airside.Model.Traffic.* (32 tests) and Airside.Model.Occupancy.* cover the
// BEHAVIOUR unchanged through the normal UGroundTraffic path; these cover the STRUCTURE.

// (a) FDeadlockResolver, constructed bare, resolves a reservation cycle with no
// UGroundTraffic, no dispatch and no route search - two agents, each blocking the
// other's RESERVATION on a hand-built table.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficSplitDeadlockResolverStandaloneTest,
	"Airside.Model.Traffic.DeadlockResolverStandalone",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficSplitDeadlockResolverStandaloneTest::RunTest(const FString& Parameters)
{
	FGuidelineNodeId NodeX;
	NodeX.Index = 1;
	FGuidelineNodeId NodeY;
	NodeY.Index = 2;

	FTrafficOccupancy Occupancy;

	// Agent 1 reserves NodeY; agent 2 reserves NodeX. Neither is OCCUPIED - both are ahead
	// of the nose, not under it - which is exactly the "cycle of reservations, not bodies"
	// case spec §5 says is a YIELD, not a deadlock.
	{
		FTrafficClaim Claim;
		Claim.AgentId = 1;
		Claim.Resource = FTrafficResource::OfNode(NodeY);
		Claim.bOccupied = false;
		FTrafficClaim Blocker;
		Occupancy.TryClaim(Claim, Blocker);
	}
	{
		FTrafficClaim Claim;
		Claim.AgentId = 2;
		Claim.Resource = FTrafficResource::OfNode(NodeX);
		Claim.bOccupied = false;
		FTrafficClaim Blocker;
		Occupancy.TryClaim(Claim, Blocker);
	}

	TArray<FRoadAgent> Agents;
	Agents.SetNum(2);
	Agents[0].Id = 1;
	Agents[0].WaitingOn = 2;
	Agents[0].BlockedResource = FTrafficResource::OfNode(NodeX);
	Agents[0].StalledSeconds = 10.0;
	Agents[1].Id = 2;
	Agents[1].WaitingOn = 1;
	Agents[1].BlockedResource = FTrafficResource::OfNode(NodeY);
	Agents[1].StalledSeconds = 10.0;

	FTrafficRules Rules; // StallSeconds 3, RetrySeconds 5 - both agents are well past both.
	FNodeReachCache Reach;
	FPlanReResolver PlanReResolver;

	// A bare network with nothing on it: the yield branch never reads it (it returns before
	// CanReplanAtBlockedStep or any guideline lookup), so this only has to exist.
	URoadNetwork* Network = NewObject<URoadNetwork>();

	FDeadlockResolver Resolver;
	Resolver.Resolve(Agents, *Network, Rules, Occupancy, Reach, PlanReResolver, /*SimSeconds=*/100.0);

	TestEqual(TEXT("one cycle detected"), Resolver.CyclesSeen.Num(), 1);
	TestEqual(TEXT("settled by a yield, not a replan"), Resolver.Yields, 1);

	// Tie-broken to the HIGHER id (both agents default to Aircraft, so ranks tie) - see
	// FDeadlockResolver::Resolve's ByYieldOrder sort.
	TestEqual(TEXT("the higher-id member yields"), Resolver.LastYieldedAgent, 2);

	TestFalse(TEXT("the yielder's reservation is gone"),
		Occupancy.FindClaim(2, FTrafficResource::OfNode(NodeX)) != nullptr);
	TestTrue(TEXT("the other member's reservation survives - it did not yield"),
		Occupancy.FindClaim(1, FTrafficResource::OfNode(NodeY)) != nullptr);

	return true;
}

// (b) FClaimPass, constructed bare, runs a non-Taxiing agent's claim pass with no
// UGroundTraffic - HoldRunwayOnly claims RunwayHeld into a table nothing else touched.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficSplitClaimPassStandaloneTest,
	"Airside.Model.Traffic.ClaimPassStandalone",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficSplitClaimPassStandaloneTest::RunTest(const FString& Parameters)
{
	FRoadSegmentId Strip;
	Strip.Index = 7;

	FRoadAgent Agent;
	Agent.Id = 1;
	Agent.Phase = EAgentPhase::Parked; // Non-Taxiing: Run takes the HoldRunwayOnly branch.
	Agent.RunwayHeld.Add(Strip);

	FTrafficRules Rules;
	FTrafficOccupancy Occupancy;
	FNodeReachCache Reach;
	URoadNetwork* Network = NewObject<URoadNetwork>();

	FClaimPass Pass{Rules, Occupancy, Reach};
	Pass.Run(Agent, *Network);

	int32 Holder = 0;
	TestTrue(TEXT("the runway segment is held"),
		Occupancy.IsHeld(FTrafficResource::OfSurface(Strip), 0, &Holder));
	TestEqual(TEXT("held by this agent"), Holder, 1);

	return true;
}

// (c) FPlanReResolver, constructed bare, refuses to replan an agent that is not Taxiing -
// the first guard in the full contract UGroundTraffic::ReplanAt documents - with no
// UGroundTraffic, no registry lookup, nothing but the struct and the agent.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficSplitPlanReResolverStandaloneTest,
	"Airside.Model.Traffic.PlanReResolverStandalone",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficSplitPlanReResolverStandaloneTest::RunTest(const FString& Parameters)
{
	FRoadAgent Agent;
	Agent.Id = 1;
	Agent.Phase = EAgentPhase::Parked; // Not Taxiing: ReplanAt's first guard refuses it.

	FTrafficRules Rules;
	FTrafficOccupancy Occupancy;
	URoadNetwork* Network = NewObject<URoadNetwork>();

	FPlanReResolver PlanReResolver;
	const bool bReplanned = PlanReResolver.ReplanAt(
		Agent, *Network, /*SpliceStep=*/0, FGuidelineEdgeId(), FGuidelineNodeId(), Rules, Occupancy);

	TestFalse(TEXT("a non-Taxiing agent is refused, with no UGroundTraffic involved"), bReplanned);

	return true;
}

#endif
