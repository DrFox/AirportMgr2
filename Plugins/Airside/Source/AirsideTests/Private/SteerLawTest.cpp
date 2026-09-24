#include "AirsideTestFixtures.h"
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RouteFollower.h"
#include "Model/RouteSearch.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSteerLawWithoutAxlesFallsBackTest,
	"Airside.Model.SteerLawWithoutAxlesFallsBack",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSteerLawWithoutAxlesFallsBackTest::RunTest(const FString& Parameters)
{
	// THE BUG THIS ENUM EXISTS TO MAKE IMPOSSIBLE. The law used to be INFERRED from whether
	// anyone had measured the axles - FChassis::HasAxles() was the selector - so forgetting
	// to measure a vehicle silently swapped which physical law governed it. Reported from
	// play 2026-09-14: "the truck drives up to the stand, stops, swings 90 degrees on the
	// spot and drives off." That is the pivot law, running on something nobody meant to
	// pivot.
	//
	// Declaring the law and measuring the axles are now two statements that must agree, and
	// the consumer checks rather than assumes - CLAUDE.md's "lists that must agree are ONE
	// list", applied where UE forces two.
	// THE ERROR IS THE POINT, so it is declared rather than silenced. The automation harness
	// fails any test that logs at Error, which is exactly the behaviour that makes the
	// fallback worth having - a mis-declared airframe cannot slip past a test run unnoticed.
	// Expecting it here asserts that it IS emitted; delete this line and the test fails.
	AddExpectedError(
		TEXT("declares RollingSteer with no wheelbase"),
		EAutomationExpectedErrorFlags::Contains, 1);

	FAirframe Unmeasured;
	Unmeasured.Chassis.SteerLaw = ESteerLaw::RollingSteer;
	Unmeasured.Chassis.SteerAxleX = 0.0;
	Unmeasured.Chassis.FixedAxleX = 0.0;

	// #176: EffectiveSteerLaw is PURE now - it no longer logs from here, because it sat on
	// FRouteFollower::Advance's hot path and logged twice per agent per SUBSTEP. The warning
	// is WarnIfSteerLawUnsupported's job, called once per dispatch by the structs that bind
	// an airframe to a phase; asked for explicitly here so this test still pins that the
	// warning IS emitted for exactly this airframe, which is what the AddExpectedError above
	// is checking.
	WarnIfSteerLawUnsupported(Unmeasured.Chassis);

	TestEqual(
		TEXT("an airframe claiming to steer geometrically with no wheelbase falls back to pivot"),
		Unmeasured.Chassis.EffectiveSteerLaw(), ESteerLaw::Pivot);

	FAirframe Measured;
	Measured.Chassis.SteerLaw = ESteerLaw::RollingSteer;
	Measured.Chassis.SteerAxleX = 360.0;
	Measured.Chassis.FixedAxleX = 0.0;

	TestEqual(
		TEXT("and one with real axles keeps the law it declared"),
		Measured.Chassis.EffectiveSteerLaw(), ESteerLaw::RollingSteer);

	// A PIVOT AIRFRAME IS NOT PROMOTED BY ACCIDENT. Measured axles on something declared
	// Pivot must STAY Pivot - otherwise the enum is decoration and the wheelbase is still
	// the selector, which is exactly the arrangement this replaces. A belt loader that
	// happens to have its axles filled in must not silently start steering like a truck.
	FAirframe DeliberatePivot;
	DeliberatePivot.Chassis.SteerLaw = ESteerLaw::Pivot;
	DeliberatePivot.Chassis.SteerAxleX = 360.0;
	DeliberatePivot.Chassis.FixedAxleX = 0.0;

	TestEqual(
		TEXT("measured axles do not promote an airframe that declared itself a pivot"),
		DeliberatePivot.Chassis.EffectiveSteerLaw(), ESteerLaw::Pivot);

	// AND THE DEFAULT IS THE SAFE ONE. An airframe nobody has authored at all is exactly the
	// one that must not claim to steer geometrically: TightestFollowableRadius would divide
	// by a zero wheelbase and FRouteFollower would yaw without bound.
	const FAirframe Unauthored;
	TestEqual(
		TEXT("an unauthored airframe defaults to the law that needs no measurements"),
		Unauthored.Chassis.EffectiveSteerLaw(), ESteerLaw::Pivot);

	return true;
}

/**
 * #176: EffectiveSteerLaw sat on FRouteFollower::Advance's hot path and logged an Error from
 * there - TWICE PER AGENT PER SUBSTEP, about 3800 lines/s at 60 fps across a busy apron for
 * one mis-authored type, the "log stops being read" failure the traffic code elsewhere
 * throttles carefully. Moving the log to Start (see FRouteFollower::Start) is only a fix if
 * Advance itself stays silent - this pins that a dispatch that calls Advance thousands of
 * times over a real taxi still logs the Error exactly once.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSteerLawWarnsOncePerDispatchTest,
	"Airside.Model.SteerLawWarnsOncePerDispatch",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSteerLawWarnsOncePerDispatchTest::RunTest(const FString& Parameters)
{
	// EXACTLY ONE, not "at least one" and not "one per Advance" - the count is the whole of
	// what this test measures. Reverting the log to EffectiveSteerLaw (or removing Start's
	// call to WarnIfSteerLawUnsupported) fails this by producing zero or thousands instead.
	AddExpectedError(
		TEXT("declares RollingSteer with no wheelbase"),
		EAutomationExpectedErrorFlags::Contains, 1);

	// A REAL AIRFRAME'S FIGURES, mis-declared only in the one field under test - Piper()
	// gives Ground/Climb/Approach/Engine that can actually move the follower, so this
	// exercises the same Advance loop TurnRateTest does rather than a struct that would
	// divide by zero on its ground performance before the steer law is ever asked about.
	FAirframe Bad = TestAirframes::Piper();
	Bad.Chassis.SteerLaw = ESteerLaw::RollingSteer;
	Bad.Chassis.SteerAxleX = 0.0;
	Bad.Chassis.FixedAxleX = 0.0;

	FRoutePlan Plan;
	Plan.Result = ERouteResult::Found;
	Plan.Polyline = { FVector2D(0.0, 0.0), FVector2D(20000.0, 0.0) };
	Plan.Length = 20000.0;

	FRouteFollower Follower;
	Follower.Start(Plan, Bad.Chassis);

	// MANY ADVANCE CALLS, not one - a single call would pass even with the old inline log,
	// since "twice" and "many thousands" both round to "more than the one Start already
	// produced" only if Advance is actually silent, which is the thing under test.
	int32 Frames = 0;
	for (; Frames < 3000 && !Follower.HasArrived(); ++Frames)
	{
		FVector2D At;
		double Heading = 0.0;
		if (!Follower.Advance(1.0 / 60.0, Bad.Chassis, At, Heading))
		{
			break;
		}
	}

	TestTrue(TEXT("the follower actually ran (a route this long takes more than one frame)"),
		Frames > 60);

	return true;
}

#endif
