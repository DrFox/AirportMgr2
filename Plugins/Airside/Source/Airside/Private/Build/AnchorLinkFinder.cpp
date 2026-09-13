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

		const FGuidelineNode* EndA = Network.GetGuidelineNode(Edge.A);
		const FGuidelineNode* EndB = Network.GetGuidelineNode(Edge.B);
		if (EndA == nullptr || EndB == nullptr)
		{
			continue;
		}

		TArray<FVector2D> Points;
		GuidelineGeom::Sample(EndA->Position, Edge.Control, EndB->Position, Points);

		FGuidelineEdgeId Id;
		Id.Index = Index;
		Id.Generation = Edge.Generation;

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

		const FGuidelineNode* EndA = Network.GetGuidelineNode(Edge.A);
		const FGuidelineNode* EndB = Network.GetGuidelineNode(Edge.B);
		if (EndA == nullptr || EndB == nullptr)
		{
			continue;
		}

		TArray<FVector2D> Points;
		GuidelineGeom::Sample(EndA->Position, Edge.Control, EndB->Position, Points);

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
		BestEdge.Index = Index;
		BestEdge.Generation = Edge.Generation;
	}

	if (!BestEdge.IsSet())
	{
		return false;
	}
	OutHit.Edge = BestEdge;
	OutHit.Param = BestParam;
	return true;
}

bool FLaneLinkFinder::Find(const URoadNetwork& Network, const FPendingLink& Link,
	const TSet<FGuidelineNodeId>& AnchorNodes, FLinkHit& OutHit) const
{
	double Best = Link.Reach;
	FGuidelineEdgeId BestEdge;
	double BestParam = 0.0;
	FGuidelineEdgeId BestLaneEdge;
	double BestLaneParam = 0.0;

	const TArray<FGuidelineEdge>& Edges = Network.GetGuidelineEdges();
	for (int32 Index = 0; Index < Edges.Num(); ++Index)
	{
		const FGuidelineEdge& Edge = Edges[Index];
		if (!IsJoinable(Edge, Link, AnchorNodes))
		{
			continue;
		}

		const FGuidelineNode* EndA = Network.GetGuidelineNode(Edge.A);
		const FGuidelineNode* EndB = Network.GetGuidelineNode(Edge.B);
		if (EndA == nullptr || EndB == nullptr)
		{
			continue;
		}

		TArray<FVector2D> Points;
		GuidelineGeom::Sample(EndA->Position, Edge.Control, EndB->Position, Points);

		FGuidelineEdgeId Id;
		Id.Index = Index;
		Id.Generation = Edge.Generation;

		// LANE TO ROAD: closest approach between two polylines, so a road drawn PARALLEL to
		// the lane is measured side to side rather than corner to corner. That parallel case
		// is the whole point - it is how a player draws a service road along a row of stands.
		for (const FGuidelineEdgeId& LaneId : Link.Lane)
		{
			const FGuidelineEdge* LaneEdge = Network.GetGuidelineEdge(LaneId);
			const FGuidelineNode* LaneA = LaneEdge != nullptr ? Network.GetGuidelineNode(LaneEdge->A) : nullptr;
			const FGuidelineNode* LaneB = LaneEdge != nullptr ? Network.GetGuidelineNode(LaneEdge->B) : nullptr;
			if (LaneA == nullptr || LaneB == nullptr)
			{
				continue;
			}

			TArray<FVector2D> LanePoints;
			GuidelineGeom::Sample(LaneA->Position, LaneEdge->Control, LaneB->Position, LanePoints);

			int32 LaneSpan = 0, RoadSpan = 0;
			double LaneFraction = 0.0, RoadFraction = 0.0;
			const double Distance = GuidelineGeom::NearestBetweenPolylines(
				LanePoints, Points, LaneSpan, LaneFraction, RoadSpan, RoadFraction);

			if (Distance <= FAnchorLink::LeadInWeldTolerance || Distance >= Best)
			{
				continue;
			}

			Best = Distance;
			BestEdge = Id;
			BestParam = GuidelineGeom::ParamAtSample(RoadSpan, RoadFraction, Points.Num());
			BestLaneEdge = LaneId;
			BestLaneParam = GuidelineGeom::ParamAtSample(LaneSpan, LaneFraction, LanePoints.Num());
		}
	}

	if (!BestEdge.IsSet())
	{
		return false;
	}
	OutHit.Edge = BestEdge;
	OutHit.Param = BestParam;
	OutHit.LaneEdge = BestLaneEdge;
	OutHit.LaneParam = BestLaneParam;
	return true;
}

const ILinkFinder& LinkFinderFor(ELinkKind Kind)
{
	static const FRayLinkFinder Ray;
	static const FProximityLinkFinder Proximity;
	static const FLaneLinkFinder Lane;

	switch (Kind)
	{
	case ELinkKind::Ray:        return Ray;
	case ELinkKind::Lane:       return Lane;
	case ELinkKind::Proximity:
	default:                    return Proximity;
	}
}
