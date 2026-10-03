#include "Model/PushbackRun.h"

#include "Model/RouteJoin.h"
#include "Solve/GuidelineGeom.h"

bool FPushbackRun::Start(const FRoutePlan& InPlan, double InPushSpeed, double InPushAccel,
	bool bInNeedsThrust)
{
	FVector2D At = FVector2D::ZeroVector;
	double Tangent = 0.0;
	if (!InPlan.IsDrivable()
		|| !GuidelineGeom::PointAtDistance(InPlan.Polyline, 0.0, At, Tangent))
	{
		// NOTHING TOUCHED. A manoeuvre that cannot be flown must leave no trace of itself
		// rather than one half-armed - the rule FRoadAgent::StartArrival states for a landing,
		// and for the same reason: a caller that ignored this return would otherwise find a
		// struct that looks armed and walks a plan it never accepted.
		return false;
	}

	Plan = InPlan;
	Travelled = 0.0;
	Speed = 0.0;
	// THE CLEARANCE, as DepartAgent granted it: the whole of this plan (#502 review - see ClearedTo).
	ClearedTo = InPlan.Length;

	// FACING THE WAY IT PARKED, which is this line's tangent turned about - see the header.
	// Seeded here so a caller reading the pose before the first Advance sees the aeroplane on
	// its stand rather than at an unset FVector2D.
	Heading = FMath::UnwindRadians(Tangent + UE_DOUBLE_PI);
	PushSpeed = FMath::Max(0.0, InPushSpeed);

	// CLAMPED AWAY FROM ZERO because Advance divides by it for the braking distance. A rules
	// asset edited to 0 would otherwise be a division by zero rather than a slow push.
	PushAccel = FMath::Max(UE_KINDA_SMALL_NUMBER, InPushAccel);
	bNeedsThrust = bInNeedsThrust;
	return true;
}

bool FPushbackRun::Rejoin(const FRoutePlan& Route, double Along, const FVector2D& From)
{
	FVector2D OnLine = FVector2D::ZeroVector;
	double Tangent = 0.0;
	if (!Route.IsDrivable() || Route.Steps.Num() == 0 || Along < 0.0 || Along > Route.Length
		|| !GuidelineGeom::PointAtDistance(Route.Polyline, Along, OnLine, Tangent))
	{
		return false;
	}

	// THE CLEARANCE LEFT, carried into the new route's distances below whichever way it is joined (#502 review): a rejoin
	// moves the push onto another line, not further than it was cleared to go.
	const double ClearanceLeft = ClearedTo - Travelled;

	// ON THE LINE ALREADY - a lead-in shortened along its own axis, a split: the new route is walked from the projection.
	const double Offset = FVector2D::Distance(From, OnLine);
	if (Offset < 1.0)
	{
		Plan = Route;
		Travelled = Along;
		ClearedTo = Travelled + ClearanceLeft;
		return true;
	}

	// THE JOIN MEETS THE LINE AHEAD, not square across: JoinLead offsets ahead of the projection, so the leg meets the line
	// at atan(1/4), 14.04 degrees, where a square join would turn the body through 90 and back - the heading IS the
	// line's tangent turned about (see the header), so a kink in the line is an instant yaw of the pose. Where the leg
	// LEAVES, the turn is that plus or minus the angle between the old line and the new (1.4 degrees, toward, on
	// PushbackJunctionMovedBehindItCompletes). WITHIN THE FIRST STEP, so every step's end is still a vertex of the line it
	// names and only the first step's span grows by the leg, as UGroundTraffic's held taxi out's join grows its first.
	//
	// AND REFUSED when the first step has not JoinLead offsets left past the projection (#501 re-review). Clamping the
	// join to the step's end met the line steeply (an arm shifted 5 m, 2 m from its end: 68 degrees), and a push already
	// past the moved node projects onto the edge's END and the leg ran BACKWARD - a 180-degree flip in one frame. Refused,
	// the push takes the strand-and-hold path: it stops where it is and a way out is planned from there.
	// ENFORCED BY: Airside.Model.PushbackArmShiftedNearItsEndHolds, Airside.Model.PushbackPastTheMovedNodeHolds
	constexpr double JoinLead = 4.0;
	const double JoinTo = Along + JoinLead * Offset;
	if (JoinTo > Route.Steps[0].EndDistance + UE_KINDA_SMALL_NUMBER)
	{
		return false;
	}
	// THE LEG ITSELF IS RouteJoin::Prepend's (#502), the one join-leg shape the held taxi out's join shares: the polyline
	// re-laid from where the aeroplane stands, every step re-based. Refused there, untouched here.
	FRoutePlan Joined;
	if (!RouteJoin::Prepend(Route, From, JoinTo, Joined))
	{
		return false;
	}
	Plan = Joined;
	Travelled = 0.0;
	ClearedTo = ClearanceLeft;
	return true;
}

void FPushbackRun::AppendRemainingRun(bool bReverse, TArray<FRouteRun>& Out) const
{
	// BY THE ARC LENGTH Advance WALKS (PointAtDistance), so the run starts under the steered axle, where the pose is.
	FVector2D At = FVector2D::ZeroVector;
	double Tangent = 0.0;
	if (HasArrived() || !GuidelineGeom::PointAtDistance(Plan.Polyline, Travelled, At, Tangent))
	{
		return;
	}
	FRouteRun Run;
	Run.bReverse = bReverse;
	Run.Points.Add(At);
	double Walked = 0.0;
	for (int32 Index = 1; Index < Plan.Polyline.Num(); ++Index)
	{
		Walked += FVector2D::Distance(Plan.Polyline[Index - 1], Plan.Polyline[Index]);
		if (Walked > Travelled + UE_KINDA_SMALL_NUMBER)
		{
			Run.Points.Add(Plan.Polyline[Index]);
		}
	}
	if (Run.Points.Num() >= 2)
	{
		Out.Add(MoveTemp(Run));
	}
}

double FPushbackRun::SecondsFor(const FRoutePlan& InPlan, double InPushSpeed, double InPushAccel)
{
	FPushbackRun Probe;
	if (!Probe.Start(InPlan, InPushSpeed, InPushAccel, /*bInNeedsThrust*/ false))
	{
		return 0.0;
	}
	constexpr double Step = 1.0 / 30.0;
	constexpr int32 MaxSteps = 30 * 1200;
	FVector2D At = FVector2D::ZeroVector;
	double Heading = 0.0;
	double Seconds = 0.0;
	for (int32 Guard = 0; Guard < MaxSteps && Probe.Advance(Step, TNumericLimits<double>::Max(), true, At, Heading); ++Guard)
	{
		Seconds += Step;
	}
	return Seconds;
}

bool FPushbackRun::Advance(double DeltaSeconds, double StopWithin, bool bHasThrust,
	FVector2D& OutPosition, double& OutHeading)
{
	if (HasArrived())
	{
		// AT REST, said here because a push can now be over with speed on it: the trapezoid below ends at exactly zero,
		// but a plan a rebuild killed or cut short behind Travelled ends the push mid-motion, and DescribeMotion reads
		// this as the ground speed of an aeroplane that is standing still.
		// ENFORCED BY: Airside.Model.PushbackOnDeletedGroundStops (at rest)
		Speed = 0.0;
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
		FVector2D Waiting = FVector2D::ZeroVector;
		double Tangent = 0.0;
		if (GuidelineGeom::PointAtDistance(Plan.Polyline, Travelled, Waiting, Tangent))
		{
			OutPosition = Waiting;
		}

		// THE BODY'S heading, never the line's - it is standing on its stand facing the
		// terminal, and the line under it points the other way.
		OutHeading = Heading;
		return true;
	}

	// WHERE IT MAY GET TO THIS FRAME: the end of the route, or wherever arbitration stopped
	// it, whichever is nearer. StopWithin is the ONE input into this motion, exactly as it is
	// for the follower - there is no second evaluator of where an agent may go.
	//
	// RELATIVE, AS THE FOLLOWER READS IT (#502 review): FClaimPass::StopWithinFor answers how much further the agent may go
	// from where it stands, and this read it as a distance along the plan. A refused push stopped where that figure fell in
	// plan distance - half way to its stop line, closing on the half point for minutes (40 m short of the van on
	// PushbackRejoinedOntoAnOccupiedLineWaits) - and one refused further along than the figure would have been clamped
	// BACKWARD to it. A push is granted whole, so no refusal in play had shown it; only PushbackRun's own case 5, which fed a
	// constant StopWithin, agreed with the old reading.
	// ENFORCED BY: Airside.Model.PushbackRun (case 5: a stop line fed as the claim pass feeds it),
	// Airside.Model.PushbackRejoinedOntoAnOccupiedLineWaits
	const double StopAt = FMath::Min(Plan.Length, Travelled + FMath::Max(0.0, StopWithin));

	// TRAPEZOIDAL, AND IT ENDS AT REST. The aeroplane is about to reverse its direction of
	// travel: handing the follower a non-zero speed would have it pull away forwards at the
	// speed it was just being pushed backwards at, on the handover frame.
	const double Remaining = FMath::Max(0.0, StopAt - Travelled);
	const double BrakingDistance = Speed * Speed / (2.0 * PushAccel);
	Speed = BrakingDistance >= Remaining
		? FMath::Max(0.0, Speed - PushAccel * DeltaSeconds)
		: FMath::Min(PushSpeed, Speed + PushAccel * DeltaSeconds);

	const double WasTravelled = Travelled;
	const double Wanted = Travelled + Speed * DeltaSeconds;
	Travelled = FMath::Clamp(Wanted, 0.0, StopAt);

	// HELD AT ITS STOP, IT READS THE MOTION IT MADE (#502 review). The ramp above is asked against a stop line arbitration
	// sets, and held there it alternated zero and one frame's PushAccel - 2 uu/s - while the clamp kept it still: ground
	// speed on an aeroplane at rest, its wheels creeping (PushbackRejoinedOntoAnOccupiedLineWaits). Clamped, the speed is
	// the distance the clamp let it move this frame - none at a stop, a leader's pace behind one that moves on.
	// ENFORCED BY: Airside.Model.PushbackRejoinedOntoAnOccupiedLineWaits (no speed standing at the stop)
	if (Wanted > StopAt && DeltaSeconds > 0.0)
	{
		Speed = FMath::Max(0.0, Travelled - WasTravelled) / DeltaSeconds;
	}

	// AND IT IS ACTUALLY STOPPED WHEN IT STOPS. The ramp above is discrete, so it lands within
	// one frame's PushAccel of zero and HasArrived ends the run on the next frame before the
	// ramp can finish. Half a centimetre per second is nothing to look at, but
	// FRoadAgent::DescribeMotion reads this figure as the agent's ground speed and
	// UAirsideAgentAnim turns the wheels at speed over radius: an aeroplane whose push ended
	// with its wheels still creeping is the same class of defect as the landing that touched
	// down with its wheels perfectly still.
	if (Travelled >= Plan.Length - UE_KINDA_SMALL_NUMBER)
	{
		Speed = 0.0;
	}

	// THE STEERED AXLE IS ON THE LINE AND THE BODY FACES THE OTHER WAY. That is the whole
	// law - see the header for why it needs no phases. The tangent rotates smoothly through
	// the sweep onto the taxiway, so the body swings with it and arrives pointing exactly
	// along the route it is about to taxi.
	FVector2D At = FVector2D::ZeroVector;
	double Tangent = 0.0;
	if (!GuidelineGeom::PointAtDistance(Plan.Polyline, Travelled, At, Tangent))
	{
		// The pose is left exactly as the caller had it rather than written from an unset
		// FVector2D. Still true: declining to MOVE is not the same as handing over.
		return true;
	}

	OutPosition = At;
	Heading = FMath::UnwindRadians(Tangent + UE_DOUBLE_PI);
	OutHeading = Heading;

	// TRUE WHILE IT IS STILL MINE. HasArrived is re-asked at the TOP of the next frame rather
	// than answered here, so the last frame of the push still reports its own motion. A frame
	// with no motion at all is exactly the step the handover-continuity test exists to catch.
	return true;
}
