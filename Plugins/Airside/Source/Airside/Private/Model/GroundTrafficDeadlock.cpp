// FDeadlockResolver: the wait-for graph, its cycles, the per-cycle retry window - spec
// 2026-09-06 §4, §5.
//
// PULLED OFF UGroundTraffic BY ISSUE #84: this was UGroundTraffic::ResolveDeadlocks and
// UGroundTraffic::CanReplanAtBlockedStep, along with the six bookkeeping fields
// (CyclesSeen..DeadlockLogLines) that are FDeadlockResolver's own public fields now - see
// Model/GroundTraffic.h for the struct itself and why those fields are public. UGroundTraffic
// keeps ONE forwarder, ReplanAt (below), because it is public API other modules call by
// AgentId; everything that used to be UGroundTraffic::ResolveDeadlocks is FDeadlockResolver::
// Resolve, which AdvanceOnce calls directly - no forwarder needed for a private method with
// no outside caller.

#include "Model/GroundTraffic.h"

#include "AirsideLog.h"
#include "Model/RoadNetwork.h"
#include "Model/TrafficClaims.h"

namespace
{
	// FDeadlockResolver has no id->agent index of its own - UGroundTraffic keeps the
	// registry (see GroundTraffic.h's file map) - so Resolve is handed the array directly
	// and searches it exactly as UGroundTraffic::FindAgent/FindIndex do.
	const FRoadAgent* FindAgentIn(const TArray<FRoadAgent>& Agents, int32 AgentId)
	{
		return Agents.FindByPredicate([AgentId](const FRoadAgent& A) { return A.Id == AgentId; });
	}

	FRoadAgent* FindAgentIn(TArray<FRoadAgent>& Agents, int32 AgentId)
	{
		return Agents.FindByPredicate([AgentId](const FRoadAgent& A) { return A.Id == AgentId; });
	}

	int32 FindIndexIn(const TArray<FRoadAgent>& Agents, int32 AgentId)
	{
		return Agents.IndexOfByPredicate([AgentId](const FRoadAgent& A) { return A.Id == AgentId; });
	}
}

bool UGroundTraffic::ReplanAt(int32 AgentId, const URoadNetwork& Network, int32 SpliceStep,
	FGuidelineEdgeId BannedEdge, FGuidelineNodeId BannedNode)
{
	// THE WHOLE MECHANISM IS FPlanReResolver::ReplanAt NOW (issue #84) - this stays only
	// because it is public API ReofferStands, tests and AirportOps call by AgentId; the full
	// contract (guards, PRECONDITION, the two rejected alternatives) is documented on this
	// declaration in GroundTraffic.h, unchanged, because that is what a caller reads.
	const int32 Index = FindIndex(AgentId);
	if (Index == INDEX_NONE)
	{
		return false;
	}
	return PlanReResolver.ReplanAt(Agents[Index], Network, SpliceStep, BannedEdge, BannedNode, Rules, Occupancy);
}

bool FDeadlockResolver::CanReplanAtBlockedStep(const FRoadAgent& Agent, const URoadNetwork* Network,
	const FTrafficRules& Rules, FNodeReachCache& Reach) const
{
	if (Agent.Phase != EAgentPhase::Taxiing
		|| Agent.Follower.Speed >= KINDA_SMALL_NUMBER
		|| Agent.BlockedStep < 0)
	{
		return false;
	}

	const FRoutePlan& Plan = Agent.Follower.Plan;
	if (Agent.BlockedStep >= Plan.Steps.Num())
	{
		// A BlockedStep that outran its plan - a replan between the refusal and here. Nothing
		// to ban and no node to turn at, so this agent is not a candidate this window.
		return false;
	}

	// AT THE NODE THE REFUSED STEP LEAVES FROM, which is where the alternatives are. Negative
	// means the agent is INSIDE the edge it was refused, and taking another edge out of that
	// node would mean reversing - out of M2 by spec §1. The upper bound is where the agent is
	// actually allowed to stop: the box-entry rule parks it a GAP short of the box's start,
	// and Travelled is the CENTRE, so its nose is half a footprint further on again.
	//
	// AND THE NODE'S REACH along the edge it is arriving on: a refusal at a node whose
	// lines part late parks the agent that much further back again (StopWithinFor), and
	// measured against the plain tolerance a waiter at a stand's join was never a
	// candidate for the resolver that exists to turn it round.
	const double ToNode = UGroundTraffic::StepStart(Plan, Agent.BlockedStep) - Agent.Follower.Travelled;
	double Excess = 0.0;
	if (Network != nullptr && Agent.BlockedStep > 0)
	{
		Excess = FClaimPass::ReachExcessAt(Rules, Reach, *Network,
			UGroundTraffic::StepFromNode(Plan, Agent.BlockedStep),
			Plan.Steps[Agent.BlockedStep - 1].Edge, Agent.Class);
	}
	return ToNode >= -KINDA_SMALL_NUMBER
		&& ToNode <= Rules.GapFor(Agent.Class) + Rules.FootprintFor(Agent.Class) * 0.5 + Excess;
}

void FDeadlockResolver::Resolve(TArray<FRoadAgent>& Agents, const URoadNetwork& Network,
	const FTrafficRules& Rules, FTrafficOccupancy& Occupancy, FNodeReachCache& Reach,
	FPlanReResolver& PlanReResolver, double SimSeconds)
{
	// ONE EDGE PER STALLED WAITER. StalledSeconds only accrues while an agent is Taxiing,
	// stopped and naming a blocker (see FRoadAgent::Advance's caller in AdvanceOnce), and
	// FClaimPass::Run clears WaitingOn the moment an agent stops taxiing - so a parked or
	// retired agent cannot contribute an edge, and a cycle through one is not representable
	// rather than merely unlikely.
	TMap<int32, int32> Waiting;
	for (const FRoadAgent& Agent : Agents)
	{
		if (Agent.StalledSeconds > Rules.StallSeconds && Agent.WaitingOn != 0)
		{
			Waiting.Add(Agent.Id, Agent.WaitingOn);
		}
	}
	if (Waiting.Num() == 0)
	{
		return;
	}

	// Every agent whose walk has been made, this tick. Out-degree is one, so the walk from an
	// agent already walked would follow the identical chain and find the identical cycle:
	// skipping it is what makes this pass linear rather than quadratic, and what stops one
	// cycle being handled once per member.
	TSet<int32> Visited;

	for (const TPair<int32, int32>& Start : Waiting)
	{
		if (Visited.Contains(Start.Key))
		{
			continue;
		}

		// THE WALK IS BOUNDED BY CONSTRUCTION: every step either stops or appends an agent
		// not already on the path, and Waiting is finite - so no counter is needed and none
		// is used, which is better than a guard whose limit would be a second rule about how
		// big a jam may be. Position is the path membership test and the cycle's start index
		// in one, because the cycle is the path FROM the revisited agent onward, not all of it
		// (an agent can wait on a jam it is not part of).
		TArray<int32> Path;
		TMap<int32, int32> Position;
		int32 At = Start.Key;
		int32 CycleAt = INDEX_NONE;
		while (true)
		{
			if (const int32* Where = Position.Find(At))
			{
				CycleAt = *Where;
				break;
			}
			if (Visited.Contains(At))
			{
				break;
			}
			const int32* Next = Waiting.Find(At);
			if (Next == nullptr)
			{
				// Waiting on somebody who is not a stalled waiter - a moving agent, or one
				// that has not been stopped long enough. That is a queue, not a deadlock.
				break;
			}
			Position.Add(At, Path.Num());
			Path.Add(At);
			At = *Next;
		}
		for (const int32 Walked : Path)
		{
			Visited.Add(Walked);
		}
		if (CycleAt == INDEX_NONE)
		{
			continue;
		}

		TArray<int32> Cycle;
		Cycle.Append(Path.GetData() + CycleAt, Path.Num() - CycleAt);

		// THE RETRY WINDOW, and it is what makes an unresolvable jam a CADENCE rather than a
		// line per tick. (What makes one jam count ONCE is the min-id key below, which is a
		// different mechanism - conflating the two is a comment defect this line was written
		// to fix.) A cycle is re-detected on every tick for as long as it lasts; nothing is
		// done - not even a log line - until some member has gone Rules.RetrySeconds without
		// an attempt. Some member and not every member, because a cycle that has just gained
		// a fresh waiter deserves the same retry the old members were already due.
		bool bDue = false;
		for (const int32 Id : Cycle)
		{
			const FRoadAgent* Member = FindAgentIn(Agents, Id);
			bDue = bDue || (Member != nullptr && Member->LastResolveAttempt <= SimSeconds - Rules.RetrySeconds);
		}
		if (!bDue)
		{
			continue;
		}

		// THE KEY: the lowest member id. Used twice below - once to remember a yield, once to
		// count the cycle - and computed once so the two cannot disagree.
		int32 Key = Cycle[0];
		for (const int32 Id : Cycle)
		{
			Key = FMath::Min(Key, Id);
		}

		// A CYCLE OF RESERVATIONS IS A YIELD, NOT A DEADLOCK. Spec §5, amended 2026-09-07.
		//
		// The wait-for graph does not know whether a blocker is STANDING on the contested
		// ground or has merely reserved it ahead of its nose, and the two are different
		// situations. Two aircraft closing on a 392 uu stub between two junctions from
		// opposite sides, each ~2000 uu short of it (samples/routing2.png): the first had
		// reserved the stub, the second had reserved the node at its far end - the node's
		// reach asks for it earlier than the window reaches the stub - and each was refused
		// the other's reservation. Nobody was in anybody's way. Read as a deadlock, the
		// resolver replanned the second round the whole taxiway loop, 135 km of taxi in place
		// of 16, when all it had to do was let go of a node it was not standing on.
		//
		// So: when EVERY refusal in the cycle is against a reservation, the member the replan
		// below would have chosen gives up its reservations instead. It keeps the ground its
		// body is on (ReleaseReservations, not ReleaseAll - see that function), and re-claims
		// on its next pass, by which time the other member has taken what it needed: the
		// yielder is the lowest-ranked, highest-id member, and Arbitrate asks higher ranks
		// first and lower ids first, so the others always claim before it does.
		//
		// GUARDED BY YieldedAt: a cycle that re-forms within a retry window of yielding is
		// one a yield could not settle - the other member is also refused something a third
		// party holds - and takes the replan path rather than yielding on every window.
		bool bAllReservations = true;
		for (const int32 Id : Cycle)
		{
			const FRoadAgent* Member = FindAgentIn(Agents, Id);
			const FTrafficClaim* Blocking = Member != nullptr
				? Occupancy.FindClaim(Member->WaitingOn, Member->BlockedResource) : nullptr;
			// A blocker nobody can find is treated as standing there: the replan path is the
			// conservative one, and a stale refusal must not be able to talk the resolver
			// into releasing anything.
			bAllReservations = bAllReservations && Blocking != nullptr && !Blocking->bOccupied;
		}
		const double* LastYield = YieldedAt.Find(Key);
		if (bAllReservations && (LastYield == nullptr || *LastYield < SimSeconds - Rules.RetrySeconds))
		{
			TArray<int32> ByYieldOrder = Cycle;
			ByYieldOrder.Sort([&Agents](const int32 A, const int32 B)
			{
				const FRoadAgent* AgentA = FindAgentIn(Agents, A);
				const FRoadAgent* AgentB = FindAgentIn(Agents, B);
				const int32 RankA = AgentA ? TraversalPriority(AgentA->Class) : 0;
				const int32 RankB = AgentB ? TraversalPriority(AgentB->Class) : 0;
				return RankA != RankB ? RankA < RankB : A > B;
			});
			const int32 Yielder = ByYieldOrder[0];

			FString YieldMembers;
			for (const int32 Id : Cycle)
			{
				const int32 Index = FindIndexIn(Agents, Id);
				if (Index != INDEX_NONE)
				{
					Agents[Index].LastResolveAttempt = SimSeconds;
				}
				YieldMembers += YieldMembers.IsEmpty() ? FString::Printf(TEXT("%d"), Id) : FString::Printf(TEXT(", %d"), Id);
			}

			Occupancy.ReleaseReservations(Yielder);
			YieldedAt.Add(Key, SimSeconds);
			++Yields;
			LastYieldedAgent = Yielder;
			UE_LOG(LogAirsideTraffic, Log, TEXT("Reservation cycle among agents [%s]: agent %d yields its reservations"),
				*YieldMembers, Yielder);
			++DeadlockLogLines;
			CyclesSeen.Add(Key);
			continue;
		}

		// EVERY WAITER THAT CAN ACTUALLY TURN, in the order they should be asked: lowest
		// class priority first, so a van goes round rather than an aeroplane; ties to the
		// HIGHEST id, which is the later arrival - the one with least of its journey already
		// made. A LIST rather than the single best, because the best candidate's replan can
		// fail - a departure at a bar has exactly one way onto the runway - and the first
		// version of this stopped there and logged "no member can turn" while the other
		// member had a whole taxiway system to turn into (PIE, 2026-09-06).
		TArray<int32> Candidates;
		bool bAllAircraft = true;
		FString Members;
		for (const int32 Id : Cycle)
		{
			const FRoadAgent* Member = FindAgentIn(Agents, Id);
			if (Member == nullptr)
			{
				// A MEMBER NOBODY CAN FIND IS NOT AN AIRCRAFT. The flag raises the line to
				// Warning because an all-aircraft cycle is a DESIGN problem the player must
				// be told about, and a cycle it could not read the classes of is not something
				// this can claim - a lookup miss must not be able to promote the line.
				bAllAircraft = false;
				continue;
			}
			Members += Members.IsEmpty() ? FString::Printf(TEXT("%d"), Id) : FString::Printf(TEXT(", %d"), Id);
			bAllAircraft = bAllAircraft && Member->Class == ETraversalClass::Aircraft;

			if (!CanReplanAtBlockedStep(*Member, &Network, Rules, Reach))
			{
				continue;
			}

			// AN AGENT REFUSED THE RUNWAY IT IS GOING TO cannot go round it. A departure at
			// a bar was the first candidate the replay tried (higher id, the tie-break), and
			// its "replan" was a detour through the junction's other arm to enter the same
			// strip from the other side - a different route, so ReplanAt accepted it, and the
			// cycle was logged resolved twice before the arrival, which had a whole taxiway
			// system to turn into, was asked. What it was refused is the destination; no ban
			// makes a route to it that avoids it.
			if (Member->BlockedResource.Kind == ETrafficResourceKind::Surface
				&& Network.IsGuidelineNodeOnRunway(Member->GoalNode, Member->BlockedResource.Surface))
			{
				continue;
			}

			Candidates.Add(Id);
		}
		Candidates.Sort([&Agents](const int32 A, const int32 B)
		{
			const int32 RankA = TraversalPriority(FindAgentIn(Agents, A)->Class);
			const int32 RankB = TraversalPriority(FindAgentIn(Agents, B)->Class);
			return RankA != RankB ? RankA < RankB : A > B;
		});

		// STAMPED ON EVERY MEMBER, WHATEVER HAPPENS NEXT, and before the replan rather than
		// after it: the stamp is what schedules the next attempt, and a cycle whose replan
		// fails must not be re-tried on the very next tick for ever. A successful replan
		// clears the stalled member's clock anyway (see FPlanReResolver::ReplanAt).
		for (const int32 Id : Cycle)
		{
			const int32 Index = FindIndexIn(Agents, Id);
			if (Index != INDEX_NONE)
			{
				Agents[Index].LastResolveAttempt = SimSeconds;
			}
		}

		bool bResolved = false;
		int32 Candidate = 0;
		for (const int32 Id : Candidates)
		{
			// READ BEFORE THE REPLAN, because ReplanAt rewrites the plan and clears
			// BlockedStep - the ban would be read off the new plan otherwise, banning an edge
			// of the route that was just chosen.
			FRoadAgent* Turner = FindAgentIn(Agents, Id);
			const int32 Step = Turner->BlockedStep;
			const FGuidelineEdgeId BannedEdge = Turner->Follower.Plan.Steps[Step].Edge;

			// BAN WHAT REFUSED IT. A node with an aircraft standing on it is a wall from every
			// direction, so the whole node goes; an edge or a runway surface bans the step's
			// edge (and every replan already refuses runway-derived edges - see ReplanAt).
			const FGuidelineNodeId BannedNode =
				Turner->BlockedResource.Kind == ETrafficResourceKind::Node
					? Turner->BlockedResource.Node : FGuidelineNodeId();

			if (PlanReResolver.ReplanAt(*Turner, Network, Step, BannedEdge, BannedNode, Rules, Occupancy))
			{
				bResolved = true;
				Candidate = Id;
				LastResolvedAgent = Id;
				break;
			}
		}

		// ALL-AIRCRAFT CYCLES ARE A DESIGN PROBLEM, NOT A TRAFFIC ONE - the input to the
		// build-tool warning of the systems map §6 - so they are raised to Warning whatever
		// the outcome. A cycle nobody can break is a Warning either way.
		if (bResolved)
		{
			if (bAllAircraft)
			{
				UE_LOG(LogAirsideTraffic, Warning, TEXT("All-aircraft deadlock among agents [%s] resolved: agent %d replans"),
					*Members, Candidate);
			}
			else
			{
				UE_LOG(LogAirsideTraffic, Log, TEXT("Deadlock among agents [%s] resolved: agent %d replans"),
					*Members, Candidate);
			}
		}
		else
		{
			UE_LOG(LogAirsideTraffic, Warning, TEXT("%sDeadlock among agents [%s]: no member can turn; retrying in %.0f s"),
				bAllAircraft ? TEXT("All-aircraft ") : TEXT(""), *Members, Rules.RetrySeconds);
		}

		++DeadlockLogLines;

		// COUNTED WHEN LOGGED, keyed by the lowest member id: the same ring re-formed later is
		// the same jam to a player reading the log. The KEY is why this set has one entry per
		// jam however many lines the jam produced; DeadlockLogLines above is the line count,
		// and the two answer different questions.
		CyclesSeen.Add(Key);
	}
}
