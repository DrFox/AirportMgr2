#include "Model/PushbackRun.h"

#include "Solve/GuidelineGeom.h"

bool FPushbackRun::PlanPushDistance(const FRoutePlan& InPlan, double SwingLength,
	double& OutBackDistance, double& OutPushDistance, double& OutTargetHeading)
{
	if (!InPlan.IsValid() || InPlan.Steps.Num() == 0)
	{
		return false;
	}

	// STEPS[0] IS THE STRAIGHT LEAD-IN. FAnchorLink::Gather casts it as a straight ray out of
	// the pose node - along Heading + PI, because +X faces the terminal - and builds the
	// corner's entry sweeps as steps of their own. So Back needs no steering law at all: the
	// body simply holds the heading it parked at while the tug pulls it out.
	const double Back = InPlan.Steps[0].EndDistance;

	// PAST THE CORNER, and this is the whole reason SwingLength exists - see
	// FTrafficRules::PushSwingLength for what stopping at Back would leave on screen.
	//
	// CLAMPED TO THE PLAN: a route shorter than a swing length is pushed as far as it goes,
	// which is better than sampling off the end of the polyline. PointAtDistance would clamp
	// anyway and hold the last real heading, but then PushDistance and the distance actually
	// reachable would disagree, and HasArrived is written against PushDistance.
	const double Push = FMath::Min(Back + SwingLength, InPlan.Length);

	FVector2D At = FVector2D::ZeroVector;
	double Tangent = 0.0;
	if (!GuidelineGeom::PointAtDistance(InPlan.Polyline, Push, At, Tangent))
	{
		// A polyline too short to have a direction at all. HONOURED rather than ignored: the
		// out-parameters stay untouched, so this cannot be the "FVector2D X; use it anyway"
		// that puts things at the world origin.
		return false;
	}

	OutBackDistance = Back;
	OutPushDistance = Push;
	OutTargetHeading = Tangent;
	return true;
}

bool FPushbackRun::Start(const FRoutePlan& InPlan, double InParkedHeading, double InPushSpeed,
	double InPushAccel, double InSwingLength, bool bInNeedsThrust)
{
	double Back = 0.0;
	double Push = 0.0;
	double Target = 0.0;
	if (!PlanPushDistance(InPlan, InSwingLength, Back, Push, Target))
	{
		// NOTHING TOUCHED. A manoeuvre that cannot be flown must leave no trace of itself
		// rather than one half-armed - the rule FRoadAgent::StartArrival states for a landing
		// and for the same reason: a caller that ignored this return would otherwise find a
		// struct that looks armed and walks a plan it never accepted.
		return false;
	}

	Plan = InPlan;
	Phase = EPushPhase::Back;
	Travelled = 0.0;
	Speed = 0.0;
	Heading = InParkedHeading;
	ParkedHeading = InParkedHeading;
	BackDistance = Back;
	PushDistance = Push;
	TargetHeading = Target;
	PushSpeed = FMath::Max(0.0, InPushSpeed);

	// CLAMPED AWAY FROM ZERO because Advance divides by it for the braking distance. A rules
	// asset edited to 0 would otherwise be a division by zero rather than a slow push.
	PushAccel = FMath::Max(UE_KINDA_SMALL_NUMBER, InPushAccel);
	bNeedsThrust = bInNeedsThrust;
	return true;
}

bool FPushbackRun::Advance(double DeltaSeconds, double StopWithin, bool bHasThrust,
	FVector2D& OutPosition, double& OutHeading)
{
	if (HasArrived())
	{
		return false;
	}

	// A POWERBACK IS THE ENGINE DOING THE WORK, so it holds at rest until there is thrust. An
	// aeroplane on a tug bar moves from the first frame whatever its propeller is doing.
	//
	// RETURNS TRUE rather than false: the manoeuvre is still current, it is simply not moving
	// yet. Handing over here would put a stopped aeroplane straight onto the taxi with the
	// push undone.
	if (bNeedsThrust && !bHasThrust)
	{
		Speed = 0.0;
		FVector2D At = FVector2D::ZeroVector;
		double LineHeading = 0.0;
		if (GuidelineGeom::PointAtDistance(Plan.Polyline, Travelled, At, LineHeading))
		{
			OutPosition = At;
		}

		// THE BODY'S heading, never the line's - it is standing on the stand facing the
		// terminal, and the line under it points the other way.
		OutHeading = Heading;
		return true;
	}

	// WHERE IT MAY GET TO THIS FRAME: the end of the push, or wherever arbitration stopped it,
	// whichever is nearer. StopWithin is the ONE input into this motion, exactly as it is for
	// the follower - there is no second evaluator of where an agent may go.
	const double StopAt = FMath::Min(PushDistance, FMath::Max(0.0, StopWithin));

	// TRAPEZOIDAL, AND IT ENDS AT REST. The aeroplane is about to reverse its direction of
	// travel: handing the follower a non-zero speed would have it pull away forwards at the
	// speed it was just being pushed backwards at, on the handover frame. Braking distance is
	// the same v^2/2a the claim window uses, so the two agree about what "far enough to stop"
	// means.
	const double Remaining = FMath::Max(0.0, StopAt - Travelled);
	const double BrakingDistance = Speed * Speed / (2.0 * PushAccel);
	Speed = BrakingDistance >= Remaining
		? FMath::Max(0.0, Speed - PushAccel * DeltaSeconds)
		: FMath::Min(PushSpeed, Speed + PushAccel * DeltaSeconds);

	Travelled = FMath::Clamp(Travelled + Speed * DeltaSeconds, 0.0, StopAt);

	// AND IT IS ACTUALLY STOPPED WHEN IT STOPS. The ramp above is discrete, so it lands
	// within one frame's PushAccel of zero - 0.5 uu/s at 60 Hz - and HasArrived ends the run
	// on the next frame before the ramp can finish. Half a centimetre per second is nothing
	// to look at, but FRoadAgent::DescribeMotion reads this figure as the agent's ground
	// speed and UAirsideAgentAnim turns the wheels at speed over radius: an aeroplane whose
	// push has ended with its wheels still creeping is the same class of defect as the
	// landing that touched down with its wheels perfectly still. The two facts - arrived, and
	// at rest - have to agree.
	if (Travelled >= PushDistance - UE_KINDA_SMALL_NUMBER)
	{
		Speed = 0.0;
	}

	// THE STEERED AXLE IS ON THE LINE, and that never changes - see this struct's header.
	// What reverses is which end of the body leads it, which is FClaimPass::CentreOf's
	// business and not this function's.
	FVector2D At = FVector2D::ZeroVector;
	double LineHeading = 0.0;
	if (!GuidelineGeom::PointAtDistance(Plan.Polyline, Travelled, At, LineHeading))
	{
		// The pose is left exactly as the caller had it rather than written from an unset
		// FVector2D. Still true: declining to MOVE is not the same as handing over.
		return true;
	}
	OutPosition = At;

	Phase = Travelled < BackDistance ? EPushPhase::Back : EPushPhase::Swing;

	if (Phase == EPushPhase::Back)
	{
		// The lead-in is straight, so there is nothing to steer for: the body holds the
		// heading it parked at while it is pulled out.
		Heading = ParkedHeading;
	}
	else
	{
		// LERPED LINEARLY IN TRAVELLED, not in time. At a constant push speed that is a
		// constant yaw rate, and when arbitration slows the push the turn slows with it -
		// which is what a tug does. Time-based would go on turning an aeroplane that had been
		// stopped by the traffic ahead, and would reach TargetHeading before it reached
		// PushDistance, leaving the last of the push crabbing sideways down the line.
		//
		// UNWOUND FIRST so the turn is taken the short way round rather than very nearly all
		// the way about, and so the 180 degree dead-end apron case resolves to a half turn in
		// one direction rather than oscillating about the seam.
		const double Swing = FMath::Max(UE_KINDA_SMALL_NUMBER, PushDistance - BackDistance);
		const double Alpha = FMath::Clamp((Travelled - BackDistance) / Swing, 0.0, 1.0);
		Heading = ParkedHeading + Alpha * FMath::UnwindRadians(TargetHeading - ParkedHeading);
	}

	OutHeading = Heading;

	// TRUE WHILE IT IS STILL MINE. HasArrived is re-asked at the TOP of the next frame rather
	// than answered here, so the last frame of the push still reports its own motion. A frame
	// with no motion at all is exactly the step the handover-continuity test exists to catch.
	return true;
}
