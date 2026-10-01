// UGroundTraffic::OnGraphRebuilt (kept here: tick-order orchestration, not mechanism) and
// FPlanReResolver (issue #84) - the replan MECHANISM both this and the deadlock resolver
// share. Spec 2026-09-06 §6, and §4 for ReplanAt/SpliceReplan. See Model/GroundTraffic.h for
// which file holds what, and RoadEditFacadeSurfaces.cpp for the one-class-many-files precedent.

#include "Model/GroundTraffic.h"

#include "AirsideLog.h"
#include "Algo/AllOf.h"
#include "Model/ArrivalPlanner.h"
#include "Model/GroundTrafficRejoin.h"
#include "Model/RoadNetwork.h"
#include "Model/RoutePolicy.h"
#include "Model/RouteChange.h"
#include "Model/RunwayQuery.h"
#include "Model/TrafficClaims.h"
#include "Model/TrafficContext.h"
#include "Model/VehicleFit.h"
#include "Solve/GuidelineGeom.h"

bool FPlanReResolver::SpliceReplan(const URoadNetwork& Network, const FRouteQuery& Query,
	int32 KeepSteps, FRoutePlan& Plan)
{
	// THE TAIL IS SEARCHED UNSEEDED: it starts at a node ahead of the vehicle, where its chain
	// is not yet - the live chain belongs to the WHOLE splice, judged just below.
	//
	// SO A TOW SPLICED AT A REVERSE LEG'S START IS REFUSED (since 2026-09-26): its tail opens with
	// the reverse, and VehicleFit::JudgePlan refuses a reverse-first plan with no cab to solve it
	// from (ReverseUnsolvable) rather than passing it unjudged. The replan then fails and the tow
	// keeps the route it had, which is safe. Traced 2026-09-27, not pinned by a test: a stand's
	// route OUT has no reverse leg and its route home opens with one (a Reversing agent, which
	// ReplanAt refuses), so neither reaches this; the Taxiing routes with a reverse past their
	// first step are the rig course's and a recalled tow's (UGroundTraffic::RerouteAgent), and a
	// deadlock splice exactly at that reverse is where a refusal here would first be seen.
	FRouteQuery TailQuery = Query;
	TailQuery.TowSeed.Reset();
	const FRoutePlan Tail = RouteSearch::Find(Network, TailQuery);
	if (!Tail.IsValid())
	{
		return false;
	}

	const FRoutePlan Spliced = RouteSearch::Splice(Plan, KeepSteps, Tail);
	if (!Spliced.IsValid())
	{
		return false;
	}

	// THE SPLICE JUDGED WHOLE, from the live chain (review of 9441ccf1): a tail that holds from a
	// straight start can still fold a trailer already swung by the kept prefix. Refused like a
	// failed search - Plan untouched - and said at Log level: a re-route that folds is the reason
	// a tow will next be stranded or replanned, and nobody else logs it.
	if (Query.Vehicle != nullptr && Query.Vehicle->HasTrailer())
	{
		const FFitVerdict Whole = VehicleFit::JudgePlan(Spliced, *Query.Vehicle, Network, Query.TowSeed.GetPtrOrNull());
		if (!Whole.Fits())
		{
			UE_LOG(LogAirsideTraffic, Log, TEXT("Re-route %d -> %d refused: the spliced route does not hold the %s's tow (%s)"),
				Query.Start.Index, Query.Goal.Index, *Query.Vehicle->TypeCode.ToString(), *Whole.Describe());
			return false;
		}
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
	// this guard stay as narrow as it is. IsReplannable is that guard's name (#455): the one predicate
	// the resolver's candidate test and the alert's filter ask too, so the three cannot disagree.
	if (!Agent.IsReplannable() || !Plan.IsValid()
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
	// AND THE PENALTY FROM THE SAME RULES, in the same call (WithRules, #449). Both replan errands allow a FREE runway
	// end (Held, not All), so the multiplier is what keeps one a last resort rather than a shortcut. Read off the
	// instance, so a level that tuned it is obeyed.
	Query.WithRules(Rules, Occupancy, Agent.Id);

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
	//
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
	//
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
	//
	// ALL OF IT IS ONE Splice (issue #429): Replace, the reservations, the arbitration fields and the stall clock, in
	// that order, by FRoadAgent::ApplyRouteChange - the aftermath every route change shares, where this was one of
	// nine hand-typed subsets. The goal is KEPT: the search above was to the agent's own goal, so the spliced plan
	// ends where the old one did, and a replan owes the goal node no claim traffic.
	// ENFORCED BY: Airside.Model.RouteChange.SpliceEndsTheWait (the resolver's replan, with the goal kept)
	Agent.ApplyRouteChange(FRouteChange::Splice(Spliced), ERouteGoal::Keep, Occupancy);

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

	// EVERY RUNWAY AN AGENT HOLDS OR WILL HOLD, RE-POINTED (playtest 2026-09-28). An exit built
	// onto a runway splits it, and the split kills the handles a landing, a lined-up departure,
	// an armed taxi-out and a crossing each stored - so the strip read free under an aeroplane
	// and a holding arrival was cleared onto it. FIRST, before any replan below reads the table,
	// and CLAIMED HERE rather than on the next tick for DispatchArrival's own reason: the
	// planner reads the table between ticks. The dead handles' claims are left for the next
	// claim pass to drop - nothing asks about a segment that no longer exists.
	for (FRoadAgent& Agent : Agents)
	{
		const auto Claim = [this, &Agent](const TArray<FRoadSegmentId>& Chain)
		{
			for (const FRoadSegmentId Segment : Chain)
			{
				Occupancy.Assert(FTrafficClaim::Make(Agent.Id, FTrafficResource::OfSurface(Segment),
					/*bOccupied*/ true, TraversalPriority(Agent.Class)));
			}
		};
		const auto RePoint = [&Network, &Agent](const TCHAR* What, const TArray<FRoadSegmentId>& Was,
			const FVector2D& At, TArray<FRoadSegmentId>& Out) -> bool
		{
			Out = RunwayQuery::RePointChain(Network, Was, At);
			if (Out.Num() == 0)
			{
				// NOT RELEASED: the old handles are dead either way, and saying so beats a
				// silent empty hold that reads as "this aircraft is on no runway".
				UE_LOG(LogAirsideTraffic, Warning,
					TEXT("Agent %d: the %s it held is gone from the rebuilt graph; nothing to re-point to"),
					Agent.Id, What);
				return false;
			}
			// AS A SET: the walk starts from whichever member survived, so an untouched chain can
			// come back in another order, and that is not a change worth a claim or a line.
			const bool bSame = Out.Num() == Was.Num()
				&& Algo::AllOf(Out, [&Was](const FRoadSegmentId& Segment) { return Was.Contains(Segment); });
			if (!bSame)
			{
				UE_LOG(LogAirsideTraffic, Log, TEXT("Agent %d: %s re-pointed after the rebuild, %d -> %d segment(s)"),
					Agent.Id, What, Was.Num(), Out.Num());
			}
			return !bSame;
		};

		TArray<FRoadSegmentId> Now;
		if (Agent.RunwayHeld.Num() > 0 && RePoint(TEXT("runway"), Agent.RunwayHeld, Agent.RunwayHeldAt, Now))
		{
			Agent.HoldRunway(Now, RunwayQuery::PointOnChain(Network, Now));
			Claim(Now);
		}
		if (Agent.GetDepartureRunway().Num() > 0
			&& RePoint(TEXT("departure runway"), Agent.GetDepartureRunway(), Agent.GetDepartureRunwayAt(), Now))
		{
			// Armed only - nothing is claimed until the LinedUp handover.
			Agent.ArmDepartureRunway(Now, RunwayQuery::PointOnChain(Network, Now));
		}
		// A CROSSING stores a seed, re-expanded per tick through RunwayChains - and a dead seed
		// expands to itself (GetOrSeed). Only a DEAD seed is re-pointed: a live one already
		// expands to the new chain. The body is ON the strip, so its own position is the point
		// to re-find it by.
		if (Agent.GetCrossingPhase() != ECrossingPhase::None && Agent.GetCrossingRunway().IsSet()
			&& !Network.IsRunwaySegment(Agent.GetCrossingRunway())
			&& RePoint(TEXT("crossing runway"), { Agent.GetCrossingRunway() }, Agent.GroundPosition(), Now))
		{
			Agent.BeginCrossing(Now[0], Agent.GetCrossingPhase());
			Claim(Now);
		}
	}

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
		Agent.StampResolveAttempt(-1.0e9);

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
			// RE-ROUTED TO ITS OWN END OR NOWHERE - see ReplanAt above for why a push has no other
			// place to go. This said "NOT REPLANNED, only re-pointed", which was not true: a step
			// that does not re-resolve is replanned by ReResolvePlan to Agent.GoalNode, and during a
			// push that is the runway entry DepartAgent gave the taxi out, so a deleted push end had
			// the aeroplane pushed backwards up the taxiway onto the runway (#498's probe). Pointed
			// at the push's own end first, a dead end fails that search and the push is cut back to
			// its last live node or, when none survives, stranded; FPushbackRun::HasArrived then ends
			// it early, where it stands, rather than never. A live end is still searched to - the one
			// re-route a push gets, and the taxi out still begins where it arrives. A step UNDER it
			// that fails with the pavement still a few metres off (a dragged junction) is rejoined
			// instead, and the push completes: ReResolvePlan's RejoinPush.
			// ENFORCED BY: Airside.Model.PushbackEndGoneStopsShortOfTheRunway, Airside.Model.PushbackOnDeletedGroundStops
			//
			// ONLY A LIVE PUSH'S GOAL (#501 review): a push already stranded is skipped below as not
			// valid, so pointing the goal at its dead end left a held departure naming that node, not
			// its runway, after any later edit anywhere on the map.
			// ENFORCED BY: Airside.Model.PushbackOnDeletedGroundStops (a second, unrelated rebuild)
			Plan = &Agent.Pushback.Plan;
			FromStep = CurrentStep(Agent.Pushback.Plan, Agent.Pushback.Travelled);
			if (Agent.Pushback.Plan.IsValid())
			{
				Agent.SetGoalFrom(Agent.Pushback.Plan);
			}
		}
		else if (Agent.Phase == EAgentPhase::Arriving && Agent.TaxiInPlan.Steps.Num() > 0)
		{
			// FROM 0, because not a metre of this one has been driven: it is the route the
			// aircraft will fly once it vacates, and it is just as dead after a rebuild as one
			// somebody is on. Left unresolved, an arrival would vacate onto freed handles and
			// stop dead on the runway exit.
			Plan = &Agent.TaxiInPlan;
		}
		else if (Agent.Phase == EAgentPhase::Reversing
			&& Agent.Follower.Plan.Steps.IsValidIndex(Agent.GetResumeStep()))
		{
			// A VEHICLE BACKING OUT HAS A ROUTE TO COME BACK TO (issue #434). Follower.Plan is the
			// whole route and the follower is parked on it while the reverse plays, so the taxi arm
			// above cannot take it: FromStep would be the span under the truck and the arm would
			// treat it as driving. What the rebuild has to keep live is what the truck drives NEXT -
			// the steps from ResumeStep on, cut out as the remainder the moment it has backed out
			// (RoadAgent's reverse handover). An edit during a reverse otherwise left it driving,
			// after backing out, a route of freed handles to a freed goal: claims that matched
			// nothing, invisible to arbitration, until the next edit.
			//
			// FROM ResumeStep, NOT FROM THE SPAN'S START, and that is a decision rather than a
			// shortcut. The span is played from FReverseRun's own snapshot of it, which nothing here
			// touches; re-resolving it with ReResolvePlan would let a failure inside it replan or
			// truncate the route from under the STEP INDEX the handover cuts the remainder by, and the
			// truck would pick up somewhere it is not. The span's own steps are re-pointed by a separate
			// pass that cannot do either (ReResolveSpan, below - #455, which closed the gap this comment
			// used to accept: until then a rebuild that freed the span's handles left this truck's hold
			// on it pointing at nothing until it had backed out).
			//
			// WHAT A FAILURE HERE DOES, stated because it differs by where it lands: a step after the
			// span that will not re-resolve is replanned or TRUNCATED, and the truck then parks at the
			// span's end on a live goal (Airside.Model.Traffic.RebuildDuringReverseTruncatesTheRemainder);
			// but when the span's own END node is freed with no live node at its position, Strand marks
			// the route dead, and the reversing agent - which the rebuild leaves in its phase - ends the
			// back-out where it stands and is Stranded (FRoadAgent::Advance's Reversing arm, #455).
			// ENFORCED BY: Airside.Model.Traffic.RebuildDuringReverseStrandsWhenTheSpanEndIsGone
			Plan = &Agent.Follower.Plan;
			FromStep = Agent.GetResumeStep();
		}

		// A PARKED AGENT'S GOAL IS WHERE IT STANDS, and the rebuild may have freed that node -
		// every derived one was. Re-pointed by position, or every later search from it (a
		// Depart, a stand re-offer) would start at a dead handle and fail for ever. The stand
		// pose node itself is authored and survives, so this changes nothing for an aircraft
		// parked on a stand.
		// AND A STRANDED ONE'S (#396): a player's Unstick or a later search may start from it. (The stand re-offer no
		// longer does for a stranded waiter - it searches from where the waiter stands; UGroundTraffic::ReofferStand.)
		if ((Agent.Phase == EAgentPhase::Parked || Agent.Phase == EAgentPhase::Stranded)
			&& Network.GetGuidelineNode(Agent.GoalNode) == nullptr)
		{
			const FGuidelineNodeId Here = RouteSearch::FindNearestNode(
				Network, Agent.LastMotion.Position, Agent.Class, Rules.ResolveRadius, &NodeIndex);
			if (Here.IsSet())
			{
				Agent.SetGoal(Here);
			}
		}

		// A REVERSE THAT ENDS THE ROUTE has no remainder to re-resolve (no arm above), but its goal
		// is the span's end node, freed like any other derived one, and it is where the vehicle
		// will park (issue #434): re-pointed by position, as a parked agent's is, from the end of
		// the route it is backing along.
		// ENFORCED BY: Airside.Model.Traffic.RebuildDuringAReverseThatEndsTheRoute
		if (Agent.Phase == EAgentPhase::Reversing && Plan == nullptr
			&& Agent.Follower.Plan.Polyline.Num() > 0
			&& Network.GetGuidelineNode(Agent.GoalNode) == nullptr)
		{
			const FGuidelineNodeId Ends = RouteSearch::FindNearestNode(
				Network, Agent.Follower.Plan.Polyline.Last(), Agent.Class, Rules.ResolveRadius, &NodeIndex);
			if (Ends.IsSet())
			{
				Agent.SetGoal(Ends);
			}
		}

		// THE SPAN A REVERSING AGENT IS BACKING ALONG, re-pointed at the live graph BEFORE the remainder is (#455):
		// the claim pass holds a reversing vehicle's whole span through these very steps, so a span left on freed
		// handles is a truck holding nothing for the rest of the leg. Handles only - see ReResolveSpan for why it
		// can neither replan nor move a step - and before ReResolvePlan below, whose re-point of the span's last
		// To (the node the remainder leaves) then has the last word. A reverse that ends the route has no
		// remainder and no arm above, and needs this most: nothing else would touch its steps.
		// ENFORCED BY: Airside.Model.Traffic.RebuildDuringReverseKeepsTheSpanLive,
		// Airside.Model.Traffic.RebuildDuringAReverseThatEndsTheRoute
		if (Agent.Phase == EAgentPhase::Reversing && Agent.Follower.Plan.IsValid())
		{
			int32 SpanFirst = INDEX_NONE;
			int32 SpanLast = INDEX_NONE;
			if (Agent.ReverseSpanSteps(SpanFirst, SpanLast))
			{
				// THE RESULT IS NOT ACTED ON, deliberately: false means a span step did not re-resolve (a split
				// under the span, the ground otherwise moved), and ReResolveSpan has already logged which and
				// left that step - and the ones after it - on the handles they had. It is not fatal because the
				// alternatives are worse: stranding stops a truck mid-leg for a bookkeeping miss on ground that
				// is usually still there, and a replan would move the route from under the handover's step
				// index. A span whose END node is gone, with a remainder to drive, is the case that IS fatal, and
				// it is not reported here: the remainder's ReResolvePlan below finds no node at the span's end
				// and Strands, which the reversing agent acts on (FRoadAgent::Advance). A reverse that ends the
				// route has no remainder to strand it and parks at the old end. Not in the summary's counts
				// either - they are one per agent, and the agent is counted once, by that call.
				(void)PlanReResolver.ReResolveSpan(Agent, Agent.Follower.Plan, SpanFirst, SpanLast, Context, NodeIndex);
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

		// AND THE TAXI OUT A PUSH WILL HAND OVER TO (2026-09-27). The push plan above is the
		// short lead-in; FRoadAgent::TaxiOutPlan is the route the follower takes over when the
		// push ends, and it was never re-resolved - so an aeroplane pushed back while the
		// player redrew its way out taxied along lines that no longer existed
		// (Airside.Model.PushbackRebuildReResolvesTaxiOut). FROM 0, as TaxiInPlan is: not a
		// metre of it has been driven. AFTER the push, whose re-resolve points the goal at the push's
		// end - so it is pointed back at the taxi out's own end first, the entry DepartAgent gave it
		// (#498): ReResolvePlan replans a plan whose end is gone to the AGENT's goal, and from the
		// push's end that made a deleted entry a drive out and back to it, disarmed. To the dead entry
		// the search fails, the taxi out truncates, and the push ends in the hold below.
		// ENFORCED BY: Airside.Model.Traffic.HeldTaxiOut.MidPushRunwayLossHolds
		//
		// NOT IN THE SUMMARY'S COUNTS, which are one per AGENT (Considered, above): counting a
		// second route per pushing aeroplane made "re-resolved" go negative (2026-09-27). Said
		// on its own line instead, when it did not survive.
		if (Agent.Phase == EAgentPhase::Manoeuvring && Agent.TaxiOutPlan.IsValid() && !Agent.IsWaitingFor(EAgentWait::ForTaxiOutRoute))
		{
			Agent.SetGoalFrom(Agent.TaxiOutPlan);
			const FPlanReResolver::EReResolve TaxiOut =
				PlanReResolver.ReResolvePlan(Agent, Agent.TaxiOutPlan, 0, Context, NodeIndex);
			if (TaxiOut == FPlanReResolver::EReResolve::Stranded || TaxiOut == FPlanReResolver::EReResolve::Truncated)
			{
				// A ROUTE NOT YET STARTED IS PLANNED AGAIN, not stopped on: stranding means "stop
				// where you are", which is right for the step under the wheels and wrong for one
				// the aeroplane has not reached. And NEVER ARMED FROM: its end is the old runway
				// entry, which is where the aeroplane went when this was armed anyway.
				Agent.WaitFor(EAgentWait::ForTaxiOutRoute);
				Agent.DisarmDeparture();
				UE_LOG(LogAirsideTraffic, Log,
					TEXT("Agent %d: its taxi-out did not survive the rebuild; it will be planned again where the push ends"),
					Agent.Id);
			}
			else
			{
				// RE-ARMED FROM THE ROUTE AS IT NOW ENDS: the entry, its offset and the strip's
				// length were measured when the push began, and the rebuild may have moved all
				// three. ArmDepartureIfRunway disarms first, so this is safe to repeat.
				ArmDepartureIfRunway(Agent, &Network, Agent.TaxiOutPlan);
			}
		}
	}

	// THE STAND HOLDS, SNAPSHOTTED BY ENTITY before the release below takes them (PR D review I1). A hold is
	// AirportOps' accepted flight - no agent, a negative holder - and a Node claim like any other, so the release
	// dropped it and only a load ever put it back: every edit in play left the next accept free to double-book the
	// stand. This model made the reservation, so it keeps it through its own rebuild rather than asking ops to
	// re-make it synchronously. Keyed by ENTITY, then re-held on that entity's pose after the release; a stand
	// pose is a non-derived node, so it is the same handle unless the stand itself went.
	// ENFORCED BY: Airside.Model.Traffic.RebuildKeepsStandHolds, AirportOps.Present.RuntimeEdit.KeepsAcceptedStandHold
	TArray<TPair<int32, FEntityInstanceId>> StandHolds;
	{
		TMap<FGuidelineNodeId, FEntityInstanceId> StandAtPose;
		const TArray<FEntityInstance>& Entities = Network.GetEntities();
		for (int32 Index = 0; Index < Entities.Num(); ++Index)
		{
			if (Entities[Index].IsStandCandidate())
			{
				StandAtPose.Add(Entities[Index].PoseNode, Network.EntityIdAt(Index));
			}
		}
		for (const FTrafficClaim& Claim : Occupancy.GetClaims())
		{
			// NEGATIVE HOLDER = NOT AN AGENT: agent ids count up from 1 - see HoldStand.
			if (Claim.AgentId >= 0 || Claim.Resource.Kind != ETrafficResourceKind::Node)
			{
				continue;
			}
			if (const FEntityInstanceId* Stand = StandAtPose.Find(Claim.Resource.Node))
			{
				StandHolds.Emplace(Claim.AgentId, *Stand);
			}
			else
			{
				// ITS STAND IS GONE: nothing to re-hold. The flight's Stand still names the dead entity, which is
				// what the ops HeldStandLost alert reads - the player is told there.
				UE_LOG(LogAirsideTraffic, Log, TEXT("Stand hold of holder %d dropped by the rebuild: its stand is gone"), Claim.AgentId);
			}
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
	// AND THE FLIGHTS' HOLDS, on the same stands - after the agents' goals, which are bodies and routes already
	// committed. A refusal would mean an agent's goal and a flight's hold named one stand before the rebuild too,
	// which HoldStand refuses at the door; said as a Warning, not assumed away - and SETTLED BY AIRPORTOPS (#442), whose board
	// reads the table on the edit's announcement and re-holds a flight whose copy names a stand it lost (UFlightBoard::
	// ReconcileStandHolds, a load's refusal's routine too). ENFORCED BY: AirportOps.Present.RuntimeEdit.RefusedReholdAgreesWithTheTable
	for (const TPair<int32, FEntityInstanceId>& Hold : StandHolds)
	{
		const FEntityInstance* Stand = Network.GetEntity(Hold.Value);
		if (Stand == nullptr || !Stand->PoseNode.IsSet() || !HoldStand(Hold.Key, Stand->PoseNode))
		{
			UE_LOG(LogAirsideTraffic, Warning, TEXT("Stand hold of holder %d on stand %d could not be re-made after the rebuild"),
				Hold.Key, Hold.Value.Index);
		}
	}

	// AND EVERY BODY AND ROUTE CLAIM, NOW - THE TICK'S OWN CLAIM PASS, run with no motion (push-ground-freed review I1).
	// The release above took every agent's guideline claims, and only goals and holds came back: a TAXIING or PUSHING
	// body was out of the table until the next AdvanceOnce - and a paused game (Advance(0) returns early) has none. In
	// that window the table said the ground under it was free: DepartAgent (run by the ops drain on the edit's
	// NetworkChanged) granted a push into it, and the freed diff below reported a stand free with an aircraft still on
	// it. Arbitrate is the claim pass AdvanceOnce runs first, by rank with its one re-pass - ONE claim routine, not a
	// copy of it here; it moves nobody, and the next tick's pass re-runs it over the same table, which is what every
	// tick does anyway. AFTER the holds, as a tick finds them. A STRANDED body is not re-claimed by it (the claim pass
	// gives a stranded agent only its surface, which the release above kept) - every non-stranded one is.
	// NOISE, ACCEPTED (review M-2): the pass logs its hold/resume verdicts as a tick does, so a paused edit can add one
	// "stops"/"resumes" pair per agent whose verdict the rebuild changed.
	// ENFORCED BY: Airside.Model.Traffic.PushGroundFreed.RebuildKeepsTaxiingBlocker, Airside.Model.Traffic.RunwayFreed.RebuildKeepsLeavingStandHeld,
	// Airside.Model.Traffic.RunwayFreed.RebuildKeepsCrossingHeld, AirportOps.Present.PushGroundFreed.PausedEditDepartsNothing - the last
	// calls OnGraphRebuilt on the model directly: that the actor's edit path reaches this function is not proven by it.
	Arbitrate(Network);
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

	// LAST: every claim this rebuild keeps or drops has settled, so the freed diff reads the table as the next
	// planner will. A deleted runway or stand is reported here rather than at the next Advance - the player may
	// delete and press 7 in one breath, the reason the stand claims above come back at once.
	// ENFORCED BY: Airside.Model.Traffic.RunwayFreed.DeletedRunway, Airside.Model.Traffic.RunwayFreed.StandsDiff
	DiffFreedom(Network, /*bRebuilt*/ true);
}

FRouteQuery FPlanReResolver::QueryFor(ERouteErrand Errand, FGuidelineNodeId Start, FGuidelineNodeId Goal,
	const FRoadAgent& Agent)
{
	FRouteQuery Query = FRouteQuery::For(Errand, Start, Goal, Agent.Wingspan(), Agent.Class);
	// A RE-ROUTE KEEPS THE PAVEMENT RULE its first plan had - otherwise a jet diverted by an
	// edit or a deadlock could be sent down the grass taxiway its dispatch refused.
	if (const FAirframe* Airframe = Agent.AsAircraft())
	{
		Query.NeedsPavement(Airframe->MinimumPavement);
	}
	if (const FVehicle* Vehicle = Agent.AsVehicle())
	{
		Query.WithVehicle(*Vehicle);
		// THE LIVE CHAIN (review of 9441ccf1), so a re-route of a tow is judged from where its
		// trailer IS, not from a straight lay: its axles, heading and speed, and Travelled along
		// its CURRENT plan - right for a splice's kept prefix (SpliceReplan), and overwritten by a
		// rejoin with where it starts on the new one. Not for a folded tow: it is going nowhere.
		// FRoadAgent::LiveTowSeed since issue #429 - the heading, origin and fold rules this
		// function spelled out are that function's now, and four other sites in three modules
		// that built the same seed by hand call it too.
		Query.TowSeed = Agent.LiveTowSeed();
	}
	return Query;
}

namespace
{
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
	// ENFORCED BY: AirportMgr.RigCourse.RebuildKeepsTheCourse, Airside.Model.Traffic.RebuildCoincidentTwins,
	// Airside.Model.Traffic.RebuildTwinAtCurrentStep (the Prev re-point), Airside.Model.Traffic.RebuildGoalIsATwin
	constexpr double TwinTolerance = 1.0;
	void ReResolveTwinsOf(const URoadNetwork& Network, FGuidelineNodeId Node, TArray<FGuidelineNodeId, TInlineAllocator<4>>& Out)
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
	}

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
	bool ReResolveEdgeBetween(const URoadNetwork& Network, ETraversalClass Class, FGuidelineNodeId From, FGuidelineNodeId To, FGuidelineEdgeId& Rejoined, bool& bReversed)
	{
		Network.ForEachOutgoingGuideline(From, Class, [&](FGuidelineEdgeId Candidate)
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
	//
	// NOR IS A REVERSING VEHICLE'S ROUTE (issue #434), although the plan IS the follower's: the
	// follower is not stepped while FReverseRun / FTowReverseRun play the span back, so what
	// bDriving gates has no meaning for it - no rejoin onto the pavement under it and no
	// drive-side-flip restart (RejoinTaxi and RestartTaxi both put the agent back in Taxiing, which
	// would end the reverse mid-leg), no stranding in place of a step it is not on, and no Replace
	// (Follower.Start at the handover rebuilds the profile and the reverse-leg list from the
	// remainder anyway). It is re-resolved as a route NOT YET DRIVEN, the case the taxi-in plan is.
	// ENFORCED BY: Airside.Model.Traffic.RebuildDuringReverseTruncatesTheRemainder (a step after the span fails
	// to re-resolve; without the Taxiing conjunct that step's failure strands or rejoins the reverse away)
	const bool bDriving = (&Plan == &Agent.Follower.Plan) && Agent.Phase == EAgentPhase::Taxiing;

	// THE PUSH, BY PHASE: a manoeuvring agent's plan that is not its taxi out is its push (OnGraphRebuilt's Manoeuvring
	// arm), and its address would be a fifth that agent-plan-address counts as a writer. RejoinPush and the log's name.
	// ENFORCED BY: Airside.Model.PushbackJunctionMovedBehindItCompletes (a push rejoined),
	// Airside.Model.PushbackEndGoneStopsShortOfTheRunway (named)
	const bool bPush = !bDriving && Agent.Phase == EAgentPhase::Manoeuvring && &Plan != &Agent.TaxiOutPlan;

	// WHICH PLAN, BY NAME, in the three lines this writes - asked by address, as bDriving is, the push as bPush is. A
	// push's and a taxi out's replans both printed "taxi-in" (#498's probe), which sent the reader of a log to the wrong
	// one of four routes.
	// ENFORCED BY: Airside.Model.PushbackEndGoneStopsShortOfTheRunway (the push's line),
	// Airside.Model.Traffic.HeldTaxiOut.MidPushRunwayLossHolds (the taxi out's)
	const TCHAR* const Route = &Plan == &Agent.TaxiInPlan ? TEXT("taxi-in") : &Plan == &Agent.TaxiOutPlan ? TEXT("taxi-out")
		: bPush ? TEXT("push") : TEXT("route");

	auto Strand = [&Agent, &Plan, bDriving, &Occupancy, Route](const TCHAR* Why)
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
		// together with the claim by FClaimPass::Run's non-Taxiing branch once the agent is Stranded.

		// A STRANDING IS FINAL, and that is a deliberate v1 limitation rather than an oversight.
		// Result is now Unreachable, so OnGraphRebuilt's own "!Plan->IsValid()" filter skips
		// this agent on every LATER rebuild: even if the player rebuilds the very pavement
		// that was deleted, nothing re-resolves it and it never drives again. Accepted because
		// a stranded agent is by definition standing off any live line - there is no node
		// within Rules.ResolveRadius of it - so "put it back" would mean choosing a place to
		// teleport it to, and the honest answer is that the player retires it. The Warning
		// above is what tells them there is something to retire.
		//
		// FINAL FOR THE SIMULATION, NOT FOR THE PLAYER (2026-09-29): the inspector's Unstick
		// (UGroundTraffic::RescueStranded) is the player choosing that place - a hop onto the
		// nearest line within RescueRejoinRadius - or retiring it. No REBUILD re-resolves a stranded
		// agent still, for the reason above. The one automatic rescue is a stranded aircraft WAITING
		// FOR A STAND when one frees (UGroundTraffic::ReofferStand, #429 review): the same hop the
		// player's Unstick makes, because its only other move was a restart at a node it may not be at.
		// ENFORCED BY: Airside.Model.Traffic.ReofferStrandedWaiterIsRescued
		//
		// AND IT NAMES THE RUNWAY WHEN THERE IS ONE. An agent stranded mid-crossing holds
		// that strip until the player retires it, which is a runway out of service with no
		// other evidence anywhere: the claim is visible only to the arbiter, and the next
		// refused landing says "the runway is in use" without saying who by.
		const FString Held = Agent.GetCrossingRunway().IsSet()
			? FString::Printf(TEXT(" - and it is on runway segment %d, which it holds until it is retired"),
				Agent.GetCrossingRunway().Index)
			: FString();
		UE_LOG(LogAirsideTraffic, Warning, TEXT("Agent %d's %s stranded by the rebuild: %s%s"),
			Agent.Id, Route, Why, *Held);

		// A STRANDED TAXI-IN STILL HAS A PLACE: the exit node the landing hands over at, which
		// Plan.Start was re-pointed to when it could be - a live goal for anything that asks it.
		// (The stand re-offer searches a stranded waiter's stand from where it stands, not from
		// here, since #429's review.) Spec 2026-09-07-stand-occupancy §5, amended.
		if (!bDriving && Agent.IsWaitingFor(EAgentWait::ForStand) && Plan.Start.IsSet())
		{
			Agent.SetGoal(Plan.Start);
		}
		return EReResolve::Stranded;
	};

	// THE STEP UNDER A DRIVING AGENT DID NOT RE-RESOLVE, BUT THE GROUND MAY STILL BE THERE - issue
	// #396. Re-resolve matches a step by its two end nodes, so a step whose edge was SPLIT (a new
	// junction on it) or whose start node moved along the same line fails the match with the
	// pavement intact, and was stranded as if it had been deleted. Asked of the pavement instead:
	// is there a live edge under the agent, running its way, with a route to its goal? Then it
	// carries on along that - same speed, same heading, same engine - and only when nothing is
	// there within SplitRejoinRadius is it stranded. The same aftermath as the flip's rejoin
	// below, and the same reason: a new route owns none of the old one's reservations.
	// ENFORCED BY: Airside.Model.Traffic.RebuildSplitUnderTheAgent
	auto RejoinInPlace = [&Agent, &Plan, &Context, &Occupancy]()
	{
		FRoutePlan Rejoined;
		double Travelled = 0.0;
		FVector2D At = FVector2D::ZeroVector;
		if (!GroundTrafficRejoin::RejoinNearby(Agent, Plan, Context, GroundTrafficRejoin::SplitRejoinRadius,
				Rejoined, Travelled, At))
		{
			return false;
		}
		const double Sideways = FVector2D::Distance(Agent.LastMotion.Position, At);
		// ONE Rejoin (issue #429): every guideline claim let go, the wait ended, speed, heading and
		// engine kept, the goal RE-POINTED at the rejoined route's end - the same place, maybe a new
		// handle, with no claim traffic: this rebuild re-makes every claim in its own pass.
		Agent.ApplyRouteChange(FRouteChange::Rejoin(Rejoined, Travelled, At), ERouteGoal::Repoint, Occupancy);
		UE_LOG(LogAirsideTraffic, Log,
			TEXT("Agent %d rejoined the pavement under it after the rebuild: %.0f uu sideways, %.0f uu to go"),
			Agent.Id, Sideways, Rejoined.Length - Travelled);
		return true;
	};

	// AND UNDER A PUSH (#501 review) - RejoinInPlace's question asked of the pavement under a pushing aeroplane: a live
	// edge within PushRejoinRadius, running the way it is PUSHED, with a route to the push's own end (the goal the
	// Manoeuvring arm pointed it at, or the node nearest where that end was). A junction dragged a few metres behind a push
	// fails the step match with the arm intact, and stranding it stopped the aeroplane mid-arm for good - no node within the
	// held taxi out's 30 m - where on main it had played its old line out over the pavement and departed. FPushbackRun's
	// Rejoin joins it from where it stands, or refuses a join with no room ahead (and the push strands, below).
	// ENFORCED BY: Airside.Model.PushbackJunctionMovedBehindItCompletes, Airside.Model.PushbackLeadInMovedAlongItCompletes
	//
	// THE AFTERMATH IS A REJOIN'S, SPELLED HERE (#501 re-review): FRouteChange::Rejoin is the FOLLOWER's - RejoinTaxi puts
	// the agent in Taxiing, which would end the push mid-arm - so ApplyRouteChange cannot take it, and its steps are
	// these. The old route's reservations AND guideline claims go: they name handles the rebuild freed, so they protect
	// nothing, and the push needs none kept - its clearance was a check at DepartAgent (IsPushGroundFree), not a hold,
	// and the next claim pass claims the rejoined line like any route. The arbitration fields and the stall clock reset
	// (#429's reason: the wait is over by construction, and a deadlock pass would count the old one against the new
	// line). The goal re-points at the rejoined push's end.
	auto RejoinPush = [&Agent, &Plan, &Context, &Occupancy]()
	{
		FRoutePlan Rejoined;
		double Along = 0.0;
		FVector2D At = FVector2D::ZeroVector;
		const FVector2D Here = Agent.LastMotion.Position;
		if (!GroundTrafficRejoin::RejoinNearby(Agent, Plan, Context, GroundTrafficRejoin::PushRejoinRadius,
				Rejoined, Along, At, FGuidelineNodeId(), nullptr, /*bPushed*/ true)
			|| !Agent.Pushback.Rejoin(Rejoined, Along, Here))
		{
			return false;
		}
		Occupancy.ReleaseReservations(Agent.Id);
		Occupancy.ReleaseGuidelineClaimsOf(Agent.Id);
		Agent.ClearArbitration();
		Agent.ResetStall();
		Agent.SetGoalFrom(Agent.Pushback.Plan);
		UE_LOG(LogAirsideTraffic, Log,
			TEXT("Agent %d's push rejoined the pavement under it after the rebuild: %.0f uu sideways, %.0f uu to go"),
			Agent.Id, FVector2D::Distance(Here, At), Agent.Pushback.Plan.Length - Agent.Pushback.Travelled);
		return true;
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
		if (!GroundTrafficRejoin::RejoinNearby(Agent, Plan, Context, GroundTrafficRejoin::FlipRejoinRadius,
				Rejoined, Travelled, At))
		{
			return Strand(TEXT("the drive side flipped and no lane running its way reaches its goal"));
		}
		// A REJOIN, LIKE ITS TWO SIBLINGS (issue #429, #314): the truck carries on at its speed and
		// heading, its pose moved onto the new lane. This was RestartTaxi, then the pose's position
		// re-seated by hand: the flip stopped every truck on the road dead and, until its next
		// Advance, posed it facing east (RestartTaxi's fallback LastMotion is a bare FAgentMotion - a
		// paused flip showed it so for as long as the pause lasted), where the split rejoin and the
		// rescue already kept both. RestartTaxi's fallback pose was the plan's first point; the
		// vehicle is part-way along - RejoinTaxi seats it At, where it was projected.
		// ENFORCED BY: Airside.Present.DriveSide.FlipKeepsSpeedAndHeading
		Agent.ApplyRouteChange(FRouteChange::Rejoin(Rejoined, Travelled, At), ERouteGoal::Repoint, Occupancy);
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
		if ((bDriving && RejoinInPlace()) || (bPush && RejoinPush()))
		{
			return EReResolve::Replanned;
		}
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

	// THE TWIN AND EDGE-BETWEEN RULES ARE FILE-LEVEL FUNCTIONS NOW (#455), moved with their comments above
	// ReResolveSpan, which re-points a reversing agent's span by the very same rules; these two are
	// the walk's own names for them, so the loop below reads as it always did.
	auto TwinsOf = [&Network](FGuidelineNodeId Node, TArray<FGuidelineNodeId, TInlineAllocator<4>>& Out)
	{
		ReResolveTwinsOf(Network, Node, Out);
	};
	auto EdgeBetween = [&Network, &Agent](FGuidelineNodeId From, FGuidelineNodeId To, FGuidelineEdgeId& Rejoined, bool& bReversed)
	{
		return ReResolveEdgeBetween(Network, Agent.Class, From, To, Rejoined, bReversed);
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
	//
	// A STAND CLOSED BY A TAXIWAY'S CLEARANCE STRIP IS NOT "GONE": its pose node lives, so Goal
	// resolves above and this branch never runs for it. An aircraft parked on it, or already
	// inbound to it, keeps it - the strip spec's ruling that the occupant finishes its
	// turnaround (2026-09-28). StandAdmission::Judge closes it to NEW choices only.
	// ENFORCED BY: Airside.Model.Traffic.StripClosedStandKeepsItsOccupant
	//
	// AND NOT A DEPARTURE HOLDING FOR ITS WAY OUT (issue #444). A held taxi out is DISARMED (the rebuild that stales it
	// and the taxi's runway-entry guard both disarm it), so `!bDepartureArmed` let this branch take a departing aeroplane
	// for a taxi-in whose stand had gone: retargeted to a stand, or armed to wait for one beside its taxi-out wait - the
	// double wait two flags allowed. With one wait, the stand's would REPLACE the taxi out's. It is a departure; its own
	// retry plans it a way out.
	// ENFORCED BY: Airside.Model.Traffic.HeldTaxiOut.RebuildDoesNotRetargetItToAStand
	if (!Goal.IsSet() && Agent.Class == ETraversalClass::Aircraft && Agent.AsAircraft() != nullptr
		&& !Agent.bDepartureArmed && !Agent.IsWaitingFor(EAgentWait::ForTaxiOutRoute)
		&& Failed < Plan.Steps.Num())
	{
		const FGuidelineNodeId ReplanFrom = UGroundTraffic::StepFromNode(Plan, Failed);
		const FGuidelineNodeId NewStand = ArrivalPlanner::ChooseStand(
			Network, ReplanFrom, *Agent.AsAircraft(), &Occupancy, Agent.Id);
		if (NewStand.IsSet())
		{
			Agent.SetGoal(NewStand);
			Agent.EndWait();
			UE_LOG(LogAirsideTraffic, Log, TEXT("Agent %d: its stand is gone; retargeting to the stand at node %d"),
				Agent.Id, NewStand.Index);
		}
		else
		{
			// GOAL UNCHANGED, deliberately: this branch is reached only when Goal (above)
			// failed to resolve, so the agent's existing GoalNode - whatever it is, live or
			// dead - is exactly what it carried into this call. WaitFor still takes
			// it explicitly rather than leaving the wait to flip on its own, so the
			// invariant reads the same way at every call site: the wait never moves without
			// naming the node it goes with.
			Agent.WaitFor(EAgentWait::ForStand, Agent.GoalNode);
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
		//
		// UNLESS THE PAVEMENT IS STILL UNDER IT (issue #396): a split or a moved node fails the
		// step match with the ground intact, and a rejoin onto the live edge beneath it is a
		// new route from where it IS - not Travelled re-read on other geometry - so none of the
		// above applies to it. See RejoinInPlace.
		if (Failed == FromStep)
		{
			if (RejoinInPlace())
			{
				return EReResolve::Replanned;
			}
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
		// A PUSH IS NEVER SPLICED AT ITS OWN STEP (#502) - the bDriving branch's rule above, for its reason. FPushbackRun
		// walks Plan.Polyline by Travelled as the follower does, and a splice keeps Travelled, so a tail searched from the
		// node behind the aeroplane re-read the metres already pushed on other geometry: it appeared on the detour, metres
		// sideways, and swung through the angle between the two lines in one frame (3429 uu and 26.6 degrees on
		// PushbackCurrentStepDeletedHolds; 88.6 degrees on PushbackPastTheMovedNodeEndKeptHolds, the probe's S6 with its goal
		// surviving). Under it the pavement may still be there - a lead-in shortened or moved along its own line - and
		// RejoinPush meets it by a join leg, never a hop; otherwise the push strands and holds where it stands, the rules #501
		// gave a push whose own step's node did not resolve. The rejoin used to be asked only after a failed splice, so it ran
		// only when the push's end was gone too.
		// ENFORCED BY: Airside.Model.PushbackCurrentStepDeletedHolds, Airside.Model.PushbackPastTheMovedNodeEndKeptHolds,
		// Airside.Model.PushbackLeadInMovedAlongItCompletes (the rejoin, asked first)
		if (bPush && Failed == FromStep)
		{
			if (RejoinPush())
			{
				return EReResolve::Replanned;
			}
			return Strand(TEXT("the step it is pushed along is gone and no live line within reach runs its way"));
		}

		// A PUSH IS RE-ROUTED AS A PUSH (#502), by PushbackClear - the errand PushbackPlanner planned it by: no runway edge
		// at all, free or held, and no table (a push is granted whole at DepartAgent, never queued). The rebuild's own errand
		// avoids only a HELD strip and charges a free one a penalty, so a push whose one way round to its end ran along a free
		// runway was pushed backwards down it.
		// ENFORCED BY: Airside.Model.PushbackReRouteAlongAFreeRunwayHolds
		const ERouteErrand Errand = bPush ? ERouteErrand::PushbackClear : ERouteErrand::RebuildReResolve;
		FRouteQuery Query = QueryFor(Errand, UGroundTraffic::StepFromNode(Plan, Failed), Agent.GoalNode, Agent);

		// The congestion term, as ReplanAt takes it: the guidelines that survived the rebuild
		// by handle - every hand-drawn one - still carry real queues, and a re-routed arrival
		// should be steered round them rather than into the back of one.
		// ONLY FOR AN ERRAND THAT READS THE TABLE: PushbackClear does not, and RouteSearch refuses a query that hands the
		// table to an errand that must not read it.
		if (Query.Policy.Occupancy == EOccupancyUse::Required)
		{
			Query.WithRules(Rules, Occupancy, Agent.Id);
		}

		// A COPY, so a push's re-route can be refused on its length below with Plan exactly as it came in - SpliceReplan's
		// all-or-nothing promise, kept one check further.
		FRoutePlan Spliced = Plan;
		const bool bSpliced = SpliceReplan(Network, Query, Failed, Spliced);

		// AND NO LONGER THAN THE PUSH IT WAS, BY MORE THAN ONE CLEARANCE (#502). DepartAgent granted the push whole for the
		// ground it would cover - the grant that makes a push no deadlock candidate - and a re-route round a long detour is a
		// manoeuvre nobody cleared: backing for hundreds of metres. The slack is one FootprintFor + GapFor, the figure
		// DepartAgent's push is planned to clear its junction by (a body and its gap): it takes a re-laid arm (25 uu longer on
		// PushbackReRouteShortWayCompletes) and refuses a detour (43 246 uu longer on PushbackReRouteLongDetourHolds).
		// Refused, the push is cut back to its last live node below, ends there and holds for a way out.
		// ENFORCED BY: Airside.Model.PushbackReRouteLongDetourHolds, Airside.Model.PushbackReRouteShortWayCompletes
		const double PushBound = Plan.Length + Rules.FootprintFor(Agent.Class) + Rules.GapFor(Agent.Class);
		if (bSpliced && bPush && Spliced.Length > PushBound)
		{
			UE_LOG(LogAirsideTraffic, Log,
				TEXT("Agent %d's push re-route refused by the rebuild: %.0f uu round to its end, against %.0f uu it was cleared for"),
				Agent.Id, Spliced.Length, PushBound);
		}
		else if (bSpliced)
		{
			Plan = Spliced;

			// The destination has not moved - the splice ends where the old plan did - but the
			// last step's To is now a LIVE handle, and that is what a later replan searches to.
			//
			// NOT SetGoalFrom (issue #82): that clears GoalNode to unset when Steps is empty,
			// which is right for a fresh dispatch but wrong here - FRoutePlan::IsValid() does
			// NOT guarantee Steps.Num() > 0, and a valid-but-empty splice must leave the goal
			// exactly where it was rather than blank it out from under a later replan.
			Agent.SetGoal(Plan.Steps.Num() > 0 ? Plan.Steps.Last().To : Agent.GoalNode);

			UE_LOG(LogAirsideTraffic, Log,
				TEXT("Agent %d's %s replanned by the rebuild at step %d: %.0f uu"),
				Agent.Id, Route, Failed, Plan.Length);
			return EReResolve::Replanned;
		}
	}

	if (Failed == 0)
	{
		// NOTHING SURVIVES TO KEEP. Truncating to zero steps is not a route at all - it has no
		// last step to take a length or a goal from. That is the stranded case by definition,
		// not a degenerate truncation dressed up as one.
		//
		// REACHED BY THE TWO PLANS NOBODY IS ON YET - a taxi-in and a taxi out - since a driving agent
		// and a push whose failure is at their own step were stranded above (a push on its first step
		// came through here until #502, after its splice; it strands, by the same marker, in the push
		// arm above now). This said "only a taxi-in", until the push and the taxi out
		// were re-resolved here too. An ARRIVING aircraft is not standing on its taxi-in route - it
		// is on the runway, and nothing it is driving has gone - so it gets its replan attempt first
		// and is stranded only when no route to the stand survives at all. That is why the
		// strand-in-place rule is written on the bDriving branch and not here. A PUSH stranded above,
		// with the lead-in under it gone and nothing to rejoin, stops where it stands
		// (FPushbackRun::HasArrived reads this marker); a TAXI OUT with its first step gone gets here, and
		// OnGraphRebuilt marks it to be planned again where the push ends.
		// ENFORCED BY: Airside.Model.PushbackOnDeletedGroundStops (the push, stranded by the push arm above)
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

	if (bDriving)
	{
		// Replace, NOT Start (see ReplanAt): Travelled, Speed and Heading survive and the line
		// up to the new end is the same line. The argument ALIASES Follower.Plan, deliberately -
		// the truncation was applied in place - which Replace handles: TArray's assignment
		// guards self-assignment, and what this call is here for is the speed profile, rebuilt
		// so the agent brakes to the new end instead of running off it.
		//
		// A Truncate through the seam (issue #429), with its goal RE-POINTED at the new end - the
		// SetGoalFrom this used to make itself, for every plan, just before. Nothing let go, the
		// wait left standing: see ERouteMotion::Truncate for why the rebuild's own claim pass is
		// what settles both.
		// ENFORCED BY: Airside.Model.RouteChange.TruncateKeepsTheWait
		Agent.ApplyRouteChange(FRouteChange::Truncate(Plan), ERouteGoal::Repoint, Occupancy);
	}
	else
	{
		// A PLAN NO FOLLOWER IS ON (a taxi-in, a push, a reverse's remainder): only the goal follows it.
		Agent.SetGoalFrom(Plan);
	}

	UE_LOG(LogAirsideTraffic, Log,
		TEXT("Agent %d's %s truncated by the rebuild: %d of %d steps survive, %.0f uu to the last live node"),
		Agent.Id, Route, Failed, WasSteps, Plan.Length);
	return EReResolve::Truncated;
}

bool FPlanReResolver::ReResolveSpan(FRoadAgent& Agent, FRoutePlan& Plan, int32 First, int32 Last,
	const FTrafficContext& Context, const FGuidelineNodeIndex& NodeIndex)
{
	const URoadNetwork& Network = Context.Network;
	const double Radius = Context.Rules.ResolveRadius;
	if (First < 0 || Last < First || !Plan.Steps.IsValidIndex(Last))
	{
		return false;
	}

	// THE WALK ReResolvePlan MAKES, SPAN ONLY AND WITH NO FAILURE BRANCH: the same position lookups, the same twins
	// and the same edge-between rule (they are the file-level functions above, shared, not copied), and where
	// ReResolvePlan would replan, truncate or strand this stops and says so. The reason is on the declaration: the
	// span is played from the reverse run's own snapshot and the handover cuts the remainder by a step index, so
	// nothing here may move either.
	// ENFORCED BY: Airside.Model.Traffic.RebuildDuringReverseKeepsTheSpanLive (the step count and every EndDistance
	// come out unchanged, the handles live)
	const int32 FromVertex = First == 0 ? 0 : Plan.Steps[First - 1].EndVertex;
	FGuidelineNodeId Prev = Plan.Polyline.IsValidIndex(FromVertex)
		? RouteSearch::FindNearestNode(Network, Plan.Polyline[FromVertex], Agent.Class, Radius, &NodeIndex)
		: FGuidelineNodeId();
	if (!Prev.IsSet())
	{
		UE_LOG(LogAirsideTraffic, Log,
			TEXT("Agent %d: the reverse span's start did not re-resolve - no live node where step %d begins; the span keeps the handles it had"),
			Agent.Id, First);
		return false;
	}

	// THE NODE THE SPAN LEAVES FROM is re-pointed as ReResolvePlan re-points the current step's, for the reason its
	// comment gives: the tail-node claim and the crossing arm read StepFromNode(Plan, step) every tick, and a backing
	// truck is on the span's first step from the moment it arms.
	auto RepointStart = [&Plan, First](FGuidelineNodeId Node)
	{
		if (First == 0)
		{
			Plan.Start = Node;
		}
		else
		{
			Plan.Steps[First - 1].To = Node;
		}
	};
	RepointStart(Prev);

	for (int32 Step = First; Step <= Last; ++Step)
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
			ReResolveTwinsOf(Network, Next, NextTwins);
			// The FIRST step's start is a position lookup, so it may hold the wrong twin; every later Prev was
			// chosen by the edge that reached it and is exact - ReResolvePlan's rule, for its reason.
			TArray<FGuidelineNodeId, TInlineAllocator<4>> PrevTwins;
			if (Step == First)
			{
				ReResolveTwinsOf(Network, Prev, PrevTwins);
			}
			else
			{
				PrevTwins.Add(Prev);
			}
			for (const FGuidelineNodeId From : PrevTwins)
			{
				for (const FGuidelineNodeId To : NextTwins)
				{
					if (!Rejoined.IsSet() && ReResolveEdgeBetween(Network, Agent.Class, From, To, Rejoined, bReversed))
					{
						Next = To;
						if (From != Prev)
						{
							Prev = From;
							RepointStart(Prev);
						}
					}
				}
			}
		}

		if (!Rejoined.IsSet())
		{
			UE_LOG(LogAirsideTraffic, Log,
				TEXT("Agent %d: reverse span step %d did not re-resolve - from node %d to %s; it keeps the handles it had from here on"),
				Agent.Id, Step, Prev.Index,
				Next.IsSet() ? *FString::Printf(TEXT("node %d, no edge between them"), Next.Index) : TEXT("no live node"));
			return false;
		}

		Plan.Steps[Step].Edge = Rejoined;
		Plan.Steps[Step].To = Next;
		Plan.Steps[Step].bReversed = bReversed;
		Prev = Next;
	}
	return true;
}
