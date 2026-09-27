#include "Solve/PolygonInset.h"

double PolygonInset::SignedArea(TConstArrayView<FVector2D> Polygon)
{
	double Twice = 0.0;
	for (int32 Index = 0; Index < Polygon.Num(); ++Index)
	{
		const FVector2D& A = Polygon[Index];
		const FVector2D& B = Polygon[(Index + 1) % Polygon.Num()];
		Twice += A.X * B.Y - B.X * A.Y;
	}
	return Twice * 0.5;
}

bool PolygonInset::Inset(TConstArrayView<FVector2D> Outer, double Distance, TArray<FVector2D>& OutInner)
{
	OutInner.Reset();
	const int32 Count = Outer.Num();
	const double Area = SignedArea(Outer);
	if (Count < 3 || !(Distance > 0.0) || FMath::IsNearlyZero(Area))
	{
		return false;
	}

	// INWARD IS THE LEFT OF EACH EDGE FOR A POSITIVE-AREA POLYGON, the right for a negative
	// one - read off the same SignedArea sign, so no handedness convention enters here. (The
	// frame is left-handed on screen, but this is plane arithmetic and never meets it.)
	const double Side = Area > 0.0 ? 1.0 : -1.0;

	TArray<FVector2D> Directions;
	TArray<FVector2D> Normals;
	Directions.Reserve(Count);
	Normals.Reserve(Count);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const FVector2D Edge = Outer[(Index + 1) % Count] - Outer[Index];
		const double Length = Edge.Size();
		if (FMath::IsNearlyZero(Length))
		{
			return false;
		}
		const FVector2D Direction = Edge / Length;
		Directions.Add(Direction);
		Normals.Add(FVector2D(-Direction.Y, Direction.X) * Side);
	}

	OutInner.Reserve(Count);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		// Vertex Index is where the offset of edge (Index - 1) meets the offset of edge
		// Index. Parallel neighbours (a collinear corner) meet anywhere along the shared
		// offset line, so the corner itself moved straight in is exact.
		const int32 Previous = (Index + Count - 1) % Count;
		const FVector2D PointA = Outer[Previous] + Normals[Previous] * Distance;
		const FVector2D PointB = Outer[Index] + Normals[Index] * Distance;
		const double Cross = FVector2D::CrossProduct(Directions[Previous], Directions[Index]);

		FVector2D Corner;
		if (FMath::IsNearlyZero(Cross))
		{
			Corner = PointB;
		}
		else
		{
			const double T = FVector2D::CrossProduct(PointB - PointA, Directions[Index]) / Cross;
			Corner = PointA + Directions[Previous] * T;
		}

		if (FVector2D::Distance(Corner, Outer[Index]) > MaxMitreRatio * Distance)
		{
			OutInner.Reset();
			return false;
		}
		OutInner.Add(Corner);
	}

	// COLLAPSE CHECK. An inset wider than the polygon turns edges round and flips the area -
	// either would draw a band crossing itself over the slab.
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const FVector2D InnerEdge = OutInner[(Index + 1) % Count] - OutInner[Index];
		if (FVector2D::DotProduct(InnerEdge, Directions[Index]) <= 0.0)
		{
			OutInner.Reset();
			return false;
		}
	}
	if (SignedArea(OutInner) * Side <= 0.0)
	{
		OutInner.Reset();
		return false;
	}
	return true;
}
