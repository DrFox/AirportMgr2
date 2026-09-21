#include "Build/AnchorLinkFinder.h"

#include "Build/AnchorLink.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Solve/GuidelineGeom.h"

namespace
{
	/**
	 * Every joinable guideline is bAlive, bDerived, not a self-loop, has NEITHER end already
	 * anchored, and allows Link's traversal class.
	 *
	 * Shared by all three finders rather than copied into each: every lead-in has an anchor
	 * node at one end, so excluding AnchorNodes excludes every existing lead-in with no
	 * separate mark on the edge needed - the same reasoning FAnchorLink::Build's search used
	 * to state once and each of the three finders would otherwise have had to restate.
	 */
	bool IsJoinable(const FGuidelineEdge& Edge, const FPendingLink& Link,
		const TSet<FGuidelineNodeId>& AnchorNodes)
	{
		return Edge.bAlive && Edge.bDerived && Edge.A != Edge.B
			&& !AnchorNodes.Contains(Edge.A) && !AnchorNodes.Contains(Edge.B)
			&& Edge.AllowedTraffic.Allows(Link.Class);
	}

	/** 2D cross product. Positive when B is counter-clockwise of A. */
	double Cross(const FVector2D& A, const FVector2D& B)
	{
		return A.X * B.Y - A.Y * B.X;
	}

	/**
	 * Axis-aligned box around a guideline edge's three control points (its two ends and its
	 * Control) - always CONTAINING, because a quadratic Bezier never leaves the convex hull of
	 * the points that define it, and never exact, because the curve does not fill the hull.
	 *
	 * FOR REJECTING AN EDGE BEFORE PAYING FOR URoadNetwork::SampleGuideline, which walks the
	 * whole polyline and heap-allocates it (#177): every joinable edge in the network used to be
	 * sampled by every link's own Find call, so an airport with a hundred taxiways nowhere near a
	 * given stand paid for all hundred on each of that stand's entries. This box is the cheap
	 * question "could the curve possibly be closer than Reach", asked with three subtractions
	 * instead of a walk.
	 */
	struct FEdgeBounds
	{
		FVector2D Min;
		FVector2D Max;
	};

	FEdgeBounds BoundsOf(const FVector2D& A, const FVector2D& Control, const FVector2D& B)
	{
		FEdgeBounds Bounds;
		Bounds.Min = FVector2D(FMath::Min3(A.X, Control.X, B.X), FMath::Min3(A.Y, Control.Y, B.Y));
		Bounds.Max = FVector2D(FMath::Max3(A.X, Control.X, B.X), FMath::Max3(A.Y, Control.Y, B.Y));
		return Bounds;
	}

	/**
	 * Squared distance from Point to the nearest point OF the box - zero when Point is inside
	 * it. Squared so the caller compares against a squared reach and never pays for a sqrt on an
	 * edge it is about to reject anyway.
	 */
	double DistSqrToBounds(const FVector2D& Point, const FEdgeBounds& Bounds)
	{
		const double Dx = FMath::Max(Bounds.Min.X - Point.X, FMath::Max(0.0, Point.X - Bounds.Max.X));
		const double Dy = FMath::Max(Bounds.Min.Y - Point.Y, FMath::Max(0.0, Point.Y - Bounds.Max.Y));
		return Dx * Dx + Dy * Dy;
	}

	/**
	 * True when Origin could not possibly reach Edge's curve within Best - so the caller may
	 * skip sampling it outright.
	 *
	 * SAFE FOR A RAY TOO, not just proximity, because Dir is always unit length here: the point a
	 * ray actually hits is Origin + Dir * AlongRay, so the Euclidean distance from Origin to that
	 * point equals AlongRay exactly, and the box - being a superset of the curve - can only be
	 * NEARER to Origin than that point is. A box the ray could not reach within Best therefore
	 * proves the true hit, if any, is no closer than Best either, which is exactly the bound the
	 * per-point rejection just below already enforces; this only avoids paying to compute it.
	 */
	bool CannotReachWithin(const FVector2D& Origin, const FGuidelineNode& A,
		const FGuidelineEdge& Edge, const FGuidelineNode& B, double Best)
	{
		const FEdgeBounds Bounds = BoundsOf(A.Position, Edge.Control, B.Position);
		return DistSqrToBounds(Origin, Bounds) >= Best * Best;
	}

	/**
	 * Ray against one segment. OutAlongRay is in uu; OutAlongSegment is a 0..1 fraction.
	 *
	 * A ray, not a line: a lead-in points one way, and a guideline BEHIND the stand is
	 * behind the aircraft's tail. Testing the infinite line would happily join a stand to
	 * the taxiway it is facing away from.
	 */
	bool RayHitsSegment(
		const FVector2D& Origin, const FVector2D& Dir,
		const FVector2D& P, const FVector2D& Q,
		double& OutAlongRay, double& OutAlongSegment)
	{
		const FVector2D Edge = Q - P;
		const double Denominator = Cross(Dir, Edge);
		if (FMath::IsNearlyZero(Denominator, UE_DOUBLE_SMALL_NUMBER))
		{
			// Parallel. A collinear ray running along the guideline is deliberately not a
			// hit: there is no single point to join, and picking one would be arbitrary.
			return false;
		}

		const FVector2D ToP = P - Origin;
		OutAlongRay = Cross(ToP, Edge) / Denominator;
		OutAlongSegment = Cross(ToP, Dir) / Denominator;

		return OutAlongSegment >= 0.0 && OutAlongSegment <= 1.0;
	}
}

bool FRayLinkFinder::Find(const URoadNetwork& Network, const FPendingLink& Link,
	const TSet<FGuidelineNodeId>& AnchorNodes, FLinkHit& OutHit) const
{
	double Best = Link.Reach;
	FGuidelineEdgeId BestEdge;
	double BestParam = 0.0;

	// Re-read each time: a previous anchor may have split the very guideline this one is
	// about to hit, and it must see the halves rather than the edge that is gone.
	const TArray<FGuidelineEdge>& Edges = Network.GetGuidelineEdges();
	for (int32 Index = 0; Index < Edges.Num(); ++Index)
	{
		const FGuidelineEdge& Edge = Edges[Index];
		if (!IsJoinable(Edge, Link, AnchorNodes))
		{
			continue;
		}

		const FGuidelineNode* NodeA = Network.GetGuidelineNode(Edge.A);
		const FGuidelineNode* NodeB = Network.GetGuidelineNode(Edge.B);
		if (NodeA == nullptr || NodeB == nullptr)
		{
			continue;
		}

		// REJECTED BEFORE SAMPLING, for #177 - see CannotReachWithin. Best tightens as a
		// nearer edge is found, so this gets MORE aggressive over the loop, exactly like the
		// per-point AlongRay test a few lines down.
		if (CannotReachWithin(Link.At, *NodeA, Edge, *NodeB, Best))
		{
			continue;
		}

		// Network.GuidelineEdgeIdAt, not a hand-built handle (#79, #173).
		const FGuidelineEdgeId Id = Network.GuidelineEdgeIdAt(Index);

		TArray<FVector2D> Points;
		if (!Network.SampleGuideline(Id, Points))
		{
			continue;
		}

		for (int32 At = 1; At < Points.Num(); ++At)
		{
			double AlongRay = 0.0;
			double AlongSegment = 0.0;
			if (!RayHitsSegment(Link.At, Link.Dir, Points[At - 1], Points[At], AlongRay, AlongSegment))
			{
				continue;
			}

			// Strictly ahead, and no further than the cap.
			if (AlongRay <= FAnchorLink::LeadInWeldTolerance || AlongRay >= Best)
			{
				continue;
			}

			Best = AlongRay;
			BestParam = GuidelineGeom::ParamAtSample(At - 1, AlongSegment, Points.Num());
			BestEdge = Id;
		}
	}

	if (!BestEdge.IsSet())
	{
		return false;
	}
	OutHit.Edge = BestEdge;
	OutHit.Param = BestParam;
	return true;
}

bool FProximityLinkFinder::Find(const URoadNetwork& Network, const FPendingLink& Link,
	const TSet<FGuidelineNodeId>& AnchorNodes, FLinkHit& OutHit) const
{
	double Best = Link.Reach;
	FGuidelineEdgeId BestEdge;
	double BestParam = 0.0;

	const TArray<FGuidelineEdge>& Edges = Network.GetGuidelineEdges();
	for (int32 Index = 0; Index < Edges.Num(); ++Index)
	{
		const FGuidelineEdge& Edge = Edges[Index];
		if (!IsJoinable(Edge, Link, AnchorNodes))
		{
			continue;
		}

		const FGuidelineNode* NodeA = Network.GetGuidelineNode(Edge.A);
		const FGuidelineNode* NodeB = Network.GetGuidelineNode(Edge.B);
		if (NodeA == nullptr || NodeB == nullptr)
		{
			continue;
		}

		// REJECTED BEFORE SAMPLING, for #177: this finder used to sample EVERY joinable edge
		// in the network on EVERY call, so a stand's four declared entries each paid for a full
		// scan of every taxiway and every other stand's lane on the airport, almost none of
		// which could ever be within ServiceLinkRadius. See CannotReachWithin.
		if (CannotReachWithin(Link.At, *NodeA, Edge, *NodeB, Best))
		{
			continue;
		}

		TArray<FVector2D> Points;
		// Network.GuidelineEdgeIdAt, not a hand-built handle (#79, #173).
		const FGuidelineEdgeId ThisId = Network.GuidelineEdgeIdAt(Index);
		if (!Network.SampleGuideline(ThisId, Points))
		{
			continue;
		}

		// PROXIMITY, ANY DIRECTION. A vehicle may genuinely arrive from any side, and an
		// anchor's authored heading has no representation on screen for a player to aim by -
		// so measuring a distance is the only rule they can actually satisfy.
		int32 Span = 0;
		double Fraction = 0.0;
		const double Distance = GuidelineGeom::NearestOnPolyline(Points, Link.At, Span, Fraction);
		if (Distance <= FAnchorLink::LeadInWeldTolerance || Distance >= Best)
		{
			continue;
		}

		Best = Distance;
		BestParam = GuidelineGeom::ParamAtSample(Span, Fraction, Points.Num());
		BestEdge = ThisId;
	}

	if (!BestEdge.IsSet())
	{
		return false;
	}
	OutHit.Edge = BestEdge;
	OutHit.Param = BestParam;
	return true;
}

// FLaneLinkFinder IS DELETED, 2026-09-16, and a deleted strategy needs an argument.
//
// It measured the closest approach between a stand's WHOLE LANE and a road, because the ring
// it was written for declared no entrance: the pass had to find a point on the lane to join
// AT as well as something to join TO, and the point it found then had to be cut into the lane.
// A stand declares its entries now - see UEntityDefinition::ServiceLane's Entry waypoints, and
// FStandLayoutBuild::FResult::Entries for the nodes they became - so the FROM end of the link is
// a node that already exists, and a node's rule is proximity. One question, one finder.
//
// What it really bought, a road drawn PARALLEL to a lane being measured side to side rather
// than corner to corner, is not lost: the four declared entries ARE the corners, and
// FAnchorLink::Gather gives each point of road to the entry nearest it. The measurement itself
// survives as GuidelineGeom::NearestBetweenPolylines, which the clearance tests still use.

const ILinkFinder& LinkFinderFor(ELinkKind Kind)
{
	static const FRayLinkFinder Ray;
	static const FProximityLinkFinder Proximity;

	switch (Kind)
	{
	case ELinkKind::Ray:        return Ray;
	case ELinkKind::Proximity:
	default:                    return Proximity;
	}
}
