#include "Build/GroundCoverMask.h"

#include "Solve/RoadGeom.h"

namespace
{
	double Cross(const FVector2D& U, const FVector2D& V)
	{
		return U.X * V.Y - U.Y * V.X;
	}

	/**
	 * Inclusive, winding-agnostic: P is in (or on) the triangle when the three edge functions do
	 * not disagree in sign. A zero on one edge is "on the edge" and counts.
	 */
	bool InTriangle(const FVector2D& P, const FVector2D& A, const FVector2D& B, const FVector2D& C)
	{
		const double D1 = Cross(B - A, P - A);
		const double D2 = Cross(C - B, P - B);
		const double D3 = Cross(A - C, P - C);
		const bool bAnyNegative = D1 < 0.0 || D2 < 0.0 || D3 < 0.0;
		const bool bAnyPositive = D1 > 0.0 || D2 > 0.0 || D3 > 0.0;
		return !(bAnyNegative && bAnyPositive);
	}
}

FGroundCoverMask::FGroundCoverMask(double InBucketSizeUu)
	: BucketSizeUu(FMath::Max(InBucketSizeUu, 1.0))
{
}

FIntPoint FGroundCoverMask::BucketOf(const FVector2D& Point) const
{
	return FIntPoint(FMath::FloorToInt32(Point.X / BucketSizeUu), FMath::FloorToInt32(Point.Y / BucketSizeUu));
}

void FGroundCoverMask::AddTriangle(const FVector2D& A, const FVector2D& B, const FVector2D& C)
{
	if (FMath::Abs(Cross(B - A, C - A)) <= UE_SMALL_NUMBER)
	{
		return;
	}
	const int32 Index = Triangles.Add(FTriangle{ A, B, C });
	// THE SAME FLOOR BucketOf USES, at both corners of the bounds: a point inside the bounds
	// therefore buckets inside this range, even exactly on a bucket line.
	const FIntPoint Lo = BucketOf(FVector2D(FMath::Min3(A.X, B.X, C.X), FMath::Min3(A.Y, B.Y, C.Y)));
	const FIntPoint Hi = BucketOf(FVector2D(FMath::Max3(A.X, B.X, C.X), FMath::Max3(A.Y, B.Y, C.Y)));
	for (int32 X = Lo.X; X <= Hi.X; ++X)
	{
		for (int32 Y = Lo.Y; Y <= Hi.Y; ++Y)
		{
			Buckets.FindOrAdd(FIntPoint(X, Y)).Triangles.Add(Index);
		}
	}
}

void FGroundCoverMask::AddPolygon(TArray<FVector2D> Outline)
{
	if (Outline.Num() < 3)
	{
		return;
	}
	FBox2D Bounds(ForceInit);
	for (const FVector2D& Point : Outline)
	{
		Bounds += Point;
	}
	const int32 Index = Polygons.Add(MoveTemp(Outline));
	const FIntPoint Lo = BucketOf(Bounds.Min);
	const FIntPoint Hi = BucketOf(Bounds.Max);
	for (int32 X = Lo.X; X <= Hi.X; ++X)
	{
		for (int32 Y = Lo.Y; Y <= Hi.Y; ++Y)
		{
			Buckets.FindOrAdd(FIntPoint(X, Y)).Polygons.Add(Index);
		}
	}
}

bool FGroundCoverMask::IsCovered(const FVector2D& Point) const
{
	const FBucket* Bucket = Buckets.Find(BucketOf(Point));
	if (Bucket == nullptr)
	{
		return false;
	}
	for (const int32 Index : Bucket->Triangles)
	{
		const FTriangle& T = Triangles[Index];
		if (InTriangle(Point, T.A, T.B, T.C))
		{
			return true;
		}
	}
	for (const int32 Index : Bucket->Polygons)
	{
		// RoadGeom's winding test is NOT edge-exact (its own comment) - acceptable here, because
		// a plot outline is a yard, not a slab: its edge has no lip for a tuft to hide.
		if (RoadGeom::PointInPolygon(Polygons[Index], Point))
		{
			return true;
		}
	}
	return false;
}
