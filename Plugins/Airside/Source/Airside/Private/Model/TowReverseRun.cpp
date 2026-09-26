#include "Model/TowReverseRun.h"

#include "Model/Vehicle.h"
#include "Model/VehicleFit.h"

bool FTowReverseRun::Start(const FRoutePlan& InPlan, const FVehicle& Vehicle, const FVector2D& Origin, double Heading,
	TArrayView<const FVector2D> Axles, double InReverseSpeed, FString* OutReason)
{
	TowReverse::FInput In;
	In.Body = VehicleFit::BodyOf(Vehicle);
	In.MaxSteerRadians = FMath::DegreesToRadians(Vehicle.Chassis.Ground.MaxSteerDegrees);
	In.Line = InPlan.Polyline;
	FVector2D Forward;
	In.Fixed = VehicleFit::FixedAxleAt(Vehicle.Chassis, Origin, Heading, Forward);
	In.Heading = Heading;
	In.Axles.Append(Axles.GetData(), Axles.Num());

	TowReverse::FSolution Solution = TowReverse::Solve(In);
	if (!Solution.IsValid())
	{
		if (OutReason != nullptr)
		{
			*OutReason = Solution.Describe();
		}
		return false;
	}

	Plan = InPlan;
	ReverseSpeed = InReverseSpeed;
	Speed = 0.0;
	Samples = MoveTemp(Solution.Samples);
	// FROM WHERE THE LEADING AXLE IS, not from the line's start: the vehicle stopped with its
	// steered axle at the leg's start, so its trailer axle is a chain's length back along the
	// approach the leg retraces - the rigid code's `Reverse.Travelled = Wheelbase`, measured
	// rather than assumed, since a chain's length depends on its hitch angle.
	Along = Samples[0].Along;
	SteerDegrees = Samples[0].SteerDegrees;
	Cursor = 0;
	if (OutReason != nullptr)
	{
		*OutReason = Solution.Describe();
	}
	return true;
}

void FTowReverseRun::Reset()
{
	Plan = FRoutePlan();
	Samples.Reset();
	Speed = 0.0;
	SteerDegrees = 0.0;
	Along = 0.0;
	Cursor = 0;
}

void FTowReverseRun::PoseAt(const FVehicle& Vehicle, FVector2D& OutOrigin, double& OutHeading, TArray<FVector2D>& OutAxles)
{
	while (Cursor + 2 < Samples.Num() && Samples[Cursor + 1].Along <= Along)
	{
		++Cursor;
	}
	const TowReverse::FSample& From = Samples[Cursor];
	const TowReverse::FSample& To = Samples[FMath::Min(Cursor + 1, Samples.Num() - 1)];
	const double Span = To.Along - From.Along;
	const double T = Span > UE_KINDA_SMALL_NUMBER ? FMath::Clamp((Along - From.Along) / Span, 0.0, 1.0) : 1.0;

	const FVector2D Fixed = FMath::Lerp(From.Fixed, To.Fixed, T);
	OutHeading = FMath::UnwindRadians(From.Heading + FMath::UnwindRadians(To.Heading - From.Heading) * T);
	// Back to the body ORIGIN, which is what every phase reports (DescribeMotion adds FixedAxleX
	// back on) - the same point today, FixedAxleX being 0 on every vehicle (RoadAgent.cpp's own
	// note), converted anyway so the day one differs is not a silent offset.
	OutOrigin = Fixed - FVector2D(FMath::Cos(OutHeading), FMath::Sin(OutHeading)) * Vehicle.Chassis.FixedAxleX;
	OutAxles.SetNum(From.Axles.Num());
	for (int32 Index = 0; Index < From.Axles.Num() && Index < To.Axles.Num(); ++Index)
	{
		OutAxles[Index] = FMath::Lerp(From.Axles[Index], To.Axles[Index], T);
	}
	SteerDegrees = FMath::Lerp(From.SteerDegrees, To.SteerDegrees, T);
}

bool FTowReverseRun::Advance(double DeltaSeconds, double StopWithin, const FVehicle& Vehicle, FVector2D& OutOrigin,
	double& OutHeading, TArray<FVector2D>& OutAxles)
{
	if (!IsArmed())
	{
		Speed = 0.0;
		return false;
	}
	if (HasArrived())
	{
		// OVER, AND NOTHING MOVED - but the outputs still carry the final pose, which is what the
		// caller hands to Park or to the drive-on (issue #289's lesson on FReverseRun).
		Speed = 0.0;
		PoseAt(Vehicle, OutOrigin, OutHeading, OutAxles);
		return false;
	}

	const double Before = Along;
	const double Step = FMath::Min(ReverseSpeed * DeltaSeconds, FMath::Max(0.0, StopWithin));
	Along = FMath::Min(Along + Step, Samples.Last().Along);
	Speed = DeltaSeconds > 0.0 ? (Along - Before) / DeltaSeconds : 0.0;
	PoseAt(Vehicle, OutOrigin, OutHeading, OutAxles);
	return true;
}
