#include "Model/ReverseRun.h"

#include "AirsideLog.h"
#include "Solve/GuidelineGeom.h"

namespace
{
	/**
	 * Signed curvature of the plan at a distance along it, radians per uu. Positive turns the
	 * same way FMath::UnwindRadians counts.
	 *
	 * TURN OVER LENGTH, which is the form FSpeedProfile::Build already uses for its own
	 * radius - "Radius = Length / Turn". Stating the same geometry a second way is how a route
	 * comes to be judged drivable by one rule and driven by another, which this codebase has
	 * paid for before; see the note at FReverseRun::Start about asking the profile rather than
	 * re-deriving its limit.
	 *
	 * MEASURED OVER A WINDOW, not between adjacent samples, and the window is the caller's
	 * because the only sensible length is the vehicle's own. GuidelineGeom::PointAtDistance
	 * reports the SEGMENT's heading, so heading along a polyline is a staircase: a difference
	 * taken over a short span reads zero for most frames and the whole of a vertex's turn on
	 * one of them, which would flick the steered wheels rather than turn them. A window of one
	 * wheelbase averages that out and is the distance over which the steering actually acts.
	 *
	 * Clamped to the ends of the plan, so the window shortens rather than sampling off the
	 * curve, and the divisor is the span actually covered.
	 */
	double SignedCurvature(const TArray<FVector2D>& Polyline, double Length, double At,
		double Window)
	{
		const double Half = FMath::Max(Window, UE_DOUBLE_KINDA_SMALL_NUMBER) * 0.5;
		const double From = FMath::Clamp(At - Half, 0.0, Length);
		const double To = FMath::Clamp(At + Half, 0.0, Length);
		const double Span = To - From;
		if (Span <= UE_DOUBLE_KINDA_SMALL_NUMBER)
		{
			return 0.0;
		}

		// BOTH RETURNS HONOURED. PointAtDistance leaves its out-parameters untouched when it
		// fails, and a heading read out of an uninitialised double would be a steering angle
		// made of stack rubbish - see CLAUDE.md, "honour the return of anything that fills an
		// out-parameter".
		FVector2D Ignored = FVector2D::ZeroVector;
		double Before = 0.0;
		double After = 0.0;
		if (!GuidelineGeom::PointAtDistance(Polyline, From, Ignored, Before)
			|| !GuidelineGeom::PointAtDistance(Polyline, To, Ignored, After))
		{
			return 0.0;
		}

		return FMath::UnwindRadians(After - Before) / Span;
	}
}

bool FReverseRun::Start(const FRoutePlan& InPlan, const FAirframe& Airframe, double InReverseSpeed)
{
	if (!InPlan.IsValid() || InPlan.Polyline.Num() < 2 || InPlan.Length <= UE_KINDA_SMALL_NUMBER)
	{
		return false;
	}

	// LOGGED ONCE, HERE, rather than from EffectiveSteerLaw itself - see #176. Advance below
	// calls it every frame this manoeuvre runs; Start runs once per bay entry.
	WarnIfSteerLawUnsupported(Airframe);

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

	// STRAIGHT UNTIL THE FIRST Advance, which FRoadAgent calls with a zero delta on the arming
	// frame precisely so the view has a pose before the phase changes. The steer angle is a
	// fact about where the vehicle IS on the curve, so that zero-second call fills it in.
	SteerDegrees = 0.0;
	return true;
}

bool FReverseRun::Advance(double DeltaSeconds, const FAirframe& Airframe, double StopWithin,
	FVector2D& OutPosition, double& OutHeading)
{
	if (!Plan.IsValid() || Plan.Polyline.Num() < 2)
	{
		Speed = 0.0;
		SteerDegrees = 0.0;
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

	// WHERE THE WHEELS POINT. Backing along an arc, a rigid vehicle pivots about its FIXED
	// axle - the point this run walks along the line - so tan(steer) = Wheelbase / Radius, the
	// exact inverse of FAirframe::TightestReversibleRadius. Curvature is 1/Radius with a sign,
	// which is why it is expressed that way round and never divides by a radius that could be
	// a straight line's infinity.
	//
	// NEGATED, AND THAT IS THE PHYSICS. A reversing vehicle counter-steers: the front wheels
	// go right to swing the back of the body left. Derived rather than asserted - the body
	// faces the tangent turned through 180 degrees, so its heading rate is +curvature times
	// speed, while the bicycle model gives heading rate = v tan(steer) / L with v NEGATIVE
	// going backwards. Equate the two and the sign falls out. Airside.Model.
	// ReverseSteersRatherThanSliding checks the steer against the yaw for exactly this reason.
	//
	// UNDER THE LOCK BY CONSTRUCTION, because Start refused any curve tighter than the vehicle
	// can hold - clamped anyway, since a curvature read across a vertex on a plan that only
	// just passed could round the wrong side of it, and a wheel through its own stop is a
	// thing the player sees.
	if (Airframe.EffectiveSteerLaw() == ESteerLaw::RollingSteer
		&& Airframe.Wheelbase() > UE_DOUBLE_KINDA_SMALL_NUMBER)
	{
		const double Curvature =
			SignedCurvature(Plan.Polyline, Plan.Length, Travelled, Airframe.Wheelbase());
		const double Lock = FMath::Clamp(Airframe.Ground.MaxSteerDegrees, 0.0, 90.0);
		SteerDegrees = FMath::Clamp(
			FMath::RadiansToDegrees(-FMath::Atan(Curvature * Airframe.Wheelbase())), -Lock, Lock);
	}
	else
	{
		// No steered wheel, so the view must not draw one turning - the rule FRouteFollower
		// states in the same words at its own SteerDegrees.
		SteerDegrees = 0.0;
	}

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
