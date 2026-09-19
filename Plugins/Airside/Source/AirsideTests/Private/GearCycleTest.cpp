#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Entities/AircraftType.h"
#include "Model/RoadAgent.h"
#include "Model/RoadEntity.h"
#include "Present/AirsideAgentAnim.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGearCycleSequencesDoorsTest,
	"Airside.Model.GearCycleSequencesDoors",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGearCycleSequencesDoorsTest::RunTest(const FString& Parameters)
{
	// 1 s of door travel and 7 s of gear travel - the 737 figures, so the numbers below are
	// the ones that will actually be flown rather than a convenient round set.
	FGearPerformance Gear;
	Gear.TravelSeconds = 7.0;
	Gear.DoorSeconds = 1.0;

	// ONE DOOR MOVEMENT, NOT TWO. The doors are linked to the strut: open with the gear down,
	// shut only once it is stowed - so there is no door stage at the gear-down end at all.
	TestEqual(TEXT("the cycle is one gear travel plus one door movement"),
		Gear.CycleSeconds(), 8.0);

	double GearDown = -1.0;
	double DoorOpen = -1.0;

	// RETRACTING: THE GEAR GOES FIRST. The doors are already open, so nothing has to happen
	// before the leg can move - which is the opposite of extension below.
	Gear.FractionsAt(0.0, /*bRaising*/ true, GearDown, DoorOpen);
	TestEqual(TEXT("a retraction starts from gear down"), GearDown, 1.0);
	TestEqual(TEXT("with the bay ALREADY open - no stage opens it"), DoorOpen, 1.0);

	Gear.FractionsAt(3.5, true, GearDown, DoorOpen);
	TestTrue(TEXT("half way through the travel the gear is part way up"),
		GearDown > 0.4 && GearDown < 0.6);
	TestEqual(TEXT("and the doors are held open across the whole travel"), DoorOpen, 1.0);

	// THE DOORS DO NOT SHUT UNTIL THE GEAR IS STOWED, asserted at the instant it arrives.
	Gear.FractionsAt(7.0, true, GearDown, DoorOpen);
	TestEqual(TEXT("at 7 s the gear is fully up"), GearDown, 0.0);
	TestEqual(TEXT("and only now may the doors begin to shut"), DoorOpen, 1.0);

	Gear.FractionsAt(8.0, true, GearDown, DoorOpen);
	TestEqual(TEXT("the cycle ends with the gear up"), GearDown, 0.0);
	TestEqual(TEXT("and the bay shut over it"), DoorOpen, 0.0);

	// EXTENDING: THE DOORS LEAD, because they are shut over the stowed wheel and it cannot
	// come down through them. This is the asymmetry a trapezoid could not express.
	Gear.FractionsAt(0.0, /*bRaising*/ false, GearDown, DoorOpen);
	TestEqual(TEXT("an extension starts from gear up"), GearDown, 0.0);
	TestEqual(TEXT("behind a shut bay"), DoorOpen, 0.0);

	Gear.FractionsAt(0.99, false, GearDown, DoorOpen);
	TestEqual(TEXT("at 0.99 s the gear has not begun to come down"), GearDown, 0.0);
	TestTrue(TEXT("while the doors are nearly but not quite open"),
		DoorOpen > 0.9 && DoorOpen < 1.0);

	Gear.FractionsAt(8.0, false, GearDown, DoorOpen);
	TestEqual(TEXT("and it ends gear down"), GearDown, 1.0);
	TestEqual(TEXT("with the bay STILL open, which is where it stays"), DoorOpen, 1.0);

	// PAST THE END IS STILL THE END, in both directions.
	Gear.FractionsAt(100.0, true, GearDown, DoorOpen);
	TestEqual(TEXT("an overrun retraction holds gear up"), GearDown, 0.0);
	TestEqual(TEXT("and does not reopen the doors"), DoorOpen, 0.0);
	Gear.FractionsAt(100.0, false, GearDown, DoorOpen);
	TestEqual(TEXT("an overrun extension holds gear down"), GearDown, 1.0);
	TestEqual(TEXT("and does not shut the doors on it"), DoorOpen, 1.0);

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
	//
	// A TIME-REVERSAL, NOT A COMPLEMENT. The two directions are no longer one curve with the
	// gear flipped: the door movement sits at the gear-up end of BOTH, so extension is
	// retraction played backwards. The complement this used to assert was a property of the
	// trapezoid, and asserting it now would be pinning the shape that was wrong.
	FGearPerformance Gear;
	Gear.TravelSeconds = 7.0;
	Gear.DoorSeconds = 1.0;

	const double Cycle = Gear.CycleSeconds();

	double UpGearDown = -1.0;
	double UpDoorOpen = -1.0;
	double DownGearDown = -1.0;
	double DownDoorOpen = -1.0;

	for (const double At : {0.0, 0.5, 1.0, 3.5, 7.0, 7.5, 8.0})
	{
		Gear.FractionsAt(Cycle - At, /*bRaising*/ true, UpGearDown, UpDoorOpen);
		Gear.FractionsAt(At, /*bRaising*/ false, DownGearDown, DownDoorOpen);

		TestEqual(FString::Printf(
			TEXT("lowering at %.1f s is raising at %.1f s, for the gear"), At, Cycle - At),
			DownGearDown, UpGearDown);
		TestEqual(FString::Printf(
			TEXT("and for the doors at %.1f s"), At), DownDoorOpen, UpDoorOpen);
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
	// OPEN, WHICH IS THE BIND POSE. An airframe with no bay doors has no door bones either,
	// so the only safe value is the one that asks the animgraph to rotate nothing.
	TestEqual(TEXT("and the door fraction never leaves its bind-pose value"), DoorOpen, 1.0);

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
	TestEqual(TEXT("with its bay at the bind pose, since it has no doors"), DoorOpen, 1.0);

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
	TestEqual(TEXT("and its bay OPEN - a 737's nose doors are linked to the strut"),
		Parked.BayDoorOpenFraction, 1.0);

	// MID-CYCLE, where the two disagree with both resting poses - which is the only place a
	// forwarder that returned a constant would be caught.
	Agent.GearPhase = EGearPhase::Raising;
	Agent.GearCycleSeconds = 3.5;

	const FAgentMotion Climbing = Agent.DescribeMotion(FVector2D::ZeroVector, 0.0);
	TestTrue(TEXT("half way up the gear is part way retracted"),
		Climbing.GearDownFraction > 0.4 && Climbing.GearDownFraction < 0.6);
	TestEqual(TEXT("with the bay held fully open around it"),
		Climbing.BayDoorOpenFraction, 1.0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGear737IsAuthoredAndTravelsTest,
	"Airside.Model.Gear737IsAuthoredAndTravels",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGear737IsAuthoredAndTravelsTest::RunTest(const FString& Parameters)
{
	// THE ASSET, NOT THE C++ BUILDER, and the difference is the whole point of this test.
	// UAircraftType::Build737 is called from tests and from nowhere else - it builds the
	// PAPER 737, which carries no mesh and never flies. Figures authored there would have
	// been pinned by a green test and still absent from every aeroplane on the runway.
	//
	// DA_Aircraft_Plane4 is the one that flies, authored by Tools/Python/build_plane4_type.py
	// against SK_Plane4 - the only rig in the fleet with gear_* and door_nose_* bones.
	UAircraftType* Type = Cast<UAircraftType>(StaticLoadObject(
		UAircraftType::StaticClass(), nullptr, TEXT("/Game/Entities/DA_Aircraft_Plane4")));
	if (!TestNotNull(TEXT("DA_Aircraft_Plane4 loads - run build_plane4_type.py if not"), Type))
	{
		return false;
	}

	const FAirframe Frame = Type->Airframe();

	TestTrue(TEXT("the 737 has retractable gear"), Frame.Gear.IsSet());
	TestEqual(TEXT("with the real transit time"), Frame.Gear.TravelSeconds, 7.0);
	TestEqual(TEXT("and a second of nose bay door each side"), Frame.Gear.DoorSeconds, 1.0);
	TestEqual(TEXT("making an eight second cycle - one travel, one door movement"),
		Frame.Gear.CycleSeconds(), 8.0);

	// THE CUE HEIGHTS TRAVEL WITH IT, and their ORDER is the thing worth pinning: the extend
	// height sits ABOVE the retract height, which is exactly what makes altitude alone an
	// unsafe discriminator. See Airside.Model.GearDescendingArrivalDoesNotRetract.
	TestEqual(TEXT("gear up passing 9000 uu"), Frame.Gear.RetractAboveHeight, 9000.0);
	TestEqual(TEXT("gear down below 15000 uu"), Frame.Gear.ExtendBelowHeight, 15000.0);
	TestTrue(TEXT("and the extend height is the higher of the two"),
		Frame.Gear.ExtendBelowHeight > Frame.Gear.RetractAboveHeight);

	// THE BOUNDARY, PINNED FROM THE OTHER SIDE. One aeroplane's figures live in one place, so
	// the paper 737 must declare NO cycle - otherwise the two copies drift and the one nobody
	// is watching wins. A later edit that "completes" Build737 by copying the figures back in
	// fails here rather than shipping a second source of truth.
	UAircraftType* Paper = NewObject<UAircraftType>();
	UAircraftType::Build737(Paper);
	TestFalse(TEXT("the paper 737 declares no gear cycle - nothing ever flies it"),
		Paper->Airframe().Gear.IsSet());

	// A PIPER IS NOT AUTHORED EITHER. Its rig cannot show a retraction, so it declares none -
	// which catches an edit that sprays gear figures across every type for completeness.
	UAircraftType* Piper = NewObject<UAircraftType>();
	UAircraftType::BuildPiperMeridian(Piper);
	TestFalse(TEXT("the Meridian declares no gear cycle, because its rig cannot show one"),
		Piper->Airframe().Gear.IsSet());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGearAnglesFollowTheFractionsTest,
	"Airside.Present.GearAnglesFollowTheFractions",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGearAnglesFollowTheFractionsTest::RunTest(const FString& Parameters)
{
	// STATIC AND FREE OF THE INSTANCE, the same construction PropStepDegrees and
	// WheelStepDegrees use, so the arithmetic can be tested with no actor and no skeleton.
	float GearAngle = -1.0f;
	float DoorAngle = -1.0f;

	// plane4's measured rig angles: the gear folds 90 degrees from a bind pose that is DOWN,
	// and the doors are SHUT at 81 from a bind pose that is OPEN - "found by sweeping: the two
	// free edges meet on the centreline to 0.0 mm", per its build_export.py.
	const float Retracted = 90.0f;
	const float DoorClosed = 81.0f;

	// A PARKED AEROPLANE ROTATES NEITHER BONE, and that is the single most useful fact in this
	// file: the bind pose IS the parked pose - gear down, bay hanging open, because a 737's
	// nose doors are linked to the strut. It is asserted first because it is what the player
	// looks at for all but eight seconds of a flight.
	UAirsideAgentAnim::GearAnglesFrom(1.0f, 1.0f, Retracted, DoorClosed, GearAngle, DoorAngle);
	TestEqual(TEXT("gear down is no rotation at all"), GearAngle, 0.0f);
	TestEqual(TEXT("and an open bay is no rotation either - a parked 737 is the bind pose"),
		DoorAngle, 0.0f);

	// STOWED ROTATES BOTH. The other rest state, and the one a trapezoid could never reach
	// because it returned the doors to whatever value they started at.
	UAirsideAgentAnim::GearAnglesFrom(0.0f, 0.0f, Retracted, DoorClosed, GearAngle, DoorAngle);
	TestEqual(TEXT("gear up is the full fold"), GearAngle, 90.0f);
	TestEqual(TEXT("and a shut bay is the full sweep, because its bind pose is open"),
		DoorAngle, 81.0f);

	// MID-RETRACTION: the leg part way up with the bay still held open around it. BOTH shipped
	// versions of the trapezoid got this frame wrong - the first shut the doors on the way in,
	// the second shut them while parked - and both looked like sequencing bugs.
	UAirsideAgentAnim::GearAnglesFrom(0.5f, 1.0f, Retracted, DoorClosed, GearAngle, DoorAngle);
	TestEqual(TEXT("half retracted is half the fold"), GearAngle, 45.0f);
	TestEqual(TEXT("with the bay open around it, rotating nothing"), DoorAngle, 0.0f);

	// AND THE DOOR IS A TRAVEL, NOT A SWITCH.
	UAirsideAgentAnim::GearAnglesFrom(0.0f, 0.5f, Retracted, DoorClosed, GearAngle, DoorAngle);
	TestEqual(TEXT("half shut is half the sweep"), DoorAngle, 40.5f);

	return true;
}

#endif
