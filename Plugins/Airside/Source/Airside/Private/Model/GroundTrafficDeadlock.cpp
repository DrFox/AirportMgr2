// UGroundTraffic's deadlock resolver and the replan it reaches for - spec 2026-09-06 §4, §5.
// The wait-for graph, its cycles, the per-cycle retry window, and ReplanAt, which is the one
// thing the resolver and a graph rebuild share. One class across four translation units; see
// Model/GroundTraffic.h for which file holds what, and RoadEditFacadeSurfaces.cpp for the
// precedent.

#include "Model/GroundTraffic.h"

#include "AirsideLog.h"
#include "Model/RoadNetwork.h"

bool UGroundTraffic::ReplanAt(int32 AgentId, const URoadNetwork& Network, int32 SpliceStep, FGuidelineEdgeId BannedEdge)
{
	const int32 Index = FindIndex(AgentId);
	if (Index == INDEX_NONE)
	{
		return false;
	}

	FRoadAgent& Agent = Agents[Index];
	const FRoutePlan& Plan = Agent.Follower.Plan;

	// EVERY GUARD BEFORE ANYTHING IS WRITTEN, and that is the whole shape of this function:
	// the agent is not touched until both the search and the splice have succeeded, so a
	// refusal at any of them leaves it driving exactly the plan it had. An implementation
	// that replaced the follower first and repaired afterwards would leave a stuck agent
	// worse off than before it asked.
	if (Agent.Phase != EAgentPhase::Taxiing || !Plan.IsValid()
		|| SpliceStep < 0 || SpliceStep > Plan.Steps.Num())
	{
		return false;
	}

	// NEVER BEHIND THE AGENT. Travelled is preserved across the splice (that is the point of
	// Replace), so splicing at a step the agent has already driven past re-maps the same
	// route distance onto a DIFFERENT polyline - the agent would appear somewhere else on
	// the airport in one frame, which is the lateral teleport every "no jump" test exists to
	// catch. Refused rather than clamped: a caller asking to replan behind the agent has the
	// wrong node, and quietly moving its splice point would hide that.
	if (SpliceStep < CurrentStep(Plan, Agent.Follower.Travelled))
	{
		UE_LOG(LogAirsideTraffic, Warning,
			TEXT("ReplanAt %d refused: splice step %d is behind the agent, which is on step %d"),
			AgentId, SpliceStep, CurrentStep(Plan, Agent.Follower.Travelled));
		return false;
	}

	FRouteQuery Query;
	Query.Start = StepFromNode(Plan, SpliceStep);
	Query.Goal = Agent.GoalNode;
	Query.Class = Agent.Class;
	Query.Wingspan = Agent.Airframe.Wingspan;
	Query.BannedEdge = BannedEdge;

	// THE COST TERM IS THE POINT OF REPLANNING, not the ban. The ban removes the one edge
	// the caller knows is hopeless; the congestion cost is what stops the new route from
	// being the next queue along, which a plain shortest path would walk straight into.
	Query.Occupancy = &Occupancy;
	Query.QueryingAgent = AgentId;
	Query.CongestionWeight = Rules.CongestionWeight;

	// A COPY, because SpliceReplan writes in place and this function promises the agent keeps
	// the plan it had until BOTH the search and the splice have succeeded - see the guard
	// paragraph above. The copy is the price of that promise, paid once per replan.
	FRoutePlan Spliced = Plan;
	if (!SpliceReplan(Network, Query, SpliceStep, Spliced))
	{
		return false;
	}

	// Read before Replace overwrites the plan under the reference, so the log line compares
	// the two journeys rather than one journey against itself.
	const double WasRemaining = Plan.Length - Agent.Follower.Travelled;

	// Replace, NOT Start: the line up to the splice is unchanged and the agent is part way
	// along it, so Travelled, Speed and Heading all survive. See FRouteFollower::Replace.
	Agent.Follower.Replace(Spliced);

	// THE RESERVATIONS, AND ONLY THOSE. They were made for a route that no longer exists past
	// the splice, so holding them would block the line the agent has just been re-routed away
	// from, for a journey nobody is making. What the agent is STANDING on is a different
	// thing entirely and survives: this was ReleaseAll, and a replanned aircraft standing on
	// a runway then showed the strip free to ArrivalPlanner for the frame before the next
	// Arbitrate - long enough to clear a landing onto it.
	//
	// THAT FRAME IS REAL AND IT IS SAFE. The resolver runs at the END of Advance, so the
	// agent holds no reservations until the next tick's claim pass, and in that window a
	// higher-ranked agent may take the line ahead of it. It is safe because the agent is by
	// construction STOPPED - it is a deadlocked waiter - and because the ground under it is
	// still claimed, so nobody can be granted a node or a strip its body is on. The rejected
	// alternative, re-claiming here, would be a second claim pass outside Arbitrate's order:
	// this agent would claim after everyone had moved, which is exactly the interleaving
	// Advance's header refuses.
	//
	// A stale OCCUPIED claim on an edge the new plan does not use is dropped by the next
	// ClaimAhead's ReleaseExcept, which keeps only what was asked for this pass.
	Occupancy.ReleaseReservations(AgentId);

	// The wait is over BY CONSTRUCTION - the thing it was waiting for is not on its route
	// any more - so the arbitration fields say so at once rather than a tick later. The
	// stall clock resets with them, or the deadlock pass that asked for this replan would
	// see the same stalled agent again on the very next tick and ask again.
	Agent.WaitingOn = 0;
	Agent.BlockedStep = INDEX_NONE;
	Agent.StalledSeconds = 0.0;

	// CrossingRunway AND CrossingPhase ARE DELIBERATELY LEFT ALONE. Between them they say the
	// agent's body is physically on a strip, which is a fact about where the aeroplane IS,
	// not about where it is going: a replan cannot move it off the runway, and clearing them
	// here would hand the strip back with an aeroplane standing on it. ClaimAhead's body
	// geometry ends the crossing, and it reads the SPLICED plan from the next tick on, which
	// is the same line up to the splice - so the tail-clear test it makes is the one it would
	// have made anyway.

	UE_LOG(LogAirsideTraffic, Log, TEXT("Agent %d replanned at step %d: %.0f uu remaining -> %.0f"),
		AgentId, SpliceStep, WasRemaining, Spliced.Length - Agent.Follower.Travelled);
	return true;
}

bool UGroundTraffic::CanReplanAtBlockedStep(const FRoadAgent& Agent) const
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
	const double ToNode = StepStart(Plan, Agent.BlockedStep) - Agent.Follower.Travelled;
	return ToNode >= -KINDA_SMALL_NUMBER
		&& ToNode <= Rules.GapFor(Agent.Class) + Rules.FootprintFor(Agent.Class) * 0.5;
}

void UGroundTraffic::ResolveDeadlocks(const URoadNetwork& Network)
{
	// ONE EDGE PER STALLED WAITER. StalledSeconds only accrues while an agent is Taxiing,
	// stopped and naming a blocker (see Advance), and ClaimAhead clears WaitingOn the moment
	// an agent stops taxiing - so a parked or retired agent cannot contribute an edge, and a
	// cycle through one is not representable rather than merely unlikely.
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
			const FRoadAgent* Member = FindAgent(Id);
			bDue = bDue || (Member != nullptr && Member->LastResolveAttempt <= SimSeconds - Rules.RetrySeconds);
		}
		if (!bDue)
		{
			continue;
		}

		// THE LOWEST-RANKED WAITER THAT CAN ACTUALLY TURN. Lowest class priority first, so a
		// van goes round rather than an aeroplane; ties to the HIGHEST id, which is the later
		// arrival - the one with least of its journey already made.
		int32 Candidate = 0;
		int32 CandidateRank = 0;
		bool bAllAircraft = true;
		FString Members;
		for (const int32 Id : Cycle)
		{
			const FRoadAgent* Member = FindAgent(Id);
			if (Member == nullptr)
			{
				continue;
			}
			Members += Members.IsEmpty() ? FString::Printf(TEXT("%d"), Id) : FString::Printf(TEXT(", %d"), Id);
			bAllAircraft = bAllAircraft && Member->Class == ETraversalClass::Aircraft;

			if (!CanReplanAtBlockedStep(*Member))
			{
				continue;
			}
			const int32 Rank = TraversalPriority(Member->Class);
			if (Candidate == 0 || Rank < CandidateRank || (Rank == CandidateRank && Id > Candidate))
			{
				Candidate = Id;
				CandidateRank = Rank;
			}
		}

		// STAMPED ON EVERY MEMBER, WHATEVER HAPPENS NEXT, and before the replan rather than
		// after it: the stamp is what schedules the next attempt, and a cycle whose replan
		// fails must not be re-tried on the very next tick for ever. A successful replan
		// clears the stalled member's clock anyway (see ReplanAt).
		for (const int32 Id : Cycle)
		{
			const int32 Index = FindIndex(Id);
			if (Index != INDEX_NONE)
			{
				Agents[Index].LastResolveAttempt = SimSeconds;
			}
		}

		bool bResolved = false;
		if (Candidate != 0)
		{
			// READ BEFORE THE REPLAN, because ReplanAt rewrites the plan and clears
			// BlockedStep - the ban would be read off the new plan otherwise, banning an edge
			// of the route that was just chosen.
			const FRoadAgent* Turner = FindAgent(Candidate);
			const int32 Step = Turner->BlockedStep;
			const FGuidelineEdgeId Banned = Turner->Follower.Plan.Steps[Step].Edge;
			bResolved = ReplanAt(Candidate, Network, Step, Banned);
			if (bResolved)
			{
				LastResolvedAgent = Candidate;
			}
		}

		// ALL-AIRCRAFT CYCLES ARE A DESIGN PROBLEM, NOT A TRAFFIC ONE - the input to the
		// build-tool warning of the systems map §6 - so they are raised to Warning whatever
		// the outcome. A cycle nobody can break is a Warning either way.
		if (bResolved)
		{
			if (bAllAircraft)
			{
				UE_LOG(LogAirsideTraffic, Warning, TEXT("All-aircraft Deadlock among agents [%s] resolved: agent %d replans"),
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
		int32 Key = Cycle[0];
		for (const int32 Id : Cycle)
		{
			Key = FMath::Min(Key, Id);
		}
		CyclesSeen.Add(Key);
	}
}
