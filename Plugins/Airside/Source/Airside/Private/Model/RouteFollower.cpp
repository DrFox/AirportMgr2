#include "Model/RouteFollower.h"

#include "AirsideLog.h"

#include "Solve/GuidelineGeom.h"
#include "Solve/RoadGeom.h"

void FRouteFollower::Start(const FRoutePlan& InPlan, const FChassis& InChassis, double InitialSpeed,
	TOptional<double> InitialHeading, double InitialTravelled)
{
	Plan = InPlan;
	// Normally the polyline's first point. A handover from the rollout arrives a frame's
	// worth PAST the plan's start - the landing run stops the tick after it crosses
	// VacateAt, not on it - and restarting from zero threw that overshoot away: measured
	// as 1.9 uu of motion on a frame that should have carried 16.7, an 890 uu/s step
	// (2026-09-06). The overshoot is handed in and the taxi begins that far along the arc.
	Travelled = FMath::Max(InitialTravelled, 0.0);
	// Ground is NOT copied here any more (issue #83) - Advance and Replace take the airframe
	// fresh from their caller every time instead.

	// RESET WITH THE POLYLINE (issue #190): CursorVertex/CursorWalked checkpoint a walk over
	// Plan.Polyline specifically, and Plan was just replaced above - a stale checkpoint would
	// name a vertex on the PREVIOUS route, on the same or a different polyline where it means
	// nothing. PointAtDistance's own Distance<Walked guard would catch a walk backwards, but
	// InitialTravelled can be seeded FORWARD too (a rollout handover, see the comment below),
	// which the guard cannot see is wrong on a polyline that changed under it.
	CursorVertex = 1;
	CursorWalked = 0.0;

	// RECOMPUTED WITH THE PLAN, for the same reason: ReverseLegSteps describes THIS
	// Plan.Steps array, and a Start always means a new one - see RebuildReverseLegSteps.
	RebuildReverseLegSteps();

	// LOGGED ONCE, HERE - not from EffectiveSteerLaw, which Advance below calls twice every
	// frame this follower runs. #176: that used to log inline and turned one mis-authored
	// airframe into ~3800 lines/s at 60 fps across a busy apron. Start runs once per dispatch,
	// which is the granularity the warning actually wants.
	WarnIfSteerLawUnsupported(InChassis);

	// WHICH AXLE TAKES THE LAST TURN, decided once per plan - see ArmFinalTurn. A dispatch
	// is never mid-handover, so nothing from a previous route is kept.
	FinalTurnHandoverOffset = FVector2D::ZeroVector;
	FinalTurnHandoverHeading = 0.0;
	ArmFinalTurn(InChassis, false);

	// The whole route costed before the first frame. See FSpeedProfile: once braking is
	// limited, a corner discovered by arriving at it is already twenty-five metres too late.
	{
		// PER SPAN, because a route may contain a bay's reverse leg and judging that by the
		// forward limit refuses a manoeuvre that is legal - see FSpeedProfile's overload. The
		// follower does not DRIVE those spans (FRoadAgent hands them to FReverseRun) but it
		// profiles the plan it was given, and a profile that lies about part of it is read by
		// everything downstream, including the warning a human acts on.
		TArray<EDriveDirection> Spans;
		Plan.DescribeSpanDirections(Spans);
		Profile.Build(Plan.Polyline, InChassis, Spans);
	}

	// FROM REST BY DEFAULT. An aeroplane on a stand is stopped, and snapping to taxi speed
	// on the first frame is the same defect as the corner this class was just taught about
	// - an acceleration no airframe has - only at the one moment the player is certain to
	// be looking, because they just dispatched it.
	//
	// FROM THE CALLER'S SPEED WHEN IT HAS ONE. The Vacated handover used to start the taxi
	// from rest while the rollout had just handed over at taxi speed, so the aircraft
	// stopped dead at the exit for a frame and pulled away again (2026-09-06). Clamped to
	// what the route permits at its first vertex, so a handover can never begin above the
	// speed the profile would have braked to - the profile is the authority on speed,
	// this is only where the number starts.
	Speed = FMath::Clamp(InitialSpeed, 0.0, Profile.LimitAt(0.0));

	// Seeded from the line, not left at zero. An agent that starts facing due east and
	// slews to its actual heading pirouettes on the stand the instant it is dispatched -
	// which reads as a routing bug rather than as an uninitialised field.
	//
	// OR FROM THE CALLER'S HEADING when it has one: a handover from the rollout arrives
	// pointing down the runway, and the arc's first sampled span is a degree or two off
	// that. Seeding from the line would turn the nose by that much in one frame; seeding
	// from the aircraft lets the slew below close the gap at the airframe's rate, which
	// is what heading-as-state exists to do.
	FVector2D Unused;
	if (InitialHeading.IsSet())
	{
		Heading = InitialHeading.GetValue();
	}
	else if (!GuidelineGeom::PointAtDistance(Plan.Polyline, 0.0, Unused, Heading))
	{
		Heading = 0.0;
	}
}

bool FRouteFollower::Advance(double DeltaSeconds, const FChassis& InChassis, double StopWithin,
	FVector2D& OutPosition, double& OutHeading)
{
	if (!Plan.IsDrivable())
	{
		return false;
	}

	const FGroundPerformance& Ground = InChassis.Ground;

	// The stop point in route distance, fixed BEFORE the move: StopWithin was measured from
	// where the agent was when the arbiter looked, and re-measuring it after moving would
	// let the agent creep past it one frame at a time.
	//
	// NOT CONVERTED for the steered axle, and that is worth stating because the obvious
	// thing to do here is wrong. Travelled measures the STEERED axle while the claim pass
	// measures its windows from the BODY CENTRE (FClaimPass::CentreOf), so it looks as
	// though the two frames must be reconciled. They reconcile themselves: StopWithinFor
	// returns a distance RELATIVE to the agent's own route position, this adds it to that
	// same agent's Travelled, and the offset between the two points appears on both sides
	// and cancels. Adding SteerAxleX here double-counts it - it parked a deviating airframe
	// a wheelbase short, which Airside.Model.AuthoredStopPointsDoNotMove caught.
	const double StopAt = FMath::Min(Plan.Length, Travelled + FMath::Max(0.0, StopWithin));

	// Kept for the fixed-axle law, which reads the steering off how far the mains rolled.
	const double WasTravelled = Travelled;

	// Clamped rather than allowed to run on, so a long frame - a hitch, or a breakpoint -
	// leaves the agent at its destination instead of somewhere past the end of the world.
	//
	// Speed is LAST frame's, decided at the bottom of this function. One frame of lag, 16 ms
	// at the rate this is watched at, and it buys the whole loop a single PointAtDistance
	// call: reading the line, deciding a speed and then moving would need two, one before
	// the move and one after, on every agent on the airport.
	Travelled = FMath::Clamp(Travelled + Speed * DeltaSeconds, 0.0, StopAt);

	// Where the LINE points here. Not where the aircraft points - those are now two
	// different things, and that gap is the whole of this function.
	//
	// HINTED (issue #190): Travelled only grows between Start/Replace calls (see
	// CursorVertex's own comment), so this walks forward from where the LAST substep left
	// off instead of from vertex 0 - the whole polyline was being re-walked once per agent
	// per substep for a distance that had barely moved.
	double LineHeading = 0.0;
	if (!GuidelineGeom::PointAtDistance(Plan.Polyline, Travelled, OutPosition, LineHeading,
		CursorVertex, CursorWalked))
	{
		return false;
	}

	// WHICH AXLE HOLDS THE LINE, and so which law turns the body this frame - see
	// EFinalTurnAxle. The nose law is every frame of every route but the last turn of an
	// aircraft's, and was the whole of this block until 2026-09-27; it moved into
	// SteerNoseOnLine unchanged, with its reasoning, so the two laws read side by side.
	const bool bMainsHold = FinalTurnFrom >= 0.0 && Travelled >= FinalTurnFrom
		&& InChassis.EffectiveSteerLaw() == ESteerLaw::RollingSteer;
	if (bMainsHold && WasTravelled < FinalTurnFrom)
	{
		// THE HANDOVER FRAME: what the nose law left, measured against what the fixed-axle law
		// wants, so HoldFinalTurn can decay the difference out - see FinalTurnHandoverOffset.
		//
		// MEASURED AT LAST FRAME'S POSE, WasTravelled, not at this frame's Travelled: that is
		// the last pose the nose law actually produced (the nose ON the line, at the Heading it
		// left), and HoldFinalTurn's decay is keyed to FinalTurnFrom, so the offset is carried
		// forward to that point by the same e^(-s/L). Measured at Travelled instead, the first
		// frame decayed only the part of the frame past FinalTurnFrom and the nosewheel dipped
		// 1.8 degrees for a frame (Airside.Model.FinalTurnHandoverIsSmooth).
		const double L = InChassis.Wheelbase();
		FVector2D NoseWas = FVector2D::ZeroVector;
		FVector2D MainsWanted = FVector2D::ZeroVector;
		double Unused = 0.0;
		GuidelineGeom::PointAtDistance(Plan.Polyline, WasTravelled, NoseWas, Unused);
		GuidelineGeom::PointAtDistance(Plan.Polyline, FMath::Max(0.0, WasTravelled - L), MainsWanted, Unused);
		const double CarriedTo = FMath::Exp(-(FinalTurnFrom - WasTravelled) / L);
		FinalTurnHandoverOffset = (RoadGeom::TrailPoint(NoseWas, Heading, -L) - MainsWanted) * CarriedTo;
		FinalTurnHandoverHeading = FMath::UnwindRadians(
			Heading - FinalTurnHeadingAt(WasTravelled - L, L * FinalTurnHalfWindowOfWheelbase)) * CarriedTo;
		UE_LOG(LogAirsideTraffic, Log,
			TEXT("Final turn: main gear takes the line at %.0f uu, %.2f deg and %.1f uu off it at the handover."),
			Travelled, FMath::RadiansToDegrees(FinalTurnHandoverHeading), FinalTurnHandoverOffset.Size());
	}
	const double Crab = bMainsHold
		? HoldFinalTurn(DeltaSeconds, InChassis, WasTravelled, OutPosition)
		: SteerNoseOnLine(DeltaSeconds, InChassis, LineHeading);

	// TWO THINGS DECIDE THE TARGET SPEED, and they have different jobs.
	//
	// The PROFILE is the plan: it knows what is coming and is the only reason the aircraft
	// is ever slow BEFORE a corner rather than after it. It is also the only one that can
	// bring the aircraft to a stop, at the destination.
	//
	// The CRAB TERM is the feedback: it reacts to the line the aircraft is actually being
	// given. It should almost never bind - if the profile has done its job the crab stays
	// near zero on anything but a genuine corner - but it is what makes "the nose stays
	// within CrabAtMinSpeedDegrees of the line" a property of this loop rather than a
	// prediction that happens to come true. A plan alone would have nothing to notice with.
	//
	// It floors at MinSteeringSpeed and the profile does not, which is what lets the aircraft
	// creep through a turn but still stop when it has arrived.
	const double Slowing = 1.0 - FMath::Clamp(Crab / CrabAtMinSpeedDegrees, 0.0, 1.0);
	// THREE-WAY, not two. MinSteeringSpeed is the airframe's physics and may legitimately be
	// zero - a truck can stop with the wheel turned - so the solver's own epsilon has to sit
	// beside it or this loop has nothing to climb out of. See FRouteFollower::ProgressEpsilon.
	const double CrabLimit = FMath::Max3(
		Ground.MinSteeringSpeed, ProgressEpsilon, Ground.Taxi.SpeedCap * Slowing);

	// The THIRD cap: what the stop point permits. sqrt(2 a s), the braking curve, so the
	// agent arrives at the stop at rest having braked at the rate it actually has - the
	// profile already does this for corners and the destination; this does it for whatever
	// the arbiter put in the way this tick. Zero distance is zero speed, which is a stop.
	const double StopCap = FMath::Sqrt(2.0 * Ground.Taxi.Decel * FMath::Max(0.0, StopAt - Travelled));

	const double Target = FMath::Min3(Profile.LimitAt(Travelled), CrabLimit, StopCap);

	// AND THE TARGET IS APPROACHED, NOT TAKEN. Speed used to be assigned outright, so
	// meeting a corner cost 920 uu/s in a single frame - 552 m/s2, fifty-six g. Thrust and
	// brakes are separate figures because they are not equal: wheel brakes beat a propeller.
	Speed = Target > Speed
		? FMath::Min(Target, Speed + Ground.Taxi.Accel * DeltaSeconds)
		: FMath::Max(Target, Speed - Ground.Taxi.Decel * DeltaSeconds);

	// WHAT THE CALLER GETS IS THE ORIGIN, unchanged in meaning from before this law existed:
	// ARoadAgentActor::SetPose puts it on the ground, stands compose against it, and claims
	// derive the body centre from it. OutPosition is the STEERED AXLE under both laws - on
	// the line under the nose law, outside it under HoldFinalTurn - and on every conforming
	// airframe the axle and the origin are the same point: TrailPoint then returns
	// OutPosition untouched rather than nudging it by a rounding error.
	OutPosition = RoadGeom::TrailPoint(OutPosition, Heading, -InChassis.SteerAxleX);
	OutHeading = Heading;
	return true;
}

double FRouteFollower::SteerNoseOnLine(double DeltaSeconds, const FChassis& InChassis, double LineHeading)
{
	const FGroundPerformance& Ground = InChassis.Ground;

	const double Error = FMath::UnwindRadians(LineHeading - Heading);

	// HOW FAR THE NOSE MAY COME ROUND THIS FRAME, and there are two laws because there are
	// two kinds of vehicle on an airport - see FChassis::HasAxles.
	//
	// ROLLING-STEER is the kinematic bicycle model, and the change is smaller than it
	// sounds because the steering angle was already being computed here: Error IS the angle
	// between the body axis and the direction the steered axle is being asked to travel.
	// Only the limit and what it yields are new.
	//
	//     d    = clamp(Error, +/- lock)
	//     Step = v * sin(d) / L * dt
	//
	// sin rather than tan because Speed is the STEERED axle's speed along the line - that
	// is what Travelled measures. The rear-axle form of the same model would overstate the
	// yaw by 1/cos(d): invisible at small angles, double at full lock.
	//
	// No lookahead and no gain to tune, which is why this is not a pursuit controller with
	// the usual oscillation: the steered axle is CONSTRAINED to the line rather than
	// chasing it, so there is no lateral error to feed back on.
	//
	// PIVOT keeps the flat rate, permanently rather than pending measurement. A van is
	// authored at 90 deg/s with the note "a van CAN pivot"; at its 0.5 m/s creep that needs
	// 83 degrees of lock, so it is not steering at all and a geometric law would cripple it.
	const double Lock = FMath::DegreesToRadians(FMath::Max(0.0, Ground.MaxSteerDegrees));

	double MaxStep = 0.0;
	if (InChassis.EffectiveSteerLaw() == ESteerLaw::RollingSteer)
	{
		const double Steer = FMath::Clamp(Error, -Lock, Lock);
		SteerDegrees = FMath::RadiansToDegrees(Steer);
		MaxStep = FMath::Abs(Speed * FMath::Sin(Steer) / InChassis.Wheelbase()) * DeltaSeconds;
	}
	else
	{
		// No steered wheel, so the view must not draw one turning.
		SteerDegrees = 0.0;
		MaxStep = FMath::DegreesToRadians(Ground.MaxTurnRateDegPerSec) * DeltaSeconds;
	}

	// Unwound first, so a turn across the +/-PI seam is taken the short way round rather
	// than very nearly all the way about. Step kept apart from the slew itself (RoadGeom::
	// SlewAngle) because Crab below needs the exact clamped step, not just the new heading.
	const double Step = FMath::Clamp(Error, -MaxStep, MaxStep);
	Heading = RoadGeom::SlewAngle(Heading, LineHeading, MaxStep);
	YawRateDegPerSec = DeltaSeconds > 0.0
		? FMath::RadiansToDegrees(Step) / DeltaSeconds
		: 0.0;

	// WHAT COULD NOT BE TAKEN OUT, which is the crab the player is now looking at - and the
	// two laws disagree about what that means, which is the subtlety that cost a test run.
	//
	// PIVOT: what is left after this frame's slew. Measured after the slew rather than
	// before it - an airframe that CAN make the turn has no reason to slow for it, and
	// taking the pre-slew error would have shaved a few percent off every agent on every
	// gentle bend for nothing.
	//
	// ROLLING-STEER: what is left after FULL LOCK, not after one frame's yaw. A steady
	// heading error IS steering under this law - a body following a radius R settles at
	// asin(L/R), 8.7 degrees on an ordinary taxiway bend - so measuring the crab the pivot
	// way reads correct steering as a failure to keep up and crawls through every corner at
	// a seventh of the speed. What genuinely cannot be tracked is only the error the lock
	// itself cannot absorb, and on a corner too tight for the lock that is exactly what
	// grows - so the crab term still does its job where it should.
	return InChassis.EffectiveSteerLaw() == ESteerLaw::RollingSteer
		? FMath::RadiansToDegrees(FMath::Max(0.0, FMath::Abs(Error) - Lock))
		: FMath::RadiansToDegrees(FMath::Abs(Error - Step));
}

double FRouteFollower::HoldFinalTurn(double DeltaSeconds, const FChassis& InChassis, double WasTravelled,
	FVector2D& OutSteerAxle)
{
	// THE MAINS ON THE LINE, THE NOSE WHERE THAT PUTS IT. Closed form rather than integrated:
	// a fixed axle that rolls along a path without slipping points along the path, so the
	// heading is the line's own direction at the mains and there is no state to drift. See
	// EFinalTurnAxle for why the last turn is flown this way.
	const double L = InChassis.Wheelbase();
	const double MainsAt = Travelled - L;
	const double LineHeadingAtMains = FinalTurnHeadingAt(MainsAt, L * FinalTurnHalfWindowOfWheelbase);

	// UNHINTED, unlike Advance's own walk: CursorVertex checkpoints TRAVELLED's walk, and the
	// mains are a wheelbase behind it, so the hint would have to walk backwards. Paid only
	// through the last turn, on a polyline of ~50-100 points (2026-09-27).
	FVector2D MainsOnLine = FVector2D::ZeroVector;
	double Unused = 0.0;
	GuidelineGeom::PointAtDistance(Plan.Polyline, FMath::Max(0.0, MainsAt), MainsOnLine, Unused);

	// THE HANDOVER, decayed out over a wheelbase - see FinalTurnHandoverOffset. Measured from
	// FinalTurnFrom, not re-measured per frame, so the decay is a function of distance alone.
	const double Decay = FMath::Exp(-FMath::Max(0.0, Travelled - FinalTurnFrom) / L);
	const FVector2D Mains = MainsOnLine + FinalTurnHandoverOffset * Decay;
	const double NewHeading = LineHeadingAtMains + FinalTurnHandoverHeading * Decay;

	// THE STEERING THAT TURNED IT, read back off the motion: a fixed axle that yaws dh over ds
	// of rolling is being steered at atan(L dh/ds). Held over a frame that rolled nothing, so a
	// parked aircraft keeps its last wheel angle rather than a 0/0.
	const double Turned = FMath::UnwindRadians(NewHeading - Heading);
	const double Rolled = Travelled - WasTravelled;
	if (Rolled > UE_KINDA_SMALL_NUMBER)
	{
		SteerDegrees = FMath::RadiansToDegrees(FMath::Atan(L * Turned / Rolled));
	}
	YawRateDegPerSec = DeltaSeconds > 0.0 ? FMath::RadiansToDegrees(Turned) / DeltaSeconds : 0.0;
	Heading = NewHeading;

	OutSteerAxle = RoadGeom::TrailPoint(Mains, Heading, L);

	// THE CRAB, in the nose law's own terms: only what the lock cannot absorb. ArmFinalTurn
	// refuses a turn that needs more, so this is zero on every turn it arms - it is kept so the
	// speed loop below still has its feedback if that check and this law ever disagree.
	// ENFORCED BY: Airside.Model.FinalTurnParksSquare (steering stays inside the lock)
	return FMath::Max(0.0, FMath::Abs(SteerDegrees) - FMath::Max(0.0, InChassis.Ground.MaxSteerDegrees));
}

double FRouteFollower::FinalTurnHeadingAt(double MainsAt, double HalfWindow) const
{
	const TArray<FVector2D>& Points = Plan.Polyline;
	if (HalfWindow <= UE_KINDA_SMALL_NUMBER)
	{
		FVector2D Unused = FVector2D::ZeroVector;
		double Tangent = 0.0;
		GuidelineGeom::PointAtDistance(Points, FMath::Clamp(MainsAt, 0.0, Plan.Length), Unused, Tangent);
		return Tangent;
	}

	// EXACT PER SPAN, not sampled: each span has one direction, so its share of the average is
	// that direction times the triangle's area over the span - which has a closed form. A
	// sampled average would bring back, in smaller steps, the vertex-crossing jumps this
	// function exists to remove. Beyond either end of the line the end span's direction
	// carries on, as a straight would.
	const double Lo = MainsAt - HalfWindow;
	const double Hi = MainsAt + HalfWindow;
	const double TwoH2 = 2.0 * HalfWindow * HalfWindow;
	const auto Weight = [&](double X)
	{
		X = FMath::Clamp(X, Lo, Hi);
		return X <= MainsAt ? FMath::Square(X - Lo) / TwoH2 : 1.0 - FMath::Square(Hi - X) / TwoH2;
	};

	// Unwound against the first direction met, so a window across the +/-PI seam averages
	// the short way round.
	TOptional<double> Reference;
	double Sum = 0.0;
	double Walked = 0.0;
	const int32 Spans = Points.Num() - 1;
	for (int32 Span = 0; Span < Spans && Walked <= Hi; ++Span)
	{
		const FVector2D Along = Points[Span + 1] - Points[Span];
		const double Length = Along.Size();
		const double A = Span == 0 ? Lo - 1.0 : Walked;
		const double B = Span == Spans - 1 ? Hi + 1.0 : Walked + Length;
		Walked += Length;
		if (Length <= UE_KINDA_SMALL_NUMBER)
		{
			continue;
		}
		const double Share = Weight(B) - Weight(A);
		if (Share <= 0.0)
		{
			continue;
		}
		const double Direction = FMath::Atan2(Along.Y, Along.X);
		if (!Reference.IsSet())
		{
			Reference = Direction;
		}
		Sum += Share * FMath::UnwindRadians(Direction - Reference.GetValue());
	}
	return Reference.Get(0.0) + Sum;
}

void FRouteFollower::ArmFinalTurn(const FChassis& InChassis, bool bKeepIfDriving)
{
	const double Was = FinalTurnFrom;
	const bool bWasDriving = bKeepIfDriving && Was >= 0.0 && Travelled >= Was;
	FinalTurnFrom = -1.0;

	// SILENT FOR STEERED: it is the default, and a line per dispatch saying so would bury the
	// aircraft's.
	if (InChassis.FinalTurnAxle != EFinalTurnAxle::Fixed)
	{
		return;
	}

	const auto Refuse = [&](const FString& Why)
	{
		UE_LOG(LogAirsideTraffic, Log, TEXT("Final turn: nose gear holds the line to the stop - %s."), *Why);
	};

	if (InChassis.EffectiveSteerLaw() != ESteerLaw::RollingSteer)
	{
		Refuse(TEXT("no wheelbase to steer about"));
		return;
	}

	const TArray<FVector2D>& Points = Plan.Polyline;
	const int32 Spans = Points.Num() - 1;
	if (!Plan.IsDrivable() || Spans < 2)
	{
		return;
	}

	const double L = InChassis.Wheelbase();
	const double HalfWindow = L * FinalTurnHalfWindowOfWheelbase;

	// A STRAIGHT IS A RUN WHOSE SPANS STAY WITHIN HALF A DEGREE OF ONE ANOTHER. Cumulative
	// against one reference span, not span to span: a gentle curve sampled finely turns less
	// than that at every vertex and is still a curve.
	constexpr double StraightTolerance = UE_DOUBLE_PI / 360.0;

	TArray<double> SpanHeading;
	TArray<double> VertexDistance;
	SpanHeading.SetNum(Spans);
	VertexDistance.SetNum(Spans + 1);
	VertexDistance[0] = 0.0;
	for (int32 Span = 0; Span < Spans; ++Span)
	{
		const FVector2D Along = Points[Span + 1] - Points[Span];
		SpanHeading[Span] = Along.SizeSquared() > UE_KINDA_SMALL_NUMBER || Span == 0
			? FMath::Atan2(Along.Y, Along.X) : SpanHeading[Span - 1];
		VertexDistance[Span + 1] = VertexDistance[Span] + Along.Size();
	}
	const auto Within = [&](int32 Span, double Reference)
	{
		return FMath::Abs(FMath::UnwindRadians(SpanHeading[Span] - Reference)) <= StraightTolerance;
	};

	// THE FINAL STRAIGHT: back from the stop while the spans hold the last one's heading.
	int32 FinalFrom = Spans - 1;
	while (FinalFrom > 0 && Within(FinalFrom - 1, SpanHeading[Spans - 1]))
	{
		--FinalFrom;
	}
	if (FinalFrom == 0)
	{
		return;   // Straight in: nothing to square, and the nose law is already exact.
	}

	// LONG ENOUGH TO FINISH ON. The mains stop a wheelbase behind the nose, and the heading
	// is averaged half a window beyond them; both must be on the straight or the aircraft stops with
	// the curve still in its heading - and with its nose off the line, short of the mark.
	const double FinalStraight = Plan.Length - VertexDistance[FinalFrom];
	const double Need = L + HalfWindow;
	if (FinalStraight < Need)
	{
		Refuse(FString::Printf(TEXT("final straight %.0f uu is shorter than wheelbase %.0f plus half-window %.0f"),
			FinalStraight, L, HalfWindow));
		return;
	}

	// THE TURN'S START: the latest vertex with a straight of Need behind it. The mains hand
	// over while still on that straight, so the handover is continuous and the only thing the
	// blend has to absorb is what an EARLIER turn left in the heading. A scan per candidate,
	// quadratic in the worst case, on ~50-100 points once per dispatch (2026-09-27).
	int32 Enter = INDEX_NONE;
	for (int32 Vertex = FinalFrom - 1; Vertex >= 1 && Enter == INDEX_NONE; --Vertex)
	{
		const double Reference = SpanHeading[Vertex - 1];
		int32 Back = Vertex - 1;
		while (Back > 0 && Within(Back - 1, Reference))
		{
			--Back;
		}
		if (VertexDistance[Vertex] - VertexDistance[Back] >= Need)
		{
			Enter = Vertex;
		}
	}
	if (Enter == INDEX_NONE)
	{
		Refuse(FString::Printf(TEXT("no straight of %.0f uu before the last turn to hand over on"), Need));
		return;
	}

	// FORWARD ONLY: a reverse leg is FReverseRun's to drive, never this follower's, and a
	// turn backed round is not a turn onto a stand.
	TArray<EDriveDirection> Directions;
	Plan.DescribeSpanDirections(Directions);
	for (int32 Span = Enter - 1; Span < Directions.Num(); ++Span)
	{
		if (Directions[Span] != EDriveDirection::Forward)
		{
			Refuse(TEXT("the last turn is part of a reverse leg"));
			return;
		}
	}

	// WITHIN THE LOCK, or the law would crab - measured on the heading it will actually fly,
	// easing and all, every 25 uu of the mains' travel from the handover to the stop.
	constexpr double SampleStep = 25.0;
	const double From = VertexDistance[Enter];
	double PeakSteer = 0.0;
	double Previous = FinalTurnHeadingAt(From - L, HalfWindow);
	for (double MainsAt = From - L + SampleStep; MainsAt <= Plan.Length - L; MainsAt += SampleStep)
	{
		const double Here = FinalTurnHeadingAt(MainsAt, HalfWindow);
		PeakSteer = FMath::Max(PeakSteer, FMath::RadiansToDegrees(
			FMath::Atan(L * FMath::Abs(FMath::UnwindRadians(Here - Previous)) / SampleStep)));
		Previous = Here;
	}
	if (PeakSteer > InChassis.Ground.MaxSteerDegrees)
	{
		Refuse(FString::Printf(TEXT("the mains would need %.1f deg of steering against %.0f of lock"),
			PeakSteer, InChassis.Ground.MaxSteerDegrees));
		return;
	}

	// ALREADY IN THE TURN. A replan that keeps the same turn keeps its law; anything else past
	// the handover point stays on the nose, since starting the fixed-axle law mid-turn is the
	// jump the blend exists to avoid, only larger.
	if (Travelled > From && !(bWasDriving && FMath::IsNearlyEqual(Was, From, 1.0)))
	{
		if (bWasDriving)
		{
			UE_LOG(LogAirsideTraffic, Warning,
				TEXT("Final turn: a replan moved the last turn from %.0f to %.0f uu while the mains held it; the nose takes the line back."),
				Was, From);
		}
		else
		{
			Refuse(FString::Printf(TEXT("already %.0f uu past the handover at %.0f"), Travelled - From, From));
		}
		return;
	}

	FinalTurnFrom = From;
	if (!bWasDriving)
	{
		UE_LOG(LogAirsideTraffic, Log,
			TEXT("Final turn: main gear holds the line from %.0f uu of %.0f - %.0f deg turn, final straight %.0f, wheelbase %.0f, peak steer %.1f of %.0f deg lock."),
			From, Plan.Length,
			FMath::RadiansToDegrees(FMath::Abs(FMath::UnwindRadians(SpanHeading[Spans - 1] - SpanHeading[Enter - 1]))),
			FinalStraight, L, PeakSteer, InChassis.Ground.MaxSteerDegrees);
	}
}

void FRouteFollower::Replace(const FRoutePlan& NewPlan, const FChassis& InChassis)
{
	// See Start's own comment: a replan re-binds the airframe just as a dispatch does, so it
	// gets the same once-per-call warning rather than none at all.
	WarnIfSteerLawUnsupported(InChassis);

	Plan = NewPlan;
	Travelled = FMath::Clamp(Travelled, 0.0, Plan.Length);

	// RESET WITH THE POLYLINE - see Start's own comment. A splice keeps the polyline's
	// PREFIX unchanged up to the splice point, but the array is still a new one (NewPlan is
	// a distinct FRoutePlan), so a checkpoint into the old Plan.Polyline's storage would be
	// undefined the moment this assignment runs, not merely stale.
	CursorVertex = 1;
	CursorWalked = 0.0;

	// RECOMPUTED WITH THE PLAN, for the same reason - see RebuildReverseLegSteps. The
	// cursor starts at 0 again too: NextReverseLegRun's own "behind us" check will walk it
	// forward past whatever the splice already drove, exactly as the per-tick scan this
	// replaces would re-discover on its very next call.
	RebuildReverseLegSteps();

	// RE-ARMED WITH THE PLAN, keeping the fixed-axle law if it is already driving the same
	// turn - a splice ahead of the aircraft must not drop its nose back onto the line.
	ArmFinalTurn(InChassis, true);
	{
		// PER SPAN, because a route may contain a bay's reverse leg and judging that by the
		// forward limit refuses a manoeuvre that is legal - see FSpeedProfile's overload. The
		// follower does not DRIVE those spans (FRoadAgent hands them to FReverseRun) but it
		// profiles the plan it was given, and a profile that lies about part of it is read by
		// everything downstream, including the warning a human acts on.
		TArray<EDriveDirection> Spans;
		Plan.DescribeSpanDirections(Spans);
		Profile.Build(Plan.Polyline, InChassis, Spans);
	}
}

void FRouteFollower::RebuildReverseLegSteps()
{
	// EVERY bReverseLeg STEP, not just run starts: a run whose first step failed to arm
	// (see NextReverseLegRun's own comment) must still be found from its second step once
	// the first falls behind, which only works if the second step is itself a candidate in
	// this array - a "run starts only" list would drop it.
	ReverseLegSteps.Reset();
	for (int32 Step = 0; Step < Plan.Steps.Num(); ++Step)
	{
		if (Plan.Steps[Step].bReverseLeg)
		{
			ReverseLegSteps.Add(Step);
		}
	}
	ReverseLegCursor = 0;
}

bool FRouteFollower::NextReverseLegRun(int32& OutFrom, int32& OutTo)
{
	// MIRRORS THE PER-STEP SCAN THIS REPLACES EXACTLY (issue #190): that scan walked every
	// step of Plan.Steps looking for the first one that was bReverseLeg AND not yet behind
	// Travelled, then extended forward through the contiguous run. Only the population of
	// candidates changed - precomputed once instead of re-tested every tick - and the
	// "behind us" rule is applied one candidate at a time, in the same order, so a plan
	// whose first reverse-leg step could not be armed still offers its second exactly when
	// the original rescan would have.
	while (ReverseLegSteps.IsValidIndex(ReverseLegCursor))
	{
		const int32 Step = ReverseLegSteps[ReverseLegCursor];
		const double SpanStart = Step == 0 ? 0.0 : Plan.Steps[Step - 1].EndDistance;
		if (SpanStart + UE_DOUBLE_KINDA_SMALL_NUMBER < Travelled)
		{
			// Behind us, PERMANENTLY: Travelled only grows between Start/Replace calls,
			// which are the only two places that touch this array or the cursor.
			++ReverseLegCursor;
			continue;
		}

		OutFrom = Step;
		OutTo = Step;
		while (Plan.Steps.IsValidIndex(OutTo + 1) && Plan.Steps[OutTo + 1].bReverseLeg)
		{
			++OutTo;
		}
		return true;
	}
	return false;
}

bool FRouteFollower::HasArrived() const
{
	if (!Plan.IsDrivable())
	{
		// An agent that cannot move has, for every purpose the caller has, finished. The
		// alternative is a cube that never despawns because it never got a route.
		return true;
	}

	return Travelled >= Plan.Length;
}
