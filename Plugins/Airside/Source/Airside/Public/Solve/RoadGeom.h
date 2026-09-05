#pragma once

#include "CoreMinimal.h"

/** A 2D ray. Dir is always normalised. */
struct FRay2D
{
	FVector2D Origin = FVector2D::ZeroVector;
	FVector2D Dir    = FVector2D(1.0, 0.0);
};

/**
 * Dependency-free 2D geometry used by the junction solver.
 * Must not gain any dependency beyond CoreMinimal.h.
 */
namespace RoadGeom
{
	/** Counter-clockwise perpendicular: (x,y) -> (-y,x). */
	AIRSIDE_API FVector2D PerpCCW(const FVector2D& V);

	/** Rotate by Radians counter-clockwise. */
	AIRSIDE_API FVector2D Rotate(const FVector2D& V, double Radians);

	/** atan2 bearing in (-UE_DOUBLE_PI, UE_DOUBLE_PI]. */
	AIRSIDE_API double Bearing(const FVector2D& Dir);

	/** CCW angle from From to To, in [0, 2*UE_DOUBLE_PI). */
	AIRSIDE_API double CcwAngleBetween(const FVector2D& From, const FVector2D& To);

	/** Intersection of the two infinite lines. False if near-parallel. */
	AIRSIDE_API bool LineIntersect(const FRay2D& A, const FRay2D& B, FVector2D& OutPoint);

	/**
	 * Parameter in [0,1] of the point on segment A->B closest to P.
	 *
	 * Clamped, so it describes the closest point on the SEGMENT, not on its infinite
	 * line: a t of exactly 0 or 1 therefore means the closest point is an endpoint, which
	 * is the signal the segment snap rule uses to stand down and let the node rule own
	 * that neighbourhood. A zero-length segment returns 0.
	 */
	AIRSIDE_API double ClosestPointOnSegment(const FVector2D& A, const FVector2D& B, const FVector2D& P);

	/**
	 * True when segments A0-A1 and B0-B1 cross each other properly.
	 *
	 * A shared endpoint is NOT a crossing: consecutive edges of an outline meet at a corner
	 * by construction, and reporting that as self-intersection would refuse every polygon
	 * with more than two sides. Collinear overlap is not reported either - the same
	 * degeneracies IsSimplePolygon documents, and for the same reason.
	 */
	AIRSIDE_API bool SegmentsCross(const FVector2D& A0, const FVector2D& A1,
		const FVector2D& B0, const FVector2D& B1);

	/**
	 * True when Point lies inside the polygon, by crossing number.
	 *
	 * Winding-agnostic, so it answers the same for an outline stored either way round. A
	 * point exactly on an edge is not guaranteed either answer, which is acceptable for
	 * picking - the cursor is never exactly on a line - and would not be for containment
	 * that decides geometry.
	 */
	AIRSIDE_API bool PointInPolygon(TArrayView<const FVector2D> Polygon, const FVector2D& Point);

	/** Signed area via the shoelace formula. Positive means CCW winding. */
	AIRSIDE_API double PolygonArea(TArrayView<const FVector2D> Points);

	/**
	 * True if no pair of non-adjacent edges crosses transversally. O(n^2); n is tiny here.
	 *
	 * This is a transversal-crossing test, and that is all it claims to be. Three kinds
	 * of degeneracy are deliberately NOT reported as crossings:
	 *   - non-adjacent edges that touch exactly at a vertex (the intersection parameters
	 *     are clamped to the open interval, so an endpoint touch does not count);
	 *   - collinear overlapping edges (the near-zero determinant is treated as
	 *     non-crossing, so an edge doubling back along another reads as simple);
	 *   - any pair involving a zero-length edge, for the same reason - so a rim that
	 *     pinches down to a repeated point also reads as simple.
	 *
	 * It reliably catches a boundary that folds through itself, which is what it is for.
	 * Do not use it as a general validity or non-degeneracy check.
	 */
	AIRSIDE_API bool IsSimplePolygon(TArrayView<const FVector2D> Points);

	/** Result of rounding one corner between two adjacent road edges. */
	struct FFillet
	{
		/** False when the corner could not be solved at all (parallel, non-intersecting edges). */
		bool bValid = false;

		/** True when the edges are collinear: no arc, join the cuts with a straight line. */
		bool bStraightThrough = false;

		FVector2D Corner   = FVector2D::ZeroVector;  // X: where the two edge lines cross
		FVector2D Centre   = FVector2D::ZeroVector;  // C: centre of the tangent arc
		FVector2D TangentA = FVector2D::ZeroVector;  // T_A: arc touches edge A here
		FVector2D TangentB = FVector2D::ZeroVector;  // T_B: arc touches edge B here

		/** |R / tan(Theta/2)|. Always non-negative: how far OUTWARD from the corner
		 *  each tangent point sits, and therefore how much further back the fillet
		 *  pushes each arm's cut. */
		double Distance = 0.0;

		/** The requested radius. The solve never clamps it; a caller that needs the
		 *  trim to fit a finite segment must clamp before calling. */
		double Radius = 0.0;

		/** CCW angle from A.Dir to B.Dir, in [0, 2*UE_DOUBLE_PI). */
		double Theta = 0.0;

		/** Parameter of T_A along edge A, from A.Origin. Equals ReachA + Distance.
		 *  May be negative at a reflex corner; the cut clamps at zero. */
		double ParamA = 0.0;

		/** Parameter of T_B along edge B, from B.Origin. Equals ReachB + Distance. */
		double ParamB = 0.0;
	};

	/**
	 * Round the corner between edge A (left edge of the earlier segment in CCW bearing
	 * order) and edge B (right edge of the next segment). Both Dir point AWAY from the
	 * shared node.
	 *
	 * The tangent points sit OUTWARD from the corner point along both edges, so the
	 * fillet pushes each arm's cut back rather than carving into the corner. The only
	 * difference between the inside and the outside of a bend is which side of edge A
	 * the arc centre falls on, which flips as Theta passes PI.
	 */
	AIRSIDE_API FFillet SolveFillet(const FRay2D& A, const FRay2D& B, double Radius);

	/** Sample an arc from TangentA to TangentB about Centre, inclusive of both ends. */
	AIRSIDE_API void SampleArc(const FFillet& Fillet, int32 SegmentCount, TArray<FVector2D>& OutPoints);

	/** Which of RayToPlaneZ's three guards refused, or None on success. */
	enum class ERayToPlaneRefusal : uint8
	{
		None,

		/** Direction nearly parallel to the plane: no intersection to find. */
		Parallel,

		/** The plane sits behind Origin along Direction (Distance <= 0). */
		BehindOrigin,

		/** Distance > MaxDistance. */
		BeyondMaxDistance,
	};

	/**
	 * Where a ray meets the horizontal plane Z == PlaneZ, as an XY position.
	 *
	 * The one 3D function in an otherwise 2D file: both build drivers pick the road plane
	 * with a ray from the camera, and until issue #33 each carried its own copy of this
	 * maths (ARoadBuildController::CursorOnRoadPlane, URoadBuildEditorTool::RayToPlane) -
	 * one of the two things this file exists to stop.
	 *
	 * Refuses three ways, in order: Direction nearly parallel to the plane (no intersection
	 * to find); the plane behind Origin along Direction (Distance <= 0 - without this a
	 * click on the sky would land on the plane's mirror image); and Distance > MaxDistance,
	 * because near the horizon the ray is almost parallel to the plane and the distance
	 * runs away toward infinity, so a click a few pixels too high would land kilometres out.
	 * Pass a very large MaxDistance to opt out of the third guard, as the editor tool does -
	 * it has no notion of "current view distance" to measure a cap against.
	 *
	 * OutWhy, when given, names which guard tripped - so a caller that wants to LOG a
	 * refusal (the controller's CursorOnRoadPlane, which needs the reason but not the
	 * plugin's business logging it) reads that back instead of re-running this function's
	 * own arithmetic a second time to guess.
	 *
	 * OutDistance, when given, is written on every path where a distance was computed -
	 * BehindOrigin, BeyondMaxDistance and success - but left untouched on Parallel, where
	 * there is none to report. Lets a BeyondMaxDistance log say how far past the cap the
	 * click actually landed, instead of only naming the cap it exceeded.
	 */
	AIRSIDE_API bool RayToPlaneZ(const FVector& Origin, const FVector& Direction, double PlaneZ,
		double MaxDistance, FVector2D& OutXY, ERayToPlaneRefusal* OutWhy = nullptr,
		double* OutDistance = nullptr);
}
