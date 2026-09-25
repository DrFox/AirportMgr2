// UGroundTraffic::OnGraphRebuilt (kept here: tick-order orchestration, not mechanism) and
// FPlanReResolver (issue #84) - the replan MECHANISM both this and the deadlock resolver
// share. Spec 2026-09-06 §6, and §4 for ReplanAt/SpliceReplan. See Model/GroundTraffic.h for
// which file holds what, and RoadEditFacadeSurfaces.cpp for the one-class-many-files precedent.

#include "Model/GroundTraffic.h"

#include "AirsideLog.h"
#include "Model/ArrivalPlanner.h"
#include "Model/RoadNetwork.h"
#include "Model/RoutePolicy.h"
#include "Model/TrafficClaims.h"
#include "Model/TrafficContext.h"
#include "Solve/GuidelineGeom.h"

bool FPlanReResolver::SpliceReplan(const URoadNetwork& Network, const FRouteQuery& Query,
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

bool FPlanReResolver::ReplanAt(FRoadAgent& Agent, int32 SpliceStep, FGuidelineEdgeId BannedEdge,
	FGuidelineNodeId BannedNode, const FTrafficContext& Context)
{
	// REBOUND TO THE OLD NAMES (issue #175) - see FDeadlockResolver::Resolve's identical
	// rebinding for why: the body below is unchanged from before Network, Rules and Occupancy
	// became one FTrafficContext parameter.
	const URoadNetwork& Network = Context.Network;
	const FTrafficRules& Rules = Context.Rules;
	FTrafficOccupancy& Occupancy = Context.Occupancy;

	const FRoutePlan& Plan = Agent.Follower.Plan;

	// EVERY GUARD BEFORE ANYTHING IS WRITTEN, and that is the whole shape of this function:
	// the agent is not touched until both the search and the splice have succeeded, so a
	// refusal at any of them leaves it driving exactly the plan it had. An implementation
	// that replaced the follower first and repaired afterwards would leave a stuck agent
	// worse off than before it asked.
	//
	// TAXIING AND NOT IsOnRoute(): a push is deliberately NOT replannable. There is no
	// alternative way off a stand - the lead-in is the only line - so a manoeuvring agent has
	// no move to make and the deadlock resolver must never pick one. That is exactly why
	// UGroundTraffic::DepartAgent grants the whole push up front instead of letting
	// arbitration stop it half way: clearance makes the manoeuvre atomic, which is what lets
	// this guard stay as narrow as it is.
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
	if (SpliceStep < UGroundTraffic::CurrentStep(Plan, Agent.Follower.Travelled))
	{
		UE_LOG(LogAirsideTraffic, Warning,
			TEXT("ReplanAt %d refused: splice step %d is behind the agent, which is on step %d"),
			Agent.Id, SpliceStep, UGroundTraffic::CurrentStep(Plan, Agent.Follower.Travelled));
		return false;
	}

	FRouteQuery Query = QueryFor(ERouteErrand::Replan,
		UGroundTraffic::StepFromNode(Plan, SpliceStep), Agent.GoalNode, Agent);
	Query.BannedEdge = BannedEdge;
	Query.BannedNode = BannedNode;

	// NEVER ALONG A RUNWAY SOMEBODY ELSE HOLDS. The first replan this resolver ever made in
	// play looped an arrival round a runway's end taxiway and back over a runway-derived
	// edge; that edge re-reserved the strip (spec §3.1, route one) against the departure
	// waiting at the bar, which was the very agent the loop was meant to get round. That
	// departure HELD the strip - a bar claim is a reservation on the chain - which is what
	// Held reads. The outright ban this replaced (All) also refused a free runway end as a
	// turnaround, and sent an aircraft round the whole taxiway loop past one it could have
	// used (samples/routing.png, 2026-09-07). Crossings are turn paths and nodes, not
	// runway edges, so they stay open either way. See ERunwayAvoidance.
	//
	// THE CHOICE NOW LIVES IN FRoutePolicy::For(Replan) and this paragraph is its
	// justification; the assignment that used to stand here would be a second source of
	// truth for it.

	// THE COST TERM IS THE POINT OF REPLANNING, not the ban. The ban removes the one edge
	// the caller knows is hopeless; the congestion cost is what stops the new route from
	// being the next queue along, which a plain shortest path would walk straight into.
	Query.WithCongestion(Occupancy, Agent.Id, Rules.CongestionWeight);

	// AND THE PENALTY FROM THE SAME RULES. Both replan errands allow a FREE runway end
	// (Held, not All), so the multiplier is what keeps one a last resort rather than a
	// shortcut. Read off the instance, so a level that tuned it is obeyed.
	Query.RunwayPenalty = Rules.RunwayPenalty;

	// A COPY, because SpliceReplan writes in place and this function promises the agent keeps
	// the plan it had until BOTH the search and the splice have succeeded - see the guard
	// paragraph above. The copy is the price of that promise, paid once per replan.
	FRoutePlan Spliced = Plan;
	if (!SpliceReplan(Network, Query, SpliceStep, Spliced))
	{
		return false;
	}

	// THE SAME ROUTE IS NOT A REPLAN. A ban that removes nothing the search wanted returns
	// the plan the agent already has, and accepting it would report a deadlock "resolved"
	// every retry window while nobody moved. Refused, so the resolver goes on to the next
	// candidate - which is how a bar-holder with only one way out hands the turn to the
	// aircraft that has two.
	bool bSameRoute = Spliced.Steps.Num() == Plan.Steps.Num();
	for (int32 StepIndex = 0; bSameRoute && StepIndex < Spliced.Steps.Num(); ++StepIndex)
	{
		bSameRoute = Spliced.Steps[StepIndex].Edge == Plan.Steps[StepIndex].Edge;
	}
	if (bSameRoute)
	{
		return false;
	}

	// Read before Replace overwrites the plan under the reference, so the log line compares
	// the two journeys rather than one journey against itself.
	const double WasRemaining = Plan.Length - Agent.Follower.Travelled;

	// Replace, NOT Start: the line up to the splice is unchanged and the agent is part way
	// along it, so Travelled, Speed and Heading all survive. See FRouteFollower::Replace.
	Agent.Follower.Replace(Spliced, Agent.Chassis());

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
	// FClaimPass::Run's ReleaseExcept, which keeps only what was asked for this pass.
	Occupancy.ReleaseReservations(Agent.Id);

	// The wait is over BY CONSTRUCTION - the thing it was waiting for is not on its route
	// any more - so the arbitration fields say so at once rather than a tick later. The
	// stall clock resets with them, or the deadlock pass that asked for this replan would
	// see the same stalled agent again on the very next tick and ask again.
	//
	// ClearArbitration() ALSO RESETS StopWithin, which this triple used to leave alone - a
	// change with no observable effect: nothing reads StopWithin between here and the next
	// tick's Arbitrate, which overwrites it for every Taxiing/Manoeuvring agent regardless
	// (issue #174 - this was the very triple ClearArbitration exists for, hand-typed a
	// fourth time because nothing had gone looking for other copies of it).
	Agent.ClearArbitration();
	Agent.ResetStall();

	// CrossingRunway AND CrossingPhase ARE DELIBERATELY LEFT ALONE. Between them they say the
	// agent's body is physically on a strip, which is a fact about where the aeroplane IS,
	// not about where it is going: a replan cannot move it off the runway, and clearing them
	// here would hand the strip back with an aeroplane standing on it. FClaimPass::Run's body
	// geometry ends the crossing, and it reads the SPLICED plan from the next tick on, which
	// is the same line up to the splice - so the tail-clear test it makes is the one it would
	// have made anyway.

	UE_LOG(LogAirsideTraffic, Log, TEXT("Agent %d replanned at step %d: %.0f uu remaining -> %.0f"),
		Agent.Id, SpliceStep, WasRemaining, Spliced.Length - Agent.Follower.Travelled);
	return true;
}

void UGroundTraffic::OnGraphRebuilt(const URoadNetwork& Network)
{
	// Every node and edge the reach table named has just been freed. The revision check
	// would catch it on the next lookup; dropping it here says so where the rebuild is.
	NodeReach.Invalidate();

	// SAME REASONING, for the chains a runway seed used to answer for (issue #170): a
	// rebuild is exactly the event GetEditRevision() exists to catch, and dropping it here
	// rather than waiting for the next Get/GetOrSeed says so where the rebuild is, same as
	// NodeReach above.
	RunwayChains.Invalidate();

	// ONE INDEX FOR THE WHOLE REBUILD (#172), built before any agent is touched: every
	// FindNearestNode call below - the parked-agent branch just past this loop's top, and
	// every one ReResolvePlan makes for the from-node, each remaining step, and the goal -
	// used to scan every live guideline node itself, so one committed edit cost A agents x
	// S remaining steps x N guideline nodes of linear search. Rules.ResolveRadius both sizes
	// the grid cell and is the MaxDistance every one of those calls already passed, so the
	// index answers each of them from its own cell and the eight around it rather than the
	// whole graph. See Airside.Model.Traffic.GraphRebuildNodeVisits for the measurement.
	const FGuidelineNodeIndex NodeIndex(Network, Rules.ResolveRadius);

	// ONE CONTEXT FOR THE WHOLE REBUILD TOO (issue #175), for the same reason as NodeIndex
	// just above: Network, Rules, Occupancy, NodeReach and RunwayChains are all fixed for the
	// duration of this call, so every ReResolvePlan call and the stand re-offer pass below
	// share the one bundle rather than each naming the five UGroundTraffic members again.
	FTrafficContext Context{Network, Rules, Occupancy, NodeReach, RunwayChains, SimSeconds};
	Context.bLanesMirrored = LastDriveSide.IsSet() && LastDriveSide.GetValue() != Network.GetDriveSide();
	LastDriveSide = Network.GetDriveSide();

	int32 Considered = 0;
	int32 Replanned = 0;
	int32 Truncated = 0;
	int32 Stranded = 0;

	// BY INDEX rather than by range-for: FPlanReResolver::ReplanAt, reached from
	// ReResolvePlan below, writes through its own reference into this same array. Nothing
	// here adds or removes an agent so a reference would in fact survive, but the index says
	// so without a reader having to go and check ReplanAt to find that out.
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
			// left with dead handles. See FPlanReResolver::ReResolvePlan.
			Plan = &Agent.Follower.Plan;
			FromStep = CurrentStep(Agent.Follower.Plan, Agent.Follower.Travelled);
		}
		else if (Agent.Phase == EAgentPhase::Manoeuvring && Agent.Pushback.Plan.Steps.Num() > 0)
		{
			// A PUSH IS ON A ROUTE TOO, and it is the departure route the follower will
			// inherit in a few seconds. A player who redraws a taxiway while an aeroplane is
			// being pushed off its stand leaves it exactly the dead handles a taxiing agent
			// would have, and the taxi out would then start on them.
			//
			// NOT REPLANNED, only re-pointed - see ReplanAt above for why a push has no
			// alternative to replan TO. If the rebuild truncates the plan shorter than the
			// push needed, FPushbackRun::HasArrived clamps to the new length and the
			// manoeuvre ends early rather than never.
			Plan = &Agent.Pushback.Plan;
			FromStep = CurrentStep(Agent.Pushback.Plan, Agent.Pushback.Travelled);
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
				Network, Agent.LastMotion.Position, Agent.Class, Rules.ResolveRadius, &NodeIndex);
			if (Here.IsSet())
			{
				Agent.SetGoal(Here);
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
		switch (PlanReResolver.ReResolvePlan(Agent, *Plan, FromStep, Context, NodeIndex))
		{
		case FPlanReResolver::EReResolve::Replanned: ++Replanned; break;
		case FPlanReResolver::EReResolve::Truncated: ++Truncated; break;
		case FPlanReResolver::EReResolve::Stranded:  ++Stranded;  break;
		case FPlanReResolver::EReResolve::Intact:    break;
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
	{
		FClaimPass Pass{Context};
		for (FRoadAgent& Agent : Agents)
		{
			Pass.ClaimGoalNode(Agent, Network);
		}
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

FRouteQuery FPlanReResolver::QueryFor(ERouteErrand Errand, FGuidelineNodeId Start, FGuidelineNodeId Goal,
	const FRoadAgent& Agent)
{
	FRouteQuery Query = FRouteQuery::For(Errand, Start, Goal, Agent.Wingspan(), Agent.Class);
	if (const FVehicle* Vehicle = Agent.AsVehicle())
	{
		Query.WithVehicle(*Vehicle);
	}
	return Query;
}

namespace
{
	/**
	 * How far a driving vehicle may be moved sideways onto a lane running its way after a
	 * drive-side flip, uu: two Wide lanes (2 x 450), rounded up. The flip moves every lane by
	 * the lane spacing; anything further is a different road.
	 */
	constexpr double FlipRejoinRadius = 1000.0;

	/**
	 * After a drive-side flip: the nearest point, within FlipRejoinRadius of the vehicle, on an
	 * edge running the way it was driving with a route to its goal from there; that route and
	 * how far along its first step the point is. The vehicle hops sideways onto it - a visible
	 * 3-4 m jump, once, on a deliberate airport-wide setting change; the alternative was
	 * stranding every truck on the road.
	 */
	bool RejoinAfterFlip(FRoadAgent& Agent, const FRoutePlan& Plan, const FTrafficContext& Context,
		FRoutePlan& OutPlan, double& OutTravelled, FVector2D& OutAt)
	{
		const URoadNetwork& Network = Context.Network;
		const FVector2D Here = Agent.LastMotion.Position;
		const FVector2D Facing(FMath::Cos(Agent.LastMotion.Heading), FMath::Sin(Agent.LastMotion.Heading));

		// The goal is usually an anchor or a stand pose, whose handle survives any rebuild. A
		// lane end does not, and the NEAREST node to where the plan ended is the wrong stand-in
		// for the same reason the start is: the old lane end's position now holds the start of
		// the lane running the other way. So: the nearest node the vehicle can ARRIVE at still
		// heading the way the old plan arrived.
		FGuidelineNodeId Goal = Network.GetGuidelineNode(Agent.GoalNode) != nullptr ? Agent.GoalNode : FGuidelineNodeId();
		if (!Goal.IsSet() && Plan.Polyline.Num() >= 2)
		{
			const FVector2D End = Plan.Polyline.Last();
			const FVector2D Arriving = (End - Plan.Polyline[Plan.Polyline.Num() - 2]).GetSafeNormal();
			double Best = FlipRejoinRadius;
			const TArray<FGuidelineNode>& All = Network.GetGuidelineNodes();
			for (int32 Index = 0; Index < All.Num(); ++Index)
			{
				const double Distance = FVector2D::Distance(All[Index].Position, End);
				if (!All[Index].bAlive || Distance >= Best)
				{
					continue;
				}
				bool bArrivesFacing = false;
				for (const FGuidelineEdgeId EdgeId : All[Index].Incident)
				{
					const FGuidelineEdge* Edge = Network.GetGuidelineEdge(EdgeId);
					if (Edge == nullptr || !Edge->AllowedTraffic.Allows(Agent.Class))
					{
						continue;
					}
					const bool bAtB = Edge->B == Network.GuidelineNodeIdAt(Index);
					const bool bMayArrive = Edge->Direction == EGuidelineDir::Bidirectional
						|| (bAtB && Edge->Direction == EGuidelineDir::AToB)
						|| (!bAtB && Edge->Direction == EGuidelineDir::BToA);
					bArrivesFacing |= bMayArrive
						&& FVector2D::DotProduct((All[Index].Position - Edge->Control).GetSafeNormal(), Arriving) > 0.5;
				}
				if (bArrivesFacing)
				{
					Best = Distance;
					Goal = Network.GuidelineNodeIdAt(Index);
				}
			}
		}
		if (!Goal.IsSet())
		{
			return false;
		}

		// EDGES, NOT NODES. A straight lane is ONE edge with nodes only at its cut ends, so a
		// search for nearby NODES found one only for a truck beside a lane end, and stranded
		// every other truck on the road for good (review of 2026-09-23). The vehicle is
		// projected onto the nearest edge running its way and restarts part-way along it.
		//
		// A LINEAR SCAN, sampling every edge, once per driving vehicle, once per flip -
		// O(vehicles x edges x samples), UNMEASURED on 2026-09-23 and accepted because a flip
		// is a deliberate, rare setting change. Bound it with a spatial index before anything
		// makes a flip cheap to repeat.
		struct FCandidate
		{
			FGuidelineEdgeId Edge;
			FGuidelineNodeId From;
			double Distance = 0.0;
			double Along = 0.0;
			FVector2D At = FVector2D::ZeroVector;
		};
		TArray<FCandidate> Candidates;
		const TArray<FGuidelineEdge>& Edges = Network.GetGuidelineEdges();
		for (int32 Index = 0; Index < Edges.Num(); ++Index)
		{
			const FGuidelineEdge& Edge = Edges[Index];
			if (!Edge.bAlive || !Edge.AllowedTraffic.Allows(Agent.Class))
			{
				continue;
			}
			const FGuidelineEdgeId Id = Network.GuidelineEdgeIdAt(Index);
			TArray<FVector2D> Points;
			if (!Network.SampleGuideline(Id, Points) || Points.Num() < 2)
			{
				continue;
			}
			int32 Span = 0;
			double Fraction = 0.0;
			const double Distance = GuidelineGeom::NearestOnPolyline(Points, Here, Span, Fraction);
			if (Distance > FlipRejoinRadius)
			{
				continue;
			}

			// Arc length from A to the projection, on the SAME samples the plan will walk.
			double FromA = 0.0;
			double Total = 0.0;
			for (int32 P = 1; P < Points.Num(); ++P)
			{
				const double Leg = FVector2D::Distance(Points[P - 1], Points[P]);
				FromA += P - 1 < Span ? Leg : (P - 1 == Span ? Leg * Fraction : 0.0);
				Total += Leg;
			}
			const FVector2D AlongAToB = (Points[Span + 1] - Points[Span]).GetSafeNormal();
			const FVector2D At = FMath::Lerp(Points[Span], Points[Span + 1], Fraction);

			const bool bMayAToB = Edge.Direction != EGuidelineDir::BToA;
			const bool bMayBToA = Edge.Direction != EGuidelineDir::AToB;
			if (bMayAToB && FVector2D::DotProduct(AlongAToB, Facing) > 0.5)
			{
				Candidates.Add({ Id, Edge.A, Distance, FromA, At });
			}
			else if (bMayBToA && FVector2D::DotProduct(-AlongAToB, Facing) > 0.5)
			{
				Candidates.Add({ Id, Edge.B, Distance, Total - FromA, At });
			}
		}
		Candidates.Sort([](const FCandidate& L, const FCandidate& R) { return L.Distance < R.Distance; });

		for (const FCandidate& Candidate : Candidates)
		{
			FRouteQuery Query = FPlanReResolver::QueryFor(ERouteErrand::RebuildReResolve,
				Candidate.From, Goal, Agent);
			Query.WithCongestion(Context.Occupancy, Agent.Id, Context.Rules.CongestionWeight);
			const FRoutePlan Found = RouteSearch::Find(Network, Query);
			// The route must BEGIN with the edge the vehicle is on, or starting it part-way
			// along the first step would put it on some other road.
			if (Found.IsValid() && Found.Steps.Num() > 0 && Found.Steps[0].Edge == Candidate.Edge)
			{
				OutPlan = Found;
				OutTravelled = Candidate.Along;
				OutAt = Candidate.At;
				Agent.SetGoal(Goal);
				return true;
			}
		}
		return false;
	}
}

FPlanReResolver::EReResolve FPlanReResolver::ReResolvePlan(
	FRoadAgent& Agent, FRoutePlan& Plan, int32 FromStep, const FTrafficContext& Context,
	const FGuidelineNodeIndex& NodeIndex)
{
	// REBOUND TO THE OLD NAMES (issue #175) - see FDeadlockResolver::Resolve's identical
	// rebinding for why: the body below, including the nested Strand lambda, is unchanged
	// from before Network, Rules and Occupancy became one FTrafficContext parameter.
	const URoadNetwork& Network = Context.Network;
	const FTrafficRules& Rules = Context.Rules;
	FTrafficOccupancy& Occupancy = Context.Occupancy;

	// WHOSE PLAN THIS IS, asked by address. The two callers hand in one of exactly two plans
	// and the difference matters twice below: only the follower's plan gets Replace (nothing
	// is following a TaxiInPlan yet), and only the follower's plan can be replanned through
	// ReplanAt. A bool parameter would say the same thing and could be passed wrongly; this
	// cannot be out of step with the reference it describes.
	// A PUSHBACK PLAN IS NOT "DRIVING" BY THIS TEST, and that is the right answer rather than
	// an oversight: what bDriving gates is FRouteFollower::Replace, whose whole job is
	// rebuilding the SPEED PROFILE. FPushbackRun has no profile - it is a trapezoid to
	// PushDistance - so there is nothing to rebuild, and HasArrived's clamp to Plan.Length
	// covers the one thing a truncation can do to it.
	const bool bDriving = (&Plan == &Agent.Follower.Plan);

	auto Strand = [&Agent, &Plan, bDriving, &Occupancy](const TCHAR* Why)
	{
		// THE GROUND UNDER THE AGENT IS GONE. There is no line left to put it on and no node
		// to search from, so the plan is marked unreachable - which is what FClaimPass::Run
		// and FRouteFollower::HasArrived both read to stop asking anything of it - and the
		// agent gives back every GUIDELINE it holds, because holding lines it will never
		// drive would block whatever the player builds in their place.
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
		// together with the claim by FClaimPass::Run's non-Taxiing branch once the agent parks.

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
		const FString Held = Agent.GetCrossingRunway().IsSet()
			? FString::Printf(TEXT(" - and it is on runway segment %d, which it holds until it is retired"),
				Agent.GetCrossingRunway().Index)
			: FString();
		UE_LOG(LogAirsideTraffic, Warning, TEXT("Agent %d stranded by the rebuild: %s%s"),
			Agent.Id, Why, *Held);

		// A STRANDED TAXI-IN STILL HAS A PLACE: the exit node the landing hands over at, which
		// Plan.Start was re-pointed to when it could be. The re-offer (ReofferStands) searches
		// from GoalNode, so a dead handle here would leave a waiting aircraft waiting for ever.
		// Spec 2026-09-07-stand-occupancy §5, amended.
		if (!bDriving && Agent.bAwaitingStand && Plan.Start.IsSet())
		{
			Agent.SetGoal(Plan.Start);
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

	// THE DRIVE SIDE FLIPPED. Position is no guide now - the old lane's positions hold the
	// opposite lane's nodes exactly - so a driving vehicle rejoins by HEADING and re-routes.
	// Only ground vehicles: aircraft use centreline taxiways, which a flip does not move.
	// ENFORCED BY: Airside.Present.DriveSide.Composition
	if (Context.bLanesMirrored && bDriving && Agent.Class == ETraversalClass::GroundVehicle)
	{
		FRoutePlan Rejoined;
		double Travelled = 0.0;
		FVector2D At = FVector2D::ZeroVector;
		if (!RejoinAfterFlip(Agent, Plan, Context, Rejoined, Travelled, At))
		{
			return Strand(TEXT("the drive side flipped and no lane running its way reaches its goal"));
		}
		Occupancy.ReleaseReservations(Agent.Id);
		Occupancy.ReleaseGuidelineClaimsOf(Agent.Id);
		Agent.ClearArbitration();
		Agent.ResetStall();
		const FGuidelineNodeId Goal = Agent.GoalNode;
		Agent.RestartTaxi(Rejoined, Travelled);
		// RestartTaxi's fallback pose is the plan's first point; the vehicle is part-way along.
		Agent.LastMotion.Position = At;
		Agent.SetGoal(Goal);
		UE_LOG(LogAirsideTraffic, Log, TEXT("Agent %d rejoined its side after the drive side flipped: %.0f uu to go"),
			Agent.Id, Rejoined.Length - Travelled);
		return EReResolve::Replanned;
	}

	const double Radius = Rules.ResolveRadius;
	const int32 FromVertex = (FromStep == 0) ? 0 : Plan.Steps[FromStep - 1].EndVertex;
	if (!Plan.Polyline.IsValidIndex(FromVertex))
	{
		return Strand(TEXT("its polyline does not reach the step it is on"));
	}

	// THE NODE THE CURRENT STEP LEAVES FROM, found by where that node WAS. Everything below
	// walks forward from here, so this is the one lookup nothing can recover from.
	FGuidelineNodeId Prev = RouteSearch::FindNearestNode(Network, Plan.Polyline[FromVertex], Agent.Class, Radius, &NodeIndex);
	if (!Prev.IsSet())
	{
		return Strand(TEXT("no live node holds the position its current step starts from"));
	}

	// THE NODE THE AGENT IS DRIVING AWAY FROM IS RE-POINTED, and it is not optional: four
	// readers ask StepFromNode for it every tick - the crossing arm, the tail-node claim,
	// RankAt, and ReplanAt's own Query.Start when the failed step is the current one. See
	// UGroundTraffic::OnGraphRebuilt for what each of them does with a dead handle. The last
	// of those is why this line has to come BEFORE the replan below rather than after it:
	// without it the search starts from a freed slot, returns NoStart, and the truncation
	// that follows writes that same dead handle into GoalNode.
	if (FromStep == 0)
	{
		Plan.Start = Prev;
	}
	else
	{
		Plan.Steps[FromStep - 1].To = Prev;
	}

	// COINCIDENT NODES ARE ONE PLACE HELD TWICE, and position alone cannot tell them apart. At a
	// straight-through node where both arms have the same lane offset, each arm's lane end sits
	// on the shared cut line - the same point - joined by a zero-length turn path; the plan
	// steps through both. FindNearestNode breaks the tie by slot order, so it hands back the
	// SAME node for both positions (or the wrong one of the pair), no edge joins Prev to it, and
	// the step "fails" on a graph that lost nothing. That failure then replans to the goal and
	// drops every via point the route carried: a lone node placed off the rig course re-routed
	// the utility past all three dead ends (2026-09-25, PIE and
	// AirportMgr.RigCourse.RebuildKeepsTheCourse). So a position resolves to the node found AND
	// its twins - the nodes a zero-length edge joins it to - and the step takes whichever of
	// them its edge actually reaches. Twins by EDGE, not by a second spatial query: the builder
	// joins every such pair with its turn path, and a coincident node nothing joins is not the
	// same place for routing anyway.
	// ENFORCED BY: AirportMgr.RigCourse.RebuildKeepsTheCourse, Airside.Model.Traffic.RebuildCoincidentTwins
	constexpr double TwinTolerance = 1.0;
	auto TwinsOf = [&Network](FGuidelineNodeId Node, TArray<FGuidelineNodeId, TInlineAllocator<4>>& Out)
	{
		Out.Reset();
		Out.Add(Node);
		const FGuidelineNode* Here = Network.GetGuidelineNode(Node);
		if (Here == nullptr)
		{
			return;
		}
		for (const FGuidelineEdgeId EdgeId : Here->Incident)
		{
			const FGuidelineEdge* Edge = Network.GetGuidelineEdge(EdgeId);
			const FGuidelineNodeId Other = Edge == nullptr ? FGuidelineNodeId() : (Edge->A == Node ? Edge->B : Edge->A);
			const FGuidelineNode* There = Other.IsSet() ? Network.GetGuidelineNode(Other) : nullptr;
			if (There != nullptr && Other != Node && !Out.Contains(Other)
				&& FVector2D::Distance(There->Position, Here->Position) <= TwinTolerance)
			{
				Out.Add(Other);
			}
		}
	};

	// THE EDGE IS IDENTIFIED BY ITS TWO ENDS, never by its geometry. Two guideline nodes have at
	// most one line between them that this class can mean, and matching a Bezier control point
	// across a rebuild would be a second evaluator of the very thing that was just regenerated -
	// see the guideline graph's "samples ONCE".
	//
	// ForEachOutgoingGuideline, not GetOutgoingGuidelines (#190): a re-resolve runs once per
	// agent per rebuild, on the same edit path RouteSearch's own switch (#171) already stopped
	// paying a fresh TArray<FGuidelineEdgeId> per node expansion. Visit has no early-exit signal,
	// so every outgoing edge is still visited - the `if (Rejoined.IsSet())` guard below is what
	// keeps a node with more than one candidate from letting a later edge overwrite the first
	// match, the same effect the old loop's `break` had.
	auto EdgeBetween = [&Network, &Agent](FGuidelineNodeId From, FGuidelineNodeId To, FGuidelineEdgeId& Rejoined, bool& bReversed)
	{
		Network.ForEachOutgoingGuideline(From, Agent.Class, [&](FGuidelineEdgeId Candidate)
		{
			if (Rejoined.IsSet())
			{
				return;
			}

			const FGuidelineEdge* Edge = Network.GetGuidelineEdge(Candidate);
			if (Edge == nullptr)
			{
				return;
			}

			const FGuidelineNodeId Other = (Edge->A == From) ? Edge->B : Edge->A;
			if (Other == To)
			{
				Rejoined = Candidate;

				// Re-derived from the LIVE edge and never carried over from the dead step:
				// the builder is free to have laid this one down A-to-B where the old one
				// ran B-to-A, and a stale flag would reverse the sampled points under an
				// agent that is already driving them.
				bReversed = (Edge->B == From);
			}
		});
		return Rejoined.IsSet();
	};

	// The first step that could NOT be re-resolved, or the step count when every one could.
	int32 Failed = Plan.Steps.Num();
	for (int32 Step = FromStep; Step < Plan.Steps.Num(); ++Step)
	{
		const int32 EndVertex = Plan.Steps[Step].EndVertex;
		FGuidelineNodeId Next = Plan.Polyline.IsValidIndex(EndVertex)
			? RouteSearch::FindNearestNode(Network, Plan.Polyline[EndVertex], Agent.Class, Radius, &NodeIndex)
			: FGuidelineNodeId();

		FGuidelineEdgeId Rejoined;
		bool bReversed = false;
		if (Next.IsSet())
		{
			TArray<FGuidelineNodeId, TInlineAllocator<4>> NextTwins;
			TwinsOf(Next, NextTwins);
			// The FIRST step's start is a position lookup too, so it may hold the wrong twin; every
			// later Prev was chosen by the edge that reached it and is exact.
			TArray<FGuidelineNodeId, TInlineAllocator<4>> PrevTwins;
			if (Step == FromStep)
			{
				TwinsOf(Prev, PrevTwins);
			}
			else
			{
				PrevTwins.Add(Prev);
			}
			for (const FGuidelineNodeId From : PrevTwins)
			{
				for (const FGuidelineNodeId To : NextTwins)
				{
					if (!Rejoined.IsSet() && EdgeBetween(From, To, Rejoined, bReversed))
					{
						Next = To;
						if (From != Prev)
						{
							// The twin the step leaves from - re-pointed as the node the current
							// step leaves, above, was.
							Prev = From;
							if (FromStep == 0)
							{
								Plan.Start = Prev;
							}
							else
							{
								Plan.Steps[FromStep - 1].To = Prev;
							}
						}
					}
				}
			}
		}

		if (!Rejoined.IsSet())
		{
			// SAID, because a failure here is what turns an edit anywhere on the map into a
			// replan that can drop every via point the route carried (2026-09-25: a lone node
			// placed off the rig course re-routed the utility past three dead ends).
			const FGuidelineNode* PrevNode = Network.GetGuidelineNode(Prev);
			const FVector2D Wanted = Plan.Polyline.IsValidIndex(EndVertex) ? Plan.Polyline[EndVertex] : FVector2D::ZeroVector;
			UE_LOG(LogAirsideTraffic, Log,
				TEXT("Agent %d: step %d did not re-resolve - from node %d (%.0f, %.0f) to %s near (%.0f, %.0f)"),
				Agent.Id, Step, Prev.Index, PrevNode != nullptr ? PrevNode->Position.X : 0.0,
				PrevNode != nullptr ? PrevNode->Position.Y : 0.0,
				Next.IsSet() ? *FString::Printf(TEXT("node %d, no edge between them"), Next.Index) : TEXT("no live node"),
				Wanted.X, Wanted.Y);
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
	// When every step re-resolved, the goal IS the last step's end, chosen by the edge that
	// reaches it: a position lookup there could land on its twin (see TwinsOf).
	const FGuidelineNodeId Goal = (Failed == Plan.Steps.Num() && Failed > FromStep)
		? Plan.Steps.Last().To
		: RouteSearch::FindNearestNode(Network, Plan.Polyline.Last(), Agent.Class, Radius, &NodeIndex);
	if (Goal.IsSet())
	{
		Agent.SetGoal(Goal);
	}

	// THE GOAL WAS A STAND AND THE STAND IS GONE - or a taxiway to it. An aircraft whose goal
	// no longer resolves, and that is not lined up for a runway, is retargeted at whichever
	// FREE stand is nearest the node it will replan from, BEFORE the replan below runs - so
	// the replan searches to a live stand rather than to a freed handle and truncates. No
	// stand: it is marked awaiting, and the truncation that follows gives it a node to wait
	// at. Spec 2026-09-07-stand-occupancy §5. Vehicles and departures keep M2's rules.
	if (!Goal.IsSet() && Agent.Class == ETraversalClass::Aircraft && Agent.AsAircraft() != nullptr
		&& !Agent.bDepartureArmed
		&& Failed < Plan.Steps.Num())
	{
		const FGuidelineNodeId ReplanFrom = UGroundTraffic::StepFromNode(Plan, Failed);
		const FGuidelineNodeId NewStand = ArrivalPlanner::ChooseStand(
			Network, ReplanFrom, *Agent.AsAircraft(), &Occupancy, Agent.Id);
		if (NewStand.IsSet())
		{
			Agent.SetGoal(NewStand);
			Agent.ClearAwaitingStand();
			UE_LOG(LogAirsideTraffic, Log, TEXT("Agent %d: its stand is gone; retargeting to the stand at node %d"),
				Agent.Id, NewStand.Index);
		}
		else
		{
			// GOAL UNCHANGED, deliberately: this branch is reached only when Goal (above)
			// failed to resolve, so the agent's existing GoalNode - whatever it is, live or
			// dead - is exactly what it carried into this call. SetAwaitingStand still takes
			// it explicitly rather than leaving bAwaitingStand to flip on its own, so the
			// invariant reads the same way at every call site: the flag never moves without
			// naming the node it goes with.
			Agent.SetAwaitingStand(Agent.GoalNode);
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
		if (ReplanAt(Agent, Failed, FGuidelineEdgeId(), FGuidelineNodeId(), Context))
		{
			return EReResolve::Replanned;
		}
	}
	else
	{
		FRouteQuery Query = QueryFor(ERouteErrand::RebuildReResolve,
			UGroundTraffic::StepFromNode(Plan, Failed), Agent.GoalNode, Agent);

		// The congestion term, as ReplanAt takes it: the guidelines that survived the rebuild
		// by handle - every hand-drawn one - still carry real queues, and a re-routed arrival
		// should be steered round them rather than into the back of one.
		Query.WithCongestion(Occupancy, Agent.Id, Rules.CongestionWeight);
		Query.RunwayPenalty = Rules.RunwayPenalty;

		if (SpliceReplan(Network, Query, Failed, Plan))
		{
			// The destination has not moved - the splice ends where the old plan did - but the
			// last step's To is now a LIVE handle, and that is what a later replan searches to.
			//
			// NOT SetGoalFrom (issue #82): that clears GoalNode to unset when Steps is empty,
			// which is right for a fresh dispatch but wrong here - FRoutePlan::IsValid() does
			// NOT guarantee Steps.Num() > 0, and a valid-but-empty splice must leave the goal
			// exactly where it was rather than blank it out from under a later replan.
			Agent.SetGoal(Plan.Steps.Num() > 0 ? Plan.Steps.Last().To : Agent.GoalNode);

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
		Agent.Follower.Replace(Plan, Agent.Chassis());
	}

	UE_LOG(LogAirsideTraffic, Log,
		TEXT("Agent %d truncated by the rebuild: %d of %d steps survive, %.0f uu to the last live node"),
		Agent.Id, Failed, WasSteps, Plan.Length);
	return EReResolve::Truncated;
}
