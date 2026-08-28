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
}
