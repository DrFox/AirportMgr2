#include "Model/ReverseRun.h"

#include "AirsideLog.h"
#include "Solve/GuidelineGeom.h"

bool FReverseRun::Start(const FRoutePlan& InPlan, const FAirframe& Airframe, double InReverseSpeed)
{
	if (!InPlan.IsValid() || InPlan.Polyline.Num() < 2 || InPlan.Length <= UE_KINDA_SMALL_NUMBER)
	{
		return false;
	}

	// THE CHECK THAT MAKES THIS SAFE TO PLAY BACK. A pre-computed manoeuvre is only as good as
	// the curve it was given, and playing back a curve the body cannot hold is exactly the
	// crabbing this design exists to remove - it would simply do it silently, because playback
	// has no error term to notice with.
	//
	// ASKED OF THE PROFILE, not re-derived here. FSpeedProfile is the one authority on whether
	// a line is drivable, and it now knows which way round the vehicle is going. Re-implementing
	// its rule is what let four attempts at stand routing ship green - see
	// FSpeedProfile::WasTighterThanLock.
	FSpeedProfile Check;
	Check.Build(InPlan.Polyline, Airframe, EDriveDirection::Reverse);

	if (Check.WasTighterThanLock())
	{
		UE_LOG(LogAirside, Warning,
			TEXT("Reverse manoeuvre refused: asks for R=%.0f uu at %.0f, but this vehicle can "
			     "only hold R>=%.0f going backwards (wheelbase %.0f, lock %.0f deg). Widen the "
			     "bay's approach."),
			Check.GetTightestRadius(), Check.GetTightestAt(),
			Airframe.TightestReversibleRadius(), Airframe.Wheelbase(),
			Airframe.Ground.MaxSteerDegrees);
		return false;
	}

	if (Check.HasSharpVertex())
	{
		// A SEPARATE REFUSAL, because it is a separate defect. A vertex whose heading changes
		// instantly is untakeable whichever way the vehicle points - nothing about reversing
		// lets a body rotate without moving - and it would not be caught by the radius rule,
		// which has no Length to divide by at a zero-length turn.
		UE_LOG(LogAirside, Warning,
			TEXT("Reverse manoeuvre refused: %d vertex/vertices turn instantly, sharpest %.0f "
			     "deg at %.0f. A bay's approach must be a curve, not a corner."),
			Check.GetSharpVertexCount(), Check.GetSharpestDegrees(), Check.GetSharpestAt());
		return false;
	}

	Plan = InPlan;
	Travelled = 0.0;
	ReverseSpeed = FMath::Max(0.0, InReverseSpeed);

	// FROM REST, like FRouteFollower::Speed and FPushbackRun::Speed. A manoeuvre that armed
	// reporting its cap would spin the wheels on the frame before it had moved at all.
	Speed = 0.0;
	return true;
}

bool FReverseRun::Advance(double DeltaSeconds, double StopWithin,
	FVector2D& OutPosition, double& OutHeading)
{
	if (!Plan.IsValid() || Plan.Polyline.Num() < 2)
	{
		Speed = 0.0;
		return false;
	}

	// HELD, NOT FINISHED. Arbitration asking for a stop is not the manoeuvre ending, so the
	// vehicle stays where it is and keeps its pose rather than handing over - the same reading
	// of StopWithin the follower uses.
	const double Room = FMath::Max(0.0, StopWithin);
	const double Step = FMath::Min(ReverseSpeed * DeltaSeconds, Room);

	// MEASURED AFTER THE CLAMP, NOT BEFORE IT, and that is the whole point of the field. Step
	// is what this frame WANTED; Travelled is clamped to Plan.Length on the frame that
	// arrives, and Room is zero for every frame arbitration holds the vehicle. Reporting Step
	// - or worse ReverseSpeed, which is what DescribeMotion used to read - tells the view a
	// stationary truck is doing a metre a second, and its wheels spin on the spot. Reported
	// from play, 2026-09-20.
	const double Before = Travelled;
	Travelled = FMath::Min(Travelled + Step, Plan.Length);
	Speed = DeltaSeconds > UE_DOUBLE_SMALL_NUMBER ? (Travelled - Before) / DeltaSeconds : 0.0;

	double LineHeading = 0.0;
	if (!GuidelineGeom::PointAtDistance(Plan.Polyline, Travelled, OutPosition, LineHeading))
	{
		return false;
	}

	// THE WHOLE HEADING LAW, and it is one line because the curve was solved for this vehicle
	// before the manoeuvre was armed. Facing is the tangent turned through 180 degrees for the
	// length of the run: the body travels backwards along the line it is on, which is what
	// backing into a bay is. The steered axle trails; the fixed axle is the point on the line.
	OutHeading = FMath::UnwindRadians(LineHeading + UE_DOUBLE_PI);

	return !HasArrived();
}
