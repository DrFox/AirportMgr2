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
	ROADNET_API FVector2D PerpCCW(const FVector2D& V);

	/** Rotate by Radians counter-clockwise. */
	ROADNET_API FVector2D Rotate(const FVector2D& V, double Radians);

	/** atan2 bearing in (-UE_DOUBLE_PI, UE_DOUBLE_PI]. */
	ROADNET_API double Bearing(const FVector2D& Dir);

	/** CCW angle from From to To, in [0, 2*UE_DOUBLE_PI). */
	ROADNET_API double CcwAngleBetween(const FVector2D& From, const FVector2D& To);

	/** Intersection of the two infinite lines. False if near-parallel. */
	ROADNET_API bool LineIntersect(const FRay2D& A, const FRay2D& B, FVector2D& OutPoint);

	/** Signed area via the shoelace formula. Positive means CCW winding. */
	ROADNET_API double PolygonArea(TArrayView<const FVector2D> Points);

	/** True if no pair of non-adjacent edges intersects. O(n^2); n is tiny here. */
	ROADNET_API bool IsSimplePolygon(TArrayView<const FVector2D> Points);

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

		/** d, signed. Positive at a convex corner, negative at a reflex one. */
		double Distance = 0.0;

		/** Actual radius after clamping; may differ from the requested radius. */
		double Radius = 0.0;

		/** CCW angle from A.Dir to B.Dir, in [0, 2*UE_DOUBLE_PI). */
		double Theta = 0.0;

		/** Distance of T_A along A from A.Origin. Guaranteed >= 0 on success. */
		double ParamA = 0.0;

		/** Distance of T_B along B from B.Origin. Guaranteed >= 0 on success. */
		double ParamB = 0.0;
	};

	/**
	 * Round the corner between edge A (left edge of the earlier segment in CCW bearing
	 * order) and edge B (right edge of the next segment). Both Dir point AWAY from the
	 * shared node.
	 *
	 * The radius is clamped so both tangent parameters are non-negative. At a convex
	 * corner that clamps the radius DOWN; at a reflex corner it clamps it UP, because
	 * d = R / tan(Theta/2) changes sign as Theta passes PI.
	 */
	ROADNET_API FFillet SolveFillet(const FRay2D& A, const FRay2D& B, double Radius);

	/** Sample an arc from TangentA to TangentB about Centre, inclusive of both ends. */
	ROADNET_API void SampleArc(const FFillet& Fillet, int32 SegmentCount, TArray<FVector2D>& OutPoints);
}
