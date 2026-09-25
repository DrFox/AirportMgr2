#include "Solve/UTurnGeom.h"

#include "Solve/GuidelineGeom.h"

namespace
{
	/**
	 * THE OLD SHAPE, kept ONLY to measure the ruled footprint (UTurnGeom::FootprintFor): six
	 * pieces for one radius, in a frame whose origin is the lane ends' midpoint, U along the axis
	 * (out of the road) and V across it toward OutEnd. Half is half the lane spacing; InEnd is at
	 * (-Half, 0), OutEnd at (+Half, 0).
	 *
	 * Each reverse curve is two quadratics meeting at the midpoint of their control points,
	 * which is what makes the joint tangent-continuous: C1 lies on InEnd's tangent line
	 * (x = -Half), C2 on the circle's leftmost tangent line (x = -R), and the inflection is
	 * their midpoint. The quarter circles use the tangent-line crossing as control.
	 */
	TArray<UTurnGeom::FPiece> LegacyPieces(const FVector2D& O, const FVector2D& U, const FVector2D& V,
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

	/**
	 * THE RESHAPED BALLOON for a circle of radius Rc centred Height past the lane ends, same frame
	 * as LegacyPieces: the lane change out to the circle's side (GuidelineGeom::LaneChange across
	 * Height), CirclePieces quadratics round the half circle - each from one point of the circle to
	 * the next, controlled at their tangents' crossing - and the mirror lane change back. OutEnd is
	 * written verbatim as the last End: the builder joins that piece to the leaving lane's node BY
	 * HANDLE, and the position must be that node's exactly.
	 */
	TArray<UTurnGeom::FPiece> ReshapedPieces(const FVector2D& O, const FVector2D& U, const FVector2D& V,
		double Half, const FVector2D& OutEnd, double Rc, double Height)
	{
		auto P = [&](double X, double Y) { return O + V * X + U * Y; };

		TArray<UTurnGeom::FPiece> Out;
		Out.Reserve(4 + UTurnGeom::CirclePieces);
		// Out to the circle's side: GuidelineGeom::LaneChange, THE S a width taper lays too (one
		// construction since the review of 5660420c) - tangent to the lane at the start, to the
		// circle at the end, joined at the midpoint. Seeded with the chord's own points, so a
		// shift under a uu (LaneChange declines, outputs untouched) lays a straight piece, not
		// garbage; Balloon never asks for one (Rc >= 1.01 Half).
		FVector2D C1 = P(-Half, Height * 0.25);
		FVector2D Mid = P((-Half - Rc) * 0.5, Height * 0.5);
		FVector2D C2 = P(-Rc, Height * 0.75);
		GuidelineGeom::LaneChange(P(-Half, 0.0), P(-Rc, Height), U, C1, Mid, C2);
		Out.Add({ Mid, C1 });
		Out.Add({ P(-Rc, Height), C2 });
		// Round the top, from angle pi down to 0 in (V, U): clockwise with U up and V right.
		const double Step = UE_DOUBLE_PI / UTurnGeom::CirclePieces;
		const double TangentLength = Rc * FMath::Tan(Step * 0.5);
		FVector2D Start = P(-Rc, Height);
		for (int32 Piece = 0; Piece < UTurnGeom::CirclePieces; ++Piece)
		{
			const double From = UE_DOUBLE_PI - Piece * Step;
			const double To = From - Step;
			// Travelling this way the tangent at angle a is (sin a, -cos a) in (V, U).
			const FVector2D Control = Start + (V * FMath::Sin(From) - U * FMath::Cos(From)) * TangentLength;
			// The top and the far side are written as the exact points they are, not via
			// cos/sin, so the footprint's extremes are exactly the box's numbers.
			const FVector2D End = Piece + 1 == UTurnGeom::CirclePieces ? P(Rc, Height)
				: (Piece + 1) * 2 == UTurnGeom::CirclePieces ? P(0.0, Height + Rc)
				: P(Rc * FMath::Cos(To), Height + Rc * FMath::Sin(To));
			Out.Add({ End, Control });
			Start = End;
		}
		// And the mirror lane change back onto the leaving lane.
		FVector2D D2 = P(Rc, Height * 0.75);
		FVector2D BackMid = P((Rc + Half) * 0.5, Height * 0.5);
		FVector2D D1 = P(Half, Height * 0.25);
		GuidelineGeom::LaneChange(P(Rc, Height), P(Half, 0.0), -U, D2, BackMid, D1);
		Out.Add({ BackMid, D2 });
		Out.Add({ OutEnd, D1 });
		return Out;
	}

	/** The frame both shapes are laid in; false for coincident ends or no axis. */
	bool BalloonFrame(const FVector2D& InEnd, const FVector2D& OutEnd, const FVector2D& Axis,
		FVector2D& OutO, FVector2D& OutU, FVector2D& OutV, double& OutHalf)
	{
		OutU = Axis.GetSafeNormal();
		const FVector2D Across = (OutEnd - InEnd) - OutU * FVector2D::DotProduct(OutEnd - InEnd, OutU);
		if (OutU.IsNearlyZero() || Across.IsNearlyZero())
		{
			return false;
		}
		OutV = Across.GetSafeNormal();
		OutHalf = Across.Size() * 0.5;
		OutO = (InEnd + OutEnd) * 0.5;
		return true;
	}
}

UTurnGeom::FFootprint UTurnGeom::FootprintFor(double HalfSpacing, double NeededRadius)
{
	// THE OLD SEARCH, VERBATIM, in a unit frame - MEASURED, NOT DERIVED, as it always was: the
	// circle's quarters lose 1/sqrt(2) of R at their midpoints and the reverse curves lose more
	// the steeper they are, so R grows 3% a step from NeededRadius until every piece clears it.
	// The Python prototype settled on 1.56x at the figures in UTurnGeom.h; 80 steps reach ~10x.
	const FVector2D O = FVector2D::ZeroVector;
	const FVector2D U(0.0, 1.0);
	const FVector2D V(1.0, 0.0);
	const FVector2D InEnd(-HalfSpacing, 0.0);
	const FVector2D OutEnd(HalfSpacing, 0.0);
	double R = FMath::Max(NeededRadius, HalfSpacing * 2.0);
	for (int32 Step = 0; Step < 80 && TightestOf(LegacyPieces(O, U, V, HalfSpacing, OutEnd, R), InEnd) < NeededRadius; ++Step)
	{
		R *= 1.03;
	}
	FFootprint Out;
	Out.Reach = (HeightFactor + 1.0) * R;
	Out.HalfWidth = R;
	return Out;
}

TArray<UTurnGeom::FPiece> UTurnGeom::Balloon(const FVector2D& InEnd, const FVector2D& OutEnd,
	const FVector2D& Axis, double NeededRadius, double* OutRadius)
{
	FVector2D O, U, V;
	double Half = 0.0;
	if (!BalloonFrame(InEnd, OutEnd, Axis, O, U, V, Half))
	{
		return {};
	}
	const FFootprint Box = FootprintFor(Half, NeededRadius);

	// THE GENTLEST SHAPE THE BOX HOLDS. The circle fills the box's width at most (Rc <= HalfWidth)
	// and the box's reach fixes its centre's height (Height = Reach - Rc), so one number decides
	// the shape. The circle's pieces deliver cos(pi / 2N) Rc, rising with Rc; the lane change
	// delivers less as Rc rises (more to shift across, less height to do it in). Where they cross
	// is the best - bisected on the pieces' own TightestRadius, not on a closed form, so what is
	// chosen is what the router measures. If the circle is still the tighter at the box's full
	// width, the full width it is. A symmetric S was measured against free tangent lengths on
	// the three tiers (Python, 2026-09-25) and the free ones gained nothing.
	auto Radii = [&](double Rc, double& OutCircle, double& OutShift)
	{
		const TArray<FPiece> Pieces = ReshapedPieces(O, U, V, Half, OutEnd, Rc, Box.Reach - Rc);
		OutShift = FMath::Min(GuidelineGeom::TightestRadius(InEnd, Pieces[0].Control, Pieces[0].End),
			GuidelineGeom::TightestRadius(Pieces[0].End, Pieces[1].Control, Pieces[1].End));
		OutCircle = GuidelineGeom::TightestRadius(Pieces[1].End, Pieces[2].Control, Pieces[2].End);
	};
	double Rc = Box.HalfWidth;
	double Circle = 0.0;
	double Shift = 0.0;
	Radii(Rc, Circle, Shift);
	if (Shift < Circle)
	{
		double Lo = Half * 1.01;
		double Hi = Box.HalfWidth;
		for (int32 Iteration = 0; Iteration < 60; ++Iteration)
		{
			const double Mid = 0.5 * (Lo + Hi);
			Radii(Mid, Circle, Shift);
			(Circle < Shift ? Lo : Hi) = Mid;
		}
		// The side of the crossing whose tightest is the larger - the two differ by a hair.
		double CircleLo = 0.0, ShiftLo = 0.0, CircleHi = 0.0, ShiftHi = 0.0;
		Radii(Lo, CircleLo, ShiftLo);
		Radii(Hi, CircleHi, ShiftHi);
		Rc = FMath::Min(CircleLo, ShiftLo) >= FMath::Min(CircleHi, ShiftHi) ? Lo : Hi;
	}
	if (OutRadius != nullptr)
	{
		*OutRadius = Rc;
	}
	return ReshapedPieces(O, U, V, Half, OutEnd, Rc, Box.Reach - Rc);
}

UTurnGeom::FMeasured UTurnGeom::Measure(const TArray<FPiece>& Pieces, const FVector2D& InEnd, const FVector2D& OutEnd,
	const FVector2D& Axis)
{
	FMeasured Out;
	FVector2D O, U, V;
	double Half = 0.0;
	if (Pieces.Num() == 0 || !BalloonFrame(InEnd, OutEnd, Axis, O, U, V, Half))
	{
		return Out;
	}
	Out.Tightest = TightestOf(Pieces, InEnd);
	FVector2D Prev = InEnd;
	TArray<FVector2D> Points;
	for (const FPiece& Piece : Pieces)
	{
		Points.Reset();
		GuidelineGeom::Sample(Prev, Piece.Control, Piece.End, Points);
		for (const FVector2D& Point : Points)
		{
			Out.Reach = FMath::Max(Out.Reach, FVector2D::DotProduct(Point - O, U));
			Out.HalfWidth = FMath::Max(Out.HalfWidth, FMath::Abs(FVector2D::DotProduct(Point - O, V)));
		}
		Prev = Piece.End;
	}
	return Out;
}
