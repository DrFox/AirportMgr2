// What UGroundTraffic does when the guideline graph is rebuilt under its agents - spec
// 2026-09-06 §6. Re-pointing steps by position, splicing or truncating what no longer
// resolves, and SpliceReplan, the all-or-nothing search-and-splice both this and ReplanAt
// are built on. One class across four translation units; see Model/GroundTraffic.h for which
// file holds what, and RoadEditFacadeSurfaces.cpp for the precedent.

#include "Model/GroundTraffic.h"

#include "AirsideLog.h"
#include "Model/ArrivalPlanner.h"
#include "Model/RoadNetwork.h"

bool UGroundTraffic::SpliceReplan(const URoadNetwork& Network, const FRouteQuery& Query,
	int32 KeepSteps, FRoutePlan& Plan)
{
	const FRoutePlan Tail = RouteSearch::Find(Network, Query);
	if (!Tail.IsValid())
	{
		return false;
	}

	const FRoutePlan Spliced = RouteSearch::Splice(Plan, KeepSteps, Tail);
	if (!Spliced.IsValid())
	{
		return false;
	}

	// LAST, so a failure at either step above leaves Plan exactly as the caller handed it in.
	// That is the whole contract of this function, and writing the answer anywhere earlier
	// would break it for the splice case in particular - Splice returns an invalid plan when
	// its two halves do not meet, which is precisely when the old plan must survive.
	Plan = Spliced;
	return true;
}

void UGroundTraffic::OnGraphRebuilt(const URoadNetwork& Network)
{
	// Every node and edge the reach table named has just been freed. The revision check
	// would catch it on the next lookup; dropping it here says so where the rebuild is.
	NodeReach.Invalidate();

	int32 Considered = 0;
	int32 Replanned = 0;
	int32 Truncated = 0;
	int32 Stranded = 0;

	// BY INDEX rather than by range-for: ReplanAt, reached from ReResolvePlan below, re-finds
	// its agent by id and writes through its own reference into this same array. Nothing here
	// adds or removes an agent so a reference would in fact survive, but the index says so
	// without a reader having to go and check ReplanAt to find that out.
	for (int32 Index = 0; Index < Agents.Num(); ++Index)
	{
		FRoadAgent& Agent = Agents[Index];

		// EVERY WAITER RETRIES NOW, spec §5. An unresolvable deadlock is re-reported and
		// re-attempted once per Rules.RetrySeconds, and a graph rebuild is exactly the event
		// that may have added the edge out of it - the player has just built the bypass. Made
		// to wait out the rest of its retry window, the jam would clear seconds after the fix
		// rather than on the frame of it.
		Agent.LastResolveAttempt = -1.0e9;

		FRoutePlan* Plan = nullptr;
		int32 FromStep = 0;
		if (Agent.Phase == EAgentPhase::Taxiing && Agent.Follower.Plan.Steps.Num() > 0)
		{
			// FROM THE STEP THE AGENT IS ON. Its own from-node is re-pointed too - four
			// readers still ask for that one every tick - and only the steps behind THAT are
			// left with dead handles. See ReResolvePlan.
			Plan = &Agent.Follower.Plan;
			FromStep = CurrentStep(Agent.Follower.Plan, Agent.Follower.Travelled);
		}
		else if (Agent.Phase == EAgentPhase::Arriving && Agent.TaxiInPlan.Steps.Num() > 0)
		{
			// FROM 0, because not a metre of this one has been driven: it is the route the
			// aircraft will fly once it vacates, and it is just as dead after a rebuild as one
			// somebody is on. Left unresolved, an arrival would vacate onto freed handles and
			// stop dead on the runway exit.
			Plan = &Agent.TaxiInPlan;
		}

		// A PARKED AGENT'S GOAL IS WHERE IT STANDS, and the rebuild may have freed that node -
		// every derived one was. Re-pointed by position, or every later search from it (a
		// Depart, a stand re-offer) would start at a dead handle and fail for ever. The stand
		// pose node itself is authored and survives, so this changes nothing for an aircraft
		// parked on a stand.
		if (Agent.Phase == EAgentPhase::Parked && Network.GetGuidelineNode(Agent.GoalNode) == nullptr)
		{
			const FGuidelineNodeId Here = RouteSearch::FindNearestNode(
				Network, Agent.LastMotion.Position, Agent.Class, Rules.ResolveRadius);
			if (Here.IsSet())
			{
				Agent.GoalNode = Here;
			}
		}

		// Parked, Departing and Gone hold nothing anybody is about to drive over the graph
		// that just changed - a departure runs on FTakeoffRun and a parked agent's route is
		// finished - so they are left alone rather than re-resolved into a truncation of a
		// journey already made.
		if (Plan == nullptr || !Plan->IsValid())
		{
			continue;
		}

		++Considered;
		switch (ReResolvePlan(Agent, *Plan, FromStep, Network))
		{
		case EReResolve::Replanned: ++Replanned; break;
		case EReResolve::Truncated: ++Truncated; break;
		case EReResolve::Stranded:  ++Stranded;  break;
		case EReResolve::Intact:    break;
		}
	}

	// THE GUIDELINE CLAIMS ONLY - NOT Clear(), which would take the runway holds with them.
	// See the header, and FTrafficOccupancy::ReleaseGuidelineClaims for the reason the two
	// kinds part company here. AFTER the re-resolution and not before it, because the replans
	// above take the congestion cost, and claims on the guidelines that survived the rebuild
	// by handle (every hand-drawn one) are real queues a route out of a deleted taxiway
	// should still be steered around.
	Occupancy.ReleaseGuidelineClaims();

	// STAND CLAIMS COME BACK AT ONCE, not on the next tick: they are node claims, so the
	// release above dropped them, and the planner may be asked between now and the next
	// Advance - the player deletes a stand and presses 7 in the same breath. And a rebuild
	// may have ADDED a stand, so the re-offer pass asks for every waiter.
	for (FRoadAgent& Agent : Agents)
	{
		ClaimGoalNode(Agent, Network);
	}
	bStandsMayHaveFreed = true;

	// RE-RESOLVED EXCLUDES THE STRANDED. An agent whose ground was deleted was not
	// re-resolved into anything - it was given up on - and counting it in both columns made
	// the line read as though something had been salvaged.
	LastRebuild.ReResolved = Considered - Stranded;
	LastRebuild.Replanned = Replanned;
	LastRebuild.Truncated = Truncated;
	LastRebuild.Stranded = Stranded;

	UE_LOG(LogAirsideTraffic, Log,
		TEXT("Graph rebuilt: %d agents re-resolved, %d replanned, %d truncated, %d stranded"),
		LastRebuild.ReResolved, Replanned, Truncated, Stranded);
}

UGroundTraffic::EReResolve UGroundTraffic::ReResolvePlan(
	FRoadAgent& Agent, FRoutePlan& Plan, int32 FromStep, const URoadNetwork& Network)
{
	// WHOSE PLAN THIS IS, asked by address. The two callers hand in one of exactly two plans
	// and the difference matters twice below: only the follower's plan gets Replace (nothing
	// is following a TaxiInPlan yet), and only the follower's plan can be replanned through
	// ReplanAt. A bool parameter would say the same thing and could be passed wrongly; this
	// cannot be out of step with the reference it describes.
	const bool bDriving = (&Plan == &Agent.Follower.Plan);

	auto Strand = [this, &Agent, &Plan, bDriving](const TCHAR* Why)
	{
		// THE GROUND UNDER THE AGENT IS GONE. There is no line left to put it on and no node
		// to search from, so the plan is marked unreachable - which is what ClaimAhead and
		// FRouteFollower::HasArrived both read to stop asking anything of it - and the agent
		// gives back every GUIDELINE it holds, because holding lines it will never drive
		// would block whatever the player builds in their place.
		Plan.Result = ERouteResult::Unreachable;

		// AND KEEPS ITS RUNWAY. ReleaseAll was called here and was wrong: a rebuild does not
		// move an aeroplane, so an aircraft stranded mid-crossing is still standing on the
		// asphalt, and ArrivalPlanner::Plan reads this table directly at DispatchArrival,
		// BETWEEN ticks. Dropping the strip claim showed the runway free for as long as it
		// took the player to click - the window Task 7 closed, re-opened by the one path that
		// stops the agent re-claiming on its next tick. See ReleaseGuidelineClaimsOf.
		Occupancy.ReleaseGuidelineClaimsOf(Agent.Id);

		// THE CROSSING FIELDS SURVIVE TOO, and that is the same statement in the agent's own
		// state: CrossingPhase says the body is on a strip and CrossingRunway says which, and
		// neither has stopped being true because a route died. Cleared here (which is what
		// this did) they would have contradicted the claim that is now kept, and the log line
		// that announced the release would have been a log that lies. They are cleared
		// together with the claim by ClaimAhead's non-Taxiing branch once the agent parks.

		// A STRANDING IS FINAL, and that is a deliberate v1 limitation rather than an oversight.
		// Result is now Unreachable, so OnGraphRebuilt's own "!Plan->IsValid()" filter skips
		// this agent on every LATER rebuild: even if the player rebuilds the very pavement
		// that was deleted, nothing re-resolves it and it never drives again. Accepted because
		// a stranded agent is by definition standing off any live line - there is no node
		// within Rules.ResolveRadius of it - so "put it back" would mean choosing a place to
		// teleport it to, and the honest answer is that the player retires it. The Warning
		// above is what tells them there is something to retire.
		//
		// AND IT NAMES THE RUNWAY WHEN THERE IS ONE. An agent stranded mid-crossing holds
		// that strip until the player retires it, which is a runway out of service with no
		// other evidence anywhere: the claim is visible only to the arbiter, and the next
		// refused landing says "the runway is in use" without saying who by.
		const FString Held = Agent.CrossingRunway.IsSet()
			? FString::Printf(TEXT(" - and it is on runway segment %d, which it holds until it is retired"),
				Agent.CrossingRunway.Index)
			: FString();
		UE_LOG(LogAirsideTraffic, Warning, TEXT("Agent %d stranded by the rebuild: %s%s"),
			Agent.Id, Why, *Held);

		// A STRANDED TAXI-IN STILL HAS A PLACE: the exit node the landing hands over at, which
		// Plan.Start was re-pointed to when it could be. The re-offer (ReofferStands) searches
		// from GoalNode, so a dead handle here would leave a waiting aircraft waiting for ever.
		// Spec 2026-09-07-stand-occupancy §5, amended.
		if (!bDriving && Agent.bAwaitingStand && Plan.Start.IsSet())
		{
			Agent.GoalNode = Plan.Start;
		}
		return EReResolve::Stranded;
	};

	// Defensive, and both callers already check: this function indexes Polyline off step
	// vertices, and an out-of-range read here would be a crash in the middle of a rebuild.
	// STRANDED rather than Intact, because a caller bug counted as a clean re-resolution is
	// a defect that reports itself as success - and the summary line is where anybody would
	// look for it.
	if (Plan.Steps.Num() == 0 || FromStep < 0 || FromStep >= Plan.Steps.Num())
	{
		return Strand(TEXT("its plan is malformed: no steps, or a current step off the end of them"));
	}

	const double Radius = Rules.ResolveRadius;
	const int32 FromVertex = (FromStep == 0) ? 0 : Plan.Steps[FromStep - 1].EndVertex;
	if (!Plan.Polyline.IsValidIndex(FromVertex))
	{
		return Strand(TEXT("its polyline does not reach the step it is on"));
	}

	// THE NODE THE CURRENT STEP LEAVES FROM, found by where that node WAS. Everything below
	// walks forward from here, so this is the one lookup nothing can recover from.
	FGuidelineNodeId Prev = RouteSearch::FindNearestNode(Network, Plan.Polyline[FromVertex], Agent.Class, Radius);
	if (!Prev.IsSet())
	{
		return Strand(TEXT("no live node holds the position its current step starts from"));
	}

	// THE NODE THE AGENT IS DRIVING AWAY FROM IS RE-POINTED, and it is not optional: four
	// readers ask StepFromNode for it every tick - the crossing arm, the tail-node claim,
	// RankAt, and ReplanAt's own Query.Start when the failed step is the current one. See
	// this function's header for what each of them does with a dead handle. The last of those
	// is why this line has to come BEFORE the replan below rather than after it: without it
	// the search starts from a freed slot, returns NoStart, and the truncation that follows
	// writes that same dead handle into GoalNode.
	if (FromStep == 0)
	{
		Plan.Start = Prev;
	}
	else
	{
		Plan.Steps[FromStep - 1].To = Prev;
	}

	// The first step that could NOT be re-resolved, or the step count when every one could.
	int32 Failed = Plan.Steps.Num();
	for (int32 Step = FromStep; Step < Plan.Steps.Num(); ++Step)
	{
		const int32 EndVertex = Plan.Steps[Step].EndVertex;
		const FGuidelineNodeId Next = Plan.Polyline.IsValidIndex(EndVertex)
			? RouteSearch::FindNearestNode(Network, Plan.Polyline[EndVertex], Agent.Class, Radius)
			: FGuidelineNodeId();

		FGuidelineEdgeId Rejoined;
		bool bReversed = false;
		if (Next.IsSet())
		{
			// THE EDGE IS IDENTIFIED BY ITS TWO ENDS, never by its geometry. Two guideline
			// nodes have at most one line between them that this class can mean, and matching
			// a Bezier control point across a rebuild would be a second evaluator of the very
			// thing that was just regenerated - see the guideline graph's "samples ONCE".
			for (const FGuidelineEdgeId Candidate : Network.GetOutgoingGuidelines(Prev, Agent.Class))
			{
				const FGuidelineEdge* Edge = Network.GetGuidelineEdge(Candidate);
				if (Edge == nullptr)
				{
					continue;
				}

				const FGuidelineNodeId Other = (Edge->A == Prev) ? Edge->B : Edge->A;
				if (Other == Next)
				{
					Rejoined = Candidate;

					// Re-derived from the LIVE edge and never carried over from the dead step:
					// the builder is free to have laid this one down A-to-B where the old one
					// ran B-to-A, and a stale flag would reverse the sampled points under an
					// agent that is already driving them.
					bReversed = (Edge->B == Prev);
					break;
				}
			}
		}

		if (!Rejoined.IsSet())
		{
			Failed = Step;
			break;
		}

		Plan.Steps[Step].Edge = Rejoined;
		Plan.Steps[Step].To = Next;
		Plan.Steps[Step].bReversed = bReversed;

		// EndDistance AND EndVertex ARE LEFT ALONE, here and in the truncation below. They
		// index the polyline the follower is walking, which the rebuild did not touch;
		// re-deriving them from the new edge's sampled length would move the step boundaries
		// out from under Travelled, which is the lateral jump every "no jump" test exists to
		// catch. The handles change; the geometry does not.
		Prev = Next;
	}

	// THE GOAL MOVES WITH THE GRAPH TOO. Every replan from here on - this one, and every
	// deadlock replan for the rest of the journey - searches to Agent.GoalNode, so a handle
	// left naming a freed slot would fail all of them, silently and for ever. Left alone when
	// the goal position no longer resolves: the truncation below then supplies one that does.
	const FGuidelineNodeId Goal = RouteSearch::FindNearestNode(Network, Plan.Polyline.Last(), Agent.Class, Radius);
	if (Goal.IsSet())
	{
		Agent.GoalNode = Goal;
	}

	// THE GOAL WAS A STAND AND THE STAND IS GONE - or a taxiway to it. An aircraft whose goal
	// no longer resolves, and that is not lined up for a runway, is retargeted at whichever
	// FREE stand is nearest the node it will replan from, BEFORE the replan below runs - so
	// the replan searches to a live stand rather than to a freed handle and truncates. No
	// stand: it is marked awaiting, and the truncation that follows gives it a node to wait
	// at. Spec 2026-09-07-stand-occupancy §5. Vehicles and departures keep M2's rules.
	if (!Goal.IsSet() && Agent.Class == ETraversalClass::Aircraft && !Agent.bDepartureArmed
		&& Failed < Plan.Steps.Num())
	{
		const FGuidelineNodeId ReplanFrom = StepFromNode(Plan, Failed);
		const FGuidelineNodeId NewStand = ArrivalPlanner::ChooseStand(
			Network, ReplanFrom, Agent.Airframe, &Occupancy, Agent.Id);
		if (NewStand.IsSet())
		{
			Agent.GoalNode = NewStand;
			Agent.bAwaitingStand = false;
			UE_LOG(LogAirsideTraffic, Log, TEXT("Agent %d: its stand is gone; retargeting to the stand at node %d"),
				Agent.Id, NewStand.Index);
		}
		else
		{
			Agent.bAwaitingStand = true;
			UE_LOG(LogAirsideTraffic, Warning, TEXT("Agent %d: its stand is gone and no free stand is reachable; it will wait"),
				Agent.Id);
		}
	}

	if (Failed == Plan.Steps.Num())
	{
		return EReResolve::Intact;
	}

	if (bDriving)
	{
		// THE STEP UNDER THE AGENT IS THE ONE THAT WENT: STRANDED IN PLACE, spec §6.2 - "only
		// an agent whose current step itself is gone is stranded in place".
		//
		// NOT REPLANNED, and this is the one case where a replan is worse than no plan at all.
		// ReplanAt keeps Travelled across the splice, which is what makes it seamless for a
		// failure AHEAD of the agent: the metres already driven still name the same points.
		// Splice at the CURRENT step and that stops being true - the same route distance is
		// re-read on new geometry - and the agent steps sideways by however far the two lines
		// differ, measured at 3310 uu on Airside.Model.Traffic.GraphRebuild's case 5 and
		// unbounded in principle. In the game that is a vehicle jumping across the apron the
		// instant a player deletes the taxiway under it.
		//
		// NOR TRUNCATED. Truncation keeps the longest prefix of the line that is still
		// pavement, and there is none: the failure is at the step the agent is standing on,
		// so the only node behind it is one it has already passed - the truncation would end
		// the route BEHIND the agent, which FRouteFollower reads as arrived and which is a
		// jump backwards for anything watching the position.
		//
		// So the agent stops where it is, keeps its runway claim if it has one, and the
		// player retires it. See Strand.
		if (Failed == FromStep)
		{
			return Strand(TEXT("the step it is driving on is gone - the pavement under it was deleted"));
		}

		// THROUGH ReplanAt, not through SpliceReplan directly: a plan under a moving follower
		// needs Travelled preserved, the reservations ahead dropped and the stall clock reset,
		// and that aftermath is the whole of what ReplanAt adds. The ban is UNSET - nothing
		// here knows of an edge that must be avoided, and the edge that failed is not in the
		// graph at all, so no search could pick it anyway.
		//
		// AHEAD OF THE AGENT ONLY, by the branch above: ReplanAt's own precondition is that
		// the splice is at or ahead of the step the agent is on, and it is now the caller
		// that guarantees the strict half of that rather than the callee that tolerates it.
		if (ReplanAt(Agent.Id, Network, Failed, FGuidelineEdgeId(), FGuidelineNodeId()))
		{
			return EReResolve::Replanned;
		}
	}
	else
	{
		FRouteQuery Query;
		Query.Start = StepFromNode(Plan, Failed);
		Query.Goal = Agent.GoalNode;
		Query.Class = Agent.Class;
		Query.Wingspan = Agent.Airframe.Wingspan;

		// The congestion term, as ReplanAt takes it: the guidelines that survived the rebuild
		// by handle - every hand-drawn one - still carry real queues, and a re-routed arrival
		// should be steered round them rather than into the back of one.
		Query.Occupancy = &Occupancy;
		Query.QueryingAgent = Agent.Id;
		Query.CongestionWeight = Rules.CongestionWeight;

		if (SpliceReplan(Network, Query, Failed, Plan))
		{
			// The destination has not moved - the splice ends where the old plan did - but the
			// last step's To is now a LIVE handle, and that is what a later replan searches to.
			Agent.GoalNode = Plan.Steps.Num() > 0 ? Plan.Steps.Last().To : Agent.GoalNode;

			UE_LOG(LogAirsideTraffic, Log,
				TEXT("Agent %d taxi-in replanned by the rebuild at step %d: %.0f uu"),
				Agent.Id, Failed, Plan.Length);
			return EReResolve::Replanned;
		}
	}

	if (Failed == 0)
	{
		// NOTHING SURVIVES TO KEEP. Truncating to zero steps is not a route at all - it has no
		// last step to take a length or a goal from. That is the stranded case by definition,
		// not a degenerate truncation dressed up as one.
		//
		// REACHED ONLY BY A TAXI-IN PLAN NOW, since a driving agent whose failure is at its
		// own step was stranded above and FromStep is 0 for the other caller. An ARRIVING
		// aircraft is not standing on its taxi-in route - it is on the runway, and nothing it
		// is driving has gone - so it gets its replan attempt first and is stranded only when
		// no route to the stand survives at all. That is why the strand-in-place rule is
		// written on the bDriving branch and not here.
		return Strand(TEXT("its very next step is gone and no route replaces it"));
	}

	// TRUNCATE. Neither the old route nor any new one reaches the goal, so the agent keeps the
	// longest prefix of its own line that is still pavement and drives to a stop at the end of
	// it. Stopping it dead where it stands was the rejected alternative: it would freeze an
	// aircraft mid-taxiway the moment a player deleted anything ahead of it, while the line
	// under it is perfectly good and leads somewhere.
	const int32 WasSteps = Plan.Steps.Num();
	Plan.Steps.SetNum(Failed);
	Plan.Polyline.SetNum(Plan.Steps.Last().EndVertex + 1);
	Plan.Length = Plan.Steps.Last().EndDistance;
	Agent.SetGoalFrom(Plan);

	if (bDriving)
	{
		// Replace, NOT Start (see ReplanAt): Travelled, Speed and Heading survive and the line
		// up to the new end is the same line. The argument ALIASES Follower.Plan, deliberately -
		// the truncation was applied in place - which Replace handles: TArray's assignment
		// guards self-assignment, and what this call is here for is the speed profile, rebuilt
		// so the agent brakes to the new end instead of running off it.
		Agent.Follower.Replace(Plan);
	}

	UE_LOG(LogAirsideTraffic, Log,
		TEXT("Agent %d truncated by the rebuild: %d of %d steps survive, %.0f uu to the last live node"),
		Agent.Id, Failed, WasSteps, Plan.Length);
	return EReResolve::Truncated;
}
