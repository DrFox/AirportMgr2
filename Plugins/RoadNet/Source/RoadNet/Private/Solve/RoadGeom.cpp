#include "Solve/RoadGeom.h"

namespace
{
	constexpr double ParallelEpsilon = 1e-9;

	bool SegmentsIntersect(const FVector2D& P1, const FVector2D& P2,
	                       const FVector2D& Q1, const FVector2D& Q2)
	{
		auto Cross = [](const FVector2D& A, const FVector2D& B)
		{
			return A.X * B.Y - A.Y * B.X;
		};

		const FVector2D R = P2 - P1;
		const FVector2D S = Q2 - Q1;
		const double Denominator = Cross(R, S);
		if (FMath::Abs(Denominator) < ParallelEpsilon)
		{
			return false; // parallel or collinear; treated as non-crossing
		}

		const double T = Cross(Q1 - P1, S) / Denominator;
		const double U = Cross(Q1 - P1, R) / Denominator;
		return T > 0.0 && T < 1.0 && U > 0.0 && U < 1.0;
	}
}

FVector2D RoadGeom::PerpCCW(const FVector2D& V)
{
	return FVector2D(-V.Y, V.X);
}

FVector2D RoadGeom::Rotate(const FVector2D& V, double Radians)
{
	const double C = FMath::Cos(Radians);
	const double S = FMath::Sin(Radians);
	return FVector2D(V.X * C - V.Y * S, V.X * S + V.Y * C);
}

double RoadGeom::Bearing(const FVector2D& Dir)
{
	return FMath::Atan2(Dir.Y, Dir.X);
}

double RoadGeom::CcwAngleBetween(const FVector2D& From, const FVector2D& To)
{
	double Angle = Bearing(To) - Bearing(From);
	while (Angle < 0.0)
	{
		Angle += 2.0 * UE_DOUBLE_PI;
	}
	while (Angle >= 2.0 * UE_DOUBLE_PI)
	{
		Angle -= 2.0 * UE_DOUBLE_PI;
	}
	return Angle;
}

bool RoadGeom::LineIntersect(const FRay2D& A, const FRay2D& B, FVector2D& OutPoint)
{
	const double Denominator = A.Dir.X * B.Dir.Y - A.Dir.Y * B.Dir.X;
	if (FMath::Abs(Denominator) < ParallelEpsilon)
	{
		return false;
	}

	const FVector2D Delta = B.Origin - A.Origin;
	const double T = (Delta.X * B.Dir.Y - Delta.Y * B.Dir.X) / Denominator;
	OutPoint = A.Origin + A.Dir * T;
	return true;
}

double RoadGeom::PolygonArea(TArrayView<const FVector2D> Points)
{
	const int32 Count = Points.Num();
	if (Count < 3)
	{
		return 0.0;
	}

	double Sum = 0.0;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const FVector2D& Current = Points[Index];
		const FVector2D& Next = Points[(Index + 1) % Count];
		Sum += Current.X * Next.Y - Next.X * Current.Y;
	}
	return Sum * 0.5;
}

bool RoadGeom::IsSimplePolygon(TArrayView<const FVector2D> Points)
{
	const int32 Count = Points.Num();
	if (Count < 3)
	{
		return false;
	}

	for (int32 I = 0; I < Count; ++I)
	{
		for (int32 J = I + 1; J < Count; ++J)
		{
			const bool bAdjacent = (J == I + 1) || (I == 0 && J == Count - 1);
			if (bAdjacent)
			{
				continue;
			}
			if (SegmentsIntersect(Points[I], Points[(I + 1) % Count],
			                      Points[J], Points[(J + 1) % Count]))
			{
				return false;
			}
		}
	}
	return true;
}
