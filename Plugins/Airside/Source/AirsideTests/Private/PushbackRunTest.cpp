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
	 * A stand square to its taxiway: 40 m of lead-in running +X out of the nose-stop, then a
	 * corner onto a taxiway running +Y. The aeroplane parked facing -X, into the terminal,
	 * which is what makes the lead-in run away from it.
	 *
	 * THE ORDINARY LAYOUT, and the one the whole feature is sized for: the correct answer
	 * here is a 90 degree swing, not the 180 an earlier design would have produced.
	 */
	FRoutePlan PushbackPerpendicularPlan()
	{
		FRoutePlan Plan;
		Plan.Result = ERouteResult::Found;
		Plan.Polyline = { {0.0, 0.0}, {4000.0, 0.0}, {4000.0, 8000.0} };
		Plan.Length = GuidelineGeom::PolylineLength(Plan.Polyline);

		// ONE STEP PER LEG, and Steps[0] is the straight lead-in - which is what
		// PlanPushDistance reads. EndDistance is CUMULATIVE route distance, the meaning
		// FRouteStep::EndDistance carries everywhere else.
		FRouteStep LeadIn;
		LeadIn.EndDistance = 4000.0;
		FRouteStep Taxiway;
		Taxiway.EndDistance = Plan.Length;
		Plan.Steps = { LeadIn, Taxiway };
		return Plan;
	}

	/**
	 * A dead-end apron: the taxi out runs back the way the lead-in points, so the tug has to
	 * turn the aeroplane right round. The 180 degree case, which must still work rather than
	 * oscillate about the angle seam.
	 */
	FRoutePlan PushbackDeadEndPlan()
	{
		FRoutePlan Plan;
		Plan.Result = ERouteResult::Found;
		Plan.Polyline = { {0.0, 0.0}, {12000.0, 0.0} };
		Plan.Length = GuidelineGeom::PolylineLength(Plan.Polyline);

		FRouteStep LeadIn;
		LeadIn.EndDistance = 4000.0;
		FRouteStep Onward;
		Onward.EndDistance = Plan.Length;
		Plan.Steps = { LeadIn, Onward };
		return Plan;
	}

	/** Parked facing the terminal: the lead-in leaves along +X, so the body faces -X. */
	constexpr double PushbackParkedHeading = UE_DOUBLE_PI;

	constexpr double PushbackSwingLength = 3000.0;

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
	// 1. THE PUSH RUNS PAST THE CORNER. An earlier draft of the spec stopped it at the end of
	//    the lead-in; the plan's tangent THERE is still the LEAD-IN's, so the aeroplane would
	//    have been handed over facing straight out of its stand and FRouteFollower would have
	//    slewed 90 degrees on the spot - the pirouette this whole feature exists to remove,
	//    merely smaller. This is that defect, measured.
	{
		double Back = 0.0;
		double Push = 0.0;
		double Target = 0.0;
		if (!TestTrue(TEXT("a perpendicular stand can be pushed"),
			FPushbackRun::PlanPushDistance(PushbackPerpendicularPlan(), PushbackSwingLength,
				Back, Push, Target)))
		{
			return false;
		}
		TestEqual(TEXT("Back ends at the end of the lead-in"), Back, 4000.0, 0.01);
		TestEqual(TEXT("and the push runs a swing length past it"), Push, 7000.0, 0.01);

		// +Y, the TAXIWAY - not +X, the lead-in. The whole of item 1 is this line.
		TestEqual(TEXT("the target heading is the taxiway, not the lead-in"),
			PushbackDeltaDegrees(Target, UE_DOUBLE_HALF_PI), 0.0, 0.01);
	}

	// 2. A PERPENDICULAR STAND SWINGS 90 DEGREES, and ends with nothing left for the follower
	//    to slew. Parked facing -X, ending facing +Y.
	{
		FPushbackRun Run;
		if (!TestTrue(TEXT("the run starts"),
			Run.Start(PushbackPerpendicularPlan(), PushbackParkedHeading,
				150.0, 30.0, PushbackSwingLength, false)))
		{
			return false;
		}
		TestEqual(TEXT("it starts facing the way it parked"),
			PushbackDeltaDegrees(Run.Heading, PushbackParkedHeading), 0.0, 0.001);

		FVector2D At = FVector2D::ZeroVector;
		double Heading = 0.0;
		const int32 Frames = PushbackRunToEnd(Run, 20000, At, Heading);
		TestTrue(TEXT("the push ends rather than running for ever"), Frames < 20000);

		TestEqual(TEXT("the swing is 90 degrees, not 180"),
			PushbackDeltaDegrees(PushbackParkedHeading, Run.Heading), 90.0, 0.5);

		// THE WHOLE POINT OF THE FEATURE: the follower inherits no heading error at all.
		TestEqual(TEXT("it ends on the plan's tangent, so nothing is left to slew"),
			PushbackDeltaDegrees(Run.Heading, Run.TargetHeading), 0.0, 0.01);
		TestEqual(TEXT("and it ends where it was cleared to"),
			Run.Travelled, Run.PushDistance, 1.0);

		// IT STOPS DEAD before the handover. The aeroplane is about to reverse its direction
		// of travel; handing the follower a speed would have it pull away forwards at the
		// speed it was just being pushed backwards at.
		TestEqual(TEXT("the push ends at rest"), Run.Speed, 0.0, 0.01);
	}

	// 3. THE BODY IS REVERSED THROUGH THE STRAIGHT. Travelled grows along +X while the body
	//    faces -X: the mains lead and the nose gear trails, which is what a towbar push is.
	{
		FPushbackRun Run;
		Run.Start(PushbackPerpendicularPlan(), PushbackParkedHeading,
			150.0, 30.0, PushbackSwingLength, false);

		FVector2D At = FVector2D::ZeroVector;
		double Heading = 0.0;
		for (int32 Frame = 0; Frame < 600 && Run.Travelled < 2000.0; ++Frame)
		{
			Run.Advance(PushbackFrame, TNumericLimits<double>::Max(), true, At, Heading);
		}

		TestTrue(TEXT("it has moved out along the lead-in"), Run.Travelled > 500.0);
		TestEqual(TEXT("the nose gear is on the line"), At.Y, 0.0, 0.01);
		TestEqual(TEXT("and the body still faces the terminal"),
			PushbackDeltaDegrees(Heading, PushbackParkedHeading), 0.0, 0.01);
		TestEqual(TEXT("Back is still the phase"), Run.Phase, EPushPhase::Back);
	}

	// 4. A DEAD-END APRON TURNS IT RIGHT ROUND - 180 degrees, and that is correct rather than
	//    a bug: the taxi out runs back the way the lead-in points, so the tug has to.
	{
		FPushbackRun Run;
		Run.Start(PushbackDeadEndPlan(), PushbackParkedHeading,
			150.0, 30.0, PushbackSwingLength, false);

		FVector2D At = FVector2D::ZeroVector;
		double Heading = 0.0;
		PushbackRunToEnd(Run, 20000, At, Heading);

		TestEqual(TEXT("a dead end swings the full half turn"),
			PushbackDeltaDegrees(PushbackParkedHeading, Run.Heading), 180.0, 0.5);
	}

	// 5. A POWERBACK WAITS FOR THRUST; A TUG DOES NOT. The engine is doing the work in one
	//    case and not in the other. This is the ONE place in slice 1 where the pushback need
	//    changes anything, and it is justified because it is a fact about the aeroplane's
	//    physics rather than about a tug that does not exist yet.
	{
		FPushbackRun Powerback;
		Powerback.Start(PushbackPerpendicularPlan(), PushbackParkedHeading,
			200.0, 30.0, PushbackSwingLength, /*bNeedsThrust*/ true);

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
		TestEqual(TEXT("but it is posed at the stand while it waits"), At.X, 0.0, 0.01);
		TestEqual(TEXT("facing the way it parked"),
			PushbackDeltaDegrees(Heading, PushbackParkedHeading), 0.0, 0.01);

		Powerback.Advance(PushbackFrame, TNumericLimits<double>::Max(), true, At, Heading);
		TestTrue(TEXT("and it moves the frame it has thrust"), Powerback.Travelled > 0.0);

		FPushbackRun Towed;
		Towed.Start(PushbackPerpendicularPlan(), PushbackParkedHeading,
			150.0, 30.0, PushbackSwingLength, /*bNeedsThrust*/ false);
		Towed.Advance(PushbackFrame, TNumericLimits<double>::Max(), false, At, Heading);
		TestTrue(TEXT("a towed aeroplane moves on frame one whatever the propeller is doing"),
			Towed.Travelled > 0.0);
	}

	// 6. ARBITRATION IS THE ONE INPUT, exactly as it is for the follower: a push held short
	//    stops where it is told and does not creep past it.
	{
		FPushbackRun Run;
		Run.Start(PushbackPerpendicularPlan(), PushbackParkedHeading,
			150.0, 30.0, PushbackSwingLength, false);

		FVector2D At = FVector2D::ZeroVector;
		double Heading = 0.0;
		for (int32 Frame = 0; Frame < 2000; ++Frame)
		{
			Run.Advance(PushbackFrame, /*StopWithin*/ 1000.0, true, At, Heading);
		}

		TestEqual(TEXT("a push stops where arbitration says"), Run.Travelled, 1000.0, 1.0);
		TestFalse(TEXT("and has not arrived, so it is still the driving phase"), Run.HasArrived());
	}

	// 7. AN UNPUSHABLE PLAN IS REFUSED AND LEAVES NOTHING HALF-ARMED - the rule
	//    FRoadAgent::StartArrival already follows for a landing that cannot be flown.
	{
		FRoutePlan Empty;
		double Back = -1.0;
		double Push = -1.0;
		double Target = -1.0;
		TestFalse(TEXT("a plan with no steps cannot be pushed"),
			FPushbackRun::PlanPushDistance(Empty, PushbackSwingLength, Back, Push, Target));
		TestEqual(TEXT("and the outputs are untouched"), Back, -1.0, 0.0001);

		FPushbackRun Run;
		TestFalse(TEXT("Start declines it"),
			Run.Start(Empty, PushbackParkedHeading, 150.0, 30.0, PushbackSwingLength, false));
		TestEqual(TEXT("and arms nothing"), Run.PushDistance, 0.0, 0.0001);
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
