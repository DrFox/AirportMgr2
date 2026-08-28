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

	// How far along each edge the corner point sits, measured from the node.
	const double ReachA = FVector2D::DotProduct(Corner - A.Origin, A.Dir);
	const double ReachB = FVector2D::DotProduct(Corner - B.Origin, B.Dir);

	// The tangent points sit OUTWARD from the corner along both edges, never inward.
	// Rounding a junction corner cannot carve material out of the corner itself -
	// that would make the two arms overlap through the node - so the fillet instead
	// pushes each arm's cut further back by this distance.
	const double Magnitude = FMath::Abs(Radius / TanHalf);

	// Which side of edge A the arc centre lies on. A convex corner (Theta < PI) is
	// rounded toward the node's far side; a reflex corner is rounded the other way.
	// This sign is the only difference between the inside and the outside of a bend.
	const double Side = (Result.Theta < UE_DOUBLE_PI) ? 1.0 : -1.0;

	Result.bValid = true;
	Result.Radius = Radius;
	Result.Distance = Magnitude;
	Result.TangentA = Corner + A.Dir * Magnitude;
	Result.TangentB = Corner + B.Dir * Magnitude;
	Result.Centre = Result.TangentA + PerpCCW(A.Dir) * (Side * Radius);
	Result.ParamA = ReachA + Magnitude;
	Result.ParamB = ReachB + Magnitude;

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
