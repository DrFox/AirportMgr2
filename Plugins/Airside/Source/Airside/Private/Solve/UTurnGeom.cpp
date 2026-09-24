#include "Solve/UTurnGeom.h"

#include "Solve/GuidelineGeom.h"

namespace
{
	/**
	 * The six pieces for one radius, in a frame whose origin is the lane ends' midpoint, U
	 * along the axis (out of the road) and V across it toward OutEnd. Half is half the lane
	 * spacing; InEnd is at (-Half, 0), OutEnd at (+Half, 0).
	 *
	 * Each reverse curve is two quadratics meeting at the midpoint of their control points,
	 * which is what makes the joint tangent-continuous: C1 lies on InEnd's tangent line
	 * (x = -Half), C2 on the circle's leftmost tangent line (x = -R), and the inflection is
	 * their midpoint. The quarter circles use the tangent-line crossing as control.
	 */
	TArray<UTurnGeom::FPiece> Pieces(const FVector2D& O, const FVector2D& U, const FVector2D& V,
		double Half, const FVector2D& OutEnd, double R)
	{
		const double H = UTurnGeom::HeightFactor * R;
		const double K = H / 4.0;
		auto P = [&](double X, double Y) { return O + V * X + U * Y; };

		const FVector2D C1 = P(-Half, K);
		const FVector2D C2 = P(-R, H - K);
		const FVector2D D2 = P(R, H - K);
		const FVector2D D1 = P(Half, K);

		TArray<UTurnGeom::FPiece> Out;
		Out.Reserve(6);
		Out.Add({ (C1 + C2) * 0.5, C1 });
		Out.Add({ P(-R, H), C2 });
		Out.Add({ P(0.0, H + R), P(-R, H + R) });
		Out.Add({ P(R, H), P(R, H + R) });
		Out.Add({ (D2 + D1) * 0.5, D2 });
		// OutEnd itself, not P(Half, 0): the builder joins this piece to the leaving lane's
		// node BY HANDLE, and the position must be that node's exactly.
		Out.Add({ OutEnd, D1 });
		return Out;
	}

	double TightestOf(const TArray<UTurnGeom::FPiece>& Pieces, const FVector2D& InEnd)
	{
		double Tightest = TNumericLimits<double>::Max();
		FVector2D Prev = InEnd;
		for (const UTurnGeom::FPiece& Piece : Pieces)
		{
			Tightest = FMath::Min(Tightest, GuidelineGeom::TightestRadius(Prev, Piece.Control, Piece.End));
			Prev = Piece.End;
		}
		return Tightest;
	}
}

TArray<UTurnGeom::FPiece> UTurnGeom::Balloon(const FVector2D& InEnd, const FVector2D& OutEnd,
	const FVector2D& Axis, double NeededRadius, double* OutRadius)
{
	const FVector2D U = Axis.GetSafeNormal();
	const FVector2D Across = (OutEnd - InEnd) - U * FVector2D::DotProduct(OutEnd - InEnd, U);
	if (U.IsNearlyZero() || Across.IsNearlyZero())
	{
		return {};
	}
	const FVector2D V = Across.GetSafeNormal();
	const double Half = Across.Size() * 0.5;
	const FVector2D O = (InEnd + OutEnd) * 0.5;

	// MEASURED, NOT DERIVED. The circle's quarters lose 1/sqrt(2) of R at their midpoints and
	// the reverse curves lose more the steeper they are; rather than a closed form for both,
	// grow R until every piece clears. The Python prototype settled on 1.56x at the figures
	// in UTurnGeom.h; 80 steps of 3% reach ~10x, far past any lock.
	double R = FMath::Max(NeededRadius, Half * 2.0);
	TArray<FPiece> Best = Pieces(O, U, V, Half, OutEnd, R);
	for (int32 Step = 0; Step < 80 && TightestOf(Best, InEnd) < NeededRadius; ++Step)
	{
		R *= 1.03;
		Best = Pieces(O, U, V, Half, OutEnd, R);
	}
	if (OutRadius != nullptr)
	{
		*OutRadius = R;
	}
	return Best;
}
