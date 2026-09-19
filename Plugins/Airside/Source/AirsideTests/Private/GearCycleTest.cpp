#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadAgent.h"
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGearRetractsAtHeightNotLiftOffTest,
	"Airside.Model.GearRetractsAtHeightNotLiftOff",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGearRetractsAtHeightNotLiftOffTest::RunTest(const FString& Parameters)
{
	// THE CORRECTION THIS WHOLE FEATURE TURNS ON. Retraction is a pilot command given a few
	// hundred feet up, not something that happens when the wheels leave the tarmac -
	// FAgentMotion::bAirborne's own comment ("Stage 2's gear retraction hangs on this")
	// predicted otherwise and is what this test exists to contradict.
	FRoadAgent Agent;
	Agent.Airframe.Gear.TravelSeconds = 7.0;
	Agent.Airframe.Gear.DoorSeconds = 1.0;
	Agent.Airframe.Gear.RetractAboveHeight = 9000.0;
	Agent.Phase = EAgentPhase::Departing;

	// AIRBORNE BUT LOW. The rotation is already guarded by Airside.Present.AgentMotion case
	// 4, which asserts a rotating aircraft is not airborne at all; this is the next question
	// along - airborne, climbing, and still below the height the gear comes up at.
	Agent.LastMotion.bAirborne = true;
	Agent.LastMotion.Altitude = 3000.0;
	Agent.AdvanceGear(0.5);
	TestEqual(TEXT("airborne at 30 m the gear has not started up"),
		Agent.GearPhase, EGearPhase::Down);

	// PAST THE CUE. Nothing about the aircraft has changed except its height.
	Agent.LastMotion.Altitude = 9500.0;
	Agent.AdvanceGear(0.5);
	TestEqual(TEXT("past the retract height the cycle begins"),
		Agent.GearPhase, EGearPhase::Raising);

	// AND IT ARRIVES. Flown out over the whole 9 s cycle in 0.5 s steps, with one spare.
	for (int32 Step = 0; Step < 20; ++Step)
	{
		Agent.AdvanceGear(0.5);
	}
	TestEqual(TEXT("and the gear ends up stowed"), Agent.GearPhase, EGearPhase::Up);

	double GearDown = -1.0;
	double DoorOpen = -1.0;
	Agent.GearFractions(GearDown, DoorOpen);
	TestEqual(TEXT("reporting nothing left down"), GearDown, 0.0);
	TestEqual(TEXT("behind a shut bay"), DoorOpen, 0.0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGearDescendingArrivalDoesNotRetractTest,
	"Airside.Model.GearDescendingArrivalDoesNotRetract",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGearDescendingArrivalDoesNotRetractTest::RunTest(const FString& Parameters)
{
	// THE DEFECT THE TWO HEIGHTS INVITE. ExtendBelowHeight (15000) sits ABOVE
	// RetractAboveHeight (9000), so an arrival descending through 9000 uu satisfies
	// "airborne and above the retract height" word for word. If altitude alone decided, a
	// landing aeroplane would raise its gear on short final.
	FRoadAgent Agent;
	Agent.Airframe.Gear.TravelSeconds = 7.0;
	Agent.Airframe.Gear.DoorSeconds = 1.0;
	Agent.Airframe.Gear.RetractAboveHeight = 9000.0;
	Agent.Airframe.Gear.ExtendBelowHeight = 15000.0;

	Agent.Phase = EAgentPhase::Arriving;
	Agent.LastMotion.bAirborne = true;
	Agent.LastMotion.Altitude = 9500.0;

	Agent.AdvanceGear(0.5);
	TestEqual(TEXT("an arrival descending through the retract height keeps its gear down"),
		Agent.GearPhase, EGearPhase::Down);

	// AND THE SAME HEIGHT ON A DEPARTURE DOES RETRACT, which is what proves the phase is
	// what discriminates rather than something incidental about the arrival.
	Agent.Phase = EAgentPhase::Departing;
	Agent.AdvanceGear(0.5);
	TestEqual(TEXT("while a departure at that exact height raises it"),
		Agent.GearPhase, EGearPhase::Raising);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGearFixedWhenUnauthoredTest,
	"Airside.Model.GearFixedWhenUnauthored",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGearFixedWhenUnauthoredTest::RunTest(const FString& Parameters)
{
	// plane2's Twin Otter, and every ground vehicle. An unauthored FGearPerformance is a
	// statement that the gear is FIXED, not that nobody has measured it - so it must stay
	// down through a whole climb rather than snapping up at the cue height.
	FRoadAgent Agent;
	Agent.Phase = EAgentPhase::Departing;
	Agent.LastMotion.bAirborne = true;

	for (int32 Step = 0; Step < 60; ++Step)
	{
		// Climbing steadily past every height in the spec, including ClearAltitude.
		Agent.LastMotion.Altitude = Step * 500.0;
		Agent.AdvanceGear(0.5);
	}

	TestEqual(TEXT("fixed gear never leaves the down phase"), Agent.GearPhase, EGearPhase::Down);

	double GearDown = -1.0;
	double DoorOpen = -1.0;
	Agent.GearFractions(GearDown, DoorOpen);
	TestEqual(TEXT("and reports itself fully down for the whole flight"), GearDown, 1.0);
	TestEqual(TEXT("with no door it does not have"), DoorOpen, 0.0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGearReachesTheMotionDescriptionTest,
	"Airside.Model.GearReachesTheMotionDescription",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGearReachesTheMotionDescriptionTest::RunTest(const FString& Parameters)
{
	// THE SEAM. FAgentMotion is everything the view is told, so a fraction the model computes
	// and does not publish here is a fraction no Animation Blueprint can ever read.
	FRoadAgent Agent;
	Agent.Airframe.Gear.TravelSeconds = 7.0;
	Agent.Airframe.Gear.DoorSeconds = 1.0;

	// AT REST FIRST, because "down and locked" is what every taxiing aeroplane reports and it
	// is the value a broken default would most plausibly be mistaken for.
	const FAgentMotion Parked = Agent.DescribeMotion(FVector2D::ZeroVector, 0.0);
	TestEqual(TEXT("a parked aircraft reports its gear down"), Parked.GearDownFraction, 1.0);
	TestEqual(TEXT("and its bay shut"), Parked.BayDoorOpenFraction, 0.0);

	// MID-CYCLE, where the two disagree with both resting poses - which is the only place a
	// forwarder that returned a constant would be caught.
	Agent.GearPhase = EGearPhase::Raising;
	Agent.GearCycleSeconds = 4.5;

	const FAgentMotion Climbing = Agent.DescribeMotion(FVector2D::ZeroVector, 0.0);
	TestTrue(TEXT("half way up the gear is part way retracted"),
		Climbing.GearDownFraction > 0.4 && Climbing.GearDownFraction < 0.6);
	TestEqual(TEXT("with the bay held fully open around it"),
		Climbing.BayDoorOpenFraction, 1.0);

	return true;
}

#endif
