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

RoadGeom::FFillet RoadGeom::SolveFillet(const FRay2D& A, const FRay2D& B, double Radius)
{
	FFillet Result;
	Result.Theta = CcwAngleBetween(A.Dir, B.Dir);

	constexpr double CollinearEpsilon = 1e-6;

	// Theta == PI means the two edges run in opposite directions along one straight
	// line: there is no corner to round. This is the COMMON case, not an edge case,
	// because long drags auto-subdivide into collinear segments. Rounding here would
	// facet every straight run.
	if (FMath::Abs(Result.Theta - UE_DOUBLE_PI) < CollinearEpsilon)
	{
		Result.bValid = true;
		Result.bStraightThrough = true;
		return Result;
	}

	FVector2D Corner;
	if (!LineIntersect(A, B, Corner))
	{
		return Result; // bValid stays false: parallel edges that never meet
	}
	Result.Corner = Corner;

	const double HalfTheta = Result.Theta * 0.5;
	const double TanHalf = FMath::Tan(HalfTheta);
	const double SinHalf = FMath::Sin(HalfTheta);

	if (FMath::Abs(TanHalf) < CollinearEpsilon || FMath::Abs(SinHalf) < CollinearEpsilon)
	{
		Result.bValid = true;
		Result.bStraightThrough = true;
		return Result;
	}

	// How far along each edge the corner point sits. The tangent points must land at
	// or after each edge's origin, so that the later perpendicular cut - taken as the
	// max over a segment's two corners - never falls behind a tangent point.
	const double ReachA = FVector2D::DotProduct(Corner - A.Origin, A.Dir);
	const double ReachB = FVector2D::DotProduct(Corner - B.Origin, B.Dir);
	const double MaxDistance = FMath::Min(ReachA, ReachB);

	const bool bConvex = Result.Theta < UE_DOUBLE_PI;

	// d = R / tan(Theta/2) is positive at a convex corner and negative at a reflex
	// one, because tan flips sign past PI/2. Radius = d * tan(Theta/2) must stay
	// non-negative either way, so d is clamped to its corner type's sign as well as
	// to MaxDistance.
	double Distance = Radius / TanHalf;
	Distance = FMath::Min(Distance, MaxDistance);
	Distance = bConvex ? FMath::Max(Distance, 0.0) : FMath::Min(Distance, 0.0);

	const double EffectiveRadius = Distance * TanHalf;

	// No fillet with a non-negative radius fits: the edges cross behind the node.
	// Degrade to a straight join rather than emitting an inverted arc.
	if (EffectiveRadius < 0.0 || (bConvex && MaxDistance < 0.0))
	{
		Result.bValid = true;
		Result.bStraightThrough = true;
		return Result;
	}

	Result.bValid = true;
	Result.Radius = EffectiveRadius;
	Result.Distance = Distance;
	Result.TangentA = Corner - A.Dir * Distance;
	Result.TangentB = Corner - B.Dir * Distance;
	Result.Centre = Corner - Rotate(A.Dir, HalfTheta) * (EffectiveRadius / SinHalf);
	Result.ParamA = ReachA - Distance;
	Result.ParamB = ReachB - Distance;

	return Result;
}

void RoadGeom::SampleArc(const FFillet& Fillet, int32 SegmentCount, TArray<FVector2D>& OutPoints)
{
	if (!Fillet.bValid || Fillet.bStraightThrough || SegmentCount < 1)
	{
		return;
	}

	const FVector2D FromCentreA = Fillet.TangentA - Fillet.Centre;
	const FVector2D FromCentreB = Fillet.TangentB - Fillet.Centre;

	const double StartAngle = Bearing(FromCentreA);

	// The arc is always the minor sweep between the two tangent points: a fillet
	// never wraps the long way round its own circle.
	double Sweep = Bearing(FromCentreB) - StartAngle;
	while (Sweep >  UE_DOUBLE_PI) { Sweep -= 2.0 * UE_DOUBLE_PI; }
	while (Sweep < -UE_DOUBLE_PI) { Sweep += 2.0 * UE_DOUBLE_PI; }

	const double ArcRadius = FromCentreA.Length();
	for (int32 Step = 0; Step <= SegmentCount; ++Step)
	{
		const double Alpha = static_cast<double>(Step) / static_cast<double>(SegmentCount);
		const double Angle = StartAngle + Sweep * Alpha;
		OutPoints.Add(Fillet.Centre + FVector2D(FMath::Cos(Angle), FMath::Sin(Angle)) * ArcRadius);
	}
}
