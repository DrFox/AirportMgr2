#include "Model/SpeedProfile.h"

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

void FSpeedProfile::Build(const TArray<FVector2D>& Points, const FGroundPerformance& Ground)
{
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

	// SPAN CAPS, from curvature. Heading changes by so many radians over so many uu, and the
	// aircraft can only supply MaxTurnRate radians a second, so v is capped at the rate
	// divided by that. This is just v = wR written without ever naming a radius, which is
	// what lets it work on a polyline that is not an arc.
	SpanCaps.SetNumUninitialized(Count - 1);
	for (int32 Span = 0; Span + 1 < Count; ++Span)
	{
		const double Length = Distances[Span + 1] - Distances[Span];
		const double Turn = FMath::Abs(FMath::UnwindRadians(Arriving[Span + 1] - Leaving[Span]));

		double Cap = Ground.Taxi.SpeedCap;
		if (Length > 0.0 && Turn > CornerEpsilon)
		{
			Cap = FMath::Min(Cap, MaxTurnRate * Length / Turn);
		}

		// Never below the creep speed. A span this tight is one the aircraft has to crab
		// through, and crawling is the slowest an aeroplane may do that at - see
		// FGroundPerformance::MinTaxiSpeed.
		SpanCaps[Span] = FMath::Max(Cap, Ground.MinTaxiSpeed);
	}

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
			Limit = Ground.MinTaxiSpeed;
		}

		if (At > 0)          { Limit = FMath::Min(Limit, SpanCaps[At - 1]); }
		if (At + 1 < Count)  { Limit = FMath::Min(Limit, SpanCaps[At]); }

		VertexLimits[At] = Limit;
	}

	// An aircraft arriving at its destination stops. Not floored at MinTaxiSpeed, because
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
