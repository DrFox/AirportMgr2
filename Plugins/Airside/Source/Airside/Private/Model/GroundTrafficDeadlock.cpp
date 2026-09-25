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
#include "Model/TrafficContext.h"

namespace
{
	// FDeadlockResolver has no id->agent index of its OWN - UGroundTraffic keeps the registry
	// (see GroundTraffic.h's file map) - but it is handed UGroundTraffic's map now (issue
	// #295), rather than searching the array by predicate the way UGroundTraffic::FindAgent/
	// FindIndex used to. See DeadlockResolver.h's own comment on Resolve's AgentIndex
	// parameter for why: these three helpers, and the Sort comparators below that used to
	// call the old FindByPredicate version of them directly, are where that scan was paid for
	// most - once per comparison, and Sort makes O(N log N) of them per cycle.
	const FRoadAgent* FindAgentIn(const TArray<FRoadAgent>& Agents, const TMap<int32, int32>& AgentIndex, int32 AgentId)
	{
		const int32* Index = AgentIndex.Find(AgentId);
		return Index != nullptr ? &Agents[*Index] : nullptr;
	}

	FRoadAgent* FindAgentIn(TArray<FRoadAgent>& Agents, const TMap<int32, int32>& AgentIndex, int32 AgentId)
	{
		const int32* Index = AgentIndex.Find(AgentId);
		return Index != nullptr ? &Agents[*Index] : nullptr;
	}

	int32 FindIndexIn(const TArray<FRoadAgent>& Agents, const TMap<int32, int32>& AgentIndex, int32 AgentId)
	{
		const int32* Index = AgentIndex.Find(AgentId);
		return Index != nullptr ? *Index : INDEX_NONE;
	}

	/**
	 * TraversalPriority of the id's agent, or 0 if AgentIndex cannot find it - the SAME
	 * fallback the yield-order sort already used (ByYieldOrder's own lambda, below), now
	 * shared with Candidates.Sort's comparator, which used to dereference FindAgentIn's
	 * result UNCHECKED (issue #295's review). Nothing removes an agent mid-Resolve() today,
	 * so the two comparators were never observed to differ - but a null-safe rank is one
	 * fewer thing a future change to that invariant would have to remember to preserve here.
	 */
	int32 RankOf(const TArray<FRoadAgent>& Agents, const TMap<int32, int32>& AgentIndex, int32 AgentId)
	{
		const FRoadAgent* Agent = FindAgentIn(Agents, AgentIndex, AgentId);
		return Agent != nullptr ? TraversalPriority(Agent->Class) : 0;
	}

	/**
	 * "id, id, id" for a log line, in Cycle's own order - skipping any id FindAgentIn cannot
	 * resolve, the same skip the loop that used to build this inline made (#190).
	 *
	 * CALLED ONLY FROM THE BRANCH THAT ACTUALLY LOGS. This used to be a member of the
	 * Candidates-gathering loop in FDeadlockResolver::Resolve, concatenated once per cycle
	 * every time one went bDue whether or not LogAirsideTraffic's runtime verbosity would
	 * keep the line - UE_LOG itself skips evaluating a suppressed call's arguments, but a
	 * string built in its own statement before the call is not one of those arguments, so it
	 * paid for the Printf/+= regardless. See each call site's own IsSuppressed guard.
	 */
	FString JoinCycleMembers(const TArray<int32>& Cycle, const TArray<FRoadAgent>& Agents, const TMap<int32, int32>& AgentIndex)
	{
		FString Out;
		for (const int32 Id : Cycle)
		{
			if (FindIndexIn(Agents, AgentIndex, Id) == INDEX_NONE)
			{
				continue;
			}
			Out += Out.IsEmpty() ? FString::Printf(TEXT("%d"), Id) : FString::Printf(TEXT(", %d"), Id);
		}
		return Out;
	}
}

void FDeadlockResolver::StampCycle(TArray<FRoadAgent>& Agents, const TArray<int32>& Cycle,
	const TMap<int32, int32>& AgentIndex, double SimSeconds)
{
	for (const int32 Id : Cycle)
	{
		const int32* Index = AgentIndex.Find(Id);
		if (Index != nullptr)
		{
			Agents[*Index].LastResolveAttempt = SimSeconds;
		}
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
	// CONTEXT IN PLACE OF Network, Rules AND Occupancy (issue #175) - see Model/TrafficContext.h.
	return PlanReResolver.ReplanAt(Agents[Index], SpliceStep, BannedEdge, BannedNode,
		FTrafficContext{Network, Rules, Occupancy, NodeReach, RunwayChains, SimSeconds});
}

bool FDeadlockResolver::CanReplanAtBlockedStep(const FRoadAgent& Agent, const URoadNetwork* Network,
	const FTrafficRules& Rules, FNodeReachCache& Reach) const
{
	if (Agent.Phase != EAgentPhase::Taxiing
		|| Agent.Follower.Speed >= KINDA_SMALL_NUMBER
		|| Agent.GetBlockedStep() < 0)
	{
		return false;
	}

	const FRoutePlan& Plan = Agent.Follower.Plan;
	if (Agent.GetBlockedStep() >= Plan.Steps.Num())
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
	const double ToNode = UGroundTraffic::StepStart(Plan, Agent.GetBlockedStep()) - Agent.Follower.Travelled;
	double Excess = 0.0;
	if (Network != nullptr && Agent.GetBlockedStep() > 0)
	{
		Excess = FClaimPass::ReachExcessAt(Rules, Reach, *Network,
			UGroundTraffic::StepFromNode(Plan, Agent.GetBlockedStep()),
			Plan.Steps[Agent.GetBlockedStep() - 1].Edge, Agent.Class);
	}
	return ToNode >= -KINDA_SMALL_NUMBER
		&& ToNode <= Rules.GapFor(Agent.Class) + Rules.FootprintFor(Agent.Class) * 0.5 + Excess;
}

void FDeadlockResolver::Resolve(TArray<FRoadAgent>& Agents, const TMap<int32, int32>& AgentIndex,
	const FTrafficContext& Context, FPlanReResolver& PlanReResolver)
{
	// REBOUND TO THE OLD NAMES (issue #175), so the whole body below - written, and read,
	// against Network/Rules/Occupancy/Reach/SimSeconds - is unchanged by the parameter object
	// that replaced four of Resolve's seven positional parameters. See Model/TrafficContext.h.
	const URoadNetwork& Network = Context.Network;
	const FTrafficRules& Rules = Context.Rules;
	FTrafficOccupancy& Occupancy = Context.Occupancy;
	FNodeReachCache& Reach = Context.Reach;
	const double SimSeconds = Context.SimSeconds;

	// ONE EDGE PER STALLED WAITER. StalledSeconds only accrues while an agent is Taxiing,
	// stopped and naming a blocker (see FRoadAgent::Advance's caller in AdvanceOnce), and
	// FClaimPass::Run clears WaitingOn the moment an agent stops taxiing - so a parked or
	// retired agent cannot contribute an edge, and a cycle through one is not representable
	// rather than merely unlikely.
	//
	// MEMBER, NOT A LOCAL (issue #190) - see the header. Reset here, not left with whatever
	// the last Resolve() call found.
	Waiting.Reset();
	for (const FRoadAgent& Agent : Agents)
	{
		if (Agent.StalledSeconds > Rules.StallSeconds && Agent.GetWaitingOn() != 0)
		{
			Waiting.Add(Agent.Id, Agent.GetWaitingOn());
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
	//
	// MEMBER, NOT A LOCAL (issue #190) - see the header.
	Visited.Reset();

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
		//
		// MEMBERS, NOT LOCALS (issue #190) - see the header. Reset per waiter walked, not
		// per Resolve() call: a cycle's path means nothing outside the walk that built it.
		Path.Reset();
		Position.Reset();
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
			const FRoadAgent* Member = FindAgentIn(Agents, AgentIndex, Id);
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
			const FRoadAgent* Member = FindAgentIn(Agents, AgentIndex, Id);
			const FTrafficClaim* Blocking = Member != nullptr
				? Occupancy.FindClaim(Member->GetWaitingOn(), Member->GetBlockedResource()) : nullptr;
			// A blocker nobody can find is treated as standing there: the replan path is the
			// conservative one, and a stale refusal must not be able to talk the resolver
			// into releasing anything.
			bAllReservations = bAllReservations && Blocking != nullptr && !Blocking->bOccupied;
		}
		const double* LastYield = YieldedAt.Find(Key);
		if (bAllReservations && (LastYield == nullptr || *LastYield < SimSeconds - Rules.RetrySeconds))
		{
			TArray<int32> ByYieldOrder = Cycle;
			ByYieldOrder.Sort([&Agents, &AgentIndex](const int32 A, const int32 B)
			{
				const int32 RankA = RankOf(Agents, AgentIndex, A);
				const int32 RankB = RankOf(Agents, AgentIndex, B);
				return RankA != RankB ? RankA < RankB : A > B;
			});
			const int32 Yielder = ByYieldOrder[0];

			// ONE FUNCTION, NOT A HAND-COPIED LOOP (issue #295) - see StampCycle's own comment;
			// this is the twin of the identical loop the replan branch below runs.
			StampCycle(Agents, Cycle, AgentIndex, SimSeconds);

			Occupancy.ReleaseReservations(Yielder);
			YieldedAt.Add(Key, SimSeconds);
			++Yields;
			LastYieldedAgent = Yielder;

			// DEFERRED TO HERE, AND ONLY IF THE LINE WILL ACTUALLY PRINT (#190) - see
			// JoinCycleMembers' own comment. Unlike that helper, this one keeps every Cycle id
			// unconditionally: StampCycle's own skip is silent (a missing agent is simply not
			// stamped), so neither did the concatenation it used to share a loop with.
			if (!LogAirsideTraffic.IsSuppressed(ELogVerbosity::Log))
			{
				FString YieldMembers;
				for (const int32 Id : Cycle)
				{
					YieldMembers += YieldMembers.IsEmpty()
						? FString::Printf(TEXT("%d"), Id) : FString::Printf(TEXT(", %d"), Id);
				}
				UE_LOG(LogAirsideTraffic, Log, TEXT("Reservation cycle among agents [%s]: agent %d yields its reservations"),
					*YieldMembers, Yielder);
			}
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
		for (const int32 Id : Cycle)
		{
			const FRoadAgent* Member = FindAgentIn(Agents, AgentIndex, Id);
			if (Member == nullptr)
			{
				// A MEMBER NOBODY CAN FIND IS NOT AN AIRCRAFT. The flag raises the line to
				// Warning because an all-aircraft cycle is a DESIGN problem the player must
				// be told about, and a cycle it could not read the classes of is not something
				// this can claim - a lookup miss must not be able to promote the line.
				bAllAircraft = false;
				continue;
			}
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
			if (Member->GetBlockedResource().Kind == ETrafficResourceKind::Surface
				&& Network.IsGuidelineNodeOnRunway(Member->GoalNode, Member->GetBlockedResource().Surface))
			{
				continue;
			}

			Candidates.Add(Id);
		}
		// RankOf, NOT A BARE FindAgentIn(...)->Class (issue #295's review): every id in
		// Candidates was found once already, above, but Sort's own comparator used to redo the
		// lookup UNCHECKED - the one place in this file that dereferenced FindAgentIn's result
		// with no null test at all. RankOf is the same null-safe fallback ByYieldOrder's own
		// comparator already used, so the two sorts stop disagreeing about what "no agent" ranks as.
		Candidates.Sort([&Agents, &AgentIndex](const int32 A, const int32 B)
		{
			const int32 RankA = RankOf(Agents, AgentIndex, A);
			const int32 RankB = RankOf(Agents, AgentIndex, B);
			return RankA != RankB ? RankA < RankB : A > B;
		});

		// STAMPED ON EVERY MEMBER, WHATEVER HAPPENS NEXT, and before the replan rather than
		// after it: the stamp is what schedules the next attempt, and a cycle whose replan
		// fails must not be re-tried on the very next tick for ever. A successful replan
		// clears the stalled member's clock anyway (see FPlanReResolver::ReplanAt). See
		// StampCycle - the twin of the yield branch's own call, above.
		StampCycle(Agents, Cycle, AgentIndex, SimSeconds);

		bool bResolved = false;
		int32 Candidate = 0;
		for (const int32 Id : Candidates)
		{
			// READ BEFORE THE REPLAN, because ReplanAt rewrites the plan and clears
			// BlockedStep - the ban would be read off the new plan otherwise, banning an edge
			// of the route that was just chosen.
			FRoadAgent* Turner = FindAgentIn(Agents, AgentIndex, Id);
			const int32 Step = Turner->GetBlockedStep();
			const FGuidelineEdgeId BannedEdge = Turner->Follower.Plan.Steps[Step].Edge;

			// BAN WHAT REFUSED IT. A node with an aircraft standing on it is a wall from every
			// direction, so the whole node goes; an edge or a runway surface bans the step's
			// edge (and every replan already refuses runway-derived edges - see ReplanAt).
			const FGuidelineNodeId BannedNode =
				Turner->GetBlockedResource().Kind == ETrafficResourceKind::Node
					? Turner->GetBlockedResource().Node : FGuidelineNodeId();

			if (PlanReResolver.ReplanAt(*Turner, Step, BannedEdge, BannedNode, Context))
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
		//
		// Members IS BUILT HERE, PER BRANCH, NOT ONCE ABOVE (#190) - see JoinCycleMembers' own
		// comment. Three branches, two verbosities, so each guards on the ONE it is about to
		// log at rather than a single check that could not speak for both.
		if (bResolved)
		{
			if (bAllAircraft)
			{
				if (!LogAirsideTraffic.IsSuppressed(ELogVerbosity::Warning))
				{
					UE_LOG(LogAirsideTraffic, Warning, TEXT("All-aircraft deadlock among agents [%s] resolved: agent %d replans"),
						*JoinCycleMembers(Cycle, Agents, AgentIndex), Candidate);
				}
			}
			else
			{
				if (!LogAirsideTraffic.IsSuppressed(ELogVerbosity::Log))
				{
					UE_LOG(LogAirsideTraffic, Log, TEXT("Deadlock among agents [%s] resolved: agent %d replans"),
						*JoinCycleMembers(Cycle, Agents, AgentIndex), Candidate);
				}
			}
		}
		else
		{
			if (!LogAirsideTraffic.IsSuppressed(ELogVerbosity::Warning))
			{
				UE_LOG(LogAirsideTraffic, Warning, TEXT("%sDeadlock among agents [%s]: no member can turn; retrying in %.0f s"),
					bAllAircraft ? TEXT("All-aircraft ") : TEXT(""), *JoinCycleMembers(Cycle, Agents, AgentIndex), Rules.RetrySeconds);
			}
		}

		++DeadlockLogLines;

		// COUNTED WHEN LOGGED, keyed by the lowest member id: the same ring re-formed later is
		// the same jam to a player reading the log. The KEY is why this set has one entry per
		// jam however many lines the jam produced; DeadlockLogLines above is the line count,
		// and the two answer different questions.
		CyclesSeen.Add(Key);
	}
}
