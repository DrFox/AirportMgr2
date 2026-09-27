#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Entities/AircraftType.h"
#include "Misc/AutomationTest.h"
#include "Model/RouteFollower.h"
#include "Model/RouteSearch.h"
#include "Solve/GuidelineGeom.h"
#include "Solve/RoadGeom.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// Prefixed against the UNITY build - these test files share one translation unit.

	constexpr double FinalTurnFrame = 1.0 / 60.0;

	/**
	 * A Code F stand lead-in as the anchor link lays one: a long taxiway approach along +X,
	 * a quarter circle at the letter's painted 6000 uu (IcaoCode's StandTurnRadius for F),
	 * and the straight to the stop mark along +Y.
	 *
	 * THE FINAL STRAIGHT IS 5000 uu, not the ~7500 a drawn F stand's setback gives, because
	 * it is the case the report was about: at 50 m nose tracking leaves about 6.6 degrees
	 * (simulated 2026-09-27), well clear of anything a tolerance could hide, and it is still
	 * longer than a wheelbase plus the easing window, so the fixed axle has room to finish square.
	 */
	FRoutePlan FinalTurnPlan(double FinalStraight)
	{
		const double R = 6000.0;
		TArray<FVector2D> Points;
		Points.Add(FVector2D(-20000.0, 0.0));
		for (int32 Step = 0; Step <= 48; ++Step)
		{
			const double Angle = -HALF_PI + (HALF_PI * Step) / 48.0;
			Points.Add(FVector2D(R * FMath::Cos(Angle), R + R * FMath::Sin(Angle)));
		}
		Points.Add(FVector2D(R, R + FinalStraight));

		FRoutePlan Plan;
		Plan.Result = ERouteResult::Found;
		Plan.Polyline = Points;
		Plan.Length = GuidelineGeom::PolylineLength(Points);
		return Plan;
	}

	/**
	 * An EARLIER turn close before the last one: left through 90 degrees, a straight of
	 * Between, then right through 90 onto the stand. Both at the Code F 6000 uu.
	 *
	 * The point is the heading the nose law hands over with. After the first turn it lags by
	 * asin(L/R) = 33 degrees and has decayed only to about 7 by the end of a 1.5-wheelbase
	 * straight - which the fixed-axle law must absorb without a jump.
	 */
	FRoutePlan FinalTurnAfterAnotherPlan(double Between)
	{
		const double R = 6000.0;
		TArray<FVector2D> Points;
		Points.Add(FVector2D(-20000.0, 0.0));
		for (int32 Step = 0; Step <= 48; ++Step)
		{
			const double Angle = -HALF_PI + (HALF_PI * Step) / 48.0;
			Points.Add(FVector2D(R * FMath::Cos(Angle), R + R * FMath::Sin(Angle)));
		}
		// Up +Y at x = R, then right about (2R, R + Between) back round to +X.
		for (int32 Step = 0; Step <= 48; ++Step)
		{
			const double Angle = PI - (HALF_PI * Step) / 48.0;
			Points.Add(FVector2D(2.0 * R + R * FMath::Cos(Angle), R + Between + R * FMath::Sin(Angle)));
		}
		Points.Add(FVector2D(2.0 * R + 5000.0, 2.0 * R + Between));

		FRoutePlan Plan;
		Plan.Result = ERouteResult::Found;
		Plan.Polyline = Points;
		Plan.Length = GuidelineGeom::PolylineLength(Points);
		return Plan;
	}

	/**
	 * plane8's figures as build_aircraft_type.py measured them off the rig (2026-09-27 log:
	 * "steer axle 0.0, fixed axle -3246.1 uu") and as aircraft/plane8.py authors its steering:
	 * 70 degrees of lock, 0.12 g.
	 */
	FChassis FinalTurnA380(EFinalTurnAxle Axle)
	{
		FChassis Chassis = TestAirframes::Piper().Chassis;
		Chassis.SteerLaw = ESteerLaw::RollingSteer;
		Chassis.SteerAxleX = 0.0;
		Chassis.FixedAxleX = -3246.1;
		Chassis.Ground.MaxSteerDegrees = 70.0;
		Chassis.Ground.MaxLateralAccelUu = 118.0;
		Chassis.FinalTurnAxle = Axle;
		return Chassis;
	}

	struct FFinalTurnRun
	{
		double ParkedHeadingErrorDeg = 0.0;
		double ParkedOffStopMark = 0.0;
		double WorstMainsSlipDeg = 0.0;
		double WorstSteerDeg = 0.0;
		double WorstSteerStepDeg = 0.0;
		FString WorstSteerStepWhere;
		double WorstNoseOutside = 0.0;
		double WorstFrameMove = 0.0;
		bool bArrived = false;
	};

	/** Drives the plan to the stop and measures the gear, the heading and the steering. */
	FFinalTurnRun DriveFinalTurn(const FRoutePlan& Plan, const FChassis& Chassis)
	{
		FFinalTurnRun Run;
		FRouteFollower Follower;
		Follower.Start(Plan, Chassis);

		double LastSteer = 0.0;
		TOptional<FVector2D> LastMains;
		TOptional<FVector2D> LastOrigin;
		FVector2D Origin = FVector2D::ZeroVector;
		double Heading = 0.0;
		for (int32 Frame = 0; Frame < 60 * 600; ++Frame)
		{
			if (!Follower.Advance(FinalTurnFrame, Chassis, Origin, Heading))
			{
				break;
			}

			// THE MAINS' SLIP: the angle between where they went this frame and where the body
			// points. Not their distance from the line - under the fixed-axle law they are PLACED
			// on it, so that would pass on any heading at all. Slip is what the no-crab promise
			// is about, and it is where easing the steering in has to pay.
			const FVector2D Mains = RoadGeom::TrailPoint(Origin, Heading, Chassis.FixedAxleX);
			if (LastMains.IsSet())
			{
				const FVector2D Moved = Mains - LastMains.GetValue();
				if (Moved.Size() > 1.0)
				{
					Run.WorstMainsSlipDeg = FMath::Max(Run.WorstMainsSlipDeg, FMath::RadiansToDegrees(
						FMath::Abs(FMath::UnwindRadians(FMath::Atan2(Moved.Y, Moved.X) - Heading))));
				}
			}
			LastMains = Mains;

			// NO JUMP: how far the origin moved in one frame. At the 1000 uu/s taxi cap that is
			// 17 uu, a little more for a nose swinging outside the mains; a law handover that
			// placed the body anywhere is a metre in a frame.
			if (LastOrigin.IsSet())
			{
				Run.WorstFrameMove = FMath::Max(Run.WorstFrameMove, FVector2D::Distance(Origin, LastOrigin.GetValue()));
			}
			LastOrigin = Origin;

			// The nose OUTSIDE the arc is what oversteer looks like - measured so a pass cannot
			// come from something that merely stopped square by another route.
			if (Origin.X > 0.0 && Origin.Y < 6000.0)
			{
				Run.WorstNoseOutside = FMath::Max(Run.WorstNoseOutside,
					FVector2D::Distance(Origin, FVector2D(0.0, 6000.0)) - 6000.0);
			}

			Run.WorstSteerDeg = FMath::Max(Run.WorstSteerDeg, FMath::Abs(Follower.SteerDegrees));
			if (FMath::Abs(Follower.SteerDegrees - LastSteer) > Run.WorstSteerStepDeg)
			{
				Run.WorstSteerStepDeg = FMath::Abs(Follower.SteerDegrees - LastSteer);
				Run.WorstSteerStepWhere = FString::Printf(TEXT("at %.0f of %.0f uu, %.2f -> %.2f deg, %.0f uu/s, final turn from %.0f"),
					Follower.Travelled, Plan.Length, LastSteer, Follower.SteerDegrees, Follower.Speed, Follower.FinalTurnFrom);
			}
			LastSteer = Follower.SteerDegrees;

			if (Follower.HasArrived())
			{
				Run.bArrived = true;
				break;
			}
		}

		// Against the plan's own last span: the stand's axis, whichever way the stand faces.
		const FVector2D LastSpan = Plan.Polyline.Last() - Plan.Polyline.Last(1);
		Run.ParkedHeadingErrorDeg = FMath::RadiansToDegrees(
			FMath::Abs(FMath::UnwindRadians(Heading - FMath::Atan2(LastSpan.Y, LastSpan.X))));
		Run.ParkedOffStopMark = FVector2D::Distance(Origin, Plan.Polyline.Last());
		return Run;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFinalTurnParksSquareTest,
	"Airside.Model.FinalTurnParksSquare",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFinalTurnParksSquareTest::RunTest(const FString& Parameters)
{
	// THE REPORT, PINNED: "our a380 fit on the F stand but it didnt line itself straight in
	// the stand, it was at an angle" (2026-09-27). With the mains holding the line through
	// the last turn the heading is the line's own at the stop.
	const FRoutePlan Plan = FinalTurnPlan(5000.0);
	const FChassis Chassis = FinalTurnA380(EFinalTurnAxle::Fixed);
	const FFinalTurnRun Run = DriveFinalTurn(Plan, Chassis);

	TestTrue(TEXT("the aircraft reached its stop mark"), Run.bArrived);

	TestTrue(FString::Printf(TEXT("parked square on the stand (%.2f deg off)"), Run.ParkedHeadingErrorDeg),
		Run.ParkedHeadingErrorDeg < 0.5);

	// The NOSE still stops on the mark: the stop is where the stand says the nose gear goes,
	// and oversteer changes how it got there, not where it ends up.
	TestTrue(FString::Printf(TEXT("the nose gear stopped on the stop mark (%.1f uu off)"), Run.ParkedOffStopMark),
		Run.ParkedOffStopMark < 5.0);

	// THE MAINS ROLL, THEY DO NOT SCRUB. The window that eases the steering in turns the body a
	// little ahead of the line's tangent at each end of the arc - about h / 6R radians, 1.3
	// degrees for a quarter-wheelbase half-window on a 60 m bend - and nowhere else. The
	// straights and the arc's middle are exact, so anything above this is a derivation bug.
	TestTrue(FString::Printf(TEXT("the mains rolled with the body (worst slip %.2f deg)"), Run.WorstMainsSlipDeg),
		Run.WorstMainsSlipDeg < 3.0);

	// OVERSTEER, not a coincidence: the nose ran outside the curve. At R = 6000 and L = 3246
	// the steady figure is sqrt(R^2 + L^2) - R, 822 uu.
	TestTrue(FString::Printf(TEXT("the nose swung outside the line (by %.0f uu)"), Run.WorstNoseOutside),
		Run.WorstNoseOutside > 600.0);

	// NO CRAB: within the lock throughout. atan(L/R) is 28 degrees here.
	TestTrue(FString::Printf(TEXT("steering stayed inside the 70 deg lock (peak %.1f)"), Run.WorstSteerDeg),
		Run.WorstSteerDeg < Chassis.Ground.MaxSteerDegrees);

	// NO SNAP: the nosewheel is wound on, not thrown. A bare tangent would put all 28 degrees
	// on in the one frame the mains crossed onto the arc.
	TestTrue(FString::Printf(TEXT("the nosewheel never jumped more than 1 deg in a frame (worst %.2f %s)"),
		Run.WorstSteerStepDeg, *Run.WorstSteerStepWhere), Run.WorstSteerStepDeg < 1.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFinalTurnNoseTrackingLagsTest,
	"Airside.Model.FinalTurnNoseTrackingLags",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFinalTurnNoseTrackingLagsTest::RunTest(const FString& Parameters)
{
	// THE CONTROL: the same lead-in with the nose on the line must leave the aircraft
	// crooked, or FinalTurnParksSquare's 0.5 degrees measures a geometry that was always
	// square. Simulated at 6.6 degrees; asserted above 3 so a change of sampling cannot
	// quietly make this lead-in forgiving.
	const FFinalTurnRun Run = DriveFinalTurn(FinalTurnPlan(5000.0), FinalTurnA380(EFinalTurnAxle::Steered));

	TestTrue(TEXT("the aircraft reached its stop mark"), Run.bArrived);
	TestTrue(FString::Printf(TEXT("nose tracking parks crooked (%.2f deg off)"), Run.ParkedHeadingErrorDeg),
		Run.ParkedHeadingErrorDeg > 3.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFinalTurnShortStraightFallsBackTest,
	"Airside.Model.FinalTurnShortStraightFallsBack",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFinalTurnShortStraightFallsBackTest::RunTest(const FString& Parameters)
{
	// A FINAL STRAIGHT SHORTER THAN THE WHEELBASE cannot be finished on the mains: the nose
	// would stop off the line, short of or beside the mark. The follower must keep the nose
	// on the line there instead, and this is the check that it does - the nose ends ON the
	// stop mark and ON the line the whole way.
	const FRoutePlan Plan = FinalTurnPlan(2000.0);
	const FChassis Chassis = FinalTurnA380(EFinalTurnAxle::Fixed);

	FRouteFollower Follower;
	Follower.Start(Plan, Chassis);
	double WorstNoseOffLine = 0.0;
	FVector2D Origin = FVector2D::ZeroVector;
	double Heading = 0.0;
	for (int32 Frame = 0; Frame < 60 * 600 && !Follower.HasArrived(); ++Frame)
	{
		if (!Follower.Advance(FinalTurnFrame, Chassis, Origin, Heading))
		{
			break;
		}
		int32 Index = 0;
		double Fraction = 0.0;
		WorstNoseOffLine = FMath::Max(WorstNoseOffLine,
			GuidelineGeom::NearestOnPolyline(Plan.Polyline, Origin, Index, Fraction));
	}

	TestTrue(TEXT("the aircraft reached its stop mark"), Follower.HasArrived());
	TestTrue(FString::Printf(TEXT("the nose held the line on a short straight (worst %.1f uu)"), WorstNoseOffLine),
		WorstNoseOffLine < 1.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFinalTurnHandoverIsSmoothTest,
	"Airside.Model.FinalTurnHandoverIsSmooth",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFinalTurnHandoverIsSmoothTest::RunTest(const FString& Parameters)
{
	// THE SEAM between the two laws, pinned: a handover while an earlier turn's lag is still
	// in the heading must neither jump the body nor throw the nosewheel, and must still park
	// square. Between is 1.5 wheelbases - long enough to arm (a wheelbase and a quarter), short
	// enough that the lag is still several degrees when the mains take over.
	const FChassis Chassis = FinalTurnA380(EFinalTurnAxle::Fixed);
	const FFinalTurnRun Run = DriveFinalTurn(FinalTurnAfterAnotherPlan(1.5 * Chassis.Wheelbase()), Chassis);

	TestTrue(TEXT("the aircraft reached its stop mark"), Run.bArrived);
	TestTrue(FString::Printf(TEXT("parked square after two turns (%.2f deg off)"), Run.ParkedHeadingErrorDeg),
		Run.ParkedHeadingErrorDeg < 0.5);
	TestTrue(FString::Printf(TEXT("no frame moved the body more than 25 uu (worst %.1f)"), Run.WorstFrameMove),
		Run.WorstFrameMove < 25.0);
	TestTrue(FString::Printf(TEXT("the nosewheel never jumped more than 1.5 deg in a frame (worst %.2f %s)"),
		Run.WorstSteerStepDeg, *Run.WorstSteerStepWhere), Run.WorstSteerStepDeg < 1.5);
	TestTrue(FString::Printf(TEXT("the mains rolled with the body (worst slip %.2f deg)"), Run.WorstMainsSlipDeg),
		Run.WorstMainsSlipDeg < 3.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFinalTurnAircraftTypeParksSquareTest,
	"Airside.Model.FinalTurnAircraftTypeParksSquare",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFinalTurnAircraftTypeParksSquareTest::RunTest(const FString& Parameters)
{
	// THE WIRING, one level up: an AUTHORED TYPE's airframe must reach the follower with the
	// fixed axle on its last turn. Every test above builds its chassis by hand, so without this
	// one UAircraftType::Airframe() could drop the setting and all of them would stay green
	// while every aircraft in the game parked crooked again.
	//
	// The Piper's own 2.4 m wheelbase parks square either way on this lead-in - its lag is
	// asin(238/6000), 2.3 degrees, and gone within a few metres - so the type is given the
	// A380's axle and lock first. The figures under test are the TYPE's, read back through
	// Airframe(), exactly as the game reads them.
	UAircraftType* Type = TestAirframes::PiperType();
	Type->SteerAxleX = 0.0;
	Type->FixedAxleX = -3246.1;
	Type->Ground.MaxSteerDegrees = 70.0;
	const FChassis Chassis = Type->Airframe().Chassis;

	TestTrue(TEXT("an aircraft type's airframe takes its last turn on the main gear"),
		Chassis.FinalTurnAxle == EFinalTurnAxle::Fixed);

	const FFinalTurnRun Run = DriveFinalTurn(FinalTurnPlan(5000.0), Chassis);
	TestTrue(TEXT("the aircraft reached its stop mark"), Run.bArrived);
	TestTrue(FString::Printf(TEXT("an authored type parks square (%.2f deg off)"), Run.ParkedHeadingErrorDeg),
		Run.ParkedHeadingErrorDeg < 0.5);
	return true;
}

#endif
