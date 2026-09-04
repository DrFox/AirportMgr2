#include "CoreMinimal.h"
#include "Entities/AircraftType.h"
#include "Misc/AutomationTest.h"
#include "Model/RouteFollower.h"
#include "Model/RouteSearch.h"
#include "Solve/GuidelineGeom.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// Prefixed against the UNITY build - these test files share one translation unit.

	/** Sixty frames a second, the rate the aircraft is actually watched at. */
	constexpr double TurnRateFrame = 1.0 / 60.0;

	/** Shortest angle between two headings, in degrees, unsigned. */
	double TurnRateDeltaDegrees(double FromRadians, double ToRadians)
	{
		return FMath::Abs(FMath::RadiansToDegrees(
			FMath::UnwindRadians(ToRadians - FromRadians)));
	}

	/** A found plan around Points, so a follower will walk it. */
	FRoutePlan TurnRatePlan(const TArray<FVector2D>& Points)
	{
		FRoutePlan Plan;
		Plan.Result = ERouteResult::Found;
		Plan.Polyline = Points;
		Plan.Length = GuidelineGeom::PolylineLength(Points);
		return Plan;
	}

	/**
	 * Two 40 m legs meeting at a GENUINE 90 degree corner.
	 *
	 * Genuine is the point: the vertex bends far past the ~20 degrees GuidelineGeom treats
	 * as a sampling artefact, so it is not smoothed and the direction of travel really does
	 * change instantly. This is the fixture the whole change exists for.
	 */
	TArray<FVector2D> TurnRateCorner()
	{
		return { FVector2D(0.0, 0.0), FVector2D(4000.0, 0.0), FVector2D(4000.0, 4000.0) };
	}

	/** A quarter circle of radius R, as the sampled polyline a swept lead-in produces. */
	TArray<FVector2D> TurnRateQuarterCircle(double Radius, int32 Samples)
	{
		TArray<FVector2D> Points;
		for (int32 Step = 0; Step < Samples; ++Step)
		{
			const double T = static_cast<double>(Step) / (Samples - 1);
			const double Angle = T * UE_DOUBLE_PI * 0.5;
			Points.Emplace(Radius * FMath::Sin(Angle), Radius * (1.0 - FMath::Cos(Angle)));
		}
		return Points;
	}

	/**
	 * How far the agent is FACING away from the line it is standing on, in degrees.
	 *
	 * Measured against the polyline's own heading at the distance the follower has reached,
	 * rather than against the last two poses: a differenced position is a second opinion
	 * about the direction of travel, and it degenerates exactly where the aircraft is
	 * moving slowest - which is the one place this number matters.
	 */
	double TurnRateCrabDegrees(const FRouteFollower& Follower, double Heading)
	{
		FVector2D Unused;
		double LineHeading = 0.0;
		if (!GuidelineGeom::PointAtDistance(
			Follower.Plan.Polyline, Follower.Travelled, Unused, LineHeading))
		{
			return 0.0;
		}

		return TurnRateDeltaDegrees(LineHeading, Heading);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTurnRateTest,
	"RoadNet.Model.TurnRate",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTurnRateTest::RunTest(const FString& Parameters)
{
	const FTaxiPerformance Piper = UAircraftType::PiperMeridianTaxi();

	if (!TestTrue(TEXT("the Meridian's ground performance is authored"), Piper.IsSet()))
	{
		// Everything below divides by these. Bail rather than report five failures that all
		// mean this one thing.
		return false;
	}

	// 1. THE MEASUREMENT. An aircraft cannot swing its nose arbitrarily fast.
	//
	//    Heading was whatever the geometry demanded - v times curvature, with no ceiling.
	//    At a genuine 90 degree corner the direction of travel changes between one frame and
	//    the next, so the aircraft's yaw rate was 90 degrees in 1/60 s: FIVE THOUSAND FOUR
	//    HUNDRED degrees a second. A Meridian can manage twenty.
	{
		FRouteFollower Follower;
		Follower.Start(TurnRatePlan(TurnRateCorner()), Piper);

		double WorstRate = 0.0;
		double Previous = Follower.Heading;

		for (int32 Frame = 0; Frame < 3000 && !Follower.HasArrived(); ++Frame)
		{
			FVector2D At;
			double Heading = 0.0;
			if (!Follower.Advance(TurnRateFrame, At, Heading))
			{
				break;
			}

			WorstRate = FMath::Max(
				WorstRate, TurnRateDeltaDegrees(Previous, Heading) / TurnRateFrame);
			Previous = Heading;
		}

		// The bound is exact - the slew is clamped - so this admits only rounding.
		TestTrue(FString::Printf(
			TEXT("yaw rate never exceeds the airframe's %.0f deg/s (worst %.1f deg/s)"),
			Piper.MaxTurnRateDegPerSec, WorstRate),
			WorstRate <= Piper.MaxTurnRateDegPerSec + 0.01);
	}

	// 2. And it pays for that by SLOWING DOWN, not by leaving the line.
	//
	//    The alternative to slowing is crabbing: the aircraft stays on the painted line but
	//    points somewhere else, sliding sideways round the bend. That is the failure
	//    GuidelineGeom::Sample and its MaxSampledTurn exist to prevent one layer down, and
	//    reintroducing it here would put it somewhere no geometry test can see.
	//
	//    A swept lead-in is the shape a real route actually contains, so this is the case
	//    that has to be clean.
	{
		FRouteFollower Follower;
		Follower.Start(TurnRatePlan(TurnRateQuarterCircle(2500.0, 16)), Piper);

		double WorstCrab = 0.0;
		double SlowestSpeed = Piper.TaxiSpeed;

		for (int32 Frame = 0; Frame < 3000 && !Follower.HasArrived(); ++Frame)
		{
			FVector2D At;
			double Heading = 0.0;
			if (!Follower.Advance(TurnRateFrame, At, Heading))
			{
				break;
			}

			WorstCrab = FMath::Max(WorstCrab, TurnRateCrabDegrees(Follower, Heading));
			SlowestSpeed = FMath::Min(SlowestSpeed, Follower.Speed);
		}

		TestTrue(FString::Printf(
			TEXT("through a swept turn the nose stays within %.0f deg of the line (worst %.2f deg)"),
			FRouteFollower::CrabAtMinSpeedDegrees, WorstCrab),
			WorstCrab <= FRouteFollower::CrabAtMinSpeedDegrees);

		// The other half of the same claim. A follower that ignored the limit would hold
		// full speed and post a tiny crab as well, so "it crabs little" alone proves
		// nothing - it has to have slowed to earn it.
		//
		// A 25 m radius at 10 m/s needs 22.9 deg/s and only 20 is available, so it must.
		TestTrue(FString::Printf(
			TEXT("and it got there by slowing down (%.0f uu/s, from %.0f)"),
			SlowestSpeed, Piper.TaxiSpeed),
			SlowestSpeed < Piper.TaxiSpeed * 0.95);
	}

	// 3. At a corner too sharp to slow into, the crab is BOUNDED and it decays at the rate.
	//
	//    An instant change of travel direction cannot be tracked at any speed, so here the
	//    aircraft does briefly point away from its motion. What must be true is that the
	//    error only ever shrinks, at the airframe's rate, over a distance a wheeled aircraft
	//    could plausibly cover - not that it is absent.
	{
		FRouteFollower Follower;
		Follower.Start(TurnRatePlan(TurnRateCorner()), Piper);

		double WorstCrab = 0.0;
		double CrabStartedAt = -1.0;
		double RealignedAt = -1.0;
		double Elapsed = 0.0;

		// Measured over the window where the crab is wide enough to PIN the aircraft to its
		// floor. Running it on to the moment the nose is straight would take in the tail of
		// the turn, where the crab is already inside CrabAtMinSpeedDegrees and the aircraft
		// is accelerating away again - distance covered under power, not creep, and it
		// swamps the thing being measured.
		double PinnedTravel = 0.0;
		double PinnedTopSpeed = 0.0;
		double Previous = Follower.Travelled;

		for (int32 Frame = 0; Frame < 3000 && !Follower.HasArrived(); ++Frame)
		{
			FVector2D At;
			double Heading = 0.0;
			if (!Follower.Advance(TurnRateFrame, At, Heading))
			{
				break;
			}
			Elapsed += TurnRateFrame;

			const double Crab = TurnRateCrabDegrees(Follower, Heading);
			WorstCrab = FMath::Max(WorstCrab, Crab);

			if (Crab > FRouteFollower::CrabAtMinSpeedDegrees)
			{
				PinnedTravel += Follower.Travelled - Previous;
				PinnedTopSpeed = FMath::Max(PinnedTopSpeed, Follower.Speed);

				if (CrabStartedAt < 0.0)
				{
					CrabStartedAt = Elapsed;
				}
			}
			else if (CrabStartedAt >= 0.0 && RealignedAt < 0.0 && Crab < 1.0)
			{
				RealignedAt = Elapsed;
			}

			Previous = Follower.Travelled;
		}

		// It never gets worse than the corner itself. A follower that wound up past the turn
		// it was tracking would show here as more than ninety.
		TestTrue(FString::Printf(
			TEXT("crab never exceeds the corner's own 90 deg (worst %.1f deg)"), WorstCrab),
			WorstCrab <= 90.0 + 0.01);

		if (TestTrue(TEXT("a genuine corner does produce a crab to measure"), RealignedAt > 0.0))
		{
			// Ninety degrees at twenty a second is four and a half. Anything longer means
			// the slew is not running at the airframe's rate.
			const double Took = RealignedAt - CrabStartedAt;
			TestTrue(FString::Printf(
				TEXT("and it is gone within 90/%.0f = %.1f s (took %.2f s)"),
				Piper.MaxTurnRateDegPerSec, 90.0 / Piper.MaxTurnRateDegPerSec, Took),
				Took <= (90.0 / Piper.MaxTurnRateDegPerSec) + 0.2);

			// The floor really is the thing holding it back, rather than the law happening
			// to land near it. Exact: past CrabAtMinSpeedDegrees the speed term goes
			// negative and MinTaxiSpeed is all that is left.
			TestTrue(FString::Printf(
				TEXT("while crabbed it is pinned to the %.0f uu/s floor (fastest %.0f)"),
				Piper.MinTaxiSpeed, PinnedTopSpeed),
				PinnedTopSpeed <= Piper.MinTaxiSpeed + 1e-9);

			// THE NUMBER THAT MAKES IT LOOK LIKE AN AEROPLANE. Held at the floor from 90
			// degrees of crab down to CrabAtMinSpeedDegrees, so the whole manoeuvre is a few
			// metres of creep - a Meridian inching round a tight corner - and not a long
			// sideways skid.
			//
			// Plus one frame at full speed: the corner is only discovered by arriving at it,
			// so the aircraft is always one tick past the vertex before it can react. That
			// term is the lag Advance documents, stated here rather than hidden in a margin.
			const double PinnedSeconds =
				(90.0 - FRouteFollower::CrabAtMinSpeedDegrees) / Piper.MaxTurnRateDegPerSec;
			const double Allowed =
				(Piper.MinTaxiSpeed * PinnedSeconds + Piper.TaxiSpeed * TurnRateFrame) * 1.05;

			TestTrue(FString::Printf(
				TEXT("covering only %.0f uu of creep, within the %.0f that allows"),
				PinnedTravel, Allowed),
				PinnedTravel <= Allowed);
		}
	}

	// 4. A STRAIGHT ROUTE IS UNTOUCHED. Most of an airport is straight, so a turn model that
	//    cost anything here would be paid for everywhere and earned almost nowhere.
	{
		const double Length = 5000.0;
		FRouteFollower Follower;
		Follower.Start(TurnRatePlan({ FVector2D(0.0, 0.0), FVector2D(Length, 0.0) }), Piper);

		double Elapsed = 0.0;
		bool bHeadingHeld = true;

		for (int32 Frame = 0; Frame < 3000 && !Follower.HasArrived(); ++Frame)
		{
			FVector2D At;
			double Heading = 0.0;
			if (!Follower.Advance(TurnRateFrame, At, Heading))
			{
				break;
			}
			Elapsed += TurnRateFrame;
			bHeadingHeld &= TurnRateDeltaDegrees(Heading, 0.0) < 1e-9;
		}

		TestTrue(TEXT("a straight route never yaws at all"), bHeadingHeld);

		// 5000 uu at 1000 uu/s is five seconds, and it must still be five - within the one
		// frame the arrival test can overshoot by.
		const double Expected = Length / Piper.TaxiSpeed;
		TestTrue(FString::Printf(
			TEXT("and takes the same %.2f s it always did (took %.2f s)"), Expected, Elapsed),
			FMath::Abs(Elapsed - Expected) <= TurnRateFrame + 1e-6);
	}

	// 5. IT ALWAYS ARRIVES, AND IT NEVER STOPS ROLLING.
	//
	//    The floor under the speed law is not a tuning choice, it is the physics: a prop
	//    makes thrust along the airframe and a nosewheel only steers where that thrust goes,
	//    so an aeroplane has no way to pivot standing still. Without the floor the law
	//    reaches zero at a sharp corner and the agent parks there for ever, which is both
	//    wrong and a deadlock.
	{
		FRouteFollower Follower;
		Follower.Start(TurnRatePlan(TurnRateCorner()), Piper);

		double Elapsed = 0.0;
		double Previous = Follower.Travelled;
		bool bAlwaysMoved = true;
		int32 Frames = 0;

		for (; Frames < 3000 && !Follower.HasArrived(); ++Frames)
		{
			FVector2D At;
			double Heading = 0.0;
			if (!Follower.Advance(TurnRateFrame, At, Heading))
			{
				break;
			}
			Elapsed += TurnRateFrame;

			bAlwaysMoved &= Follower.Travelled > Previous;
			Previous = Follower.Travelled;
		}

		TestTrue(TEXT("the aircraft is rolling on every single frame, corner included"),
			bAlwaysMoved);

		// 80 m at 10 m/s is eight seconds, plus four and a half creeping round the corner.
		TestTrue(FString::Printf(TEXT("and it arrives (%.2f s)"), Elapsed),
			Follower.HasArrived() && Elapsed < 30.0);
	}

	return true;
}

#endif
