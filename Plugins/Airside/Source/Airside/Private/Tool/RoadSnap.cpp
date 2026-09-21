#include "Tool/RoadSnap.h"

#include "Build/RoadNetworkSolver.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"
#include "Model/RoadNode.h"
#include "Solve/RoadGeom.h"

namespace
{
	/**
	 * A safe UPPER BOUND on how far a node's junction could possibly claim, computed WITHOUT
	 * a junction solve - issue #167. Every live node outside the fixed radius used to pay a
	 * full FRoadNetworkSolver::NodeClaims (SolveNodeCuts + SolveBoundary, with the TArray
	 * allocations that go with assembling arms and a boundary polygon) just to be told no; on
	 * an airport of more than a handful of nodes that is the dominant cost of every hover.
	 *
	 * DERIVED FROM THE SOLVER'S OWN CLAMP, not from "half-width + fillet radius" as the
	 * obvious-looking shortcut: FRoadNetworkSolver::SolveNodeCuts caps every arm's CutDistance
	 * at ArmAllowance, which is at most that arm's own chord length (see BuildNodeInput's
	 * "THE ALLOWANCE IS SET BY BOTH ENDS" comment in RoadNetworkSolver.cpp - ArmAllowance is
	 * MinHere + SlackShare * Slack, and Slack = Length - MinHere - MinFar, so ArmAllowance can
	 * never exceed Length). A fillet's cot(Theta/2) term has no such bound on its own and would
	 * make "half-width + fillet radius" an UNSAFE bound on a sharp, wide-radius corner. Chord
	 * length + half-width is always at least as large as the true NodeReach, so this can only
	 * ever admit a node to the real solve, never wrongly refuse one - see RoadSnap.h's own
	 * comment: "the claim is the junction's PAVEMENT", and this bound is an over-approximation
	 * of it, never a substitute.
	 *
	 * Cheap on purpose: no tangents, no arcs, no boundary polygon - just each incident
	 * segment's stored endpoints and its profile's half-width, both already resident on the
	 * node and the network.
	 */
	double MaxPossibleNodeClaimReach(const URoadNetwork& Network, const FRoadNode& Node)
	{
		double Reach = 0.0;
		for (const FRoadSegmentId& SegmentId : Node.Incident)
		{
			const FRoadSegment* Segment = Network.GetSegment(SegmentId);
			if (Segment == nullptr)
			{
				continue;
			}
			const FRoadNode* A = Network.GetNode(Segment->A);
			const FRoadNode* B = Network.GetNode(Segment->B);
			if (A == nullptr || B == nullptr)
			{
				continue;
			}
			const URoadProfile* Profile = Network.ProfileFor(*Segment);
			const double HalfWidth = Profile != nullptr ? Profile->GetMaxHalfWidth() : 0.0;
			Reach = FMath::Max(Reach, FVector2D::Distance(A->Position, B->Position) + HalfWidth);
		}
		return Reach;
	}
}

bool FRoadNodeSnapRule::Resolve(const URoadNetwork& Network, const FRoadSnapQuery& Query,
	const FRoadSnapSettings& Settings, FRoadSnapResult& Out) const
{
	const FVector2D& Cursor = Query.Cursor;

	if (Settings.NodeRadius <= 0.0)
	{
		return false;
	}

	// Compared squared throughout, so a wide radius costs no square roots.
	//
	// The radius is PER NODE, not one global value, because a junction's pavement extends
	// by HalfWidth + |R / tan(Theta/2)| - several times NodeRadius on any real road, and
	// different at every node. A fixed radius left a band where the cursor was inside a
	// junction and still resolved Free, which built a second node inside existing pavement:
	// two junction polygons at one Z, which is a z-fight rather than a surface.
	double BestSquared = TNumericLimits<double>::Max();
	int32 Best = INDEX_NONE;

	const double Fixed = Settings.NodeRadius;
	const double FixedSquared = Fixed * Fixed;

	const TArray<FRoadNode>& Nodes = Network.GetNodes();
	for (int32 Index = 0; Index < Nodes.Num(); ++Index)
	{
		if (!Nodes[Index].bAlive)
		{
			continue;
		}

		// THE NODE BEING DRAGGED DOES NOT CLAIM ITS OWN CURSOR - the cursor is on it, so
		// without this it always wins and the node can never move anywhere.
		//
		// BY HANDLE, not by position. Two nodes legitimately sit at the same coordinates for
		// the frame between a drop and the merge that resolves them, and a position test
		// would exclude the merge TARGET as well as the node in hand - which is the one
		// thing the drag most needs to find.
		if (Query.ExcludeNode.IsSet() && Query.ExcludeNode.Index == Index)
		{
			continue;
		}

		const double DistanceSquared = FVector2D::DistSquared(Nodes[Index].Position, Cursor);
		if (DistanceSquared >= BestSquared)
		{
			continue;
		}

		bool bClaimed = DistanceSquared <= FixedSquared;

		// Solved ONLY when the fixed radius has already declined. The common case - a
		// cursor sitting on a node - never pays for a junction solve at all, and the rest
		// costs one solve per candidate node on a graph of tens of nodes.
		//
		// The claim is the junction's PAVEMENT, not a circle of its reach. A circle of the
		// deepest cut covered open ground beside a tight corner, and the cursor a hand's
		// width off the concrete still snapped to the node (2026-09-06, "same node").
		//
		// A CHEAP REJECT FIRST - issue #167. On an airport of more than a handful of nodes,
		// every node outside the fixed radius used to pay the solve above just to be told no;
		// MaxPossibleNodeClaimReach answers the same question from data already on the node
		// (arm lengths and profile widths) with no solve at all, and only lets a candidate
		// through to NodeClaims when it could possibly be inside the real, tighter boundary.
		if (!bClaimed && Settings.JunctionSnapFactor > 0.0)
		{
			const double MaxReach = MaxPossibleNodeClaimReach(Network, Nodes[Index]) * Settings.JunctionSnapFactor;
			if (DistanceSquared <= MaxReach * MaxReach)
			{
				const FRoadNodeId Id = Network.NodeIdAt(Index);
				bClaimed = FRoadNetworkSolver::NodeClaims(Network, Id, Cursor, Settings.JunctionSnapFactor);
			}
		}

		if (bClaimed)
		{
			BestSquared = DistanceSquared;
			Best = Index;
		}
	}

	if (Best == INDEX_NONE)
	{
		return false;
	}

	Out.Kind = ERoadSnapKind::Node;
	Out.Node = Network.NodeIdAt(Best);

	// The node's stored position, copied - never the cursor, and never recomputed. A
	// click that reuses a node has to land on the coordinates the graph already holds.
	Out.Position = Nodes[Best].Position;
	return true;
}

bool FRoadSegmentSnapRule::Resolve(const URoadNetwork& Network, const FRoadSnapQuery& Query,
	const FRoadSnapSettings& Settings, FRoadSnapResult& Out) const
{
	const FVector2D& Cursor = Query.Cursor;

	if (!Settings.bSnapToSegments || Settings.SegmentRadius <= 0.0)
	{
		return false;
	}

	// THE ROAD YOU CAN SEE IS THE ROAD YOU HIT. SegmentRadius alone is a flat band about the
	// CENTRELINE - 1.5 m by default - so on a 6 m service road most of the pavement did not
	// snap to the road it is painted on, and the plot tool refused to anchor while the cursor
	// was plainly on the tarmac (PIE, 2026-09-16). The reach is per segment now: the
	// authored radius, or the road's own half width, whichever is larger.
	//
	// SegmentRadius keeps its meaning as the margin for a NEAR miss - hovering just off a
	// narrow road still finds it - which is why this is a floor rather than a replacement.
	//
	// EXACTLY THE FIX FRoadNodeSnapRule ALREADY MADE, one rule further down the chain: see
	// its comment on why its radius is per node. A fixed radius left a band where the cursor
	// was inside pavement and still resolved Free. This file had learned that for junctions
	// and not for the segments between them.
	double BestSquared = TNumericLimits<double>::Max();
	int32 Best = INDEX_NONE;
	double BestT = 0.0;
	FVector2D BestPoint = FVector2D::ZeroVector;

	const TArray<FRoadSegment>& Segments = Network.GetSegments();
	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		const FRoadSegment& Segment = Segments[Index];
		if (!Segment.bAlive)
		{
			continue;
		}

		// AN ARM OF THE DRAGGED NODE IS NOT SPLITTABLE BY THAT NODE'S OWN CURSOR. The node
		// rule above has already excluded the node; without the same guard here its arms
		// remain, and the cursor a little way along one of them offers to SPLIT the very
		// road it is dragging - a new node dropped mid-arm, mid-drag.
		if (Query.ExcludeNode.IsSet()
			&& (Segment.A == Query.ExcludeNode || Segment.B == Query.ExcludeNode))
		{
			continue;
		}

		const FRoadNode* EndA = Network.GetNode(Segment.A);
		const FRoadNode* EndB = Network.GetNode(Segment.B);
		if (EndA == nullptr || EndB == nullptr)
		{
			continue;
		}

		// The CHORD, not the Bezier. Every segment this tool can author is straight, and
		// AddSegment interpolates a segment's interior in a straight line regardless, so
		// the chord is the road. Curved authoring has to bring the curve's own closest
		// point with it; approximating a curve by its chord here would put the split
		// visibly off the pavement.
		const double T = RoadGeom::ClosestPointOnSegment(EndA->Position, EndB->Position, Cursor);

		// Exactly 0 or 1 is the clamp reporting that the closest point IS an endpoint.
		// That neighbourhood belongs to the node rule, which has already had its turn and
		// declined - so passing here means Free, which is right: the cursor is off the end
		// of the road, not on it.
		if (T <= 0.0 || T >= 1.0)
		{
			continue;
		}

		const FVector2D Point = FMath::Lerp(EndA->Position, EndB->Position, T);
		const double DistanceSquared = FVector2D::DistSquared(Point, Cursor);

		// Asked of the PROFILE, which is what actually decides how wide the ribbon is drawn -
		// the same source URoadSurfacePresenter builds the mesh from, so "on the pavement"
		// here and "on the pavement" on screen cannot mean two different things.
		const double Reach = FMath::Max(Settings.SegmentRadius,
			Segment.Profile != nullptr ? Segment.Profile->GetMaxHalfWidth() : 0.0);
		if (DistanceSquared > Reach * Reach || DistanceSquared > BestSquared)
		{
			continue;
		}

		// A split this close to an end leaves a stub the solver cannot trim: its two cut
		// lines would cross, and the junction it feeds would fold through itself.
		//
		// The real exclusion is the endpoint's own junction - a split inside it puts a new
		// node in pavement that already exists, which is the overlap the node rule above
		// absorbs. Stood off by the arm's CUT, which is exactly where the junction's polygon
		// ends and this segment's pavement begins: NodeReach added a half-width to that and
		// left a band of segment pavement claimed by neither rule, where a click built a node
		// inside existing concrete (2026-09-06). MinSplitFromEndpoint survives as the floor
		// for an endpoint that paves nothing to stand off from.
		const FRoadSegmentId SegmentId = Network.SegmentIdAt(Index);
		const double ClearA = FMath::Max(
			Settings.MinSplitFromEndpoint,
			FRoadNetworkSolver::ArmCutDistance(Network, SegmentId, Segment.A) * Settings.JunctionSnapFactor);
		const double ClearB = FMath::Max(
			Settings.MinSplitFromEndpoint,
			FRoadNetworkSolver::ArmCutDistance(Network, SegmentId, Segment.B) * Settings.JunctionSnapFactor);

		if (FVector2D::Distance(Point, EndA->Position) < ClearA
			|| FVector2D::Distance(Point, EndB->Position) < ClearB)
		{
			continue;
		}

		BestSquared = DistanceSquared;
		Best = Index;
		BestT = T;
		BestPoint = Point;
	}

	if (Best == INDEX_NONE)
	{
		return false;
	}

	Out.Kind = ERoadSnapKind::Segment;
	Out.Segment = Network.SegmentIdAt(Best);
	Out.SegmentT = BestT;
	Out.Position = BestPoint;
	return true;
}

FRoadSnapChain::FRoadSnapChain()
{
	AddRule(MakeUnique<FRoadNodeSnapRule>());
	AddRule(MakeUnique<FRoadSegmentSnapRule>());
}

void FRoadSnapChain::AddRule(TUniquePtr<IRoadSnapRule> Rule)
{
	if (Rule.IsValid())
	{
		Rules.Add(MoveTemp(Rule));
	}
}

FRoadSnapResult FRoadSnapChain::Resolve(const URoadNetwork& Network, const FRoadSnapQuery& Query,
	const FRoadSnapSettings& Settings) const
{
	for (const TUniquePtr<IRoadSnapRule>& Rule : Rules)
	{
		FRoadSnapResult Claimed;
		if (Rule->Resolve(Network, Query, Settings, Claimed))
		{
			return Claimed;
		}
	}

	FRoadSnapResult Free;
	Free.Kind = ERoadSnapKind::Free;
	Free.Position = Query.Cursor;
	return Free;
}
