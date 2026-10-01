// The rejoin search (#502): GroundTrafficRejoin::RejoinNearby (declared, with its radii and its contract, in
// Model/GroundTrafficRejoin.h) and UGroundTraffic::RescueStranded, its third caller - moved whole out of
// GroundTrafficRebuild.cpp, whose re-resolve is the other two. See Model/GroundTraffic.h for which file holds what.

#include "Model/GroundTrafficRejoin.h"

#include "AirsideLog.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteChange.h"
#include "Model/RoutePolicy.h"
#include "Model/TrafficContext.h"
#include "Solve/GuidelineGeom.h"

namespace GroundTrafficRejoin
{
	bool RejoinNearby(const FRoadAgent& Agent, const FRoutePlan& Plan, const FTrafficContext& Context,
		double Radius, FRoutePlan& OutPlan, double& OutTravelled, FVector2D& OutAt,
		FGuidelineNodeId WantedGoal, const FRouteQuery* QueryTemplate, bool bPushed)
	{
		const URoadNetwork& Network = Context.Network;
		const FVector2D Here = Agent.LastMotion.Position;
		const FVector2D Facing = FVector2D(FMath::Cos(Agent.LastMotion.Heading), FMath::Sin(Agent.LastMotion.Heading))
			* (bPushed ? -1.0 : 1.0);

		if (WantedGoal.IsSet() && Network.GetGuidelineNode(WantedGoal) == nullptr)
		{
			return false;
		}

		// The goal is usually an anchor or a stand pose, whose handle survives any rebuild. A
		// lane end does not, and the NEAREST node to where the plan ended is the wrong stand-in
		// for the same reason the start is: the old lane end's position now holds the start of
		// the lane running the other way. So: the nearest node the vehicle can ARRIVE at still
		// heading the way the old plan arrived.
		FGuidelineNodeId Goal = WantedGoal.IsSet() ? WantedGoal
			: Network.GetGuidelineNode(Agent.GoalNode) != nullptr ? Agent.GoalNode : FGuidelineNodeId();
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
		// makes a flip cheap to repeat. The split caller (#396) pays it only for an agent whose
		// OWN step failed to re-resolve - one or two per edit - on a graph of 49 edges in the
		// report's airport (2026-09-28).
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
			if (Distance > Radius)
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
			// THE CALLER'S KIND OF ROUTE when it names one (#429 review): a stand re-offer rescues a stranded waiter by
			// the taxi-in's policy, which never uses a runway; the rebuild's errand here would take an unheld one.
			FRouteQuery Query = QueryTemplate != nullptr ? *QueryTemplate
				: FPlanReResolver::QueryFor(ERouteErrand::RebuildReResolve, Candidate.From, Goal, Agent);
			Query.Start = Candidate.From;
			Query.Goal = Goal;
			// THE RULES IN FORCE, runway penalty included (#449): this took the congestion weight alone, so a level's
			// tuned RunwayPenalty was obeyed everywhere except the rejoin every split, flip and Unstick takes. ONLY FOR
			// AN ERRAND THAT READS THE TABLE - every rebuild errand does; a taxi-in's must not (RouteSearch refuses it).
			if (Query.Policy.Occupancy == EOccupancyUse::Required)
			{
				Query.WithRules(Context.Rules, Context.Occupancy, Agent.Id);
			}
			// THE REJOIN STARTS PART-WAY ALONG ITS FIRST STEP, so that is where its tow is judged
			// from - the chain as it is, not laid straight - and AT THE FOLLOWER'S SPEED, which
			// RejoinTaxi carries on for all three callers. This said "from rest (RestartTaxi below)"
			// and seeded Speed 0: true of the drive-side flip alone, which rejoins at speed since
			// issue #429 like the other two. FRoadAgent::LiveTowSeedJoining (see there).
			// ENFORCED BY: Airside.Model.Tow.LiveTowSeed; Check-Architecture rule 4 ('FTowSeed shaped by hand')
			Query.TowSeed = Agent.LiveTowSeedJoining(Candidate.Along);
			const FRoutePlan Found = RouteSearch::Find(Network, Query);
			// The route must BEGIN with the edge the vehicle is on, or starting it part-way
			// along the first step would put it on some other road.
			if (Found.IsValid() && Found.Steps.Num() > 0 && Found.Steps[0].Edge == Candidate.Edge)
			{
				OutPlan = Found;
				OutTravelled = Candidate.Along;
				OutAt = Candidate.At;
				return true;
			}
		}
		return false;
	}
}

bool UGroundTraffic::RescueStranded(int32 AgentId, const URoadNetwork& Network, FGuidelineNodeId Goal, EAgentEvent Cause,
	const FRouteQuery* QueryTemplate)
{
	// HERE, BESIDE THE REJOIN SEARCH (the two left the rebuild's file together, #502), because
	// RejoinNearby is this file's and the rescue is its third caller: the same projection onto an edge running the agent's way, the same route that must
	// begin with that edge, only a wider radius (RescueRejoinRadius, see there) and a goal the
	// caller may choose.
	const int32 Index = FindIndex(AgentId);
	if (Index == INDEX_NONE)
	{
		UE_LOG(LogAirsideTraffic, Warning, TEXT("RescueStranded %d refused: no such agent"), AgentId);
		return false;
	}
	FRoadAgent& Agent = Agents[Index];
	if (Agent.Phase != EAgentPhase::Stranded)
	{
		// A MOVING AGENT IS ReplanAt's: it keeps Travelled, Speed and Heading on the line it is on.
		// This re-seats the follower on a (possibly other) edge, which under a moving agent is a jump.
		UE_LOG(LogAirsideTraffic, Warning, TEXT("RescueStranded %d refused: agent is %s, not Stranded"),
			AgentId, *UEnum::GetValueAsString(Agent.Phase));
		return false;
	}

	FTrafficContext Context{Network, Rules, Occupancy, NodeReach, RunwayChains, SimSeconds};
	const FGuidelineNodeId WasGoal = Agent.GoalNode;
	FRoutePlan Rejoined;
	double Travelled = 0.0;
	FVector2D At = FVector2D::ZeroVector;
	const ERouteErrand Errand = QueryTemplate != nullptr ? QueryTemplate->Errand : ERouteErrand::RebuildReResolve;
	if (!GroundTrafficRejoin::RejoinNearby(Agent, Agent.Follower.Plan, Context, RescueRejoinRadius, Rejoined, Travelled, At, Goal, QueryTemplate))
	{
		UE_LOG(LogAirsideTraffic, Log,
			TEXT("RescueStranded %d refused: no pavement within %.0f uu running its way with a route to node %d"),
			AgentId, RescueRejoinRadius, (Goal.IsSet() ? Goal : WasGoal).Index);
		return false;
	}

	// THE GOAL MOVES THE WAY EVERY GOAL MOVES - ReleaseGoal then TakeGoal, as RedirectAgent and
	// ExtendRoute do - so the old goal's claim lets go and the departure is re-armed for the new
	// end. RejoinNearby used to write the goal straight onto the agent, which is right for the
	// rebuild (same goal, new handle) and skipped both here, so the old one was put back first for
	// ReleaseGoal to free (issue #429). It writes nothing now: the goal is still the old one here,
	// and ChangeRoute's Move releases it and takes the rejoined route's end.
	// ENFORCED BY: Airside.Model.Traffic.RescueStranded.NewGoal (the goal moves to D), Airside.Model.Traffic.StandClaim
	// (ChangeRoute's bracket: the old stand released and the new one held, at the redirect)
	//
	// The split rejoin's aftermath, for its reason: a new route owns none of the old one's
	// reservations, and a stall clock that ran while stranded is not a stall on this line. One
	// Rejoin through ChangeRoute, the same change the rebuild's two rejoins make.
	const double Sideways = FVector2D::Distance(Agent.LastMotion.Position, At);
	ChangeRoute(Agent, FRouteChange::Rejoin(Rejoined, Travelled, At), &Network);

	// THE ERRAND SAID: which kind of route the rescue drove by - the rebuild's, or a caller's (a stand re-offer's taxi-in).
	UE_LOG(LogAirsideTraffic, Log, TEXT("Agent %d rescued: %.0f uu sideways, %.0f uu to node %d (%s)"),
		AgentId, Sideways, Rejoined.Length - Travelled, Agent.GoalNode.Index, *UEnum::GetValueAsString(Errand));
	// LAST, and nothing read from Agent after it: a synchronous listener may retire the agent (the
	// re-entrancy contract UGroundTraffic::AdvanceOnce states). Rescued, the player's Unstick (#436):
	// the flight board keeps the taxi it was in, in whichever direction that was. ReOffered for the
	// stand re-offer's rescue of a stranded waiter (#429 review): its taxi IN, whatever its stand does.
	Announce(TransitionOf(Agent, EAgentPhase::Stranded, Cause));
	return true;
}
