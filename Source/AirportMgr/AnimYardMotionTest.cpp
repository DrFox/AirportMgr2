#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "AnimYardMotion.h"
#include "Model/Airframe.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** plane4's figures, the only retractable rig in the fleet. Two-second legs, one-second doors. */
	FGearPerformance RetractableGear()
	{
		FGearPerformance Gear;
		Gear.TravelSeconds = 2.0;
		Gear.DoorSeconds = 1.0;
		return Gear;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimYardMotionLoopMovesEveryChannelTest,
	"AirportMgr.View.AnimYard.LoopMovesEveryChannel",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAnimYardMotionLoopMovesEveryChannelTest::RunTest(const FString& Parameters)
{
	// THE TEST THE STAGE LIST EXISTS FOR. A demo loop is a list of stages, and the failure
	// mode of a list of stages is one that names a thing it does not do - the banner that
	// advertised "4 routes" while EKeys::Four was unbound, in another form. Walking the whole
	// loop and recording what each channel actually REACHED is what distinguishes a steering
	// sweep from a stage called SteerLeft that moves nothing.
	FYardMotion Motion;

	double MinSpeed = 0.0, MaxSpeed = 0.0;
	double MinSteer = 0.0, MaxSteer = 0.0;
	double MinGear = 1.0, MaxGear = 0.0;
	double MaxRPM = 0.0;
	bool bEverAirborne = false;
	bool bBackOnTheGroundAfterwards = false;

	TSet<EYardStage> Visited;

	// A 60th of a second, the frame rate every display figure in UAirsideAgentAnim is
	// authored against - so the walk samples the loop the way the game will.
	const double Step = 1.0 / 60.0;
	const int32 Frames = FMath::CeilToInt(FYardMotion::LoopSeconds() / Step);

	for (int32 Frame = 0; Frame < Frames; ++Frame)
	{
		Motion.Advance(Step);

		Visited.Add(Motion.CurrentStage());
		MinSpeed = FMath::Min(MinSpeed, Motion.GroundSpeed);
		MaxSpeed = FMath::Max(MaxSpeed, Motion.GroundSpeed);
		MinSteer = FMath::Min(MinSteer, Motion.SteerDegrees);
		MaxSteer = FMath::Max(MaxSteer, Motion.SteerDegrees);
		MinGear = FMath::Min(MinGear, Motion.GearCycleFraction);
		MaxGear = FMath::Max(MaxGear, Motion.GearCycleFraction);
		MaxRPM = FMath::Max(MaxRPM, Motion.EngineRPM);

		if (Motion.bAirborne) { bEverAirborne = true; }
		else if (bEverAirborne) { bBackOnTheGroundAfterwards = true; }
	}

	// 1. EVERY STAGE IS VISITED. A stage in the list that no elapsed time ever lands on is a
	// stage nobody will ever see, and the list is the only place its name appears.
	for (const FYardStage& Stage : FYardMotion::Stages())
	{
		TestTrue(*FString::Printf(TEXT("the loop visits %s, which is in the stage list"),
			FYardMotion::StageName(Stage.Stage)), Visited.Contains(Stage.Stage));
	}

	// 2. THE WHEELS GET SOMETHING TO ROLL AT. Taxi speed at least - the figure the roll-out
	// stage claims to reach.
	TestTrue(TEXT("the loop reaches taxi speed, so the wheels visibly turn"),
		MaxSpeed >= Motion.TaxiSpeed - KINDA_SMALL_NUMBER);

	// 3. THE STEERING SWEEPS BOTH WAYS. One-sided steering looks like working steering until
	// the day a rig's steer bone is mirrored, which is exactly what this bench is for.
	TestTrue(TEXT("the loop steers fully left"), MinSteer <= -Motion.MaxSteerDegrees + KINDA_SMALL_NUMBER);
	TestTrue(TEXT("the loop steers fully right"), MaxSteer >= Motion.MaxSteerDegrees - KINDA_SMALL_NUMBER);

	// 4. THE GEAR MAKES A WHOLE CYCLE AND COMES BACK. Up alone would leave the aeroplane
	// parked with its legs stowed at the wrap.
	TestTrue(TEXT("the gear reaches fully stowed"), MaxGear >= 1.0 - KINDA_SMALL_NUMBER);
	TestTrue(TEXT("the gear comes back down and locked"), MinGear <= KINDA_SMALL_NUMBER);

	// 5. THE PROPELLER WINDS UP PAST THE DISC THRESHOLD, so both sides of bPropIsDisc are on
	// screen during one loop rather than only the slow one.
	TestTrue(TEXT("the loop reaches full engine RPM"), MaxRPM >= Motion.MaxRPM - KINDA_SMALL_NUMBER);

	// 6. IT LEAVES THE GROUND AND RETURNS. bAirborne is what switches the wheel from driven
	// to decaying (UAirsideAgentAnim::WheelStepDegrees), so a loop that never sets it leaves
	// the spin-down half of that rule unwatchable.
	TestTrue(TEXT("the loop goes airborne"), bEverAirborne);
	TestTrue(TEXT("the loop lands again, so the wrap is not a snap from flight to parked"),
		bBackOnTheGroundAfterwards);

	// 7. NOTHING GOES BACKWARDS. The bench never reverses; a negative ground speed here would
	// be an interpolation running the wrong way rather than a pushback.
	TestTrue(TEXT("no stage drives the wheels backwards"), MinSpeed >= -KINDA_SMALL_NUMBER);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimYardMotionLoopWrapsToParkedTest,
	"AirportMgr.View.AnimYard.LoopWrapsToParked",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAnimYardMotionLoopWrapsToParkedTest::RunTest(const FString& Parameters)
{
	// THE WRAP IS A CUT, and the only way to make a cut invisible is to arrive at the state
	// you are about to jump to. A loop ending at flying speed with the gear up snaps to a
	// parked aeroplane every 24 seconds, in full view.
	//
	// MEASURED ACROSS THE JOIN, as the LAST FRAME BEFORE IT against the FIRST FRAME AFTER, and
	// that phrasing is the whole test. Advancing by exactly LoopSeconds() does NOT land on the
	// end: the wrap sends it to zero, so it silently measures the loop's start twice and passes
	// whatever the last stage does. That was this test's first form, and the mutation that
	// proved it - leaving the engine at full RPM through Touchdown - went straight through it.
	const double Step = 1.0 / 60.0;

	FYardMotion JustBefore;
	JustBefore.Advance(FYardMotion::LoopSeconds() - Step);

	FYardMotion JustAfter;
	JustAfter.Advance(Step);

	// TWO FRAMES' WORTH OF TOLERANCE. The join is two samples a frame apart, and each of them
	// is itself up to a frame away from the exact boundary, so one frame's worth is the margin
	// that only just holds - and a test that only just holds is one that goes red on a stage
	// duration nobody thought was load-bearing.
	const auto Joins = [this, Step](const TCHAR* What, double Before, double After, double PerSecond)
	{
		const double Tolerance = FMath::Abs(PerSecond) * Step * 2.0 + 1e-6;
		TestTrue(*FString::Printf(TEXT("%s joins across the wrap (%.3f then %.3f)"), What, Before, After),
			FMath::Abs(Before - After) <= Tolerance);
	};

	Joins(TEXT("speed"), JustBefore.GroundSpeed, JustAfter.GroundSpeed, JustBefore.TakeoffSpeed / 2.0);
	Joins(TEXT("RPM"), JustBefore.EngineRPM, JustAfter.EngineRPM, JustBefore.MaxRPM / 2.0);
	Joins(TEXT("steer"), JustBefore.SteerDegrees, JustAfter.SteerDegrees, JustBefore.MaxSteerDegrees);
	Joins(TEXT("gear"), JustBefore.GearCycleFraction, JustAfter.GearCycleFraction, 1.0);

	// AND THE STATE IT JOINS AT IS THE PARKED ONE. Two channels that merely AGREE across the
	// wrap would also be satisfied by a loop that ended and began at full power; these say
	// which state it is. A fiftieth of each channel's range is "as good as zero" - the last
	// frame of a ramp has not quite arrived, and asking for exactly zero measures the frame
	// rate rather than the loop.
	TestTrue(*FString::Printf(TEXT("the wheels are as good as stopped before the wrap (%.1f uu/s)"),
		JustBefore.GroundSpeed), JustBefore.GroundSpeed <= JustBefore.TakeoffSpeed * 0.02);
	TestTrue(*FString::Printf(TEXT("the engine is as good as shut down before the wrap (%.1f RPM)"),
		JustBefore.EngineRPM), JustBefore.EngineRPM <= JustBefore.MaxRPM * 0.02);
	TestFalse(TEXT("it is on the ground on the frame before the wrap"), JustBefore.bAirborne);
	TestFalse(TEXT("and still on the ground after it"), JustAfter.bAirborne);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimYardMotionScrubTakesControlTest,
	"AirportMgr.View.AnimYard.ScrubTakesControl",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAnimYardMotionScrubTakesControlTest::RunTest(const FString& Parameters)
{
	FYardMotion Motion;
	Motion.Advance(4.0); // somewhere inside the steering sweep

	// 1. TOUCHING A VALUE PAUSES THE LOOP. Without this the scrub is unusable: the next
	// Advance overwrites whatever was just dragged, and the rig snaps back mid-look. One
	// rule, in one place, rather than a controller that must remember to pause first.
	Motion.Scrub(EYardChannel::Steer, Motion.ChannelStep(EYardChannel::Steer));
	TestTrue(TEXT("scrubbing a channel pauses the demo loop"), Motion.bPaused);

	// 2. A PAUSED LOOP IS FROZEN. Every channel, not just the one being dragged.
	const FYardMotion Frozen = Motion;
	Motion.Advance(2.0);
	TestEqual(TEXT("paused: speed holds"), Motion.GroundSpeed, Frozen.GroundSpeed, 1e-9);
	TestEqual(TEXT("paused: steer holds"), Motion.SteerDegrees, Frozen.SteerDegrees, 1e-9);
	TestEqual(TEXT("paused: gear holds"), Motion.GearCycleFraction, Frozen.GearCycleFraction, 1e-9);
	TestEqual(TEXT("paused: RPM holds"), Motion.EngineRPM, Frozen.EngineRPM, 1e-9);

	// 3. A SCRUB MOVES ITS OWN CHANNEL AND NOTHING ELSE. This is what lets the demo loop and
	// the scrub share one set of fields without the two disagreeing - drag the steering and
	// the wheels keep rolling at whatever speed you left them at.
	const double SteerBefore = Motion.SteerDegrees;
	const double SpeedBefore = Motion.GroundSpeed;
	const double GearBefore = Motion.GearCycleFraction;
	const double RPMBefore = Motion.EngineRPM;

	Motion.Scrub(EYardChannel::Steer, Motion.ChannelStep(EYardChannel::Steer));

	TestTrue(TEXT("scrubbing steer moves steer"), !FMath::IsNearlyEqual(Motion.SteerDegrees, SteerBefore));
	TestEqual(TEXT("scrubbing steer leaves speed alone"), Motion.GroundSpeed, SpeedBefore, 1e-9);
	TestEqual(TEXT("scrubbing steer leaves the gear alone"), Motion.GearCycleFraction, GearBefore, 1e-9);
	TestEqual(TEXT("scrubbing steer leaves RPM alone"), Motion.EngineRPM, RPMBefore, 1e-9);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimYardMotionScrubClampsTest,
	"AirportMgr.View.AnimYard.ScrubClamps",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAnimYardMotionScrubClampsTest::RunTest(const FString& Parameters)
{
	// EVERY CHANNEL HAS ENDS, and a scrub that runs past them is how a rig gets asked for a
	// gear fraction of 4 or a propeller at a million RPM - states the model can never produce,
	// so a defect found there would be a defect in the bench.
	for (const EYardChannel Channel : FYardMotion::Channels())
	{
		FYardMotion Up;
		double Min = 0.0, Max = 0.0;
		Up.ChannelRange(Channel, Min, Max);

		// Two hundred steps is past the end of every channel by a wide margin.
		for (int32 i = 0; i < 200; ++i) { Up.Scrub(Channel, Up.ChannelStep(Channel)); }
		TestTrue(*FString::Printf(TEXT("%s clamps at its top end"), FYardMotion::ChannelName(Channel)),
			Up.Value(Channel) <= Max + KINDA_SMALL_NUMBER);
		TestEqual(*FString::Printf(TEXT("%s reaches its top end"), FYardMotion::ChannelName(Channel)),
			Up.Value(Channel), Max, 1e-6);

		FYardMotion Down;
		for (int32 i = 0; i < 200; ++i) { Down.Scrub(Channel, -Down.ChannelStep(Channel)); }
		TestTrue(*FString::Printf(TEXT("%s clamps at its bottom end"), FYardMotion::ChannelName(Channel)),
			Down.Value(Channel) >= Min - KINDA_SMALL_NUMBER);
		TestEqual(*FString::Printf(TEXT("%s reaches its bottom end"), FYardMotion::ChannelName(Channel)),
			Down.Value(Channel), Min, 1e-6);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimYardMotionGearUsesTheOneEvaluatorTest,
	"AirportMgr.View.AnimYard.GearUsesTheOneEvaluator",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAnimYardMotionGearUsesTheOneEvaluatorTest::RunTest(const FString& Parameters)
{
	// FGearPerformance::FractionsAt CALLS ITSELF "THE ONE EVALUATOR" and says why: a second
	// one lets the doors the player sees disagree with the doors the model thinks it opened,
	// visibly and only mid-cycle. A bench is exactly where a second one would be written -
	// it needs the same curve and has its own reason to want it - so this pins that the
	// bench asks FGearPerformance rather than interpolating a pair of fractions itself.
	//
	// DELETE THE FractionsAt CALL IN ToAgentMotion AND THIS GOES RED at every sample but the
	// two ends, which is the property that matters: the ends agree under any implementation.
	const FGearPerformance Gear = RetractableGear();

	FYardMotion Motion;
	for (int32 i = 0; i <= 20; ++i)
	{
		const double Fraction = static_cast<double>(i) / 20.0;
		Motion.GearCycleFraction = Fraction;

		const FGearPose Expected = Gear.FractionsAt(Fraction * Gear.CycleSeconds(), /*bRaising*/ true);

		const FAgentMotion Agent = Motion.ToAgentMotion(Gear);
		TestEqual(*FString::Printf(TEXT("gear fraction at %.2f through the cycle"), Fraction),
			Agent.GearPose.GearDownFraction, Expected.GearDownFraction, 1e-9);
		TestEqual(*FString::Printf(TEXT("door fraction at %.2f through the cycle"), Fraction),
			Agent.GearPose.BayDoorOpenFraction, Expected.BayDoorOpenFraction, 1e-9);

		// THE MAIN TRUCK TOO, WHICH THE BENCH NEVER NAMES. #248 added a third fraction and
		// ToAgentMotion assigns the pose whole, so it arrives without the yard knowing it
		// exists - and this is what fails if a later hand "tidies" that into two named copies.
		TestEqual(*FString::Printf(TEXT("truck fraction at %.2f through the cycle"), Fraction),
			Agent.GearPose.TruckLevelFraction, Expected.TruckLevelFraction, 1e-9);
	}

	// FIXED GEAR IS NOT A DIVISION BY ZERO. An airframe with no retractable gear has
	// CycleSeconds() == 0, and every aircraft in the yard but plane4 is one today.
	FGearPerformance Fixed;
	FYardMotion Stowed;
	Stowed.GearCycleFraction = 1.0;
	const FAgentMotion Agent = Stowed.ToAgentMotion(Fixed);
	TestEqual(TEXT("fixed gear stays down whatever the cycle channel says"),
		Agent.GearPose.GearDownFraction, 1.0, 1e-9);
	TestEqual(TEXT("fixed gear leaves its doors alone"), Agent.GearPose.BayDoorOpenFraction, 1.0, 1e-9);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimYardMotionResetIsParkedTest,
	"AirportMgr.View.AnimYard.ResetIsParked",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAnimYardMotionResetIsParkedTest::RunTest(const FString& Parameters)
{
	// RESET IS THE KEY YOU PRESS WHEN YOU HAVE LOST THE RIG, so it must land on the one state
	// every airframe agrees about: FAgentMotion's own defaults, which its header calls "a
	// parked aeroplane, gear down and bay hanging open".
	FYardMotion Motion;
	Motion.Advance(17.0);
	Motion.Scrub(EYardChannel::Steer, 5.0 * Motion.ChannelStep(EYardChannel::Steer));

	Motion.Reset();

	const FAgentMotion Parked = Motion.ToAgentMotion(RetractableGear());
	const FAgentMotion Defaults;

	TestEqual(TEXT("reset stops the wheels"), Parked.GroundSpeed, Defaults.GroundSpeed, 1e-9);
	TestEqual(TEXT("reset centres the nosewheel"), Parked.SteerAngleDegrees, Defaults.SteerAngleDegrees, 1e-9);
	TestEqual(TEXT("reset puts the gear down and locked"), Parked.GearPose.GearDownFraction, Defaults.GearPose.GearDownFraction, 1e-9);
	TestEqual(TEXT("reset leaves the bay doors open, which is what down-and-locked means"),
		Parked.GearPose.BayDoorOpenFraction, Defaults.GearPose.BayDoorOpenFraction, 1e-9);
	TestEqual(TEXT("reset shuts the engine down"), Parked.EngineRPM, Defaults.EngineRPM, 1e-9);
	TestFalse(TEXT("reset puts it back on the ground"), Parked.bAirborne);

	// AND IT RESUMES THE LOOP. Reset is how you get back to watching after a scrub; leaving
	// it paused would mean two keys to do the one thing anybody wants.
	TestFalse(TEXT("reset resumes the demo loop"), Motion.bPaused);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimYardMotionStaysOnItsMarkTest,
	"AirportMgr.View.AnimYard.StaysOnItsMark",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAnimYardMotionStaysOnItsMarkTest::RunTest(const FString& Parameters)
{
	// THE BENCH DRIVES PARTS, NOT AIRCRAFT. GroundSpeed is what the wheels roll at, but a
	// yard whose models translated would empty itself in ten seconds and a climbing one would
	// leave the frame - so position, heading, altitude and pitch stay at their defaults and
	// the models animate on their marks. This is the deliberate difference from the airport,
	// where the same FAgentMotion carries a pose as well.
	FYardMotion Motion;
	const FAgentMotion Defaults;

	// THE WHOLE LOOP, not a few seconds of it. Four seconds never reaches Climb, so a bench
	// that lifted its airborne subjects off the ground would sail through a short walk - which
	// is what the first form of this test did, and what a deliberate Altitude mutation proved.
	const double Step = 1.0 / 60.0;
	const int32 Frames = FMath::CeilToInt(FYardMotion::LoopSeconds() / Step);

	for (int32 i = 0; i < Frames; ++i)
	{
		Motion.Advance(Step);
		const FAgentMotion Agent = Motion.ToAgentMotion(RetractableGear());

		if (!Agent.Position.Equals(Defaults.Position, 1e-9)
			|| !FMath::IsNearlyEqual(Agent.Heading, Defaults.Heading, 1e-9)
			|| !FMath::IsNearlyEqual(Agent.Altitude, Defaults.Altitude, 1e-9)
			|| !FMath::IsNearlyEqual(Agent.PitchDegrees, Defaults.PitchDegrees, 1e-9))
		{
			AddError(FString::Printf(
				TEXT("the bench moved a model off its mark at %.2fs into the loop: ")
				TEXT("pos (%.3f, %.3f), heading %.3f, altitude %.3f, pitch %.3f"),
				i / 60.0, Agent.Position.X, Agent.Position.Y, Agent.Heading,
				Agent.Altitude, Agent.PitchDegrees));
			return false;
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimYardConfigurationTravelsTest,
	"AirportMgr.View.AnimYard.ConfigurationTravels",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAnimYardConfigurationTravelsTest::RunTest(const FString& Parameters)
{
	// G IS A CONFIGURATION, NOT A FLAG. Asked for from play: on the ground the wheels turn and
	// the gear is down with its bays open; airborne the wheels wind down and the legs and doors
	// come up. Those move TOGETHER, and the gear TRAVELS - a fraction that snapped from 0 to 1
	// would skip the one part of the cycle anybody is looking at, and would also hide the door
	// sequencing, which is the half rigs are most often wired backwards.
	const FGearPerformance Gear = RetractableGear();

	FYardMotion Motion;
	Motion.Reset();

	TestEqual(TEXT("the bench starts on the ground"),
		static_cast<int32>(Motion.Config), static_cast<int32>(EYardConfig::OnGround));

	// 1. GOING UP STARTS A TRAVEL, it does not arrive. The wheels are commanded to stop in the
	// same breath - UAirsideAgentAnim decays its own last rate from there, which is the
	// spin-down worth watching.
	Motion.ToggleConfiguration();
	TestEqual(TEXT("the toggle starts a retraction"),
		static_cast<int32>(Motion.Config), static_cast<int32>(EYardConfig::Retracting));
	TestTrue(TEXT("it is off the wheels at once, so the spin-down starts"), Motion.bAirborne);
	TestEqual(TEXT("and the wheels are commanded to stop"), Motion.GroundSpeed, 0.0, 1e-9);
	TestEqual(TEXT("but the gear has not moved yet"), Motion.GearCycleFraction, 0.0, 1e-9);

	// 2. A COMMANDED CHANGE RUNS WHILE THE LOOP IS PAUSED. The toggle pauses, as every manual
	// input does - and if the pause froze the travel too, G would do nothing at all and the
	// gear would sit wherever it started.
	TestTrue(TEXT("the toggle takes control of the loop"), Motion.bPaused);

	const double Travel = FYardMotion::SecondsOf(EYardStage::GearUp);
	Motion.Advance(Travel * 0.5);

	TestTrue(*FString::Printf(TEXT("the gear is part way up, not at either end (%.3f)"),
		Motion.GearCycleFraction),
		Motion.GearCycleFraction > 0.01 && Motion.GearCycleFraction < 0.99);
	TestEqual(TEXT("and is still travelling"),
		static_cast<int32>(Motion.Config), static_cast<int32>(EYardConfig::Retracting));

	// 3. IT ARRIVES, ONCE, AND STAYS. A travel that ran past its end would send the gear back
	// down the moment it finished coming up.
	Motion.Advance(Travel);
	TestEqual(TEXT("the retraction finishes"),
		static_cast<int32>(Motion.Config), static_cast<int32>(EYardConfig::Airborne));
	TestEqual(TEXT("gear fully stowed"), Motion.GearCycleFraction, 1.0, 1e-9);

	Motion.Advance(Travel);
	TestEqual(TEXT("and stays stowed however long it is left"), Motion.GearCycleFraction, 1.0, 1e-9);

	// 4. CLEAN, THE DOORS ARE SHUT AND THE LEGS ARE UP - measured through the one evaluator, so
	// this is what the rig is actually told.
	{
		const FAgentMotion Clean = Motion.ToAgentMotion(Gear);
		TestEqual(TEXT("airborne: gear up"), Clean.GearPose.GearDownFraction, 0.0, 1e-9);
		TestEqual(TEXT("airborne: bays shut"), Clean.GearPose.BayDoorOpenFraction, 0.0, 1e-9);
		TestTrue(TEXT("airborne: off the wheels"), Clean.bAirborne);
	}

	// 5. COMING BACK DOWN IS THE SAME TRAVEL IN REVERSE, and the wheels are turning again at the
	// end of it - "ground expand and open", which is what the request asked for in as many words.
	Motion.ToggleConfiguration();
	TestEqual(TEXT("the toggle starts an extension"),
		static_cast<int32>(Motion.Config), static_cast<int32>(EYardConfig::Extending));

	Motion.Advance(FYardMotion::SecondsOf(EYardStage::GearDown) * 2.0);
	TestEqual(TEXT("the extension finishes on the ground"),
		static_cast<int32>(Motion.Config), static_cast<int32>(EYardConfig::OnGround));
	TestEqual(TEXT("gear down and locked"), Motion.GearCycleFraction, 0.0, 1e-9);
	TestFalse(TEXT("back on the wheels"), Motion.bAirborne);
	TestEqual(TEXT("and the wheels are turning again"), Motion.GroundSpeed, Motion.TaxiSpeed, 1e-9);

	{
		const FAgentMotion Down = Motion.ToAgentMotion(Gear);
		TestEqual(TEXT("on the ground: gear down"), Down.GearPose.GearDownFraction, 1.0, 1e-9);
		TestEqual(TEXT("on the ground: bays open"), Down.GearPose.BayDoorOpenFraction, 1.0, 1e-9);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimYardConfigurationReversesContinuouslyTest,
	"AirportMgr.View.AnimYard.ConfigurationReversesContinuously",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAnimYardConfigurationReversesContinuouslyTest::RunTest(const FString& Parameters)
{
	// PRESSING G TWICE IN A ROW IS THE OBVIOUS THING TO DO at a bench - you are looking for the
	// pose where a rig breaks, so you run the gear up, change your mind, and run it back. The
	// leg must carry on from where it is, not jump to the top and start again: a snap there
	// reads as the rig failing rather than the bench restarting.
	FYardMotion Motion;
	Motion.Reset();

	Motion.ToggleConfiguration();
	Motion.Advance(FYardMotion::SecondsOf(EYardStage::GearUp) * 0.4);

	const double Caught = Motion.GearCycleFraction;
	TestTrue(*FString::Printf(TEXT("the gear is genuinely mid-travel (%.3f)"), Caught),
		Caught > 0.05 && Caught < 0.95);

	Motion.ToggleConfiguration();
	TestEqual(TEXT("reversing does not move the leg on the frame it is commanded"),
		Motion.GearCycleFraction, Caught, 1e-9);
	TestEqual(TEXT("and it is now extending"),
		static_cast<int32>(Motion.Config), static_cast<int32>(EYardConfig::Extending));

	// AND IT GOES THE OTHER WAY from there, rather than sitting still or carrying on up.
	Motion.Advance(FYardMotion::SecondsOf(EYardStage::GearDown) * 0.2);
	TestTrue(*FString::Printf(TEXT("the leg is coming back down (%.3f from %.3f)"),
		Motion.GearCycleFraction, Caught), Motion.GearCycleFraction < Caught);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimYardScrubCancelsTheTravelTest,
	"AirportMgr.View.AnimYard.ScrubCancelsTheTravel",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAnimYardScrubCancelsTheTravelTest::RunTest(const FString& Parameters)
{
	// TWO THINGS WRITING THE GEAR FRACTION WOULD FIGHT FOR IT EVERY FRAME. A hand on the gear
	// channel wins: the travel is abandoned where it stands and the value stays where it was
	// dragged, rather than being overwritten by the rest of a cycle nobody asked to continue.
	FYardMotion Motion;
	Motion.Reset();
	Motion.ToggleConfiguration();
	Motion.Advance(FYardMotion::SecondsOf(EYardStage::GearUp) * 0.3);

	Motion.Scrub(EYardChannel::GearCycle, Motion.ChannelStep(EYardChannel::GearCycle));
	const double Dragged = Motion.GearCycleFraction;

	TestTrue(TEXT("the travel is over"), Motion.Config != EYardConfig::Retracting
		&& Motion.Config != EYardConfig::Extending);

	Motion.Advance(FYardMotion::SecondsOf(EYardStage::GearUp) * 2.0);
	TestEqual(TEXT("and nothing carries the leg on afterwards"),
		Motion.GearCycleFraction, Dragged, 1e-9);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
