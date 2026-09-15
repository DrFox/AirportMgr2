#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadEntity.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSteerLawWithoutAxlesFallsBackTest,
	"Airside.Model.SteerLawWithoutAxlesFallsBack",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSteerLawWithoutAxlesFallsBackTest::RunTest(const FString& Parameters)
{
	// THE BUG THIS ENUM EXISTS TO MAKE IMPOSSIBLE. The law used to be INFERRED from whether
	// anyone had measured the axles - FAirframe::HasAxles() was the selector - so forgetting
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
	Unmeasured.SteerLaw = ESteerLaw::RollingSteer;
	Unmeasured.SteerAxleX = 0.0;
	Unmeasured.FixedAxleX = 0.0;

	TestEqual(
		TEXT("an airframe claiming to steer geometrically with no wheelbase falls back to pivot"),
		Unmeasured.EffectiveSteerLaw(), ESteerLaw::Pivot);

	FAirframe Measured;
	Measured.SteerLaw = ESteerLaw::RollingSteer;
	Measured.SteerAxleX = 360.0;
	Measured.FixedAxleX = 0.0;

	TestEqual(
		TEXT("and one with real axles keeps the law it declared"),
		Measured.EffectiveSteerLaw(), ESteerLaw::RollingSteer);

	// A PIVOT AIRFRAME IS NOT PROMOTED BY ACCIDENT. Measured axles on something declared
	// Pivot must STAY Pivot - otherwise the enum is decoration and the wheelbase is still
	// the selector, which is exactly the arrangement this replaces. A belt loader that
	// happens to have its axles filled in must not silently start steering like a truck.
	FAirframe DeliberatePivot;
	DeliberatePivot.SteerLaw = ESteerLaw::Pivot;
	DeliberatePivot.SteerAxleX = 360.0;
	DeliberatePivot.FixedAxleX = 0.0;

	TestEqual(
		TEXT("measured axles do not promote an airframe that declared itself a pivot"),
		DeliberatePivot.EffectiveSteerLaw(), ESteerLaw::Pivot);

	// AND THE DEFAULT IS THE SAFE ONE. An airframe nobody has authored at all is exactly the
	// one that must not claim to steer geometrically: TightestFollowableRadius would divide
	// by a zero wheelbase and FRouteFollower would yaw without bound.
	const FAirframe Unauthored;
	TestEqual(
		TEXT("an unauthored airframe defaults to the law that needs no measurements"),
		Unauthored.EffectiveSteerLaw(), ESteerLaw::Pivot);

	return true;
}

#endif
