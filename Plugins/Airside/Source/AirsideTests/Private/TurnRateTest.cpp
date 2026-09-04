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

	/**
	 * The same corner with LONG legs, so the aircraft reaches taxi speed before it has to
	 * think about the turn.
	 *
	 * The short fixture cannot answer "did it start braking at the right moment", because it
	 * never gets to full speed: the accelerate-away and brake-for-the-corner curves cross
	 * part way up, and the braking distance measured off that crossing is a fact about the
	 * fixture rather than about the aeroplane.
	 */
	TArray<FVector2D> TurnRateLongCorner()
	{
		return { FVector2D(0.0, 0.0), FVector2D(10000.0, 0.0), FVector2D(10000.0, 10000.0) };
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
	 * Straight taxiway, swept turn, straight taxiway - the shape a real route actually has.
	 *
	 * The lead-in is long enough to reach taxi speed and then brake down to what the sweep
	 * allows, which is the whole point: a bare quarter circle is over before the aircraft is
	 * going fast enough for the turn to be what limits it, so it would answer questions
	 * about the fixture's length instead of about the curve.
	 */
	TArray<FVector2D> TurnRateSweptRoute(double Radius)
	{
		TArray<FVector2D> Points;
		Points.Emplace(-8000.0, 0.0);

		// The circle starts at the origin heading due east and ends heading due north, so
		// both joins are tangent and neither reads as a corner.
		Points.Append(TurnRateQuarterCircle(Radius, 16));
		Points.Emplace(Radius, Radius + 5000.0);
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
	"Airside.Model.TurnRate",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTurnRateTest::RunTest(const FString& Parameters)
{
	const FGroundPerformance Piper = UAircraftType::PiperMeridianGround();

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
	{
		constexpr double Radius = 2500.0;

		FRouteFollower Follower;
		Follower.Start(TurnRatePlan(TurnRateSweptRoute(Radius)), Piper);

		double WorstCrab = 0.0;
		double TopSpeedOverall = 0.0;
		double TopSpeedInTurn = 0.0;

		// The MIDDLE HALF of the sweep, not all of it. Where the arc meets a straight, the
		// smoothed vertex direction is an average of the two, so the first and last spans
		// genuinely bend less than the arc does and are legitimately allowed more speed.
		// Measuring across them would be asking about the join, not about the curve.
		const double Arc = Radius * UE_DOUBLE_PI * 0.5;
		const double TurnFrom = 8000.0 + Arc * 0.25;
		const double TurnTo = 8000.0 + Arc * 0.75;

		for (int32 Frame = 0; Frame < 6000 && !Follower.HasArrived(); ++Frame)
		{
			FVector2D At;
			double Heading = 0.0;
			if (!Follower.Advance(TurnRateFrame, At, Heading))
			{
				break;
			}

			WorstCrab = FMath::Max(WorstCrab, TurnRateCrabDegrees(Follower, Heading));
			TopSpeedOverall = FMath::Max(TopSpeedOverall, Follower.Speed);

			if (Follower.Travelled > TurnFrom && Follower.Travelled < TurnTo)
			{
				TopSpeedInTurn = FMath::Max(TopSpeedInTurn, Follower.Speed);
			}
		}

		TestTrue(FString::Printf(
			TEXT("through a swept turn the nose stays within %.0f deg of the line (worst %.2f deg)"),
			FRouteFollower::CrabAtMinSpeedDegrees, WorstCrab),
			WorstCrab <= FRouteFollower::CrabAtMinSpeedDegrees);

		// It reached taxi speed on the straight, so whatever holds it back in the turn is
		// the turn. Without this the next assertion would also pass on an aeroplane that was
		// simply never given room to get going.
		TestTrue(FString::Printf(
			TEXT("it does reach taxi speed on the straight (%.0f uu/s)"), TopSpeedOverall),
			TopSpeedOverall >= Piper.Taxi.SpeedCap * 0.99);

		// THE CAP IS THE GEOMETRY, and it is arithmetic rather than a tuned number: a turn is
		// v/R, so the fastest this radius can be taken at is MaxTurnRate times R. At 20 deg/s
		// and 25 m that is 8.7 m/s, against a 10 m/s taxi speed.
		const double Allowed = FMath::DegreesToRadians(Piper.MaxTurnRateDegPerSec) * Radius;
		TestTrue(FString::Printf(
			TEXT("and the turn holds it to MaxTurnRate x R = %.0f uu/s (fastest %.0f)"),
			Allowed, TopSpeedInTurn),
			TopSpeedInTurn <= Allowed * 1.02);
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

			// The floor really is the thing holding it back, rather than the law happening to
			// land near it - a failure here reads in the hundreds, not in the eighties.
			//
			// Not asserted exactly. Approaching the corner the profile's own slope IS the
			// braking limit, so a fixed-step integrator tracks it a frame behind and crosses
			// the vertex a few uu/s high. That is discretisation, not the model: it shrinks
			// with the timestep, as section 6 demonstrates on a case where it can be measured.
			TestTrue(FString::Printf(
				TEXT("while crabbed it is held at the %.0f uu/s floor (fastest %.0f)"),
				Piper.MinTaxiSpeed, PinnedTopSpeed),
				PinnedTopSpeed <= Piper.MinTaxiSpeed * 1.25);

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
				(Piper.MinTaxiSpeed * PinnedSeconds + Piper.Taxi.SpeedCap * TurnRateFrame) * 1.05;

			TestTrue(FString::Printf(
				TEXT("covering only %.0f uu of creep, within the %.0f that allows"),
				PinnedTravel, Allowed),
				PinnedTravel <= Allowed);
		}
	}

	// 4. THE MEASUREMENT FOR THIS ROUND. An aircraft cannot change speed instantly.
	//
	//    Speed used to be a pure function of the crab in front of it, so arriving at a corner
	//    took it from 1000 uu/s to 80 between one frame and the next: 920 uu/s in 1/60 s, or
	//    552 m/s2. Fifty-six g. A Meridian brakes at two.
	{
		FRouteFollower Follower;
		Follower.Start(TurnRatePlan(TurnRateLongCorner()), Piper);

		double WorstAccel = 0.0;
		double WorstDecel = 0.0;
		double Previous = Follower.Speed;

		for (int32 Frame = 0; Frame < 6000 && !Follower.HasArrived(); ++Frame)
		{
			FVector2D At;
			double Heading = 0.0;
			if (!Follower.Advance(TurnRateFrame, At, Heading))
			{
				break;
			}

			const double Rate = (Follower.Speed - Previous) / TurnRateFrame;
			WorstAccel = FMath::Max(WorstAccel, Rate);
			WorstDecel = FMath::Max(WorstDecel, -Rate);
			Previous = Follower.Speed;
		}

		TestTrue(FString::Printf(
			TEXT("it never brakes harder than %.0f uu/s2 (worst %.0f)"),
			Piper.Taxi.Decel, WorstDecel),
			WorstDecel <= Piper.Taxi.Decel + 0.01);

		TestTrue(FString::Printf(
			TEXT("nor accelerates harder than %.0f uu/s2 (worst %.0f)"),
			Piper.Taxi.Accel, WorstAccel),
			WorstAccel <= Piper.Taxi.Accel + 0.01);
	}

	// 5. WHICH ONLY WORKS IF IT SEES THE CORNER COMING.
	//
	//    A braking limit on its own is a REGRESSION, not a fix. Discovering the turn by
	//    arriving at it and then braking at 2 m/s2 puts the aircraft twenty-five metres past
	//    the corner, crabbed at ninety degrees the whole way - far worse than the abrupt stop
	//    it replaced. So the speed it may be at is planned along the route, and this measures
	//    that the plan is right in both directions: early enough, and not a metre earlier.
	{
		const double Corner = 10000.0;

		FRouteFollower Follower;
		Follower.Start(TurnRatePlan(TurnRateLongCorner()), Piper);

		// Sampled at two distances OUT from the corner, and compared against the curve the
		// physics demands. "It was slow at the corner" is not enough - the old reactive law
		// was too, because by then the crab it reacts to has appeared.
		const double FarOut = 1000.0;
		const double NearOut = 500.0;
		double SpeedFarOut = -1.0;
		double SpeedNearOut = -1.0;

		// The first time it slows after having got up to speed. Simply watching for "below
		// the cap" would trigger on the wind-up from rest, which is not braking at all.
		bool bReachedCap = false;
		double BrakedAt = -1.0;
		double Previous = 0.0;

		for (int32 Frame = 0; Frame < 6000 && !Follower.HasArrived(); ++Frame)
		{
			FVector2D At;
			double Heading = 0.0;
			if (!Follower.Advance(TurnRateFrame, At, Heading))
			{
				break;
			}

			bReachedCap |= Follower.Speed >= Piper.Taxi.SpeedCap * 0.999;
			if (bReachedCap && BrakedAt < 0.0 && Follower.Speed < Previous)
			{
				BrakedAt = Follower.Travelled;
			}
			Previous = Follower.Speed;

			if (SpeedFarOut < 0.0 && Follower.Travelled >= Corner - FarOut)
			{
				SpeedFarOut = Follower.Speed;
			}
			if (SpeedNearOut < 0.0 && Follower.Travelled >= Corner - NearOut)
			{
				SpeedNearOut = Follower.Speed;
			}
		}

		// EARLY ENOUGH, and by the right amount. v2 = u2 + 2as says what the speed must be
		// at a given distance out if the aircraft is to be down to the floor by the vertex.
		// Ten metres out that is 6.4 m/s and five metres out 4.6, against the 10 m/s the
		// reactive law was still doing right up to the corner.
		auto Curve = [&Piper](double Out)
		{
			return FMath::Sqrt(FMath::Square(Piper.MinTaxiSpeed) + 2.0 * Piper.Taxi.Decel * Out);
		};

		TestTrue(FString::Printf(
			TEXT("%.0f uu out it is already down to %.0f uu/s, as %.0f demands"),
			FarOut, SpeedFarOut, Curve(FarOut)),
			SpeedFarOut >= 0.0 && FMath::Abs(SpeedFarOut - Curve(FarOut)) <= 20.0);

		TestTrue(FString::Printf(
			TEXT("and %.0f uu out down to %.0f, as %.0f demands"),
			NearOut, SpeedNearOut, Curve(NearOut)),
			SpeedNearOut >= 0.0 && FMath::Abs(SpeedNearOut - Curve(NearOut)) <= 20.0);

		// AND NOT A METRE EARLIER, which is the assertion that stops "look ahead" quietly
		// degenerating into "creep the whole route" - that would satisfy the two above and
		// look nothing like an aeroplane.
		const double Expected =
			(FMath::Square(Piper.Taxi.SpeedCap) - FMath::Square(Piper.MinTaxiSpeed))
			/ (2.0 * Piper.Taxi.Decel);

		if (TestTrue(TEXT("it does reach taxi speed and then brake"), BrakedAt > 0.0))
		{
			const double Actual = Corner - BrakedAt;
			TestTrue(FString::Printf(
				TEXT("braking starts %.0f uu out, against the %.0f the physics needs"),
				Actual, Expected),
				FMath::Abs(Actual - Expected) <= 100.0);
		}
	}

	// 6. A STRAIGHT ROUTE: wind up, hold, and stop at the far end.
	//
	//    Most of an airport is straight, so this is the shape most of the flying hours are
	//    spent in. It also pins the two ends that a turn never exercises - leaving a stand
	//    from rest and arriving at one - both of which used to be instantaneous.
	{
		const double Length = 20000.0;

		// Wind up, cruise, brake: three phases with closed forms, so the expected time is
		// arithmetic rather than a figure copied off a run that happened to look right.
		const double Cap = Piper.Taxi.SpeedCap;
		const double UpDistance = FMath::Square(Cap) / (2.0 * Piper.Taxi.Accel);
		const double DownDistance = FMath::Square(Cap) / (2.0 * Piper.Taxi.Decel);
		const double Expected = Cap / Piper.Taxi.Accel
			+ (Length - UpDistance - DownDistance) / Cap
			+ Cap / Piper.Taxi.Decel;

		// Run the same journey at two timesteps. A fixed-step integrator cannot land exactly
		// on a closed form, and the useful question is not how close one run gets but whether
		// the gap SHRINKS as the step does: that is what tells discretisation apart from a
		// model that is simply wrong, and it is a distinction this project has paid to learn.
		auto Journey = [&Piper, Length](double Step, double& OutTopSpeed, double& OutFinalSpeed,
			bool& bOutHeadingHeld, bool& bOutStartedFromRest)
		{
			FRouteFollower Follower;
			Follower.Start(TurnRatePlan({ FVector2D(0.0, 0.0), FVector2D(Length, 0.0) }), Piper);

			bOutStartedFromRest = Follower.Speed <= KINDA_SMALL_NUMBER;
			bOutHeadingHeld = true;
			OutTopSpeed = 0.0;

			double Elapsed = 0.0;
			const int32 Budget = FMath::CeilToInt32(120.0 / Step);

			for (int32 Frame = 0; Frame < Budget && !Follower.HasArrived(); ++Frame)
			{
				FVector2D At;
				double Heading = 0.0;
				if (!Follower.Advance(Step, At, Heading))
				{
					break;
				}
				Elapsed += Step;
				OutTopSpeed = FMath::Max(OutTopSpeed, Follower.Speed);
				bOutHeadingHeld &= TurnRateDeltaDegrees(Heading, 0.0) < 1e-9;
			}

			OutFinalSpeed = Follower.Speed;
			return Elapsed;
		};

		double TopSpeed = 0.0;
		double FinalSpeed = 0.0;
		bool bHeadingHeld = false;
		bool bFromRest = false;
		const double Coarse = Journey(TurnRateFrame, TopSpeed, FinalSpeed, bHeadingHeld, bFromRest);

		double FineTop = 0.0;
		double FineFinal = 0.0;
		bool bFineHeading = false;
		bool bFineRest = false;
		const double Fine = Journey(TurnRateFrame / 8.0, FineTop, FineFinal, bFineHeading, bFineRest);

		TestTrue(TEXT("an aircraft starts from rest, not at taxi speed"), bFromRest);
		TestTrue(TEXT("a straight route never yaws at all"), bHeadingHeld);

		TestTrue(FString::Printf(TEXT("it reaches taxi speed (%.0f uu/s)"), TopSpeed),
			TopSpeed >= Piper.Taxi.SpeedCap * 0.99);

		TestTrue(FString::Printf(
			TEXT("and stops on arrival rather than at %.0f uu/s"), FinalSpeed),
			FinalSpeed <= Piper.MinTaxiSpeed);

		const double CoarseGap = FMath::Abs(Coarse - Expected);
		const double FineGap = FMath::Abs(Fine - Expected);

		TestTrue(FString::Printf(
			TEXT("an eighth of the timestep lands closer to the %.2f s the phases add up to "
				 "(%.2f s -> %.2f s, gap %.3f -> %.3f)"),
			Expected, Coarse, Fine, CoarseGap, FineGap),
			FineGap < CoarseGap * 0.5);

		// Close enough to pin it to THIS answer rather than one nearby, but not asked to be
		// closer than the arithmetic allows.
		//
		// The gap does not fall linearly with the step - an eighth of it buys about a third
		// of the error, not an eighth. That is the braking curve's own shape: v = sqrt(2 a d)
		// has infinite slope in d where it reaches zero, so the last moments of a stop are
		// where any fixed-step integrator is least accurate, and no amount of refinement
		// makes that term first-order. It converges; it just converges slowly.
		TestTrue(FString::Printf(
			TEXT("and lands on it within a fifth of a second (%.3f s out)"), FineGap),
			FineGap <= 0.2);
	}

	// 7. IT ALWAYS ARRIVES, AND IT NEVER STOPS ROLLING WHILE THERE IS TURNING TO DO.
	//
	//    A prop makes thrust along the airframe and a nosewheel only steers where that thrust
	//    goes, so an aeroplane has no way to swing its nose standing still. Stopping AT a
	//    destination is fine and is what section 6 asks for; stopping mid-turn is not.
	{
		FRouteFollower Follower;
		Follower.Start(TurnRatePlan(TurnRateCorner()), Piper);

		double Elapsed = 0.0;
		bool bRollingWhileTurning = true;

		for (int32 Frame = 0; Frame < 6000 && !Follower.HasArrived(); ++Frame)
		{
			FVector2D At;
			double Heading = 0.0;
			if (!Follower.Advance(TurnRateFrame, At, Heading))
			{
				break;
			}
			Elapsed += TurnRateFrame;

			if (TurnRateCrabDegrees(Follower, Heading) > 1.0)
			{
				bRollingWhileTurning &= Follower.Speed >= Piper.MinTaxiSpeed - KINDA_SMALL_NUMBER;
			}
		}

		TestTrue(TEXT("it is still rolling on every frame that it is turning"),
			bRollingWhileTurning);

		TestTrue(FString::Printf(TEXT("and it arrives (%.2f s)"), Elapsed),
			Follower.HasArrived() && Elapsed < 60.0);
	}

	return true;
}

#endif
