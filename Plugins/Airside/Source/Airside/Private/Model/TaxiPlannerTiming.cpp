// FTaxiPlanner's CLOCK AND HOLD RULE - how long an edge takes, whether a joint turns instantly, whether an aircraft may
// stop at a node - apart from the search (TaxiPlanner.cpp) that asks them. Split when PR 2 (2026-10-02) grew the search
// past rule 77's 800 lines: these are answers the search reads, each from its authority (FSpeedProfile, FTrafficRules::
// IsBox), and they share nothing with the search's state but the planner's memos.

#include "Model/TaxiPlanner.h"

#include "Model/Airframe.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadTraffic.h"
#include "Model/SpeedProfile.h"
#include "Model/TrafficClaims.h"
#include "Model/TrafficRules.h"

const TArray<FVector2D>& FTaxiPlanner::SamplesOf(FGuidelineEdgeId Edge, bool bReversed)
{
	const TPair<FGuidelineEdgeId, bool> Key(Edge, bReversed);
	if (const TArray<FVector2D>* Known = EdgeSamples.Find(Key))
	{
		return *Known;
	}
	TArray<FVector2D> Points;
	// SampleGuideline, the one sampler: the array the follower will walk, never a second evaluation of the curve.
	if (!Network.SampleGuideline(Edge, Points, bReversed))
	{
		Points.Reset();
	}
	return EdgeSamples.Add(Key, MoveTemp(Points));
}

const FTaxiEdgeSeconds& FTaxiPlanner::SecondsFor(FGuidelineEdgeId Edge, bool bReversed)
{
	const TPair<FGuidelineEdgeId, bool> Key(Edge, bReversed);
	if (const FTaxiEdgeSeconds* Known = EdgeSeconds.Find(Key))
	{
		return *Known;
	}

	FTaxiEdgeSeconds Seconds;
	const TArray<FVector2D> Points = SamplesOf(Edge, bReversed);
	const FChassis& Chassis = Airframe.Chassis;
	if (Points.Num() >= 2 && Chassis.Ground.IsSet())
	{
		// THE AUTHORITY, PER PIECE: the rules the follower's whole-route profile applies, over this edge's own samples.
		// Two builds give the four ways an edge is driven - rolling or at rest at each end. Entering "rolling" is at
		// the piece's own limit there; across an edge boundary that skips the braking a NEXT edge's bend asks of this
		// one - the planner's one approximation, bounded by Airside.Model.TaxiPlan.EtaAgreesWithWholeRouteProfile.
		// An instant corner AT the boundary is not skipped: IsSharpJoint times it as a stop.
		FSpeedProfile Rolls;
		Rolls.BuildPiece(Points, Chassis, EPieceEnd::Rolls);
		FSpeedProfile Stops;
		Stops.BuildPiece(Points, Chassis, EPieceEnd::Stops);
		Seconds.RollRoll = Rolls.SecondsToDrive(Rolls.LimitAt(0.0));
		Seconds.RestRoll = Rolls.SecondsToDrive(0.0);
		Seconds.RollRest = Stops.SecondsToDrive(Stops.LimitAt(0.0));
		Seconds.RestRest = Stops.SecondsToDrive(0.0);
		Seconds.bUsable = !Rolls.IsEmpty() && !Stops.IsEmpty();
	}
	return EdgeSeconds.Add(Key, Seconds);
}

bool FTaxiPlanner::IsSharpJoint(FGuidelineEdgeId In, bool bInReversed, FGuidelineEdgeId Out, bool bOutReversed)
{
	const TTuple<FGuidelineEdgeId, bool, FGuidelineEdgeId, bool> Key(In, bInReversed, Out, bOutReversed);
	if (const bool* Known = SharpJoints.Find(Key))
	{
		return *Known;
	}

	bool bSharp = false;
	const TArray<FVector2D> Before = SamplesOf(In, bInReversed);
	const TArray<FVector2D> After = SamplesOf(Out, bOutReversed);
	if (Before.Num() >= 2 && After.Num() >= 2)
	{
		// The span into the joint and the span out of it, welded at the shared node as RouteSearch welds them, and
		// the authority asked whether that vertex turns instantly. Quiet: a piece.
		const TArray<FVector2D> Joint = { Before[Before.Num() - 2], Before.Last(), After[1] };
		FSpeedProfile Probe;
		Probe.BuildPiece(Joint, Airframe.Chassis, EPieceEnd::Rolls);
		bSharp = Probe.HasSharpVertex();
	}
	return SharpJoints.Add(Key, bSharp);
}

double FTaxiPlanner::RouteSeconds(const FRoutePlan& Route)
{
	double Total = 0.0;
	const int32 Count = Route.Steps.Num();
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const FRouteStep& Step = Route.Steps[Index];
		const FTaxiEdgeSeconds& Seconds = SecondsFor(Step.Edge, Step.bReversed);
		const bool bFromRest = Index == 0
			|| IsSharpJoint(Route.Steps[Index - 1].Edge, Route.Steps[Index - 1].bReversed, Step.Edge, Step.bReversed);
		const bool bToRest = Index == Count - 1
			|| IsSharpJoint(Step.Edge, Step.bReversed, Route.Steps[Index + 1].Edge, Route.Steps[Index + 1].bReversed);
		Total += bFromRest ? (bToRest ? Seconds.RestRest : Seconds.RestRoll) : (bToRest ? Seconds.RollRest : Seconds.RollRoll);
	}
	return Total;
}

bool FTaxiPlanner::CanHoldAt(const URoadNetwork& InNetwork, const FTrafficRules& InRules, FGuidelineEdgeId Arrived,
	FGuidelineNodeId At, FNodeReachCache* InReach)
{
	const FGuidelineEdge* Edge = InNetwork.GetGuidelineEdge(Arrived);
	const FGuidelineNode* Node = InNetwork.GetGuidelineNode(At);
	if (Edge == nullptr || Node == nullptr)
	{
		return false;
	}

	// INSIDE A JUNCTION, NEVER (spec §1): an aircraft that stops on a turn path blocks every line through the
	// junction, and its tail the node behind. A turn path is what the builder laid at a junction (AtJunction); a box
	// is any step too short to stand on clear of the node behind - the claim pass's own rule, asked, not copied. A
	// crossing's conflict node is where a road and a taxiway contend: stopping on it holds the road.
	if (Edge->AtJunction.IsSet() || InRules.IsBox(Edge->Length, ETraversalClass::Aircraft) || Node->bCrossingConflict)
	{
		return false;
	}

	// AND WAITING THERE MUST LEAVE THE NODE BEHIND FREE, by the claim pass's own measure (measured on M_ScaleGatwick at 80
	// mov/h, 2026-10-02): an aircraft held at At stops a gap short of where At's reach begins, and the node it came from
	// stays claimed while its centre is within half a footprint of that node's reach. Where the lines part slowly there,
	// a lane long enough to be no box still left the waiter claiming the node behind it - which its plan had given up -
	// and an aircraft booked through that node next waited on it, while it waited on that aircraft. So the lane must hold
	// the gap, half a body and both nodes' reaches: the claim pass's ReachExcessAt, one rule for both readers.
	FNodeReachCache Local;
	FNodeReachCache& Reach = InReach != nullptr ? *InReach : Local;
	const FGuidelineNodeId Behind = Edge->A == At ? Edge->B : Edge->A;
	const double F = InRules.FootprintFor(ETraversalClass::Aircraft);
	const double G = InRules.GapFor(ETraversalClass::Aircraft);
	const double Needed = G + F * 0.5
		+ FClaimPass::ReachExcessAt(InRules, Reach, InNetwork, Behind, Arrived, ETraversalClass::Aircraft)
		+ FClaimPass::ReachExcessAt(InRules, Reach, InNetwork, At, Arrived, ETraversalClass::Aircraft);
	return Edge->Length > Needed;
}
