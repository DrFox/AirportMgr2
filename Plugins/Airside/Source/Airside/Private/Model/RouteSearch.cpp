#include "Model/RouteSearch.h"

#include "Algo/Reverse.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Solve/GuidelineGeom.h"

namespace
{
	/** The guideline's points in its own A-to-B order. False if either end is dead. */
	bool EdgePoints(const URoadNetwork& Network, const FGuidelineEdge& Edge, TArray<FVector2D>& Out)
	{
		const FGuidelineNode* A = Network.GetGuidelineNode(Edge.A);
		const FGuidelineNode* B = Network.GetGuidelineNode(Edge.B);
		if (A == nullptr || B == nullptr)
		{
			return false;
		}

		GuidelineGeom::Sample(A->Position, Edge.Control, B->Position, Out);
		return true;
	}

	/** Negative when the edge cannot be measured, which is how the search skips it. */
	double EdgeCost(const URoadNetwork& Network, const FGuidelineEdge& Edge)
	{
		TArray<FVector2D> Points;
		if (!EdgePoints(Network, Edge, Points))
		{
			return -1.0;
		}
		return GuidelineGeom::PolylineLength(Points);
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

	FRoutePlan RunSearch(const URoadNetwork& Network, const FRouteQuery& Query, bool bIgnoreWingspan)
	{
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

		auto ByCost = [](const TPair<double, FGuidelineNodeId>& A, const TPair<double, FGuidelineNodeId>& B)
		{
			return A.Key < B.Key;
		};

		TMap<FGuidelineNodeId, double> Best;
		TMap<FGuidelineNodeId, FRouteStep> Arrived;
		TSet<FGuidelineNodeId> Closed;
		TArray<TPair<double, FGuidelineNodeId>> Open;

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

			// Traffic class and one-way direction are already applied here - this is the
			// network's own answer to "what may leave this node", so the search never
			// re-implements the rule and cannot drift from it.
			for (const FGuidelineEdgeId EdgeId : Network.GetOutgoingGuidelines(At, Query.Class))
			{
				const FGuidelineEdge* Edge = Network.GetGuidelineEdge(EdgeId);
				if (Edge == nullptr || Edge->A == Edge->B)
				{
					continue;
				}

				if (!bIgnoreWingspan && ExceedsWingspan(*Edge, Query.Wingspan))
				{
					continue;
				}

				const double Cost = EdgeCost(Network, *Edge);
				if (Cost < 0.0)
				{
					continue;
				}

				const bool bReversed = (Edge->B == At);
				const FGuidelineNodeId Next = bReversed ? Edge->A : Edge->B;
				if (Closed.Contains(Next))
				{
					continue;
				}

				const double Tentative = Reached + Cost;
				const double* Known = Best.Find(Next);
				if (Known != nullptr && *Known <= Tentative)
				{
					continue;
				}

				Best.Add(Next, Tentative);

				FRouteStep Step;
				Step.Edge = EdgeId;
				Step.To = Next;
				Step.bReversed = bReversed;
				Arrived.Add(Next, Step);

				const FGuidelineNode* NextNode = Network.GetGuidelineNode(Next);
				const double Estimate = NextNode != nullptr ? Heuristic(NextNode->Position) : 0.0;
				Open.HeapPush(TPair<double, FGuidelineNodeId>(Tentative + Estimate, Next), ByCost);
			}
		}

		if (Plan.Result != ERouteResult::Found)
		{
			return Plan;
		}

		for (FGuidelineNodeId Walk = Query.Goal; Walk != Query.Start; )
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
		Plan.Polyline.Add(StartNode->Position);
		for (const FRouteStep& Step : Plan.Steps)
		{
			const FGuidelineEdge* Edge = Network.GetGuidelineEdge(Step.Edge);
			if (Edge == nullptr)
			{
				continue;
			}

			TArray<FVector2D> Points;
			if (!EdgePoints(Network, *Edge, Points))
			{
				continue;
			}

			if (Step.bReversed)
			{
				Algo::Reverse(Points);
			}

			for (int32 At = 1; At < Points.Num(); ++At)
			{
				Plan.Polyline.Add(Points[At]);
			}
		}

		Plan.Length = GuidelineGeom::PolylineLength(Plan.Polyline);
		return Plan;
	}
}

namespace RouteSearch
{
	FRoutePlan Find(const URoadNetwork& Network, const FRouteQuery& Query)
	{
		FRoutePlan Plan;

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

		Plan = RunSearch(Network, Query, /*bIgnoreWingspan=*/false);
		if (Plan.IsValid() || Query.Wingspan <= 0.0)
		{
			return Plan;
		}

		// Paid only on failure, and only when a wingspan was actually given. The answer it
		// buys - "the taxiways are joined up, your aircraft is too big" - is a different
		// job for the player than "nothing connects these two".
		const FRoutePlan Unconstrained = RunSearch(Network, Query, /*bIgnoreWingspan=*/true);
		if (Unconstrained.IsValid())
		{
			Plan.Result = ERouteResult::TooWide;
		}

		return Plan;
	}

	FGuidelineNodeId FindNearestNode(
		const URoadNetwork& Network, const FVector2D& Position,
		ETraversalClass Class, double MaxDistance)
	{
		FGuidelineNodeId Nearest;
		double NearestDistance = MaxDistance;

		const TArray<FGuidelineNode>& Nodes = Network.GetGuidelineNodes();
		for (int32 Index = 0; Index < Nodes.Num(); ++Index)
		{
			const FGuidelineNode& Node = Nodes[Index];
			if (!Node.bAlive)
			{
				continue;
			}

			const double Distance = FVector2D::Distance(Node.Position, Position);
			if (Distance > NearestDistance)
			{
				continue;
			}

			// Incidence alone, deliberately ignoring one-way direction: this same call
			// picks both ends of a query, and a node reachable only by arriving is a
			// perfectly good DESTINATION. Direction is the search's business.
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
				continue;
			}

			FGuidelineNodeId Id;
			Id.Index = Index;
			Id.Generation = Node.Generation;

			Nearest = Id;
			NearestDistance = Distance;
		}

		return Nearest;
	}
}
