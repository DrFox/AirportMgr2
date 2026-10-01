#include "Model/PushbackRun.h"

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

	// ON THE LINE ALREADY - a lead-in shortened along its own axis, a split: the new route is walked from the projection.
	const double Offset = FVector2D::Distance(From, OnLine);
	if (Offset < 1.0)
	{
		Plan = Route;
		Travelled = Along;
		return true;
	}

	// THE JOIN MEETS THE LINE AHEAD, not square across: JoinLead offsets ahead bounds the body's swing at atan(1/4), 14
	// degrees, where a square join would turn it through 90 and back - the heading IS the line's tangent turned about (see
	// the header), so a kink in the line is a kink in the pose. WITHIN THE FIRST STEP, so every step's end is still a
	// vertex of the line it names and only the first step's span grows by the leg, as UGroundTraffic's held taxi out's
	// join grows its first.
	constexpr double JoinLead = 4.0;
	const double JoinTo = FMath::Min(Along + JoinLead * Offset, Route.Steps[0].EndDistance);
	FVector2D JoinAt = FVector2D::ZeroVector;
	if (!GuidelineGeom::PointAtDistance(Route.Polyline, JoinTo, JoinAt, Tangent))
	{
		return false;
	}
	// THE FIRST VERTEX PAST THE JOIN, by the arc length PointAtDistance and every EndDistance were measured with.
	int32 Keep = Route.Polyline.Num();
	double Walked = 0.0;
	for (int32 Index = 1; Index < Route.Polyline.Num(); ++Index)
	{
		Walked += FVector2D::Distance(Route.Polyline[Index - 1], Route.Polyline[Index]);
		if (Walked > JoinTo + UE_KINDA_SMALL_NUMBER)
		{
			Keep = Index;
			break;
		}
	}

	const double Leg = FVector2D::Distance(From, JoinAt);
	FRoutePlan Joined = Route;
	Joined.Polyline.Reset();
	Joined.Polyline.Add(From);
	Joined.Polyline.Add(JoinAt);
	for (int32 Index = Keep; Index < Route.Polyline.Num(); ++Index)
	{
		Joined.Polyline.Add(Route.Polyline[Index]);
	}
	for (FRouteStep& Step : Joined.Steps)
	{
		// The first step may end AT the join (JoinTo clamped to it): its end is then the join's own vertex.
		const bool bEndsAtJoin = Step.EndDistance <= JoinTo + UE_KINDA_SMALL_NUMBER;
		Step.EndVertex = bEndsAtJoin ? 1 : Step.EndVertex - Keep + 2;
		Step.EndDistance = bEndsAtJoin ? Leg : Leg + (Step.EndDistance - JoinTo);
	}
	Joined.Length = GuidelineGeom::PolylineLength(Joined.Polyline);
	Plan = Joined;
	Travelled = 0.0;
	return true;
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
	const double StopAt = FMath::Min(Plan.Length, FMath::Max(0.0, StopWithin));

	// TRAPEZOIDAL, AND IT ENDS AT REST. The aeroplane is about to reverse its direction of
	// travel: handing the follower a non-zero speed would have it pull away forwards at the
	// speed it was just being pushed backwards at, on the handover frame.
	const double Remaining = FMath::Max(0.0, StopAt - Travelled);
	const double BrakingDistance = Speed * Speed / (2.0 * PushAccel);
	Speed = BrakingDistance >= Remaining
		? FMath::Max(0.0, Speed - PushAccel * DeltaSeconds)
		: FMath::Min(PushSpeed, Speed + PushAccel * DeltaSeconds);

	Travelled = FMath::Clamp(Travelled + Speed * DeltaSeconds, 0.0, StopAt);

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
