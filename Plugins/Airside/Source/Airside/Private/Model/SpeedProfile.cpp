#include "Model/SpeedProfile.h"

#include "AirsideLog.h"
#include "Model/RouteFollower.h"
#include "Solve/GuidelineGeom.h"

namespace
{
	/**
	 * Below this, a vertex bend is not a corner - it is the same direction twice.
	 *
	 * Deliberately tiny. It is NOT the sampling-artefact threshold: GuidelineGeom has already
	 * made that judgement, and by the time VertexHeadings answers, a smoothed vertex reports
	 * the SAME heading arriving and leaving, so anything left is a genuine change of
	 * direction. This only keeps floating-point dust from being read as a corner.
	 */
	constexpr double CornerEpsilon = 1.0e-6;
}

void FSpeedProfile::Build(const TArray<FVector2D>& Points, const FAirframe& Airframe)
{
	const FGroundPerformance& Ground = Airframe.Ground;

	Distances.Reset();
	VertexLimits.Reset();
	SpanCaps.Reset();

	Decel = Ground.Taxi.Decel;
	Fallback = Ground.Taxi.SpeedCap;

	if (Points.Num() < 2 || !Ground.IsSet())
	{
		return;
	}

	const int32 Count = Points.Num();
	const double MaxTurnRate = FMath::DegreesToRadians(Ground.MaxTurnRateDegPerSec);

	// The heading the FOLLOWER will be handed at each vertex, arriving and leaving. Equal
	// wherever the vertex is a sampled curve; different only at a real corner.
	TArray<double> Arriving;
	TArray<double> Leaving;
	Arriving.SetNumUninitialized(Count);
	Leaving.SetNumUninitialized(Count);
	for (int32 At = 0; At < Count; ++At)
	{
		GuidelineGeom::VertexHeadings(Points, At, Arriving[At], Leaving[At]);
	}

	Distances.SetNumUninitialized(Count);
	Distances[0] = 0.0;
	for (int32 At = 1; At < Count; ++At)
	{
		Distances[At] = Distances[At - 1] + FVector2D::Distance(Points[At - 1], Points[At]);
	}

	// SPAN CAPS, from curvature. Heading changes by so many radians over so many uu, so the
	// radius is Length/Turn - v = wR written without ever naming a radius, which is what
	// lets it work on a polyline that is not an arc.
	//
	// WHAT LIMITS THE SPEED THERE depends on which law steers this airframe.
	//
	// PIVOT: the yaw rate is all there is, so the cap is MaxTurnRate * R, exactly as before.
	//
	// ROLLING-STEER: the yaw rate does NOT limit it, and that is the whole reason turns were
	// slow. Required yaw is v/R and available yaw is v*sin(lock)/L, so speed cancels and a
	// corner is either followable at every speed or at none: R >= L/sin(lock). For plane2
	// that threshold is 5.2 m, tighter than any taxiway bend. What remains is the physical
	// limit the yaw-rate cap was always standing in for - lateral acceleration, sqrt(a*R),
	// which is tyre side load and the cabin.
	const double TightestFollowable = Airframe.TightestFollowableRadius();

	// WHY THIS ROUTE IS AS SLOW AS IT IS, gathered as the caps are built and logged once at
	// the end - see the UE_LOG below for why it is worth carrying.
	double TightestRadius = TNumericLimits<double>::Max();
	double TightestAt = 0.0;
	double TightestCap = Ground.Taxi.SpeedCap;
	const TCHAR* TightestRule = TEXT("straight");

	SpanCaps.SetNumUninitialized(Count - 1);
	for (int32 Span = 0; Span + 1 < Count; ++Span)
	{
		const double Length = Distances[Span + 1] - Distances[Span];
		const double Turn = FMath::Abs(FMath::UnwindRadians(Arriving[Span + 1] - Leaving[Span]));

		double Cap = Ground.Taxi.SpeedCap;
		if (Length > 0.0 && Turn > CornerEpsilon)
		{
			const double Radius = Length / Turn;
			const TCHAR* Rule = TEXT("?");
			if (Airframe.EffectiveSteerLaw() != ESteerLaw::RollingSteer)
			{
				Cap = FMath::Min(Cap, MaxTurnRate * Radius);
				Rule = TEXT("pivot yaw rate");
			}
			else if (Radius < TightestFollowable)
			{
				// The lock cannot hold this line at any speed. Crawl and wear the crab -
				// the same answer the vertex caps below give an instant corner.
				Cap = Ground.MinSteeringSpeed;
				Rule = TEXT("TIGHTER THAN THE STEERING LOCK");
			}
			else
			{
				Cap = FMath::Min(Cap, FMath::Sqrt(
					FMath::Max(0.0, Ground.MaxLateralAccelUu) * Radius));
				Rule = TEXT("lateral accel");
			}

			if (Radius < TightestRadius)
			{
				TightestRadius = Radius;
				TightestAt = Distances[Span];
				TightestCap = FMath::Max(Cap, Ground.MinSteeringSpeed);
				TightestRule = Rule;
			}
		}

		// Never below the creep speed. A span this tight is one the aircraft has to crab
		// through, and crawling is the slowest an aeroplane may do that at - see
		// FGroundPerformance::MinSteeringSpeed.
		SpanCaps[Span] = FMath::Max(Cap, Ground.MinSteeringSpeed);
	}

	int32 SharpVertices = 0;
	double SharpestAt = 0.0;
	double SharpestDegrees = 0.0;

	// VERTEX CAPS. A vertex whose heading changes instantly cannot be taken at any speed at
	// all, so the answer there is the slowest the aircraft can still steer at, and the crab
	// is worn. The spans either side bound it too - a cap that applied only between vertices
	// could be exceeded exactly at one.
	VertexLimits.SetNumUninitialized(Count);
	for (int32 At = 0; At < Count; ++At)
	{
		double Limit = Ground.Taxi.SpeedCap;

		const double Instant = FMath::Abs(FMath::UnwindRadians(Leaving[At] - Arriving[At]));
		if (Instant > CornerEpsilon)
		{
			// THE GREATER OF THE TWO, never the airframe's figure alone. MinSteeringSpeed is
			// physics and may legitimately be zero - a truck stops with the wheel turned -
			// and zero HERE is not a crawl, it is a permanent stop: the backward pass brakes
			// the agent to rest at this vertex and LimitAt answers zero for ever after. See
			// FRouteFollower::ProgressEpsilon, and Airside.Model.SteeringFloorSharpVertexStillCreeps.
			Limit = FMath::Max(Ground.MinSteeringSpeed, FRouteFollower::ProgressEpsilon);

			// The FIRST one only, and where it is. A route with a sharp vertex crawls
			// through it whatever its curvature says, so this is the other answer the log
			// below has to be able to give.
			if (SharpVertices == 0)
			{
				SharpestAt = Distances[At];
				SharpestDegrees = FMath::RadiansToDegrees(Instant);
			}
			++SharpVertices;
		}

		if (At > 0)          { Limit = FMath::Min(Limit, SpanCaps[At - 1]); }
		if (At + 1 < Count)  { Limit = FMath::Min(Limit, SpanCaps[At]); }

		VertexLimits[At] = Limit;
	}

	// An aircraft arriving at its destination stops. Not floored at MinSteeringSpeed, because
	// that floor is about steering and there is nothing left to steer.
	VertexLimits[Count - 1] = 0.0;

	// THE BACKWARD PASS. Each cap is raised to the fastest the aircraft could be here and
	// still meet the next one by braking - v^2 = u^2 + 2as, rearranged. After this the array
	// is not a list of restrictions but a plan: follow it and no deceleration is ever asked
	// for that the airframe does not have.
	for (int32 At = Count - 2; At >= 0; --At)
	{
		const double Span = Distances[At + 1] - Distances[At];
		const double Reachable = FMath::Sqrt(
			FMath::Square(VertexLimits[At + 1]) + 2.0 * Decel * Span);

		VertexLimits[At] = FMath::Min(VertexLimits[At], Reachable);
	}

	// WHY THIS ROUTE IS AS SLOW AS IT IS, once per plan.
	//
	// Build runs on Start and Replace, so this is one line per route rather than one per
	// tick - cheap enough to leave in, and the only way to answer the question it answers.
	//
	// THREE RULES ALL REPORT MinSteeringSpeed and the inspector panel cannot tell them apart,
	// because it shows the speed and not the reason: a vertex too sharp to sample, a radius
	// tighter than the steering lock can hold, and a curvature whose lateral-accel cap
	// happens to land on the floor. "It crawls round that corner" was about to be diagnosed
	// by reasoning about which of the three it was, which is exactly the guesswork this
	// project's notes say to instrument instead.
	//
	// The lock threshold is printed even when nothing hit it, because knowing a corner was
	// 6 m against a 5.2 m limit is what says whether to widen the taxiway or retune the
	// aircraft - and that is the decision this log exists to inform.
	UE_LOG(LogAirsideTraffic, Log,
		TEXT("Speed profile: %.0f uu, %d point(s). Tightest R=%.0f uu at %.0f -> %.0f uu/s "
		     "(%s). %d sharp vertex/vertices%s. Floor %.0f, taxi cap %.0f, lock allows "
		     "R>=%.0f (wheelbase %.0f, lock %.0f deg)."),
		Distances.Last(), Count,
		TightestRadius == TNumericLimits<double>::Max() ? 0.0 : TightestRadius,
		TightestAt, TightestCap, TightestRule,
		SharpVertices,
		SharpVertices > 0
			? *FString::Printf(TEXT(", first %.0f deg at %.0f"), SharpestDegrees, SharpestAt)
			: TEXT(""),
		Ground.MinSteeringSpeed, Ground.Taxi.SpeedCap, TightestFollowable,
		Airframe.Wheelbase(), Ground.MaxSteerDegrees);
}

double FSpeedProfile::LimitAt(double Distance) const
{
	if (IsEmpty())
	{
		// Nothing was built. Reporting the taxi speed leaves the follower behaving as it did
		// before there was a profile, which is the right way to fail: an agent that crawls
		// for no reason reads as a routing bug, and one that stops reads as a crash.
		return Fallback;
	}

	const double Clamped = FMath::Clamp(Distance, 0.0, Distances.Last());

	// Linear scan from the start, matching GuidelineGeom::PointAtDistance - which the caller
	// has already run this frame over the same array. A binary search here would be a second
	// way of answering "which span am I in", free to disagree with the first at a boundary.
	for (int32 Span = 0; Span + 1 < Distances.Num(); ++Span)
	{
		if (Clamped > Distances[Span + 1])
		{
			continue;
		}

		const double Remaining = Distances[Span + 1] - Clamped;
		const double Braking = FMath::Sqrt(
			FMath::Square(VertexLimits[Span + 1]) + 2.0 * Decel * Remaining);

		return FMath::Min(SpanCaps[Span], Braking);
	}

	return VertexLimits.Last();
}
