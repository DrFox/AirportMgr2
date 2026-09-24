#include "Model/RouteSearch.h"

#include "AirsideLog.h"
#include "Algo/Reverse.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/TrafficOccupancy.h"
#include "Model/VehicleFit.h"
#include "Solve/GuidelineGeom.h"

namespace
{
	/**
	 * See RouteSearch::SearchCallCountForTest. Bumped once per full graph search - RunSearch
	 * (either of Find's two passes) or FindToGoals - rather than once per public entry point,
	 * so Find()'s own unconstrained retry on a TooWide failure still counts as the second
	 * search it actually runs.
	 */
	int32 GSearchCallCountForTest = 0;

	/**
	 * See RouteSearch::RunwaySeedResolveCountForTest. Bumped once per seed actually resolved -
	 * a memo hit does not count, which is what makes the count measure the memo rather than
	 * the traffic through it.
	 */
	int32 GRunwaySeedResolveCountForTest = 0;

	/**
	 * Cached length plus the query's congestion charge.
	 *
	 * Reads FGuidelineEdge::Length rather than sampling (#171): this used to call
	 * Network.SampleGuideline and measure the resulting polyline on EVERY relaxation of every
	 * edge, and a node can be relaxed several times before Closed catches it - so the same
	 * curve was sampled and measured again and again for geometry that had not changed since
	 * URoadNetwork last wrote it. Takes the edge by reference because every call site has
	 * already looked it up to check A != B; a second lookup here would just be the same
	 * allocation-per-relaxation complaint wearing a different hat.
	 */
	double EdgeCost(const FGuidelineEdge& Edge, FGuidelineEdgeId EdgeId, const FRouteQuery& Query,
		bool bRunwayEdge)
	{
		double Length = Edge.Length;

		// A RUNWAY THIS ERRAND IS STILL ALLOWED TO USE COSTS A MULTIPLE OF ITS LENGTH.
		// Layered UNDER the avoidance filter rather than replacing it: a filter is absolute
		// and is what an arrival's taxi needs, but an errand that slipped through without one
		// should degrade to a detour rather than to a free taxi down the strip - which is
		// exactly what the four undeclared call sites of 2026-09-21 did. The two never both
		// apply: a policy carrying All AND a penalty is refused by the table test, because
		// the edge is gone before this line is reached.
		//
		// MULTIPLICATIVE, NOT ADDITIVE, so a long strip costs proportionally more than a
		// short one; a flat charge would be swallowed by a 3km runway. Still >= Length, so
		// the straight-line heuristic stays admissible and the first pop stays optimal -
		// which is what FMath::Max guards here and ClampMin guards in the Details panel. A
		// query built in C++ never passes through the clamp, so both are needed.
		if (bRunwayEdge && Query.Policy.bPenaliseRunways)
		{
			Length *= FMath::Max(1.0, Query.RunwayPenalty);
		}

		// Congestion: what others hold on this edge, weighted. Additive and non-negative,
		// so the straight-line heuristic stays admissible and the first pop stays optimal.
		// Nodes are not costed - a held node is a moment, a held edge is a queue.
		//
		// The querying agent's OWN claims are excluded by HeldLengthOn, or an agent
		// replanning out of a jam would be charged for the very line it is standing on and
		// route round itself. With a null table this is bitwise the search that ran before
		// occupancy existed - see Airside.Model.RouteSearch.OccupancyCost's last assertion.
		if (Query.Occupancy != nullptr)
		{
			Length += Query.CongestionWeight * Query.Occupancy->HeldLengthOn(EdgeId, Query.QueryingAgent);
		}

		return Length;
	}

	/**
	 * Is this query answerable at all? Logged and refused rather than best-guessed.
	 *
	 * ONE FUNCTION, TWO ENTRY POINTS, AND NOT IN RunSearch. RunSearch looks like the single
	 * choke point and is not: Find returns early for NoStart, NoGoal and SameNode BEFORE
	 * reaching it, so a bad query with a dead start handle would be refused for the wrong
	 * reason and never logged - and on the TooWide path Find calls RunSearch TWICE, which
	 * would log one refusal twice. Find and FindToGoals are the consumers; they guard.
	 *
	 * ERROR, NOT WARNING: FAutomationTestBase's warning-fails-the-test flag is false in this
	 * project and nothing sets it, so a Warning would let a silently permissive route ship
	 * green - which is the exact failure this whole design exists to delete.
	 */
	bool IsQueryAnswerable(const FRouteQuery& Query)
	{
		if (Query.Errand == ERouteErrand::Unset)
		{
			UE_LOG(LogAirside, Error,
				TEXT("Route query has no errand (node %d -> %d); refusing. See FRoutePolicy."),
				Query.Start.Index, Query.Goal.Index);
			return false;
		}

		const bool bWants = Query.Policy.Occupancy == EOccupancyUse::Required;
		const bool bHas = Query.Occupancy != nullptr;
		if (bWants != bHas)
		{
			// BOTH WAYS. Required-without-a-table is the obvious half; Never-WITH-one is the
			// more useful, because a caller that went to the trouble of supplying occupancy
			// believes it is being weighted by it, and silently dropping the pointer leaves
			// that caller reasoning about a cost term the search never applied.
			UE_LOG(LogAirside, Error,
				TEXT("Route errand %d %s the occupancy table but %s given one; refusing."),
				static_cast<int32>(Query.Errand),
				bWants ? TEXT("requires") : TEXT("must not read"),
				bHas ? TEXT("was") : TEXT("was not"));
			return false;
		}

		return true;
	}

	/**
	 * Is this edge too narrow for the query's wingspan?
	 *
	 * MaxWingspan of 0 means UNLIMITED, so this is not a plain greater-than. Written once,
	 * here, because the same test read backwards routes a widebody onto a link built for a
	 * regional jet - and it would still find a route, which is the failure that never
	 * reports itself.
	 */
	bool ExceedsWingspan(const FGuidelineEdge& Edge, double Wingspan)
	{
		return Edge.MaxWingspan > 0.0 && Wingspan > Edge.MaxWingspan;
	}

	/** Ordering for the open-list min-heap: cheapest estimated total first. Free rather than
	 *  a lambda per search (#190), so RunSearch and FindToGoals's separate Open arrays share
	 *  one definition instead of two lambdas that could drift. */
	bool ByCost(const TPair<double, FGuidelineNodeId>& A, const TPair<double, FGuidelineNodeId>& B)
	{
		return A.Key < B.Key;
	}

	/**
	 * Relaxes every live outgoing edge of At into Best/Arrived/Open - the one neighbour-
	 * expansion rule, so RunSearch's single-goal walk and FindToGoals' multi-goal one (#190,
	 * deferred from #171/#201) cannot drift into two different answers for what "may this
	 * edge be crossed" means. Heuristic is the only thing that differs between the two searches
	 * (distance to the one goal, or zero - see FindToGoals' own comment) and is threaded
	 * through as a parameter instead. RunwayInUse is the caller's own map, threaded through by
	 * reference, so its per-runway memo is paid once per SEARCH, not once per expansion.
	 */
	void ExpandNode(const URoadNetwork& Network, const FRouteQuery& Query, bool bIgnoreSize,
		FGuidelineNodeId At, double Reached, const TSet<FGuidelineNodeId>& Closed,
		TFunctionRef<double(const FVector2D&)> Heuristic, TMap<int32, bool>& RunwayInUse,
		TMap<int32, bool>& RunwaySeeds,
		TMap<FGuidelineNodeId, double>& Best, TMap<FGuidelineNodeId, FRouteStep>& Arrived,
		TArray<TPair<double, FGuidelineNodeId>>& Open)
	{
		// Whether an edge's source segment IS a runway, once per segment seen. A slot lookup
		// plus a profile resolve, which the avoidance test below used to pay on EVERY relaxation
		// of every edge - a node is relaxed several times before Closed catches it, so a long
		// strip paid for the same unchanged answer again and again. This is #171's complaint
		// about EdgeCost's re-sampling, in the one place that survived it.
		//
		// PER SEARCH, NOT CACHED ON THE EDGE: runway-ness depends on the PROFILE, which changes
		// when a road is re-profiled - an open invalidation trigger with no writer positioned to
		// catch it, unlike FGuidelineEdge::Length's closed set of three writers. A stale true
		// would refuse taxiways for the rest of the session.
		auto IsRunwayEdge = [&Network, &RunwaySeeds](FRoadSegmentId Seed)
		{
			if (!Seed.IsSet())
			{
				return false;
			}
			if (const bool* Known = RunwaySeeds.Find(Seed.Index))
			{
				return *Known;
			}
			++GRunwaySeedResolveCountForTest;
			const bool bRunway = Network.IsRunwaySegment(Seed);
			RunwaySeeds.Add(Seed.Index, bRunway);
			return bRunway;
		};

		// Whether a runway's chain is in use - held by somebody other than the querier, or
		// occupied by the querier's own body (ERunwayAvoidance::Held says why both) - once
		// per runway segment seen: the chain walk and the table scan are not free, and every
		// edge along a long strip would otherwise pay for both.
		auto IsRunwayHeld = [&Network, &Query, &RunwayInUse](FRoadSegmentId Seed)
		{
			if (const bool* Known = RunwayInUse.Find(Seed.Index))
			{
				return *Known;
			}
			// bCountOwnOccupied true: ERunwayAvoidance::Held's own comment (FRouteQuery)
			// says why the querier's OWN occupied claim counts too.
			const bool bHeld = Query.Occupancy != nullptr
				&& Query.Occupancy->IsAnyHeld(Network.RunwaySurfaces(Seed), Query.QueryingAgent, /*bCountOwnOccupied*/ true);
			RunwayInUse.Add(Seed.Index, bHeld);
			return bHeld;
		};

		// Traffic class and one-way direction are already applied here - this is the
		// network's own answer to "what may leave this node", so the search never
		// re-implements the rule and cannot drift from it.
		//
		// ForEachOutgoingGuideline, not GetOutgoingGuidelines (#171): the array
		// GetOutgoingGuidelines built was a fresh TArray thrown away at the end of every
		// one of these node expansions, and a route search over a real airport expands
		// many nodes. Each `continue` below becomes a `return` from this visitor - the
		// same "skip this edge" the loop meant, since there is no outer loop left to
		// continue.
		Network.ForEachOutgoingGuideline(At, Query.Class, [&](FGuidelineEdgeId EdgeId)
		{
			const FGuidelineEdge* Edge = Network.GetGuidelineEdge(EdgeId);
			if (Edge == nullptr || Edge->A == Edge->B)
			{
				return;
			}

			if (Query.BannedEdge.IsSet() && EdgeId == Query.BannedEdge)
			{
				return;
			}

			// A banned NODE bans every edge INTO it, whichever arm - the deadlock replan's
			// blocker is an aircraft standing on the node, and an edge-only ban lets the
			// search re-enter round the back. See FRouteQuery::BannedNode.
			if (Query.BannedNode.IsSet()
				&& ((Edge->B == At ? Edge->A : Edge->B) == Query.BannedNode))
			{
				return;
			}

			// ONE ANSWER, TWO READERS: the filter just below and the cost term inside
			// EdgeCost. Asking the memo twice would be cheap, but reading it once is what
			// guarantees the edge the filter judged is the edge the cost charged for.
			const bool bRunwayEdge = IsRunwayEdge(Edge->DerivedFrom);

			// Runway-derived edges are the strip itself. See ERunwayAvoidance for who may
			// taxi along one and when.
			if (Query.AvoidRunways != ERunwayAvoidance::None
				&& bRunwayEdge
				&& (Query.AvoidRunways == ERunwayAvoidance::All || IsRunwayHeld(Edge->DerivedFrom)))
			{
				return;
			}

			// SIZE: an aircraft's wingspan, and since 2026-09-23 a vehicle's body (VehicleFit).
			// One flag for both, because Find's unconstrained retry lifts both to tell "too big"
			// from "not connected".
			if (!bIgnoreSize && (ExceedsWingspan(*Edge, Query.Wingspan)
				|| (Query.Vehicle != nullptr && !VehicleFit::Fits(*Edge, *Query.Vehicle, Network))))
			{
				return;
			}

			const double Cost = EdgeCost(*Edge, EdgeId, Query, bRunwayEdge);
			if (Cost < 0.0)
			{
				return;
			}

			const bool bReversed = (Edge->B == At);
			const FGuidelineNodeId Next = bReversed ? Edge->A : Edge->B;
			if (Closed.Contains(Next))
			{
				return;
			}

			const double Tentative = Reached + Cost;
			const double* Known = Best.Find(Next);
			if (Known != nullptr && *Known <= Tentative)
			{
				return;
			}

			Best.Add(Next, Tentative);

			FRouteStep Step;
			Step.Edge = EdgeId;
			Step.To = Next;
			Step.bReversed = bReversed;

			// CARRIED INTO THE PLAN, because FRoadAgent is world-free and has no network to
			// ask. Without this the follower cannot tell a span meant to be driven
			// backwards from any other, and drives it forwards - which is the 180 degree
			// flip this whole change exists to delete.
			Step.bReverseLeg = Edge->bReverseLeg;
			Arrived.Add(Next, Step);

			const FGuidelineNode* NextNode = Network.GetGuidelineNode(Next);
			const double Estimate = NextNode != nullptr ? Heuristic(NextNode->Position) : 0.0;
			Open.HeapPush(TPair<double, FGuidelineNodeId>(Tentative + Estimate, Next), ByCost);
		});
	}

	/**
	 * Backtraces Arrived from Goal to Start and welds the polyline - RunSearch's single-goal
	 * tail and FMultiGoalSearch::BuildPlan (any goal settled by a shared multi-goal search,
	 * #190) both call this instead of each re-reading the step map: the "lists that must
	 * agree are one list" rule CLAUDE.md asks for, applied to a backtrace instead of a UI list.
	 */
	FRoutePlan BuildPlanFromArrival(const URoadNetwork& Network, FGuidelineNodeId Start,
		const FGuidelineNode& StartNode, FGuidelineNodeId Goal, const TMap<FGuidelineNodeId, FRouteStep>& Arrived)
	{
		FRoutePlan Plan;
		Plan.Start = Start;

		if (Goal == Start)
		{
			Plan.Result = ERouteResult::SameNode;
			return Plan;
		}

		Plan.Result = ERouteResult::Unreachable;
		if (!Arrived.Contains(Goal))
		{
			return Plan;
		}
		Plan.Result = ERouteResult::Found;

		for (FGuidelineNodeId Walk = Goal; Walk != Start; )
		{
			const FRouteStep* Step = Arrived.Find(Walk);
			if (Step == nullptr)
			{
				// Only reachable if the arrival map and the closed set disagreed, which
				// would be a defect in this function rather than in the graph.
				Plan.Result = ERouteResult::Unreachable;
				Plan.Steps.Reset();
				return Plan;
			}

			const FGuidelineEdge* Edge = Network.GetGuidelineEdge(Step->Edge);
			if (Edge == nullptr)
			{
				Plan.Result = ERouteResult::Unreachable;
				Plan.Steps.Reset();
				return Plan;
			}

			Plan.Steps.Add(*Step);

			// The step ARRIVED at Walk, so the node before it is the other end: A when the
			// edge was walked forwards, B when it was walked backwards.
			Walk = Step->bReversed ? Edge->B : Edge->A;
		}
		Algo::Reverse(Plan.Steps);

		// One array for drawing and for driving. The weld is exact rather than tolerant:
		// GuidelineGeom::Sample evaluates the endpoints at t=0 and t=1, which for a
		// quadratic returns A and B themselves, so dropping each segment's first point
		// leaves no gap and no duplicate.
		Plan.Polyline.Add(StartNode.Position);
		for (int32 Index = 0; Index < Plan.Steps.Num(); ++Index)
		{
			FRouteStep& Step = Plan.Steps[Index];
			TArray<FVector2D> Points;
			if (!Network.SampleGuideline(Step.Edge, Points, Step.bReversed))
			{
				continue;
			}

			for (int32 At = 1; At < Points.Num(); ++At)
			{
				Plan.Polyline.Add(Points[At]);
			}

			// Measured off the array just appended, not off Points: the two are the same
			// numbers today, and reading the plan's own polyline is what keeps them the same
			// if the weld rule above ever changes.
			Step.EndVertex = Plan.Polyline.Num() - 1;
			Step.EndDistance = GuidelineGeom::PolylineLength(Plan.Polyline);
		}

		Plan.Length = GuidelineGeom::PolylineLength(Plan.Polyline);
		return Plan;
	}

	FRoutePlan RunSearch(const URoadNetwork& Network, const FRouteQuery& Query, bool bIgnoreSize)
	{
		++GSearchCallCountForTest;

		FRoutePlan Plan;
		Plan.Result = ERouteResult::Unreachable;
		Plan.Start = Query.Start;

		const FGuidelineNode* GoalNode = Network.GetGuidelineNode(Query.Goal);
		const FGuidelineNode* StartNode = Network.GetGuidelineNode(Query.Start);
		if (GoalNode == nullptr || StartNode == nullptr)
		{
			return Plan;
		}

		const FVector2D GoalAt = GoalNode->Position;

		// Straight-line distance to the goal. Admissible - and therefore optimal on first
		// pop - because an edge costs its sampled polyline length, and a polyline is never
		// shorter than the chord across its own ends.
		auto Heuristic = [&GoalAt](const FVector2D& From)
		{
			return FVector2D::Distance(From, GoalAt);
		};

		// Sized off the graph up front (#171): Best, Arrived and Closed can each hold every
		// node the search settles, and a search over a real airport commonly does, so growing
		// each of them by rehashing one node at a time paid for several reallocations a call
		// that reserved once would not.
		const int32 NumNodes = Network.GetGuidelineNodes().Num();

		TMap<FGuidelineNodeId, double> Best;
		Best.Reserve(NumNodes);
		TMap<FGuidelineNodeId, FRouteStep> Arrived;
		Arrived.Reserve(NumNodes);
		TSet<FGuidelineNodeId> Closed;
		Closed.Reserve(NumNodes);
		TMap<int32, bool> RunwayInUse;
		TMap<int32, bool> RunwaySeeds;
		TArray<TPair<double, FGuidelineNodeId>> Open;
		Open.Reserve(NumNodes);

		Best.Add(Query.Start, 0.0);
		Open.HeapPush(TPair<double, FGuidelineNodeId>(Heuristic(StartNode->Position), Query.Start), ByCost);

		while (Open.Num() > 0)
		{
			TPair<double, FGuidelineNodeId> Top;
			Open.HeapPop(Top, ByCost);

			const FGuidelineNodeId At = Top.Value;

			// A node can be pushed more than once, because a cheaper way to it may be found
			// while an older entry is still queued. Skipping the stale pop is what keeps
			// this correct without an expensive decrease-key.
			if (Closed.Contains(At))
			{
				continue;
			}
			Closed.Add(At);

			if (At == Query.Goal)
			{
				Plan.Result = ERouteResult::Found;
				break;
			}

			const FGuidelineNode* Node = Network.GetGuidelineNode(At);
			if (Node == nullptr)
			{
				continue;
			}

			const double Reached = Best.FindChecked(At);
			ExpandNode(Network, Query, bIgnoreSize, At, Reached, Closed, Heuristic, RunwayInUse, RunwaySeeds, Best, Arrived, Open);
		}

		if (Plan.Result != ERouteResult::Found)
		{
			return Plan;
		}
		return BuildPlanFromArrival(Network, Query.Start, *StartNode, Query.Goal, Arrived);
	}
}

FRouteQuery FRouteQuery::For(ERouteErrand Errand, FGuidelineNodeId Start, FGuidelineNodeId Goal,
	double Wingspan, ETraversalClass Class)
{
	FRouteQuery Query;
	Query.Start = Start;
	Query.Goal = Goal;
	Query.Class = Class;
	Query.Wingspan = Wingspan;
	Query.Errand = Errand;
	Query.Policy = FRoutePolicy::For(Errand);

	// THE TABLE OVERWRITES THE FIELD, rather than being set beside it. AvoidRunways stays a
	// public member because the search reads it in the hot loop; making the table its only
	// writer here is what stops a caller setting both and getting whichever was assigned
	// last.
	Query.AvoidRunways = Query.Policy.Avoidance;
	return Query;
}

FGuidelineNodeIndex::FGuidelineNodeIndex(const URoadNetwork& Network, double CellSizeIn)
	// GUARDED, NOT ASSERTED: Rules.ResolveRadius is EditAnywhere, so a project file can set
	// it to 0 or less, and dividing CellOf's position by a zero or negative cell size would
	// either crash or bucket every node into one cell - silently turning the index back
	// into the linear scan it exists to replace, with none of the log lines that would say
	// so. 1.0 uu is small enough that a real ResolveRadius never reaches this floor.
	: CellSize(CellSizeIn > 0.0 ? CellSizeIn : 1.0)
{
	const TArray<FGuidelineNode>& Nodes = Network.GetGuidelineNodes();
	for (int32 Index = 0; Index < Nodes.Num(); ++Index)
	{
		// DEAD SLOTS NEVER GO IN. FindNearestNode's own bAlive check would refuse them
		// again at query time, but building the index over the live ones only is what keeps
		// it close to the size of the graph a rebuild actually leaves behind, not the
		// high-water mark of every slot ever allocated.
		if (Nodes[Index].bAlive)
		{
			Cells.FindOrAdd(CellOf(Nodes[Index].Position)).Add(Index);
		}
	}
}

FRoutePlan FMultiGoalSearch::BuildPlan(const URoadNetwork& Network, FGuidelineNodeId Goal) const
{
	const FGuidelineNode* StartNode = Network.GetGuidelineNode(Start);
	if (StartNode == nullptr)
	{
		// The search this came from already required a live Start node to produce anything -
		// only reachable if Start died between FindToGoals and this call, which is the
		// caller's own network mutating mid-choice, not something this function can repair.
		FRoutePlan Plan;
		Plan.Result = ERouteResult::NoStart;
		return Plan;
	}
	return BuildPlanFromArrival(Network, Start, *StartNode, Goal, Arrived);
}

namespace
{
	/**
	 * See RouteSearch::NodeVisitCountForTest. A free variable behind the namespace functions
	 * that read and reset it, rather than a class static, because FindNearestNode itself is
	 * a free function - matching FRoadNetworkSolver::NodeClaimsCallCountForTest's own reason
	 * for being static rather than a member.
	 */
	int32 GNodeVisitCountForTest = 0;

	/**
	 * The one per-node test both FindNearestNode paths run, so the linear scan and the
	 * indexed lookup cannot drift into two different answers for what "usable" means.
	 * Advances Nearest/NearestDistance in place exactly as the old inline loop body did -
	 * skips a dead node, skips one farther than the best found so far, then skips one no
	 * edge of this Class may leave or arrive at - and counts the visit either way, which is
	 * the boundary Airside.Model.Traffic.GraphRebuildNodeVisits measures.
	 *
	 * <= NOT <, deliberately: a node exactly as far as the current best REPLACES it, which
	 * is what makes the last node visited at the smallest distance the winner on an exact
	 * tie. Preserved from the original loop rather than tightened to <, because a caller
	 * relies on that ordering - the indexed path below reproduces it exactly by visiting
	 * candidates in the same ascending-index order the linear scan always has.
	 */
	void ConsiderNode(const URoadNetwork& Network, int32 NodeIndex, const FVector2D& Position,
		ETraversalClass Class, double& NearestDistance, FGuidelineNodeId& Nearest)
	{
		++GNodeVisitCountForTest;

		const FGuidelineNode& Node = Network.GetGuidelineNodes()[NodeIndex];
		if (!Node.bAlive)
		{
			return;
		}

		const double Distance = FVector2D::Distance(Node.Position, Position);
		if (Distance > NearestDistance)
		{
			return;
		}

		// Incidence alone, deliberately ignoring one-way direction: this same call picks
		// both ends of a query, and a node reachable only by arriving is a perfectly good
		// DESTINATION. Direction is the search's business.
		bool bUsable = false;
		for (const FGuidelineEdgeId EdgeId : Node.Incident)
		{
			const FGuidelineEdge* Edge = Network.GetGuidelineEdge(EdgeId);
			if (Edge != nullptr && Edge->AllowedTraffic.Allows(Class))
			{
				bUsable = true;
				break;
			}
		}
		if (!bUsable)
		{
			return;
		}

		// GuidelineNodeIdAt, NEVER a hand-built {Index, Node.Generation} (#79's own rule,
		// flagged again at this exact site by #172): a second place that knows a handle is
		// {slot index, slot generation} is a second place that can build one wrong, and the
		// slot map already has the one function that is allowed to.
		Nearest = Network.GuidelineNodeIdAt(NodeIndex);
		NearestDistance = Distance;
	}
}

namespace RouteSearch
{
	int32 NodeVisitCountForTest() { return GNodeVisitCountForTest; }
	void ResetNodeVisitCountForTest() { GNodeVisitCountForTest = 0; }

	int32 RunwaySeedResolveCountForTest() { return GRunwaySeedResolveCountForTest; }
	void ResetRunwaySeedResolveCountForTest() { GRunwaySeedResolveCountForTest = 0; }

	int32 SearchCallCountForTest() { return GSearchCallCountForTest; }
	void ResetSearchCallCountForTest() { GSearchCallCountForTest = 0; }

	FRoutePlan Find(const URoadNetwork& Network, const FRouteQuery& Query)
	{
		FRoutePlan Plan;

		// FIRST, ahead of the handle checks: a query with no errand is malformed whatever
		// its handles say, and reporting NoStart for it would hide the real fault.
		if (!IsQueryAnswerable(Query))
		{
			return Plan;
		}

		if (Network.GetGuidelineNode(Query.Start) == nullptr)
		{
			Plan.Result = ERouteResult::NoStart;
			return Plan;
		}

		if (Network.GetGuidelineNode(Query.Goal) == nullptr)
		{
			Plan.Result = ERouteResult::NoGoal;
			return Plan;
		}

		if (Query.Start == Query.Goal)
		{
			Plan.Result = ERouteResult::SameNode;
			Plan.Start = Query.Start;
			return Plan;
		}

		Plan = RunSearch(Network, Query, /*bIgnoreSize=*/false);
		if (Plan.IsValid() || (Query.Wingspan <= 0.0 && Query.Vehicle == nullptr))
		{
			return Plan;
		}

		// Paid only on failure, and only when a wingspan was actually given. The answer it
		// buys - "the taxiways are joined up, your aircraft is too big" - is a different
		// job for the player than "nothing connects these two".
		const FRoutePlan Unconstrained = RunSearch(Network, Query, /*bIgnoreSize=*/true);
		if (Unconstrained.IsValid())
		{
			Plan.Result = Query.Vehicle != nullptr ? ERouteResult::TooNarrow : ERouteResult::TooWide;
			if (Query.Vehicle != nullptr)
			{
				// WHERE, walked along the route that WOULD have been taken: the first edge on it
				// this vehicle does not fit. Not necessarily the only one - the first is what a
				// player driving it would hit.
				for (const FRouteStep& Step : Unconstrained.Steps)
				{
					const FGuidelineEdge* Edge = Network.GetGuidelineEdge(Step.Edge);
					if (Edge != nullptr && !VehicleFit::Fits(*Edge, *Query.Vehicle, Network))
					{
						Plan.RejectedEdge = Step.Edge;
						break;
					}
				}
			}
		}

		return Plan;
	}

	FMultiGoalSearch FindToGoals(const URoadNetwork& Network, const FRouteQuery& Query,
		const TArray<FGuidelineNodeId>& Goals, TArray<FGoalReach>& OutReach)
	{
		FMultiGoalSearch Result;
		Result.Start = Query.Start;

		// SIZED BEFORE THE GUARD BELOW CAN RETURN, so a caller reading OutReach[i] against
		// Goals[i] does not index an empty array just because its query was refused.
		OutReach.Init(FGoalReach(), Goals.Num());

		// AHEAD OF THE COUNTER, because a refused query runs no search and must not be
		// counted as one - SearchCallCountForTest is how ChooseStand's own cost is measured.
		if (!IsQueryAnswerable(Query))
		{
			return Result;
		}

		++GSearchCallCountForTest;

		const FGuidelineNode* StartNode = Network.GetGuidelineNode(Query.Start);
		if (StartNode == nullptr)
		{
			return Result;
		}

		// Every distinct, live goal other than Start itself - the same SameNode/NoGoal
		// exclusion Find() applies per-goal before it ever searches, applied here up front so
		// the loop below only ever waits on a goal that could actually be settled.
		TSet<FGuidelineNodeId> Unsettled;
		for (const FGuidelineNodeId& Goal : Goals)
		{
			if (Goal.IsSet() && Goal != Query.Start && Network.GetGuidelineNode(Goal) != nullptr)
			{
				Unsettled.Add(Goal);
			}
		}

		// Sized off the graph up front, same reasoning as RunSearch's own comment (#171).
		const int32 NumNodes = Network.GetGuidelineNodes().Num();
		Result.Best.Reserve(NumNodes);
		Result.Arrived.Reserve(NumNodes);
		TSet<FGuidelineNodeId> Closed;
		Closed.Reserve(NumNodes);
		TMap<int32, bool> RunwayInUse;
		TMap<int32, bool> RunwaySeeds;
		TArray<TPair<double, FGuidelineNodeId>> Open;
		Open.Reserve(NumNodes);

		// Zero: see this function's own header comment on why a single-goal heuristic's
		// traversal-order saving is moot for a search that must settle every goal, held ones
		// included.
		auto Heuristic = [](const FVector2D&) { return 0.0; };

		Result.Best.Add(Query.Start, 0.0);
		Open.HeapPush(TPair<double, FGuidelineNodeId>(0.0, Query.Start), ByCost);

		while (Open.Num() > 0 && Unsettled.Num() > 0)
		{
			TPair<double, FGuidelineNodeId> Top;
			Open.HeapPop(Top, ByCost);

			const FGuidelineNodeId At = Top.Value;
			if (Closed.Contains(At))
			{
				continue;
			}
			Closed.Add(At);
			Unsettled.Remove(At);

			const FGuidelineNode* Node = Network.GetGuidelineNode(At);
			if (Node == nullptr)
			{
				continue;
			}

			const double Reached = Result.Best.FindChecked(At);
			ExpandNode(Network, Query, /*bIgnoreSize=*/false, At, Reached, Closed, Heuristic,
				RunwayInUse, RunwaySeeds, Result.Best, Result.Arrived, Open);
		}

		for (int32 Index = 0; Index < Goals.Num(); ++Index)
		{
			const FGuidelineNodeId Goal = Goals[Index];
			if (!Goal.IsSet() || Goal == Query.Start)
			{
				continue;
			}
			const double* Length = Result.Best.Find(Goal);
			if (Length != nullptr && Result.Arrived.Contains(Goal))
			{
				OutReach[Index].bReachable = true;
				OutReach[Index].Length = *Length;
			}
		}

		return Result;
	}

	FRoutePlan Splice(const FRoutePlan& Head, int32 KeepSteps, const FRoutePlan& Tail)
	{
		FRoutePlan Out;
		Out.Result = ERouteResult::Unreachable;
		if (!Head.IsValid() || !Tail.IsValid() || KeepSteps < 0 || KeepSteps > Head.Steps.Num()
			|| Tail.Polyline.Num() < 2)
		{
			return Out;
		}

		const FGuidelineNodeId JoinNode = KeepSteps == 0 ? Head.Start : Head.Steps[KeepSteps - 1].To;
		if (JoinNode != Tail.Start)
		{
			return Out;
		}

		const int32 JoinVertex = KeepSteps == 0 ? 0 : Head.Steps[KeepSteps - 1].EndVertex;
		const double JoinDistance = KeepSteps == 0 ? 0.0 : Head.Steps[KeepSteps - 1].EndDistance;

		Out.Result = ERouteResult::Found;
		Out.Start = Head.Start;
		for (int32 At = 0; At <= JoinVertex; ++At)
		{
			Out.Polyline.Add(Head.Polyline[At]);
		}
		for (int32 Index = 0; Index < KeepSteps; ++Index)
		{
			Out.Steps.Add(Head.Steps[Index]);
		}

		// The tail's first point IS the join node, so it is dropped - the same weld rule
		// RunSearch applies between consecutive edges.
		for (int32 At = 1; At < Tail.Polyline.Num(); ++At)
		{
			Out.Polyline.Add(Tail.Polyline[At]);
		}
		for (const FRouteStep& Step : Tail.Steps)
		{
			FRouteStep Rebased = Step;
			Rebased.EndVertex += JoinVertex;
			Rebased.EndDistance += JoinDistance;
			Out.Steps.Add(Rebased);
		}
		Out.Length = GuidelineGeom::PolylineLength(Out.Polyline);
		return Out;
	}

	FGuidelineNodeId FindNearestNode(
		const URoadNetwork& Network, const FVector2D& Position,
		ETraversalClass Class, double MaxDistance, const FGuidelineNodeIndex* Index)
	{
		FGuidelineNodeId Nearest;
		double NearestDistance = MaxDistance;

		if (Index == nullptr)
		{
			// THE UNINDEXED PATH, UNCHANGED IN COST: every node, in slot order. Still the
			// only path a single-shot caller (HoldingPointTool's one click; a test that asks
			// once) should take - building a grid to answer one query is the O(N) scan
			// wearing a slower hat.
			const TArray<FGuidelineNode>& Nodes = Network.GetGuidelineNodes();
			for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
			{
				ConsiderNode(Network, NodeIndex, Position, Class, NearestDistance, Nearest);
			}
			return Nearest;
		}

		// THE INDEXED PATH. CellRadius is how many rings of cells out a node within
		// MaxDistance could possibly be: MaxDistance divided by the cell size, rounded up,
		// floored at one so a MaxDistance smaller than the cell still gets its own ring of
		// eight neighbours rather than just its own cell (a node can sit anywhere within a
		// cell, so one that shares the query's cell is not the only one within reach of a
		// point near that cell's edge). Every caller today builds the index at the same
		// radius it queries with (Rules.ResolveRadius both times), which makes this 1; kept
		// as a division rather than hard-coded so an index built at one radius and queried
		// at a smaller one - the only shape that could ever need more than the eight - still
		// gets a wide enough ring rather than a silently wrong answer.
		const int32 CellRadius = FMath::Max(1, FMath::CeilToInt32(MaxDistance / Index->CellSize));
		const FIntPoint Origin = Index->CellOf(Position);

		// COLLECTED, THEN SORTED, THEN VISITED IN THAT ORDER - not visited cell by cell as
		// they are found. ConsiderNode's <= means the LAST node visited at the smallest
		// distance wins a tie, and the linear scan above visits every node in ascending slot
		// order; sorting the candidates back into that same order before visiting them is
		// what makes an indexed query and a linear one agree on which of two equidistant
		// nodes to return, not merely on the distance. See
		// Airside.Model.RouteSearch.IndexMatchesLinear, which is built to catch exactly this
		// if the sort is ever dropped as "obviously" unnecessary.
		TArray<int32> Candidates;
		for (int32 OffsetY = -CellRadius; OffsetY <= CellRadius; ++OffsetY)
		{
			for (int32 OffsetX = -CellRadius; OffsetX <= CellRadius; ++OffsetX)
			{
				if (const TArray<int32>* Bucket = Index->Cells.Find(FIntPoint(Origin.X + OffsetX, Origin.Y + OffsetY)))
				{
					Candidates.Append(*Bucket);
				}
			}
		}
		Candidates.Sort();

		for (int32 NodeIndex : Candidates)
		{
			ConsiderNode(Network, NodeIndex, Position, Class, NearestDistance, Nearest);
		}
		return Nearest;
	}
}

FRoutePlan RouteSearch::Section(const FRoutePlan& Plan, int32 First, int32 Last)
{
	FRoutePlan Out;
	if (!Plan.Steps.IsValidIndex(First) || !Plan.Steps.IsValidIndex(Last) || Last < First)
	{
		return Out;
	}

	const int32 FromVertex = First == 0 ? 0 : Plan.Steps[First - 1].EndVertex;
	const int32 ToVertex = Plan.Steps[Last].EndVertex;
	if (!Plan.Polyline.IsValidIndex(FromVertex) || !Plan.Polyline.IsValidIndex(ToVertex)
		|| ToVertex <= FromVertex)
	{
		return Out;
	}

	const double FromDistance = First == 0 ? 0.0 : Plan.Steps[First - 1].EndDistance;

	Out.Result = ERouteResult::Found;
	Out.Start = First == 0 ? Plan.Start : Plan.Steps[First - 1].To;
	Out.Length = Plan.Steps[Last].EndDistance - FromDistance;

	Out.Polyline.Reserve(ToVertex - FromVertex + 1);
	for (int32 At = FromVertex; At <= ToVertex; ++At)
	{
		Out.Polyline.Add(Plan.Polyline[At]);
	}

	Out.Steps.Reserve(Last - First + 1);
	for (int32 At = First; At <= Last; ++At)
	{
		FRouteStep Step = Plan.Steps[At];
		Step.EndDistance -= FromDistance;
		Step.EndVertex -= FromVertex;
		Out.Steps.Add(Step);
	}
	return Out;
}

void FRoutePlan::DescribeSpanDirections(TArray<EDriveDirection>& Out) const
{
	Out.Reset();
	if (Polyline.Num() < 2)
	{
		return;
	}

	Out.Init(EDriveDirection::Forward, Polyline.Num() - 1);

	int32 From = 0;
	for (const FRouteStep& Step : Steps)
	{
		const int32 To = FMath::Clamp(Step.EndVertex, From, Polyline.Num() - 1);
		if (Step.bReverseLeg)
		{
			for (int32 Span = From; Span < To; ++Span)
			{
				Out[Span] = EDriveDirection::Reverse;
			}
		}
		From = To;
	}
}
