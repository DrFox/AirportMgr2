#include "Model/PushbackRun.h"

#include "Solve/GuidelineGeom.h"

namespace
{
	/**
	 * How far a polyline runs straight before it first turns, or 0 if it never does.
	 *
	 * THE STRAIGHT PART OF A PUSH IS A GEOMETRIC FACT. A stand's lead-in is laid as a straight
	 * ray and its corner as sweeps, so this finds the corner wherever it is - and finds
	 * honestly that there is none when a route simply runs straight out of a plain node.
	 *
	 * MEASURED AGAINST THE FIRST SEGMENT'S DIRECTION, not against the previous segment's: a
	 * sweep is sampled into many small steps, each a degree or two from the last, so a
	 * segment-to-segment test would never see a corner at all however far round it went.
	 *
	 * TWENTY DEGREES is GuidelineGeom's own threshold for a real corner rather than a sampling
	 * artefact - FRouteFollower::CrabAtMinSpeedDegrees cites it by name for the same reason.
	 * Using one number for both keeps "the line bends here" a single opinion.
	 *
	 * Returns the distance to the START of the first turning segment: that is the last point
	 * at which the aeroplane is still square to the line it reversed down.
	 */
	constexpr double PushbackCornerDegrees = 20.0;

	double StraightRunLength(const TArray<FVector2D>& Points)
	{
		if (Points.Num() < 3)
		{
			// Two points are one straight segment with nothing after it to turn.
			return 0.0;
		}

		FVector2D First = Points[1] - Points[0];
		if (!First.Normalize())
		{
			return 0.0;
		}

		double Travelled = 0.0;
		for (int32 Index = 1; Index < Points.Num() - 1; ++Index)
		{
			Travelled += FVector2D::Distance(Points[Index - 1], Points[Index]);

			FVector2D Next = Points[Index + 1] - Points[Index];
			if (!Next.Normalize())
			{
				continue;
			}

			const double Turn = FMath::RadiansToDegrees(
				FMath::Acos(FMath::Clamp(FVector2D::DotProduct(First, Next), -1.0, 1.0)));
			if (Turn > PushbackCornerDegrees)
			{
				return Travelled;
			}
		}

		// Straight the whole way.
		return 0.0;
	}
}

bool FPushbackRun::PlanPushDistance(const FRoutePlan& InPlan, double SwingLength,
	double& OutBackDistance, double& OutPushDistance, double& OutTargetHeading)
{
	if (!InPlan.IsValid() || InPlan.Steps.Num() == 0)
	{
		return false;
	}

	// WHERE THE ROUTE STOPS BEING STRAIGHT. That is what "the lead-in" means physically, and
	// it is a fact about the GEOMETRY rather than about the step list - which is the whole
	// correction here. Steps[0] is the lead-in when the route starts at a stand pose node and
	// is something else entirely when it does not, and an earlier version took it on trust and
	// then capped it at sixty metres to bound the cases where the trust was misplaced. Both
	// halves of that were wrong: the cap cut before real corners, and the step index answered
	// the wrong question. Reported from PIE on 2026-09-13 as an aeroplane that reversed its
	// straight leg correctly and then turned to face back out of its own stand.
	const double Corner = StraightRunLength(InPlan.Polyline);

	// NO CORNER AT ALL means the route runs dead straight out of wherever the aeroplane is
	// standing. There is nothing to reverse ALONG that leads anywhere and nothing to swing
	// ONTO, so the manoeuvre is a turn on the spot: no straight leg, and the swing is the
	// whole of it. A tug turning an aeroplane round on a straight taxiway does exactly that.
	//
	// This is also what keeps a route whose first step is a two-hundred-metre taxiway from
	// being reversed down its whole length, which is what a discarded straight-back cap was
	// really for - and why there is no such cap any more. A corner is where the aeroplane MUST
	// reach, so no bound may cut before it; a straight route has no corner, so no bound is
	// needed. The knob that tried to be both was the defect.
	const double Back = Corner;

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
