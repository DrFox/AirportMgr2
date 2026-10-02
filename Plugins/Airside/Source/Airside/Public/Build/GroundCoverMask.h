#pragma once

#include "CoreMinimal.h"
#include "Model/LandGrid.h"

/**
 * "Is this ground point under a built surface?" - the question that keeps grass off every
 * road, taxiway, runway, apron, stand pad and plot.
 *
 * EXACT, NOT RASTERISED. A bitmap mask at any affordable resolution would let a tuft stand a
 * texel inside a slab or leave a bald strip along it; testing the drawn triangles themselves
 * puts the grass exactly to the edge, where its lean over the 10 cm lip is what hides it
 * (spec 2026-10-01-ground-cover-grass-design.md, 4.4). Triangles and outlines are bucketed in
 * a uniform 2D grid so a query touches only the few near it.
 *
 * INCLUSIVE: a point exactly on an edge counts as covered. A tuft centred on a slab's edge
 * would stand half on the slab; erring covered costs one tuft, erring bare shows grass on
 * concrete.
 */
class AIRSIDE_API FGroundCoverMask
{
public:
	/** 16 m buckets: a few dozen surface triangles each on a typical layout. */
	explicit FGroundCoverMask(double InBucketSizeUu = 1600.0);

	/** A covered triangle, in any winding. Degenerate (zero-area) triangles cover nothing and are dropped. */
	void AddTriangle(const FVector2D& A, const FVector2D& B, const FVector2D& C);

	/** A covered outline (a plot), closed implicitly. Fewer than three points covers nothing. */
	void AddPolygon(TArray<FVector2D> Outline);

	/**
	 * The airport's owned land (FLandGrid): everything not owned counts as covered, so no tuft grows over the void
	 * past the diorama's cut edge - where a tuft stands on nothing and gives the edge away (2026-10-02). Invalid, the
	 * default, owns everything. A point ON the cut is covered (FLandGrid::IsOwned), the same inclusive rule as a
	 * slab's: a tuft centred there would lean over it.
	 */
	void SetLand(const FLandGrid& InLand) { Land = InLand; }

	bool IsCovered(const FVector2D& Point) const;

	int32 NumTriangles() const { return Triangles.Num(); }
	int32 NumPolygons() const { return Polygons.Num(); }
	int32 NumBuckets() const { return Buckets.Num(); }

private:
	struct FTriangle
	{
		FVector2D A, B, C;
	};

	struct FBucket
	{
		TArray<int32> Triangles;
		TArray<int32> Polygons;
	};

	FIntPoint BucketOf(const FVector2D& Point) const;

	double BucketSizeUu;
	FLandGrid Land;
	TArray<FTriangle> Triangles;
	TArray<TArray<FVector2D>> Polygons;
	TMap<FIntPoint, FBucket> Buckets;
};
