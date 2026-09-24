#include "Solve/VehicleSweep.h"

VehicleSweep::FEnvelope VehicleSweep::Envelope(const FBody& Body, double SteerRadius)
{
	FEnvelope Out;
	if (SteerRadius <= Body.Wheelbase || SteerRadius <= 0.0)
	{
		return Out;
	}
	const double HalfWidth = Body.Width * 0.5;

	// The fixed axle's circle, and the tractor's corners on it: its inner side is closest to
	// the centre along the fixed axle line; its outer corners are the front and rear corners.
	const double Fixed = FMath::Sqrt(SteerRadius * SteerRadius - Body.Wheelbase * Body.Wheelbase);
	double InnerRadius = Fixed - HalfWidth;
	double OuterRadius = FMath::Max(
		FMath::Sqrt(FMath::Square(Fixed + HalfWidth) + FMath::Square(Body.FrontX)),
		FMath::Sqrt(FMath::Square(Fixed + HalfWidth) + FMath::Square(Body.RearX)));

	if (Body.KingpinToAxle > 0.0)
	{
		// The kingpin rides a circle of its own; the trailer's axle trails it, tangent, at
		// KingpinToAxle - the steady-state tractrix. A kingpin circle no larger than the
		// trailer cannot be held: the trailer would fold into it.
		const double Kingpin = FMath::Sqrt(Fixed * Fixed + Body.KingpinX * Body.KingpinX);
		if (Kingpin <= Body.KingpinToAxle)
		{
			return Out;
		}
		const double TrailerAxle = FMath::Sqrt(Kingpin * Kingpin - Body.KingpinToAxle * Body.KingpinToAxle);
		const double TrailerHalf = Body.TrailerWidth * 0.5;
		InnerRadius = FMath::Min(InnerRadius, TrailerAxle - TrailerHalf);
		OuterRadius = FMath::Max3(OuterRadius,
			FMath::Sqrt(FMath::Square(TrailerAxle + TrailerHalf) + FMath::Square(Body.KingpinToAxle + Body.TrailerFront)),
			FMath::Sqrt(FMath::Square(TrailerAxle + TrailerHalf) + FMath::Square(Body.TrailerRear)));
	}

	Out.Inner = SteerRadius - InnerRadius;
	Out.Outer = OuterRadius - SteerRadius;
	Out.bHolds = true;
	return Out;
}

namespace
{
	FVector2D Perp(const FVector2D& V) { return FVector2D(-V.Y, V.X); }
}

FVector2D VehicleSweep::TrailerHeading(const FVector2D& Kingpin, const FVector2D& TrailerAxle)
{
	return (Kingpin - TrailerAxle).GetSafeNormal();
}

bool VehicleSweep::StepTrailer(const FVector2D& Kingpin, const FVector2D& CabHeading,
	double KingpinToAxle, FVector2D& InOutTrailerAxle)
{
	// Pursuit: the axle stays its fixed distance behind the kingpin it follows (discrete
	// tractrix - Trace's inline copy before this was pulled out, 2026-09-24).
	FVector2D Trailing = TrailerHeading(Kingpin, InOutTrailerAxle);
	InOutTrailerAxle = Kingpin - Trailing * KingpinToAxle;
	Trailing = TrailerHeading(Kingpin, InOutTrailerAxle);
	return FVector2D::DotProduct(Trailing, CabHeading) >= 0.0;   // < 0: folded past square, a jack-knife
}

bool VehicleSweep::Trace(const FBody& Body, TArrayView<const FVector2D> Path,
	TArray<double>& OutInner, TArray<double>& OutOuter)
{
	OutInner.Init(0.0, Path.Num());
	OutOuter.Init(0.0, Path.Num());
	if (Path.Num() < 2)
	{
		return true;
	}

	// 10 uu steps: a tenth of the lane margin, and the step the Python prototype that set the
	// test figures used (2026-09-24).
	constexpr double Step = 10.0;
	const FVector2D InTangent = (Path[1] - Path[0]).GetSafeNormal();
	const FVector2D OutTangent = (Path.Last() - Path[Path.Num() - 2]).GetSafeNormal();
	const double Lead = Body.Wheelbase + Body.KingpinX + Body.KingpinToAxle + Body.FrontX + 200.0;

	// Which side is the inside: the sign of the turn from start through middle to end.
	const FVector2D Mid = Path[Path.Num() / 2];
	const double Turn = FVector2D::CrossProduct(Mid - Path[0], Path.Last() - Mid);
	const double InwardSign = Turn >= 0.0 ? 1.0 : -1.0;

	TArray<FVector2D> Steps;
	for (double D = Lead; D > 0.0; D -= Step)
	{
		Steps.Add(Path[0] - InTangent * D);
	}
	for (int32 Index = 0; Index + 1 < Path.Num(); ++Index)
	{
		const FVector2D A = Path[Index];
		const FVector2D B = Path[Index + 1];
		const int32 Count = FMath::Max(1, FMath::FloorToInt32(FVector2D::Distance(A, B) / Step));
		for (int32 Sub = 0; Sub < Count; ++Sub)
		{
			Steps.Add(A + (B - A) * (static_cast<double>(Sub) / Count));
		}
	}
	for (double D = 0.0; D < Lead; D += Step)
	{
		Steps.Add(Path.Last() + OutTangent * D);
	}

	FVector2D Fixed = Steps[0] - InTangent * Body.Wheelbase;
	FVector2D Kingpin = Fixed + InTangent * Body.KingpinX;
	FVector2D TrailerAxle = Kingpin - InTangent * Body.KingpinToAxle;
	const bool bTrailer = Body.KingpinToAxle > 0.0;

	TArray<FVector2D, TInlineAllocator<16>> Corners;
	for (const FVector2D& Steered : Steps)
	{
		// Pursuit: each trailing point stays its fixed distance behind the one it follows.
		FVector2D Heading = (Steered - Fixed).GetSafeNormal();
		Fixed = Steered - Heading * Body.Wheelbase;
		Heading = (Steered - Fixed).GetSafeNormal();
		const FVector2D Side = Perp(Heading) * (Body.Width * 0.5);

		Corners.Reset();
		for (const double X : { Body.FrontX, Body.RearX, 0.0 })
		{
			Corners.Add(Fixed + Heading * X + Side);
			Corners.Add(Fixed + Heading * X - Side);
		}
		if (bTrailer)
		{
			Kingpin = Fixed + Heading * Body.KingpinX;
			if (!StepTrailer(Kingpin, Heading, Body.KingpinToAxle, TrailerAxle))
			{
				return false;   // folded past square: a jack-knife
			}
			const FVector2D Trailing = TrailerHeading(Kingpin, TrailerAxle);
			const FVector2D TrailerSide = Perp(Trailing) * (Body.TrailerWidth * 0.5);
			for (const double X : { Body.KingpinToAxle + Body.TrailerFront, -Body.TrailerRear, 0.0, Body.KingpinToAxle * 0.5 })
			{
				Corners.Add(TrailerAxle + Trailing * X + TrailerSide);
				Corners.Add(TrailerAxle + Trailing * X - TrailerSide);
			}
		}

		for (const FVector2D& Corner : Corners)
		{
			double Best = TNumericLimits<double>::Max();
			int32 BestSpan = 0;
			double BestT = 0.0;
			for (int32 Span = 0; Span + 1 < Path.Num(); ++Span)
			{
				const FVector2D AB = Path[Span + 1] - Path[Span];
				const double T = FMath::Clamp(FVector2D::DotProduct(Corner - Path[Span], AB)
					/ FMath::Max(AB.SizeSquared(), UE_DOUBLE_SMALL_NUMBER), 0.0, 1.0);
				const double Distance = FVector2D::Distance(Corner, Path[Span] + AB * T);
				if (Distance < Best)
				{
					Best = Distance;
					BestSpan = Span;
					BestT = T;
				}
			}
			if ((BestSpan == 0 && BestT <= 0.0) || (BestSpan == Path.Num() - 2 && BestT >= 1.0))
			{
				continue;   // beyond an end: the straight lanes, gated by Width
			}
			const FVector2D AB = Path[BestSpan + 1] - Path[BestSpan];
			const FVector2D Foot = Path[BestSpan] + AB * BestT;
			const double Lateral = FVector2D::DotProduct(Corner - Foot, Perp(AB.GetSafeNormal()) * InwardSign);
			const int32 Sample = BestT < 0.5 ? BestSpan : BestSpan + 1;
			if (Lateral > 0.0)
			{
				OutInner[Sample] = FMath::Max(OutInner[Sample], Lateral);
			}
			else
			{
				OutOuter[Sample] = FMath::Max(OutOuter[Sample], -Lateral);
			}
		}
	}
	return true;
}
