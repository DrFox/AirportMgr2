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

	// Theta near PI was handled above. Everything that reaches here with a vanishing
	// tan or sin of the half-angle therefore has Theta near 0 or near 2*PI, which means
	// the two edges point the SAME way: near-COINCIDENT arms, not collinear-opposite
	// ones. There is no corner to round and no meaningful cut, so the corner is simply
	// unsolvable. Calling it straight-through would contribute a zero cut - the exact
	// opposite of the arbitrarily large cut such a pinched fork actually needs.
	if (FMath::Abs(TanHalf) < CollinearEpsilon || FMath::Abs(SinHalf) < CollinearEpsilon)
	{
		Result.bValid = false;
		Result.bStraightThrough = false;
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

double RoadGeom::ClosestPointOnSegment(const FVector2D& A, const FVector2D& B, const FVector2D& P)
{
	const FVector2D Along = B - A;
	const double LengthSquared = Along.SizeSquared();

	// A zero-length segment has no direction to project onto. Returning 0 rather than
	// dividing gives the caller the A endpoint, which is the only point it has.
	if (LengthSquared <= 0.0)
	{
		return 0.0;
	}

	// Divided by the squared length, not the length: the dot product is already scaled by
	// |Along| once, so one more division normalises it to a parameter rather than a
	// distance.
	return FMath::Clamp(FVector2D::DotProduct(P - A, Along) / LengthSquared, 0.0, 1.0);
}

bool RoadGeom::SegmentsCross(const FVector2D& A0, const FVector2D& A1,
	const FVector2D& B0, const FVector2D& B1)
{
	const FVector2D DirA = A1 - A0;
	const FVector2D DirB = B1 - B0;

	const double Denominator = DirA.X * DirB.Y - DirA.Y * DirB.X;

	// Parallel or collinear. Treated as not crossing, which is what IsSimplePolygon does
	// too - an edge doubling back along another reads as simple to both.
	if (FMath::IsNearlyZero(Denominator))
	{
		return false;
	}

	const FVector2D Between = B0 - A0;
	const double AlongA = (Between.X * DirB.Y - Between.Y * DirB.X) / Denominator;
	const double AlongB = (Between.X * DirA.Y - Between.Y * DirA.X) / Denominator;

	// The OPEN interval on both, so a shared endpoint is not a crossing. Consecutive edges
	// of an outline meet at a corner by construction; counting that would refuse every
	// polygon with more than two sides.
	constexpr double Edge = 1e-9;
	return AlongA > Edge && AlongA < 1.0 - Edge
		&& AlongB > Edge && AlongB < 1.0 - Edge;
}

bool RoadGeom::RayToPlaneZ(const FVector& Origin, const FVector& Direction, double PlaneZ,
	double MaxDistance, FVector2D& OutXY, ERayToPlaneRefusal* OutWhy, double* OutDistance)
{
	// One place to set OutWhy and return, so a new guard cannot add a way to refuse
	// without also saying why - which is the whole reason OutWhy exists.
	auto Refuse = [OutWhy](ERayToPlaneRefusal Reason)
	{
		if (OutWhy != nullptr)
		{
			*OutWhy = Reason;
		}
		return false;
	};

	// Parallel to the plane: no intersection to find.
	if (FMath::IsNearlyZero(Direction.Z))
	{
		return Refuse(ERayToPlaneRefusal::Parallel);
	}

	const double Distance = (PlaneZ - Origin.Z) / Direction.Z;

	// Written as soon as a distance exists, ahead of the two guards below that may still
	// refuse on it - a BeyondMaxDistance refusal wants the actual distance to log, not
	// just the cap it tripped.
	if (OutDistance != nullptr)
	{
		*OutDistance = Distance;
	}

	// Behind the origin. Without this a ray aimed away from the plane would resolve to its
	// mirror image on the far side, rather than refusing.
	if (Distance <= 0.0)
	{
		return Refuse(ERayToPlaneRefusal::BehindOrigin);
	}

	if (Distance > MaxDistance)
	{
		return Refuse(ERayToPlaneRefusal::BeyondMaxDistance);
	}

	if (OutWhy != nullptr)
	{
		*OutWhy = ERayToPlaneRefusal::None;
	}
	OutXY = FVector2D(Origin.X + Direction.X * Distance, Origin.Y + Direction.Y * Distance);
	return true;
}

bool RoadGeom::PointInPolygon(TArrayView<const FVector2D> Polygon, const FVector2D& Point)
{
	if (Polygon.Num() < 3)
	{
		return false;
	}

	// Crossing number: count the edges a ray cast in +X from Point passes through. Odd
	// means inside. Winding-agnostic, so an outline stored either way round answers the
	// same - which matters because the model normalises winding on the way in and callers
	// should not have to know that.
	bool bInside = false;
	for (int32 Index = 0, Previous = Polygon.Num() - 1; Index < Polygon.Num(); Previous = Index++)
	{
		const FVector2D& Low = Polygon[Index];
		const FVector2D& High = Polygon[Previous];

		// Half-open in Y so a vertex exactly at the ray's height is counted once, not twice.
		if ((Low.Y > Point.Y) != (High.Y > Point.Y))
		{
			const double CrossingX =
				(High.X - Low.X) * (Point.Y - Low.Y) / (High.Y - Low.Y) + Low.X;
			if (Point.X < CrossingX)
			{
				bInside = !bInside;
			}
		}
	}
	return bInside;
}
