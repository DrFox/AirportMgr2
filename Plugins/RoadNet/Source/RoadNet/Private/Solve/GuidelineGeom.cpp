#include "Solve/GuidelineGeom.h"

namespace GuidelineGeom
{
	FVector2D Eval(const FVector2D& A, const FVector2D& Control, const FVector2D& B, double T)
	{
		const double U = 1.0 - T;
		return (U * U) * A + (2.0 * U * T) * Control + (T * T) * B;
	}

	bool IsStraight(const FVector2D& A, const FVector2D& Control, const FVector2D& B)
	{
		// Scaled to the guideline's own length: a fixed tolerance is either meaningless on
		// a 3km runway or wrong on a 2m link. UE_DOUBLE_KINDA_SMALL_NUMBER of the span is
		// far below anything the builder produces deliberately.
		const FVector2D Span = B - A;
		const double Scale = FMath::Max(Span.Size(), 1.0);
		return FVector2D::Distance(Control, (A + B) * 0.5) <= Scale * 1e-6;
	}

	void Sample(
		const FVector2D& A, const FVector2D& Control, const FVector2D& B,
		TArray<FVector2D>& OutPoints, int32 Samples)
	{
		// A straight guideline needs no interior points, and giving it some would make the
		// common case cost fifteen times more to walk for geometry identical to its own
		// two ends.
		if (IsStraight(A, Control, B))
		{
			OutPoints.Add(A);
			OutPoints.Add(B);
			return;
		}

		const int32 Count = FMath::Max(Samples, 2);
		OutPoints.Reserve(OutPoints.Num() + Count);

		for (int32 Step = 0; Step < Count; ++Step)
		{
			// Endpoints are produced by evaluating at exactly 0 and 1 rather than being
			// substituted in, which for a quadratic returns A and B exactly - so a route
			// welded from consecutive edges meets at one point, not two a hair apart.
			OutPoints.Add(Eval(A, Control, B, static_cast<double>(Step) / (Count - 1)));
		}
	}

	double Length(const FVector2D& A, const FVector2D& Control, const FVector2D& B, int32 Samples)
	{
		TArray<FVector2D> Points;
		Sample(A, Control, B, Points, Samples);
		return PolylineLength(Points);
	}

	void Split(
		const FVector2D& A, const FVector2D& Control, const FVector2D& B, double T,
		FVector2D& OutMid, FVector2D& OutControlLeft, FVector2D& OutControlRight)
	{
		const double Clamped = FMath::Clamp(T, 0.0, 1.0);

		OutControlLeft = FMath::Lerp(A, Control, Clamped);
		OutControlRight = FMath::Lerp(Control, B, Clamped);
		OutMid = FMath::Lerp(OutControlLeft, OutControlRight, Clamped);
	}

	double ParamAtSample(int32 Index, double Fraction, int32 Count)
	{
		if (Count < 2)
		{
			return 0.0;
		}
		return FMath::Clamp((Index + Fraction) / (Count - 1), 0.0, 1.0);
	}

	double PolylineLength(const TArray<FVector2D>& Points)
	{
		double Total = 0.0;
		for (int32 At = 1; At < Points.Num(); ++At)
		{
			Total += FVector2D::Distance(Points[At - 1], Points[At]);
		}
		return Total;
	}

	bool PointAtDistance(
		const TArray<FVector2D>& Points, double Distance,
		FVector2D& OutPosition, double& OutHeading)
	{
		if (Points.Num() < 2)
		{
			// One point has a position but no direction, and inventing one - zero, say -
			// would point every arrived agent due east. Refusing lets the caller keep
			// whatever heading it already had.
			return false;
		}

		double Walked = 0.0;
		for (int32 At = 1; At < Points.Num(); ++At)
		{
			const FVector2D Step = Points[At] - Points[At - 1];
			const double StepLength = Step.Size();
			if (StepLength <= 0.0)
			{
				continue;
			}

			if (Distance <= Walked + StepLength)
			{
				const double Into = FMath::Clamp(Distance - Walked, 0.0, StepLength);
				OutPosition = Points[At - 1] + Step * (Into / StepLength);
				OutHeading = FMath::Atan2(Step.Y, Step.X);
				return true;
			}

			Walked += StepLength;
		}

		// Past the end, or a polyline of nothing but coincident points. Report the last
		// real position and the last real direction: an agent that overshoots by a frame
		// should be standing at its destination facing the way it arrived.
		for (int32 At = Points.Num() - 1; At >= 1; --At)
		{
			const FVector2D Step = Points[At] - Points[At - 1];
			if (!Step.IsNearlyZero())
			{
				OutPosition = Points.Last();
				OutHeading = FMath::Atan2(Step.Y, Step.X);
				return true;
			}
		}

		return false;
	}
}
