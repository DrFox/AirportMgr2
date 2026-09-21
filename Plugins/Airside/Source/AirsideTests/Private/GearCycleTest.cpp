#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
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

	// RETRACTING: THE GEAR GOES FIRST. The doors are already open, so nothing has to happen
	// before the leg can move - which is the opposite of extension below.
	FGearPose Pose = Gear.FractionsAt(0.0, /*bRaising*/ true);
	TestEqual(TEXT("a retraction starts from gear down"), Pose.GearDownFraction, 1.0);
	TestEqual(TEXT("with the bay ALREADY open - no stage opens it"), Pose.BayDoorOpenFraction, 1.0);

	Pose = Gear.FractionsAt(3.5, true);
	TestTrue(TEXT("half way through the travel the gear is part way up"),
		Pose.GearDownFraction > 0.4 && Pose.GearDownFraction < 0.6);
	TestEqual(TEXT("and the doors are held open across the whole travel"),
		Pose.BayDoorOpenFraction, 1.0);

	// THE DOORS DO NOT SHUT UNTIL THE GEAR IS STOWED, asserted at the instant it arrives.
	Pose = Gear.FractionsAt(7.0, true);
	TestEqual(TEXT("at 7 s the gear is fully up"), Pose.GearDownFraction, 0.0);
	TestEqual(TEXT("and only now may the doors begin to shut"), Pose.BayDoorOpenFraction, 1.0);

	Pose = Gear.FractionsAt(8.0, true);
	TestEqual(TEXT("the cycle ends with the gear up"), Pose.GearDownFraction, 0.0);
	TestEqual(TEXT("and the bay shut over it"), Pose.BayDoorOpenFraction, 0.0);

	// EXTENDING: THE DOORS LEAD, because they are shut over the stowed wheel and it cannot
	// come down through them. This is the asymmetry a trapezoid could not express.
	Pose = Gear.FractionsAt(0.0, /*bRaising*/ false);
	TestEqual(TEXT("an extension starts from gear up"), Pose.GearDownFraction, 0.0);
	TestEqual(TEXT("behind a shut bay"), Pose.BayDoorOpenFraction, 0.0);

	Pose = Gear.FractionsAt(0.99, false);
	TestEqual(TEXT("at 0.99 s the gear has not begun to come down"), Pose.GearDownFraction, 0.0);
	TestTrue(TEXT("while the doors are nearly but not quite open"),
		Pose.BayDoorOpenFraction > 0.9 && Pose.BayDoorOpenFraction < 1.0);

	Pose = Gear.FractionsAt(8.0, false);
	TestEqual(TEXT("and it ends gear down"), Pose.GearDownFraction, 1.0);
	TestEqual(TEXT("with the bay STILL open, which is where it stays"),
		Pose.BayDoorOpenFraction, 1.0);

	// PAST THE END IS STILL THE END, in both directions.
	Pose = Gear.FractionsAt(100.0, true);
	TestEqual(TEXT("an overrun retraction holds gear up"), Pose.GearDownFraction, 0.0);
	TestEqual(TEXT("and does not reopen the doors"), Pose.BayDoorOpenFraction, 0.0);
	Pose = Gear.FractionsAt(100.0, false);
	TestEqual(TEXT("an overrun extension holds gear down"), Pose.GearDownFraction, 1.0);
	TestEqual(TEXT("and does not shut the doors on it"), Pose.BayDoorOpenFraction, 1.0);

	// AND THE TRUCK NEVER MOVES, because a 737's main gear is a single axle and this
	// FGearPerformance leaves TruckTiltSeconds at zero. THE GUARD THAT TODAY'S FLEET IS
	// UNTOUCHED: every airframe in this game is authored exactly like the one above, so if
	// the truck stage could leak into an unauthored cycle it would do it here.
	for (const double At : {0.0, 0.5, 3.5, 7.0, 8.0, 100.0})
	{
		TestEqual(FString::Printf(TEXT("no truck is authored, so none tilts raising at %.1f s"), At),
			Gear.FractionsAt(At, true).TruckLevelFraction, 1.0);
		TestEqual(FString::Printf(TEXT("nor lowering at %.1f s"), At),
			Gear.FractionsAt(At, false).TruckLevelFraction, 1.0);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGearTruckLeadsRetractAndTrailsExtendTest,
	"Airside.Model.GearTruckLeadsRetractAndTrailsExtend",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGearTruckLeadsRetractAndTrailsExtendTest::RunTest(const FString& Parameters)
{
	// TRUCK TILT - Boeing's own term for it. A wide-body main leg carries a four- or six-wheel
	// bogie on a beam that pivots at the foot of the oleo, and the beam has to be swung before
	// it will pass into a well that is not deep enough to take it lying flat. So the tilt sits
	// at the GEAR-DOWN end of the cycle, which is the opposite end from the doors, and it
	// therefore LEADS a retraction and TRAILS an extension.
	//
	// THE WHOLE POINT OF THE TEST IS THE ORDER, not the numbers. A tilt that ran alongside the
	// leg travel would satisfy every fraction-in-range assertion anyone would think to write
	// and still put the bogie into the side of the fuselage.
	FGearPerformance Gear;
	Gear.TravelSeconds = 7.0;
	Gear.DoorSeconds = 1.0;
	Gear.TruckTiltSeconds = 2.0;

	TestEqual(TEXT("the cycle is a truck tilt, a gear travel and one door movement"),
		Gear.CycleSeconds(), 10.0);

	// RETRACTING. The truck goes first and the leg does not move until it has finished.
	FGearPose Pose = Gear.FractionsAt(0.0, /*bRaising*/ true);
	TestEqual(TEXT("a retraction starts with the truck level"), Pose.TruckLevelFraction, 1.0);
	TestEqual(TEXT("and the gear still down under it"), Pose.GearDownFraction, 1.0);

	Pose = Gear.FractionsAt(1.0, true);
	TestEqual(TEXT("a second in, the truck is half tilted"), Pose.TruckLevelFraction, 0.5);
	// THE ASSERTION THAT MAKES THIS TEST WORTH HAVING. Everything else here would still pass
	// if the tilt and the travel overlapped.
	TestEqual(TEXT("and the leg has NOT begun to fold under a part-tilted truck"),
		Pose.GearDownFraction, 1.0);

	Pose = Gear.FractionsAt(2.0, true);
	TestEqual(TEXT("at 2 s the truck is fully tilted"), Pose.TruckLevelFraction, 0.0);
	TestEqual(TEXT("and only now may the leg start up"), Pose.GearDownFraction, 1.0);

	Pose = Gear.FractionsAt(5.5, true);
	TestTrue(TEXT("mid-travel the leg is part way up"),
		Pose.GearDownFraction > 0.4 && Pose.GearDownFraction < 0.6);
	TestEqual(TEXT("with the truck held tilted, not levelling again inside the bay"),
		Pose.TruckLevelFraction, 0.0);

	Pose = Gear.FractionsAt(9.0, true);
	TestEqual(TEXT("at 9 s the gear is stowed"), Pose.GearDownFraction, 0.0);
	TestEqual(TEXT("and only now may the doors begin to shut"), Pose.BayDoorOpenFraction, 1.0);

	Pose = Gear.FractionsAt(10.0, true);
	TestEqual(TEXT("the cycle ends stowed"), Pose.GearDownFraction, 0.0);
	TestEqual(TEXT("bay shut"), Pose.BayDoorOpenFraction, 0.0);
	TestEqual(TEXT("and the truck still tilted, which is how it fits"),
		Pose.TruckLevelFraction, 0.0);

	// EXTENDING. The mirror image: doors first, then the leg, and the truck levels LAST -
	// after the leg is down and locked, which is when a real truck meets the tarmac.
	Pose = Gear.FractionsAt(0.0, /*bRaising*/ false);
	TestEqual(TEXT("an extension starts with the truck tilted"), Pose.TruckLevelFraction, 0.0);

	Pose = Gear.FractionsAt(8.0, false);
	TestEqual(TEXT("at 8 s the leg is down and locked"), Pose.GearDownFraction, 1.0);
	TestEqual(TEXT("and the truck is STILL tilted - it levels after the leg, not with it"),
		Pose.TruckLevelFraction, 0.0);

	Pose = Gear.FractionsAt(9.0, false);
	TestEqual(TEXT("a second later it is half levelled"), Pose.TruckLevelFraction, 0.5);
	TestEqual(TEXT("with the leg staying down through it"), Pose.GearDownFraction, 1.0);

	Pose = Gear.FractionsAt(10.0, false);
	TestEqual(TEXT("and the cycle ends with the truck flat on the tarmac"),
		Pose.TruckLevelFraction, 1.0);
	TestEqual(TEXT("gear down"), Pose.GearDownFraction, 1.0);
	TestEqual(TEXT("bay open, where it stays"), Pose.BayDoorOpenFraction, 1.0);

	// PAST THE END IS STILL THE END, for the truck as for the other two.
	TestEqual(TEXT("an overrun retraction does not level the truck again"),
		Gear.FractionsAt(100.0, true).TruckLevelFraction, 0.0);
	TestEqual(TEXT("and an overrun extension does not tilt it again"),
		Gear.FractionsAt(100.0, false).TruckLevelFraction, 1.0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGearWithoutATruckNeverTiltsTest,
	"Airside.Model.GearWithoutATruckNeverTilts",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGearWithoutATruckNeverTiltsTest::RunTest(const FString& Parameters)
{
	// TruckTiltSeconds = 0 means "this airframe has no truck", not "an instant tilt" - the
	// same distinction DoorSeconds draws, and a fact about the aeroplane rather than a missing
	// measurement. Every airframe in this fleet before plane6 is single-axle.
	FGearPerformance Gear;
	Gear.TravelSeconds = 4.0;
	Gear.TruckTiltSeconds = 0.0;

	TestEqual(TEXT("with no truck and no doors the cycle is the travel alone"),
		Gear.CycleSeconds(), 4.0);

	// LEVEL, WHICH IS THE BIND POSE. An airframe with no truck has no truck bone either, so
	// the only safe value is the one that asks the animgraph to rotate nothing.
	TestEqual(TEXT("the leg starts folding at t=0, with no tilt stage ahead of it"),
		Gear.FractionsAt(0.0, true).GearDownFraction, 1.0);
	TestEqual(TEXT("and is half way up at the half way point"),
		Gear.FractionsAt(2.0, true).GearDownFraction, 0.5);

	// DIVISION BY TruckTiltSeconds IS THE HAZARD and this is what proves it is guarded: a NaN
	// fraction does not show up as a stuck bogie, it shows up as a bone transform that makes
	// the whole aeroplane vanish. The same trap DoorSeconds already carries a test for.
	for (const double At : {0.0, 2.0, 4.0, 100.0})
	{
		TestEqual(FString::Printf(TEXT("the truck fraction is 1, not a NaN, raising at %.1f s"), At),
			Gear.FractionsAt(At, true).TruckLevelFraction, 1.0);
		TestEqual(FString::Printf(TEXT("nor lowering at %.1f s"), At),
			Gear.FractionsAt(At, false).TruckLevelFraction, 1.0);
	}

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
	// ALL THREE FRACTIONS, WITH A TRUCK AUTHORED. The truck is what makes the mirror worth
	// re-asserting: it is the first stage that sits at the GEAR-DOWN end, so a sign or an
	// offset that got it backwards would leave the two other columns matching perfectly.
	FGearPerformance Gear;
	Gear.TravelSeconds = 7.0;
	Gear.DoorSeconds = 1.0;
	Gear.TruckTiltSeconds = 2.0;

	const double Cycle = Gear.CycleSeconds();
	TestEqual(TEXT("the cycle spans all three stages"), Cycle, 10.0);

	// SAMPLED ON EVERY STAGE BOUNDARY AND INSIDE EVERY STAGE, so no sample can land where two
	// stages happen to agree.
	int32 TruckMidTravel = 0;
	int32 DoorMidTravel = 0;
	int32 GearMidTravel = 0;

	for (const double At : {0.0, 0.5, 1.0, 2.0, 3.5, 5.5, 9.0, 9.5, 10.0})
	{
		const FGearPose Up = Gear.FractionsAt(Cycle - At, /*bRaising*/ true);
		const FGearPose Down = Gear.FractionsAt(At, /*bRaising*/ false);

		TestEqual(FString::Printf(
			TEXT("lowering at %.1f s is raising at %.1f s, for the gear"), At, Cycle - At),
			Down.GearDownFraction, Up.GearDownFraction);
		TestEqual(FString::Printf(
			TEXT("and for the doors at %.1f s"), At), Down.BayDoorOpenFraction, Up.BayDoorOpenFraction);
		TestEqual(FString::Printf(
			TEXT("and for the truck at %.1f s"), At), Down.TruckLevelFraction, Up.TruckLevelFraction);

		// A GREEN MIRROR CAN MEASURE NOTHING. Two constants mirror each other perfectly, so
		// the run is counted and asserted below - otherwise a truck stage that never moved
		// would pass this test as comfortably as a correct one.
		GearMidTravel += (Down.GearDownFraction > 0.0 && Down.GearDownFraction < 1.0) ? 1 : 0;
		DoorMidTravel += (Down.BayDoorOpenFraction > 0.0 && Down.BayDoorOpenFraction < 1.0) ? 1 : 0;
		TruckMidTravel += (Down.TruckLevelFraction > 0.0 && Down.TruckLevelFraction < 1.0) ? 1 : 0;
	}

	TestTrue(TEXT("the samples caught the gear part way, so the gear column was live"),
		GearMidTravel > 0);
	TestTrue(TEXT("and the doors part way"), DoorMidTravel > 0);
	TestTrue(TEXT("and the TRUCK part way - without this the mirror above proves nothing"),
		TruckMidTravel > 0);

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

	FGearPose Pose = Gear.FractionsAt(2.0, true);
	TestEqual(TEXT("the gear is half way up at the half way point"), Pose.GearDownFraction, 0.5);
	// OPEN, WHICH IS THE BIND POSE. An airframe with no bay doors has no door bones either,
	// so the only safe value is the one that asks the animgraph to rotate nothing.
	TestEqual(TEXT("and the door fraction never leaves its bind-pose value"),
		Pose.BayDoorOpenFraction, 1.0);

	// DIVISION BY DoorSeconds IS THE HAZARD HERE and this is what proves it is guarded: a
	// NaN fraction does not show up as a stuck door, it shows up as a bone transform that
	// makes the whole aeroplane vanish.
	Pose = Gear.FractionsAt(0.0, true);
	TestEqual(TEXT("and the first frame is a number, not a NaN"), Pose.GearDownFraction, 1.0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGearRestingPosesComeFromTheEvaluatorTest,
	"Airside.Model.GearRestingPosesComeFromTheEvaluator",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGearRestingPosesComeFromTheEvaluatorTest::RunTest(const FString& Parameters)
{
	// A SECOND EVALUATOR WEARING A SWITCH STATEMENT. FRoadAgent::GearPose answered the Down
	// and Up phases from a hand-written table of ones and zeroes, and the table had drifted
	// from FGearPerformance::FractionsAt, which owns every other frame of the same cycle.
	//
	// THE DRIFT, EXACTLY. The table returned doors-SHUT on reaching Up for EVERY airframe,
	// including one whose DoorSeconds is zero - and zero means "no bay doors", not "instant
	// ones", so the evaluator reports them open for the whole cycle. A rig with door bones and
	// no authored door time would therefore have held them open across the entire retraction
	// and snapped them shut on the frame the phase changed. Found while adding the truck,
	// which divides the same way and would have made it three copies of one mistake.
	//
	// So the resting poses are the cycle's OWN ENDPOINTS now, and this is what says so.
	FRoadAgent Agent;
	Agent.Airframe.Gear.TravelSeconds = 4.0;
	Agent.Airframe.Gear.DoorSeconds = 0.0;

	Agent.GearPhase = EGearPhase::Raising;
	Agent.GearCycleSeconds = 3.9;
	const FGearPose NearlyUp = Agent.GearPose();
	TestEqual(TEXT("one frame from the end the doors are open, there being none"),
		NearlyUp.BayDoorOpenFraction, 1.0);

	// THE NEXT FRAME. Nothing about the aeroplane has changed except which phase it is in.
	Agent.GearPhase = EGearPhase::Up;
	Agent.GearCycleSeconds = 0.0;
	const FGearPose Stowed = Agent.GearPose();
	TestEqual(TEXT("and they are STILL open once it is stowed - no door snaps shut on arrival"),
		Stowed.BayDoorOpenFraction, 1.0);
	TestEqual(TEXT("with the gear up, which is the half the table did get right"),
		Stowed.GearDownFraction, 0.0);

	// AND AN AIRFRAME THAT DOES HAVE DOORS STILL SHUTS THEM, which is what proves the fix is
	// the evaluator being consulted rather than the door stage being lost.
	Agent.Airframe.Gear.DoorSeconds = 1.0;
	TestEqual(TEXT("an airframe with authored doors still rests with them shut"),
		Agent.GearPose().BayDoorOpenFraction, 0.0);

	// THE TRUCK DIVIDES THE SAME WAY, and gets it right by construction rather than by a
	// second table entry: tilted at rest up, level at rest down, for an airframe with a truck.
	Agent.Airframe.Gear.TruckTiltSeconds = 2.0;
	TestEqual(TEXT("a truck rests TILTED with the gear up, which is how it fits the well"),
		Agent.GearPose().TruckLevelFraction, 0.0);

	Agent.GearPhase = EGearPhase::Down;
	const FGearPose Parked = Agent.GearPose();
	TestEqual(TEXT("and rests LEVEL with the gear down"), Parked.TruckLevelFraction, 1.0);
	TestEqual(TEXT("under a leg that is down"), Parked.GearDownFraction, 1.0);
	TestEqual(TEXT("and a bay hanging open"), Parked.BayDoorOpenFraction, 1.0);

	// AND AN AIRFRAME WITH NO TRUCK RESTS LEVEL IN BOTH PHASES, which is the value that
	// rotates a bone the rig does not have by exactly nothing.
	Agent.Airframe.Gear.TruckTiltSeconds = 0.0;
	Agent.GearPhase = EGearPhase::Up;
	TestEqual(TEXT("a single-axle airframe reports a level truck even stowed"),
		Agent.GearPose().TruckLevelFraction, 1.0);

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

	const FGearPose Stowed = Agent.GearPose();
	TestEqual(TEXT("reporting nothing left down"), Stowed.GearDownFraction, 0.0);
	TestEqual(TEXT("behind a shut bay"), Stowed.BayDoorOpenFraction, 0.0);

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

	const FGearPose Pose = Agent.GearPose();
	TestEqual(TEXT("and reports itself fully down for the whole flight"),
		Pose.GearDownFraction, 1.0);
	TestEqual(TEXT("with its bay at the bind pose, since it has no doors"),
		Pose.BayDoorOpenFraction, 1.0);
	TestEqual(TEXT("and its truck at the bind pose, since it has no truck"),
		Pose.TruckLevelFraction, 1.0);

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
	Agent.Airframe.Gear.TruckTiltSeconds = 2.0;

	// AT REST FIRST, because "down and locked" is what every taxiing aeroplane reports and it
	// is the value a broken default would most plausibly be mistaken for.
	const FAgentMotion Parked = Agent.DescribeMotion(FVector2D::ZeroVector, 0.0);
	TestEqual(TEXT("a parked aircraft reports its gear down"),
		Parked.GearPose.GearDownFraction, 1.0);
	TestEqual(TEXT("and its bay OPEN - a 737's nose doors are linked to the strut"),
		Parked.GearPose.BayDoorOpenFraction, 1.0);
	TestEqual(TEXT("and its truck LEVEL, flat on the tarmac"),
		Parked.GearPose.TruckLevelFraction, 1.0);

	// MID-TILT, the first stage of a retraction, where the TRUCK disagrees with both resting
	// poses while the other two still sit at theirs. THE ONLY FRAME THAT CATCHES A TRUCK
	// FRACTION THE MODEL COMPUTES AND DOES NOT PUBLISH: at every other point in the cycle it
	// equals a value a hard-coded constant could supply.
	Agent.GearPhase = EGearPhase::Raising;
	Agent.GearCycleSeconds = 1.0;

	const FAgentMotion Tilting = Agent.DescribeMotion(FVector2D::ZeroVector, 0.0);
	TestEqual(TEXT("a second into the cycle the truck is half tilted"),
		Tilting.GearPose.TruckLevelFraction, 0.5);
	TestEqual(TEXT("with the leg still down under it"), Tilting.GearPose.GearDownFraction, 1.0);

	// MID-TRAVEL, where the GEAR disagrees with both resting poses - the same argument one
	// stage later, and what this test asserted before the truck existed.
	Agent.GearCycleSeconds = 5.5;

	const FAgentMotion Climbing = Agent.DescribeMotion(FVector2D::ZeroVector, 0.0);
	TestTrue(TEXT("half way up the gear is part way retracted"),
		Climbing.GearPose.GearDownFraction > 0.4 && Climbing.GearPose.GearDownFraction < 0.6);
	TestEqual(TEXT("with the bay held fully open around it"),
		Climbing.GearPose.BayDoorOpenFraction, 1.0);
	TestEqual(TEXT("and the truck fully tilted behind it"),
		Climbing.GearPose.TruckLevelFraction, 0.0);

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
	// NO TRUCK, AND PERMANENTLY. A 737 main leg carries one axle; the bogie beam arrives with
	// the 777. This is the guard that the truck stage did not get sprayed across the fleet for
	// completeness, which is exactly how the paper 737 acquired gear figures it never flew.
	TestEqual(TEXT("and no truck to tilt - a 737 main gear is a single axle"),
		Frame.Gear.TruckTiltSeconds, 0.0);
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
	UAircraftType* Piper = TestAirframes::PiperType();
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
	//
	// ONE FUNCTION FOR ALL THREE BONES since 2026-09-21. It was GearAnglesFrom, which did the
	// gear and the door together in two lines that looked alike and were not; the truck would
	// have made it eight parameters. What the three share is the CONVENTION - every fraction
	// in FGearPose is named for its rest state, is 1.0 there, and every rig's bind pose IS
	// that rest state - so "how far from the bind pose" is one minus the fraction each time.

	// plane4's measured rig angles: the gear folds 90 degrees from a bind pose that is DOWN,
	// and the doors are SHUT at 81 from a bind pose that is OPEN - "found by sweeping: the two
	// free edges meet on the centreline to 0.0 mm", per its build_export.py.
	const float Retracted = 90.0f;
	const float DoorClosed = 81.0f;
	// NOT A MEASUREMENT. No rig in this fleet has a truck yet - plane6 is the first and is not
	// exported - so this is a round number chosen to make the arithmetic below readable, and
	// it is flagged as such so nobody copies it into an Animation Blueprint. The real figure
	// is whatever plane6's build_rig.py measures.
	const float TruckTilted = 12.0f;

	// A PARKED AEROPLANE ROTATES NO BONE AT ALL, and that is the single most useful fact in
	// this file: the bind pose IS the parked pose - gear down, bay hanging open because a
	// 737's nose doors are linked to the strut, truck flat on the tarmac. Asserted first
	// because it is what the player looks at for all but eight seconds of a flight.
	TestEqual(TEXT("gear down is no rotation at all"),
		UAirsideAgentAnim::AngleFromRestFraction(1.0f, Retracted), 0.0f);
	TestEqual(TEXT("and an open bay is no rotation either - a parked 737 is the bind pose"),
		UAirsideAgentAnim::AngleFromRestFraction(1.0f, DoorClosed), 0.0f);
	TestEqual(TEXT("and a level truck is no rotation either"),
		UAirsideAgentAnim::AngleFromRestFraction(1.0f, TruckTilted), 0.0f);

	// THE OTHER REST STATE ROTATES ALL THREE - and it is a DIFFERENT pose for each, which is
	// the whole reason the trapezoid this replaced could never be right: it returned the doors
	// to whatever value they started at.
	TestEqual(TEXT("gear up is the full fold"),
		UAirsideAgentAnim::AngleFromRestFraction(0.0f, Retracted), 90.0f);
	TestEqual(TEXT("a shut bay is the full sweep, because its bind pose is open"),
		UAirsideAgentAnim::AngleFromRestFraction(0.0f, DoorClosed), 81.0f);
	TestEqual(TEXT("and a fully tilted truck is the full tilt, because its bind pose is level"),
		UAirsideAgentAnim::AngleFromRestFraction(0.0f, TruckTilted), 12.0f);

	// EVERY ONE IS A TRAVEL, NOT A SWITCH. BOTH shipped versions of the trapezoid got the
	// mid-cycle frame wrong - the first shut the doors on the way in, the second shut them
	// while parked - and both looked like sequencing bugs rather than sign ones.
	TestEqual(TEXT("half retracted is half the fold"),
		UAirsideAgentAnim::AngleFromRestFraction(0.5f, Retracted), 45.0f);
	TestEqual(TEXT("half shut is half the sweep"),
		UAirsideAgentAnim::AngleFromRestFraction(0.5f, DoorClosed), 40.5f);
	TestEqual(TEXT("half tilted is half the tilt"),
		UAirsideAgentAnim::AngleFromRestFraction(0.5f, TruckTilted), 6.0f);

	// AND A RIG THAT HAS NO SUCH PART ROTATES NOTHING WHATEVER THE FRACTION SAYS. This is what
	// makes TruckTiltedAngleDegrees' zero default correct rather than merely harmless: every
	// airframe in the fleet is single-axle, and none of them can be made to move a bone they
	// do not have even if a truck fraction reached them.
	TestEqual(TEXT("an unmeasured travel angle rotates nothing at any fraction"),
		UAirsideAgentAnim::AngleFromRestFraction(0.0f, 0.0f), 0.0f);

	return true;
}

#endif
