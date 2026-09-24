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
