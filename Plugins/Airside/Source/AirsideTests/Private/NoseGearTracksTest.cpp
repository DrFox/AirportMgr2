#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/RouteFollower.h"
#include "Model/RouteSearch.h"
#include "Model/SpeedProfile.h"
#include "Solve/GuidelineGeom.h"
#include "Solve/RoadGeom.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// Prefixed against the UNITY build - these test files share one translation unit.

	/** Sixty frames a second, the rate the aircraft is actually watched at. */
	constexpr double NoseGearFrame = 1.0 / 60.0;

	FRoutePlan NoseGearPlan(const TArray<FVector2D>& Points)
	{
		FRoutePlan Plan;
		Plan.Result = ERouteResult::Found;
		Plan.Polyline = Points;
		Plan.Length = GuidelineGeom::PolylineLength(Points);
		return Plan;
	}

	/**
	 * A quarter circle of radius R, entered and left along its own TANGENTS.
	 *
	 * The tangents are the point. An approach leg that meets the arc at an angle puts a
	 * genuine corner at the join, and everything measured afterwards is a measurement of
	 * that corner rather than of the curve - the aircraft would be crawling through the
	 * whole arc for a reason the test never intended. The arc runs from (0,0) heading +X
	 * round to (R,R) heading +Y, about a centre at (0,R).
	 *
	 * THE APPROACH LEG IS 80 m because the aircraft starts from rest: at the Meridian's
	 * 100 uu/s^2 it needs v^2/2a = 5000 uu to reach the 1000 uu/s taxi cap, so a shorter
	 * one measures an aeroplane that is still accelerating and calls it a corner limit.
	 */
	TArray<FVector2D> NoseGearArc(double R)
	{
		TArray<FVector2D> Points;
		Points.Add(FVector2D(-8000.0, 0.0));
		for (int32 Step = 0; Step <= 24; ++Step)
		{
			const double Angle = -HALF_PI + (HALF_PI * Step) / 24.0;
			Points.Add(FVector2D(R * FMath::Cos(Angle), R + R * FMath::Sin(Angle)));
		}
		Points.Add(FVector2D(R, R + 2000.0));
		return Points;
	}

	/** A plane2-shaped airframe: origin on the nose gear, mains 4.54 m aft. */
	FAirframe NoseGearTwinOtter()
	{
		FAirframe Airframe;
		Airframe.Ground = TestAirframes::Piper().Ground;
		Airframe.Ground.MaxSteerDegrees = 60.0;
		Airframe.Ground.MaxLateralAccelUu = 147.0;
		Airframe.SteerAxleX = 0.0;
		Airframe.FixedAxleX = -454.3;
		return Airframe;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNoseGearTracksTest,
	"Airside.Model.NoseGearTracks",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FNoseGearTracksTest::RunTest(const FString& Parameters)
{
	// THE REPORTED DEFECT, PINNED: "it keeps the centre of its rear wheels on the route line
	// rather than the nose wheel". The steered axle must be ON the line and the mains INSIDE
	// it - a trailing wheel cuts the corner, which is the whole visible difference.
	const double R = 3000.0;
	const FAirframe Airframe = NoseGearTwinOtter();

	FRouteFollower Follower;
	Follower.Start(NoseGearPlan(NoseGearArc(R)), Airframe, Airframe.Ground.Taxi.SpeedCap);

	double WorstSteerOffLine = 0.0;
	double MainsInsideBy = 0.0;
	bool bMeasuredInTheTurn = false;

	for (int32 Frame = 0; Frame < 4000; ++Frame)
	{
		FVector2D Origin = FVector2D::ZeroVector;
		double Heading = 0.0;
		if (!Follower.Advance(NoseGearFrame, Airframe, Origin, Heading))
		{
			break;
		}

		// The steered axle IS the origin for this airframe (SteerAxleX is zero), so the
		// reported position is the point that must be on the line.
		FVector2D OnLine = FVector2D::ZeroVector;
		double LineHeading = 0.0;
		GuidelineGeom::PointAtDistance(Follower.Plan.Polyline, Follower.Travelled,
			OnLine, LineHeading);
		WorstSteerOffLine = FMath::Max(WorstSteerOffLine, FVector2D::Distance(Origin, OnLine));

		// Measured in the arc proper, where "inside" has a meaning: the centre of the circle
		// is (0, R), so inside is nearer to it than the line is. The first and last spans
		// are skipped because a smoothed vertex there averages with the straights.
		if (Origin.X > 500.0 && Origin.Y < R - 500.0)
		{
			const FVector2D Mains = RoadGeom::TrailPoint(Origin, Heading, Airframe.FixedAxleX);
			const double MainsRadius = FVector2D::Distance(Mains, FVector2D(0.0, R));
			MainsInsideBy = FMath::Max(MainsInsideBy, R - MainsRadius);
			bMeasuredInTheTurn = true;
		}
	}

	TestTrue(TEXT("the arc was actually entered, so the measurements below mean something"),
		bMeasuredInTheTurn);

	// ON the line, not near it: the steered axle is CONSTRAINED to the polyline rather than
	// chasing it, so any real drift here is a derivation bug, not a tracking error.
	TestTrue(FString::Printf(
		TEXT("the steered axle stays on the line (worst %.2f uu)"), WorstSteerOffLine),
		WorstSteerOffLine < 1.0);

	// INSIDE BY THE GEOMETRY, not merely inside. A body of wheelbase L whose front axle
	// follows a radius R settles with its rear axle at sqrt(R^2 - L^2) from the centre, so
	// it cuts in by R minus that - 34 uu here. Asserting only "inside" would pass on a body
	// that barely leaned, which is the failure this whole change is about.
	const double Expected = R - FMath::Sqrt(R * R - Airframe.Wheelbase() * Airframe.Wheelbase());
	TestTrue(FString::Printf(
		TEXT("the mains cut inside by about %.1f uu, measured %.1f"), Expected, MainsInsideBy),
		FMath::Abs(MainsInsideBy - Expected) < 5.0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUnmeasuredAirframeUnchangedTest,
	"Airside.Model.UnmeasuredAirframeUnchanged",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUnmeasuredAirframeUnchangedTest::RunTest(const FString& Parameters)
{
	// EVERY VEHICLE IN THE GAME TAKES THIS PATH. A van is authored at 90 deg/s, which needs
	// 83 degrees of lock at its creep speed - it pivots, and the geometric law would cripple
	// it. So an airframe with no axles must behave EXACTLY as it does today.
	FAirframe Pivot;
	Pivot.Ground = TestAirframes::Piper().Ground;
	Pivot.Ground.MaxTurnRateDegPerSec = 90.0;

	FRouteFollower Follower;
	Follower.Start(NoseGearPlan({FVector2D(0.0, 0.0), FVector2D(4000.0, 0.0),
		FVector2D(4000.0, 4000.0)}), Pivot, Pivot.Ground.Taxi.SpeedCap);

	double Worst = 0.0;
	double Previous = Follower.Heading;
	for (int32 Frame = 0; Frame < 2000; ++Frame)
	{
		FVector2D At = FVector2D::ZeroVector;
		double Heading = 0.0;
		if (!Follower.Advance(NoseGearFrame, Pivot, At, Heading))
		{
			break;
		}
		Worst = FMath::Max(Worst, FMath::Abs(FMath::RadiansToDegrees(
			FMath::UnwindRadians(Heading - Previous))) / NoseGearFrame);
		Previous = Heading;
	}

	// The flat rate still governs, and the SPEED does not enter it - which is exactly what
	// makes it a pivot law rather than a bicycle one.
	TestTrue(FString::Printf(TEXT("the flat 90 deg/s still governs (peak %.1f)"), Worst),
		Worst <= 90.0 + 1.0);
	TestTrue(TEXT("and it is actually used, not merely not exceeded"), Worst > 45.0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSteerNeverExceedsLockTest,
	"Airside.Model.SteerNeverExceedsLock",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSteerNeverExceedsLockTest::RunTest(const FString& Parameters)
{
	// A GENUINE 90 degree corner - past the ~20 degrees GuidelineGeom treats as a sampling
	// artefact, so the direction of travel really does change instantly and the lock is the
	// only thing standing between the model and an aeroplane that swaps ends.
	FAirframe Airframe = NoseGearTwinOtter();
	Airframe.Ground.MaxSteerDegrees = 45.0;

	FRouteFollower Follower;
	Follower.Start(NoseGearPlan({FVector2D(0.0, 0.0), FVector2D(4000.0, 0.0),
		FVector2D(4000.0, 4000.0)}), Airframe, Airframe.Ground.Taxi.SpeedCap);

	double WorstSteer = 0.0;
	for (int32 Frame = 0; Frame < 4000; ++Frame)
	{
		FVector2D At = FVector2D::ZeroVector;
		double Heading = 0.0;
		if (!Follower.Advance(NoseGearFrame, Airframe, At, Heading))
		{
			break;
		}
		WorstSteer = FMath::Max(WorstSteer, FMath::Abs(Follower.SteerDegrees));
	}

	TestTrue(FString::Printf(TEXT("steering never passes the lock (worst %.1f)"), WorstSteer),
		WorstSteer <= 45.0 + 0.01);
	TestTrue(TEXT("and the corner did demand full lock, so the clamp was exercised"),
		WorstSteer > 44.0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCornerSpeedIsLateralAccelTest,
	"Airside.Model.CornerSpeedIsLateralAccel",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FCornerSpeedIsLateralAccelTest::RunTest(const FString& Parameters)
{
	// WHY THE TURN IS SLOW is the question this answers. It used to be a flat 10 deg/s,
	// which is not a fact about any aeroplane; it is now sqrt(a*R) - tyre side load and the
	// cabin, which is what actually stops a taxiing aircraft cornering faster.
	const double R = 3000.0;
	FAirframe Airframe = NoseGearTwinOtter();
	Airframe.Ground.MaxLateralAccelUu = 147.0;   // 0.15 g

	FRouteFollower Follower;
	Follower.Start(NoseGearPlan(NoseGearArc(R)), Airframe, 0.0);

	double FastestOnTheStraight = 0.0;
	double FastestInTheArc = 0.0;
	for (int32 Frame = 0; Frame < 6000; ++Frame)
	{
		FVector2D At = FVector2D::ZeroVector;
		double Heading = 0.0;
		if (!Follower.Advance(NoseGearFrame, Airframe, At, Heading))
		{
			break;
		}
		if (At.X < -500.0)
		{
			FastestOnTheStraight = FMath::Max(FastestOnTheStraight, Follower.Speed);
		}
		else if (At.X > 500.0 && At.Y < R - 500.0)
		{
			FastestInTheArc = FMath::Max(FastestInTheArc, Follower.Speed);
		}
	}

	// It got going first, or "slower in the turn" would be true of an aeroplane that never
	// moved.
	TestTrue(FString::Printf(TEXT("it reached taxi speed on the straight (%.0f uu/s)"),
		FastestOnTheStraight), FastestOnTheStraight > Airframe.Ground.Taxi.SpeedCap * 0.9);

	// sqrt(147 * 3000) = 664 uu/s. The old law gave 10 deg/s x 3000 = 524, so this is the
	// turn getting FASTER for a stated reason rather than a raised number.
	const double Expected = FMath::Sqrt(Airframe.Ground.MaxLateralAccelUu * R);
	TestTrue(FString::Printf(TEXT("the arc is taken at about sqrt(a*R) = %.0f, measured %.0f"),
		Expected, FastestInTheArc), FMath::Abs(FastestInTheArc - Expected) < 60.0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCornerTighterThanLockCrawlsTest,
	"Airside.Model.CornerTighterThanLockCrawls",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FCornerTighterThanLockCrawlsTest::RunTest(const FString& Parameters)
{
	// A corner tighter than R = L/sin(lock) cannot be followed AT ANY SPEED - the geometry
	// refuses it, not the pace. The honest answer there is the crawl the model already
	// specifies: "a turn that cannot be made at speed is made at a crawl, which is what a
	// pilot riding the brakes against idle thrust actually does."
	FAirframe Airframe = NoseGearTwinOtter();
	Airframe.Ground.MaxSteerDegrees = 10.0;   // rudder-pedal range: L/sin(10) = 26 m

	const double R = 1000.0;                      // a 10 m radius, far inside that
	const TArray<FVector2D> Tight = NoseGearArc(R);

	// MIDWAY ALONG THE ARC ITSELF, computed rather than taken as half the polyline: the
	// approach leg is much the longest part, so "half the route" is a point on the straight
	// and would have asked the profile about the wrong span entirely.
	const double MidArc = 8000.0 + (HALF_PI * R) * 0.5;

	FSpeedProfile Profile;
	Profile.Build(Tight, Airframe);
	const double Limit = Profile.LimitAt(MidArc);
	TestTrue(FString::Printf(TEXT("an impossible corner crawls (%.0f uu/s)"), Limit),
		Limit <= Airframe.Ground.MinTaxiSpeed + 1.0);

	// AND A FOLLOWABLE ONE DOES NOT, or the assertion above would pass on a profile that
	// crawled everywhere. The same arc at a tiller's 60 degrees is well within the lock.
	FAirframe Tiller = NoseGearTwinOtter();
	FSpeedProfile Roomy;
	Roomy.Build(Tight, Tiller);
	TestTrue(TEXT("the same corner at full tiller is not reduced to a crawl"),
		Roomy.LimitAt(MidArc) > Tiller.Ground.MinTaxiSpeed + 1.0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAuthoredStopPointsDoNotMoveTest,
	"Airside.Model.AuthoredStopPointsDoNotMove",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAuthoredStopPointsDoNotMoveTest::RunTest(const FString& Parameters)
{
	// WHERE AN AGENT COMES TO REST, which this change could have moved in two ways and must
	// not move in the first.
	//
	// StopWithin is a distance RELATIVE to the agent's own route position, re-supplied every
	// tick by the arbiter (FClaimPass::StopWithinFor) - not an absolute mark. So it is
	// driven here the way UGroundTraffic drives it, against a fixed target, or the agent
	// simply runs to the end of its plan with a rolling window in front of it.
	const TArray<FVector2D> Straight{FVector2D(0.0, 0.0), FVector2D(20000.0, 0.0)};
	const double Target = 5000.0;

	auto RunToRest = [&](double SteerAxleX, bool bUseTarget)
	{
		FAirframe Airframe = NoseGearTwinOtter();
		Airframe.SteerAxleX = SteerAxleX;
		Airframe.FixedAxleX = SteerAxleX - 454.3;

		FRouteFollower Follower;
		Follower.Start(NoseGearPlan(Straight), Airframe, 0.0);

		FVector2D At = FVector2D::ZeroVector;
		double Heading = 0.0;
		for (int32 Frame = 0; Frame < 8000; ++Frame)
		{
			const double StopWithin = bUseTarget
				? FMath::Max(0.0, Target - Follower.Travelled)
				: TNumericLimits<double>::Max();
			Follower.Advance(NoseGearFrame, Airframe, StopWithin, At, Heading);
		}
		return TPair<double, double>(At.X, Follower.Travelled);
	};

	// 1. AN ARBITRATED STOP IS HONOURED EXACTLY, and by the STEERED AXLE - which is the
	//    point the arbiter's own arithmetic refers to once both sides use the same body.
	const TPair<double, double> Held = RunToRest(0.0, /*bUseTarget*/ true);
	TestTrue(FString::Printf(TEXT("an arbitrated stop is honoured (travelled %.1f)"),
		Held.Value), FMath::Abs(Held.Value - Target) < 1.0);

	// 2. A CONFORMING AIRFRAME PARKS WHERE IT ALWAYS DID, at the end of its route. This is
	//    the regression guard for every type in the game but one: SteerAxleX is zero, so
	//    the derived origin is the line point untouched.
	const TPair<double, double> Conforming = RunToRest(0.0, /*bUseTarget*/ false);
	TestTrue(FString::Printf(TEXT("a conforming airframe parks its origin at the route end "
		"(%.1f)"), Conforming.Key), FMath::Abs(Conforming.Key - 20000.0) < 1.0);

	// 3. A DECLARED DEVIATION PARKS ITS STEERED AXLE THERE INSTEAD, so its origin sits that
	//    offset short - and that is the intent, not a tolerance. A stand's origin is the
	//    NOSE GEAR STOP MARK (UAircraftType's local space), so the nose gear is the point
	//    that belongs on the mark; the Piper's origin is its main-gear axle and belongs
	//    2.6 m behind it. This is the composition BuildPiperMeridian predicted would be
	//    needed "the day something does" park one on a stand.
	const TPair<double, double> Deviating = RunToRest(260.0, /*bUseTarget*/ false);
	TestTrue(FString::Printf(TEXT("a deviating airframe puts its STEERED axle at the route "
		"end (travelled %.1f)"), Deviating.Value),
		FMath::Abs(Deviating.Value - 20000.0) < 1.0);
	TestTrue(FString::Printf(TEXT("so its origin trails by the declared offset (%.1f)"),
		Deviating.Key), FMath::Abs(Deviating.Key - (20000.0 - 260.0)) < 1.0);

	return true;
}

#endif
