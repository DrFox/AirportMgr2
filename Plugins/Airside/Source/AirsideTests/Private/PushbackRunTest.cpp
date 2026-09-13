#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/GroundTraffic.h"
#include "Model/PushbackRun.h"
#include "Model/RouteSearch.h"
#include "Solve/GuidelineGeom.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// Prefixed against the UNITY build - these test files share one translation unit.

	constexpr double PushbackFrame = 1.0 / 60.0;

	/** Shortest angle between two headings, degrees, unsigned. */
	double PushbackDeltaDegrees(double FromRadians, double ToRadians)
	{
		return FMath::Abs(FMath::RadiansToDegrees(
			FMath::UnwindRadians(ToRadians - FromRadians)));
	}

	/**
	 * THE LAYOUT FROM THE REPORT, laid out the way the photographs show it.
	 *
	 * The stand is at the origin with the aeroplane facing NORTH into the terminal. Its
	 * lead-in runs SOUTH to a taxiway along y = -4000. The departure taxis WEST, so the push
	 * must reverse onto the EAST arm - finishing east of the junction and facing west, which
	 * is exactly reversePathWanted5.png.
	 *
	 * THIS IS A PUSH ROUTE, not a departure route: PushbackPlanner::Plan produces it, and the
	 * distinction is the whole correction. An earlier version walked a prefix of the DEPARTURE
	 * route, which runs the other way, so the aeroplane finished WEST of the junction having
	 * reversed along the very arm it was supposed to taxi out along - reversePath8.png.
	 */
	FRoutePlan PushbackEastArmPlan()
	{
		FRoutePlan Plan;
		Plan.Result = ERouteResult::Found;
		Plan.Polyline = { {0.0, 0.0}, {0.0, -4000.0}, {8000.0, -4000.0} };
		Plan.Length = GuidelineGeom::PolylineLength(Plan.Polyline);

		FRouteStep LeadIn;
		LeadIn.EndDistance = 4000.0;
		FRouteStep Taxiway;
		Taxiway.EndDistance = Plan.Length;
		Plan.Steps = { LeadIn, Taxiway };
		return Plan;
	}

	/** Facing north, into the terminal: the lead-in runs away from it, southward. */
	constexpr double PushbackParkedHeading = UE_DOUBLE_HALF_PI;

	/** Where the aeroplane must finish facing: west, the way it will taxi out. */
	constexpr double PushbackTaxiOutHeading = UE_DOUBLE_PI;

	/** Drives a run to completion, or until Frames runs out. Returns the frames used. */
	int32 PushbackRunToEnd(FPushbackRun& Run, int32 Frames, FVector2D& OutAt, double& OutHeading)
	{
		for (int32 Frame = 0; Frame < Frames; ++Frame)
		{
			if (!Run.Advance(PushbackFrame, TNumericLimits<double>::Max(), true, OutAt, OutHeading))
			{
				return Frame;
			}
		}
		return Frames;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPushbackRunTest,
	"Airside.Model.PushbackRun",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPushbackRunTest::RunTest(const FString& Parameters)
{
	// 1. IT STARTS FACING THE WAY IT PARKED, before any frame is advanced - the body is the
	//    line's tangent turned about, and at the stand that IS the parked heading. An
	//    aeroplane facing out along its own lead-in for one frame is the flicker this seeding
	//    exists to prevent.
	{
		FPushbackRun Run;
		if (!TestTrue(TEXT("the run starts"),
			Run.Start(PushbackEastArmPlan(), 150.0, 30.0, false)))
		{
			return false;
		}
		TestEqual(TEXT("it starts facing the way it parked, into the terminal"),
			PushbackDeltaDegrees(Run.Heading, PushbackParkedHeading), 0.0, 0.001);
	}

	// 2. THE REPORTED DEFECT, measured: where it finishes, and facing where.
	//
	//    reversePath8.png put the aeroplane WEST of the junction - already past the turn it
	//    was about to make - because the push walked the departure route, which goes west.
	//    reversePathWanted5.png puts it EAST of the junction facing west, so that driving
	//    forward carries it through the junction and away. That is a fact about WHICH LINE the
	//    push walks, and these assertions are the difference between the two photographs.
	{
		FPushbackRun Run;
		Run.Start(PushbackEastArmPlan(), 150.0, 30.0, false);

		FVector2D At = FVector2D::ZeroVector;
		double Heading = 0.0;
		const int32 Frames = PushbackRunToEnd(Run, 40000, At, Heading);
		TestTrue(TEXT("the push ends rather than running for ever"), Frames < 40000);

		TestTrue(FString::Printf(TEXT("it finishes EAST of the junction (x = %.0f)"), At.X),
			At.X > 1000.0);
		TestEqual(TEXT("on the taxiway"), At.Y, -4000.0, 1.0);

		TestEqual(TEXT("facing the way it will taxi out - west, not along its lead-in"),
			PushbackDeltaDegrees(Run.Heading, PushbackTaxiOutHeading), 0.0, 0.5);

		// A 90 DEGREE TURN from where it parked, not the 180 an aeroplane makes when it
		// reverses along its own departure arm.
		TestEqual(TEXT("which is 90 degrees off the parked heading"),
			PushbackDeltaDegrees(PushbackParkedHeading, Run.Heading), 90.0, 0.5);

		TestEqual(TEXT("and it has run the whole route"), Run.Travelled, Run.Plan.Length, 1.0);

		// IT STOPS DEAD before the handover. The aeroplane is about to reverse its direction
		// of travel; handing the follower a speed would have it pull away forwards at the
		// speed it was just being pushed backwards at.
		TestEqual(TEXT("the push ends at rest"), Run.Speed, 0.0, 0.01);
	}

	// 3. THE BODY IS REVERSED THROUGH THE STRAIGHT, and the nose gear stays on the line. The
	//    aeroplane travels SOUTH down the lead-in while facing NORTH: the mains lead and the
	//    nose gear trails, which is what a towbar push is.
	{
		FPushbackRun Run;
		Run.Start(PushbackEastArmPlan(), 150.0, 30.0, false);

		FVector2D At = FVector2D::ZeroVector;
		double Heading = 0.0;
		for (int32 Frame = 0; Frame < 600 && Run.Travelled < 2000.0; ++Frame)
		{
			Run.Advance(PushbackFrame, TNumericLimits<double>::Max(), true, At, Heading);
		}

		TestTrue(TEXT("it has moved out along the lead-in"), Run.Travelled > 500.0);
		TestTrue(TEXT("southward, away from the terminal"), At.Y < -100.0);
		TestEqual(TEXT("the nose gear is on the line"), At.X, 0.0, 0.01);
		TestEqual(TEXT("and the body still faces the terminal"),
			PushbackDeltaDegrees(Heading, PushbackParkedHeading), 0.0, 0.01);
	}

	// 4. A POWERBACK WAITS FOR THRUST; A TUG DOES NOT. The engine is doing the work in one
	//    case and not the other. This is the ONE place in this slice where the pushback need
	//    changes anything, and it is justified because it is a fact about the aeroplane's
	//    physics rather than about a tug that does not exist yet.
	{
		FPushbackRun Powerback;
		Powerback.Start(PushbackEastArmPlan(), 200.0, 30.0, /*bNeedsThrust*/ true);

		FVector2D At = FVector2D::ZeroVector;
		double Heading = 0.0;
		for (int32 Frame = 0; Frame < 120; ++Frame)
		{
			Powerback.Advance(PushbackFrame, TNumericLimits<double>::Max(),
				/*bHasThrust*/ false, At, Heading);
		}
		TestEqual(TEXT("a powerback without thrust has not moved"),
			Powerback.Travelled, 0.0, 0.0001);

		// AND IT IS POSED WHILE IT WAITS, rather than left at an unset FVector2D - the
		// "world origin" failure this project keeps rediscovering.
		TestEqual(TEXT("but it is posed on its stand while it waits"), At.Y, 0.0, 0.01);
		TestEqual(TEXT("facing the way it parked"),
			PushbackDeltaDegrees(Heading, PushbackParkedHeading), 0.0, 0.01);

		Powerback.Advance(PushbackFrame, TNumericLimits<double>::Max(), true, At, Heading);
		TestTrue(TEXT("and it moves the frame it has thrust"), Powerback.Travelled > 0.0);

		FPushbackRun Towed;
		Towed.Start(PushbackEastArmPlan(), 150.0, 30.0, /*bNeedsThrust*/ false);
		Towed.Advance(PushbackFrame, TNumericLimits<double>::Max(), false, At, Heading);
		TestTrue(TEXT("a towed aeroplane moves on frame one whatever the propeller is doing"),
			Towed.Travelled > 0.0);
	}

	// 5. ARBITRATION IS THE ONE INPUT, exactly as it is for the follower: a push held short
	//    stops where it is told and does not creep past it.
	{
		FPushbackRun Run;
		Run.Start(PushbackEastArmPlan(), 150.0, 30.0, false);

		FVector2D At = FVector2D::ZeroVector;
		double Heading = 0.0;
		for (int32 Frame = 0; Frame < 2000; ++Frame)
		{
			Run.Advance(PushbackFrame, /*StopWithin*/ 1000.0, true, At, Heading);
		}

		TestEqual(TEXT("a push stops where arbitration says"), Run.Travelled, 1000.0, 1.0);
		TestFalse(TEXT("and has not arrived, so it is still the driving phase"), Run.HasArrived());
	}

	// 6. AN UNUSABLE ROUTE IS REFUSED AND LEAVES NOTHING HALF-ARMED - the rule
	//    FRoadAgent::StartArrival already follows for a landing that cannot be flown.
	{
		FPushbackRun Run;
		TestFalse(TEXT("an invalid plan is declined"),
			Run.Start(FRoutePlan(), 150.0, 30.0, false));
		TestEqual(TEXT("and arms nothing"), Run.Plan.Length, 0.0, 0.0001);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPushSpeedsTest,
	"Airside.Model.PushSpeeds",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPushSpeedsTest::RunTest(const FString& Parameters)
{
	// FTrafficRules::PushSpeedFor IS THE ONE CONSUMER THAT MUST AGREE WITH EPushbackNeed -
	// the codebase's "lists that must agree are ONE list", applied to arithmetic. A value
	// added to the enum without a case there is a compiler warning rather than a silent zero,
	// and this pins that the mapping is the one the field comments claim.
	//
	// THE ORDERING IS THE ASSERTION, not the figures. The three numbers are invented and will
	// be retuned against what the player actually sees; what must not change without somebody
	// meaning it is that a HAND tug is the slowest thing on the apron and a powerback the
	// quickest, because that ordering is what the Pushback depot's upgrade rung is FOR.
	const FTrafficRules Rules;

	TestEqual(TEXT("a hand tug pushes at its authored speed"),
		Rules.PushSpeedFor(EPushbackNeed::HandTug), Rules.HandTugPushSpeed, 0.001);
	TestEqual(TEXT("a tug vehicle at its own"),
		Rules.PushSpeedFor(EPushbackNeed::VehicleTug), Rules.VehicleTugPushSpeed, 0.001);
	TestEqual(TEXT("and a powerback at its own"),
		Rules.PushSpeedFor(EPushbackNeed::SelfManoeuvre), Rules.SelfManoeuvrePushSpeed, 0.001);

	TestTrue(TEXT("a hand tug is slower than a tug vehicle"),
		Rules.PushSpeedFor(EPushbackNeed::HandTug)
			< Rules.PushSpeedFor(EPushbackNeed::VehicleTug));
	TestTrue(TEXT("and a tug vehicle slower than an aeroplane under its own power"),
		Rules.PushSpeedFor(EPushbackNeed::VehicleTug)
			< Rules.PushSpeedFor(EPushbackNeed::SelfManoeuvre));

	// THE THREE ARE DISTINCT. Two that happened to be equal would let PushSpeedFor return the
	// wrong one of them and still pass every assertion above.
	TestTrue(TEXT("no two push speeds are the same figure"),
		Rules.HandTugPushSpeed != Rules.VehicleTugPushSpeed
			&& Rules.VehicleTugPushSpeed != Rules.SelfManoeuvrePushSpeed);

	return true;
}

#endif
