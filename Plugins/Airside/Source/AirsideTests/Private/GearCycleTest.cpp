#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadEntity.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGearCycleSequencesDoorsTest,
	"Airside.Model.GearCycleSequencesDoors",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGearCycleSequencesDoorsTest::RunTest(const FString& Parameters)
{
	// 1 s of door travel either side of 7 s of gear travel - the 737 figures, so the numbers
	// below are the ones that will actually be flown rather than a convenient round set.
	FGearPerformance Gear;
	Gear.TravelSeconds = 7.0;
	Gear.DoorSeconds = 1.0;

	TestEqual(TEXT("the cycle is the gear travel bracketed by a door movement each side"),
		Gear.CycleSeconds(), 9.0);

	double GearDown = -1.0;
	double DoorOpen = -1.0;

	// THE GEAR DOES NOT MOVE UNTIL THE DOORS ARE OPEN. This is the whole sequencing decision
	// and it is asserted at the last instant before the gear is allowed to travel, not in the
	// middle of the door stage where a partly-working implementation would also pass.
	Gear.FractionsAt(0.99, /*bRaising*/ true, GearDown, DoorOpen);
	TestEqual(TEXT("at 0.99 s the gear is still fully down and locked"), GearDown, 1.0);
	TestTrue(TEXT("while the doors are nearly but not quite open"),
		DoorOpen > 0.9 && DoorOpen < 1.0);

	// AND IT HAS MOVED BY THE MIDDLE. Without this the assertion above passes perfectly on a
	// gear that never moves at all - which is the failure a-green-test-may-measure-nothing
	// records, and the reason every case in this file pins a value that must CHANGE.
	Gear.FractionsAt(4.5, true, GearDown, DoorOpen);
	TestTrue(TEXT("half way through the travel the gear is part way up"),
		GearDown > 0.4 && GearDown < 0.6);
	TestEqual(TEXT("and the doors are held fully open across the whole travel"), DoorOpen, 1.0);

	// THE DOORS DO NOT CLOSE UNTIL THE GEAR IS STOWED. The other half of the sequencing, at
	// the instant the gear arrives.
	Gear.FractionsAt(8.0, true, GearDown, DoorOpen);
	TestEqual(TEXT("at 8 s the gear is fully up"), GearDown, 0.0);
	TestEqual(TEXT("and only now may the doors begin to close"), DoorOpen, 1.0);

	// AT REST. Doors shut over a stowed wheel, which is what a 737 does and why plane4 has
	// door_nose_L/_R at all.
	Gear.FractionsAt(9.0, true, GearDown, DoorOpen);
	TestEqual(TEXT("the cycle ends with the gear up"), GearDown, 0.0);
	TestEqual(TEXT("and the bay shut over it"), DoorOpen, 0.0);

	// PAST THE END IS STILL THE END. A timer that overran used to be a source of flicker in
	// this project's other integrators; clamped rather than wrapped.
	Gear.FractionsAt(100.0, true, GearDown, DoorOpen);
	TestEqual(TEXT("an overrun timer holds the resting pose"), GearDown, 0.0);
	TestEqual(TEXT("and does not reopen the doors"), DoorOpen, 0.0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGearExtendMirrorsRetractTest,
	"Airside.Model.GearExtendMirrorsRetract",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGearExtendMirrorsRetractTest::RunTest(const FString& Parameters)
{
	// THE SYMMETRY IS THE ARGUMENT FOR BUILDING THE EXTEND HALF AT ALL. Nothing can currently
	// see it - an arrival joins final at FApproachPerformance::FinalAltitude, 2000 uu, about
	// 66 ft, already configured - so this test is what stops it rotting unnoticed.
	FGearPerformance Gear;
	Gear.TravelSeconds = 7.0;
	Gear.DoorSeconds = 1.0;

	double UpGearDown = -1.0;
	double UpDoorOpen = -1.0;
	double DownGearDown = -1.0;
	double DownDoorOpen = -1.0;

	for (const double At : {0.0, 0.5, 1.0, 4.5, 8.0, 8.5, 9.0})
	{
		Gear.FractionsAt(At, /*bRaising*/ true, UpGearDown, UpDoorOpen);
		Gear.FractionsAt(At, /*bRaising*/ false, DownGearDown, DownDoorOpen);

		TestEqual(FString::Printf(TEXT("the doors do the same thing either way at %.1f s"), At),
			DownDoorOpen, UpDoorOpen);
		TestEqual(FString::Printf(TEXT("and the gear is the exact complement at %.1f s"), At),
			DownGearDown, 1.0 - UpGearDown);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGearWithoutDoorsStillTravelsTest,
	"Airside.Model.GearWithoutDoorsStillTravels",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGearWithoutDoorsStillTravelsTest::RunTest(const FString& Parameters)
{
	// DoorSeconds = 0 means "no bay doors", not "instant doors". An airframe whose mains sit
	// behind a fixed fairing - which is every main gear in this fleet - still retracts.
	FGearPerformance Gear;
	Gear.TravelSeconds = 4.0;
	Gear.DoorSeconds = 0.0;

	TestEqual(TEXT("with no doors the cycle is the travel alone"), Gear.CycleSeconds(), 4.0);

	double GearDown = -1.0;
	double DoorOpen = -1.0;

	Gear.FractionsAt(2.0, true, GearDown, DoorOpen);
	TestEqual(TEXT("the gear is half way up at the half way point"), GearDown, 0.5);
	TestEqual(TEXT("and no door ever opens, rather than one snapping open"), DoorOpen, 0.0);

	// DIVISION BY DoorSeconds IS THE HAZARD HERE and this is what proves it is guarded: a
	// NaN fraction does not show up as a stuck door, it shows up as a bone transform that
	// makes the whole aeroplane vanish.
	Gear.FractionsAt(0.0, true, GearDown, DoorOpen);
	TestEqual(TEXT("and the first frame is a number, not a NaN"), GearDown, 1.0);

	return true;
}

#endif
