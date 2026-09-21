#include "AnimYardMotion.h"

#include "Model/Airframe.h"

namespace
{
	/**
	 * THE DEMO LOOP, and the only place its shape is written down. See FYardMotion::Stages.
	 *
	 * THE ORDER IS A FLIGHT, not a list of things to demonstrate, because a bench you watch for
	 * twenty-four seconds at a time is one you stop reading as a test harness and start reading
	 * as an aeroplane - and a rig that looks wrong is easier to see against something that
	 * looks right. Roll out, steer about, stop; then take off, raise the gear, lower it, land.
	 *
	 * THE STEERING COMES BEFORE THE TAKE-OFF ROLL deliberately: it is the stage most rigs have
	 * something to say about (every ground vehicle steers; only plane4 retracts), so it happens
	 * while you are still looking rather than twenty seconds in.
	 *
	 * THE LAST STAGE RETURNS EVERY CHANNEL TO ITS RESTING VALUE. The wrap is a cut, and a cut
	 * from flying speed to parked is visible every twenty-four seconds - see
	 * AirportMgr.View.AnimYard.LoopWrapsToParked, which is what stops that being reintroduced.
	 */
	const FYardStage GStages[] =
	{
		{ EYardStage::RollOut,     3.0 },
		{ EYardStage::SteerLeft,   3.0 },
		{ EYardStage::SteerRight,  3.0 },
		{ EYardStage::SteerCentre, 2.0 },
		{ EYardStage::SlowToStop,  2.0 },
		{ EYardStage::TakeoffRoll, 3.0 },
		{ EYardStage::Climb,       1.0 },
		{ EYardStage::GearUp,      2.0 },
		{ EYardStage::GearDown,    3.0 },
		{ EYardStage::Touchdown,   2.0 },
	};

	/** The order Tab walks. Speed first because it is the one every rig in the yard responds to. */
	const EYardChannel GChannels[] =
	{
		EYardChannel::GroundSpeed,
		EYardChannel::Steer,
		EYardChannel::GearCycle,
		EYardChannel::EngineRPM,
	};

	/**
	 * Move a commanded gear travel on, and settle it when it arrives.
	 *
	 * PREFIXED, like everything else that lives in this file's anonymous namespace - the module
	 * is a unity build, so these names are not file-private in practice. See AnimYardHUD.cpp,
	 * which broke the build at five unrelated lines by forgetting it.
	 *
	 * CLAMPED, NOT WRAPPED, the same rule FGearPerformance::FractionsAt states for the same
	 * reason: a cycle that ran past its end would send the gear back down the moment it
	 * finished coming up.
	 */
	void AdvanceYardConfiguration(FYardMotion& Motion, double DeltaSeconds)
	{
		const bool bRetracting = Motion.Config == EYardConfig::Retracting;
		const bool bExtending = Motion.Config == EYardConfig::Extending;
		if (!bRetracting && !bExtending)
		{
			return;
		}

		const double Travel = FYardMotion::SecondsOf(
			bRetracting ? EYardStage::GearUp : EYardStage::GearDown);

		Motion.ConfigElapsed = FMath::Clamp(Motion.ConfigElapsed + DeltaSeconds, 0.0, Travel);
		const double Alpha = Travel > 0.0 ? Motion.ConfigElapsed / Travel : 1.0;

		// ONE IS STOWED - see FYardMotion::GearCycleFraction. A retraction walks it up and an
		// extension walks it down, and FractionsAt makes the second the exact time-reverse of
		// the first, so the two read one curve from opposite ends.
		Motion.GearCycleFraction = bRetracting ? Alpha : 1.0 - Alpha;

		if (Motion.ConfigElapsed >= Travel)
		{
			Motion.Config = bRetracting ? EYardConfig::Airborne : EYardConfig::OnGround;
			Motion.ConfigElapsed = 0.0;

			// SNAPPED TO THE END rather than left at whatever the last alpha computed. A leg
			// resting at 0.9999 is a leg not quite locked, and that shows on a door whose two
			// free edges are authored to meet at 0.0 mm.
			Motion.GearCycleFraction = bRetracting ? 1.0 : 0.0;
		}
	}

	/**
	 * Which stage a loop time lands in, and how far through it, 0..1.
	 *
	 * SHARED BY Advance AND CurrentStage rather than written twice, because the HUD's stage
	 * name and the channel values it sits beside must describe the same instant - two walks of
	 * the same array are two things that can disagree by a frame.
	 */
	EYardStage StageAt(double LoopTime, double& OutAlpha)
	{
		double Start = 0.0;
		for (const FYardStage& Stage : GStages)
		{
			if (LoopTime < Start + Stage.Seconds)
			{
				OutAlpha = Stage.Seconds > 0.0 ? (LoopTime - Start) / Stage.Seconds : 0.0;
				return Stage.Stage;
			}
			Start += Stage.Seconds;
		}

		// Past the end, which Advance's wrap makes unreachable and a direct caller could still
		// manage. The last stage, finished, is the answer that cannot surprise anyone.
		OutAlpha = 1.0;
		return GStages[UE_ARRAY_COUNT(GStages) - 1].Stage;
	}
}

TArrayView<const FYardStage> FYardMotion::Stages()
{
	return TArrayView<const FYardStage>(GStages, UE_ARRAY_COUNT(GStages));
}

TArrayView<const EYardChannel> FYardMotion::Channels()
{
	return TArrayView<const EYardChannel>(GChannels, UE_ARRAY_COUNT(GChannels));
}

double FYardMotion::LoopSeconds()
{
	// SUMMED, NOT TYPED. A constant beside the array is a second figure that has to be edited
	// whenever a stage duration changes, and it is the one nobody remembers to edit.
	double Total = 0.0;
	for (const FYardStage& Stage : GStages) { Total += Stage.Seconds; }
	return Total;
}

const TCHAR* FYardMotion::StageName(EYardStage Stage)
{
	switch (Stage)
	{
	case EYardStage::RollOut:     return TEXT("roll out");
	case EYardStage::SteerLeft:   return TEXT("steer left");
	case EYardStage::SteerRight:  return TEXT("steer right");
	case EYardStage::SteerCentre: return TEXT("centre");
	case EYardStage::SlowToStop:  return TEXT("slow to stop");
	case EYardStage::TakeoffRoll: return TEXT("take-off roll");
	case EYardStage::Climb:       return TEXT("climb");
	case EYardStage::GearUp:      return TEXT("gear up");
	case EYardStage::GearDown:    return TEXT("gear down");
	case EYardStage::Touchdown:   return TEXT("touchdown");
	}
	return TEXT("?");
}

const TCHAR* FYardMotion::ChannelName(EYardChannel Channel)
{
	switch (Channel)
	{
	case EYardChannel::GroundSpeed: return TEXT("speed");
	case EYardChannel::Steer:       return TEXT("steer");
	case EYardChannel::GearCycle:   return TEXT("gear");
	case EYardChannel::EngineRPM:   return TEXT("prop RPM");
	}
	return TEXT("?");
}

void FYardMotion::Advance(double DeltaSeconds)
{
	// SERVICED BEFORE THE PAUSE CHECK, AND THAT IS THE POINT. bPaused freezes the demo LOOP; a
	// configuration the player commanded is not the loop, and ToggleConfiguration pauses as it
	// starts one. A travel that stopped here would make the key appear to do nothing.
	AdvanceYardConfiguration(*this, DeltaSeconds);

	if (bPaused) { return; }

	const double Loop = LoopSeconds();
	if (Loop <= 0.0) { return; }

	LoopTime = FMath::Fmod(LoopTime + DeltaSeconds, Loop);
	if (LoopTime < 0.0) { LoopTime += Loop; }

	double Alpha = 0.0;
	const EYardStage Stage = StageAt(LoopTime, Alpha);

	// EVERY CASE SETS EVERY CHANNEL, including the ones it is not about. Letting a stage leave
	// a channel at whatever the last one set makes the state depend on the path taken to get
	// here, and the first thing anybody does with a bench is jump the loop time to look at one
	// stage. This way the loop time alone decides what is on screen.
	switch (Stage)
	{
	case EYardStage::RollOut:
		GroundSpeed = FMath::Lerp(0.0, TaxiSpeed, Alpha);
		SteerDegrees = 0.0;
		GearCycleFraction = 0.0;
		EngineRPM = FMath::Lerp(0.0, IdleRPM, Alpha);
		bAirborne = false;
		break;

	case EYardStage::SteerLeft:
		GroundSpeed = TaxiSpeed;
		SteerDegrees = FMath::Lerp(0.0, -MaxSteerDegrees, Alpha);
		GearCycleFraction = 0.0;
		EngineRPM = IdleRPM;
		bAirborne = false;
		break;

	case EYardStage::SteerRight:
		GroundSpeed = TaxiSpeed;
		SteerDegrees = FMath::Lerp(-MaxSteerDegrees, MaxSteerDegrees, Alpha);
		GearCycleFraction = 0.0;
		EngineRPM = IdleRPM;
		bAirborne = false;
		break;

	case EYardStage::SteerCentre:
		GroundSpeed = TaxiSpeed;
		SteerDegrees = FMath::Lerp(MaxSteerDegrees, 0.0, Alpha);
		GearCycleFraction = 0.0;
		EngineRPM = IdleRPM;
		bAirborne = false;
		break;

	case EYardStage::SlowToStop:
		GroundSpeed = FMath::Lerp(TaxiSpeed, 0.0, Alpha);
		SteerDegrees = 0.0;
		GearCycleFraction = 0.0;
		EngineRPM = IdleRPM;
		bAirborne = false;
		break;

	case EYardStage::TakeoffRoll:
		GroundSpeed = FMath::Lerp(0.0, TakeoffSpeed, Alpha);
		SteerDegrees = 0.0;
		GearCycleFraction = 0.0;
		EngineRPM = FMath::Lerp(IdleRPM, MaxRPM, Alpha);
		bAirborne = false;
		break;

	case EYardStage::Climb:
		GroundSpeed = TakeoffSpeed;
		SteerDegrees = 0.0;
		GearCycleFraction = 0.0;
		EngineRPM = MaxRPM;
		// THE WHEELS STOP HERE, not in GearUp. UAirsideAgentAnim decays its own wheel rate over
		// WheelSpinDownSeconds once this goes true, and that decay is the thing to watch - so
		// it gets a stage to itself with the gear still down and the wheels still in plain
		// sight, exactly as that property's comment argues happens in a real climb.
		bAirborne = true;
		break;

	case EYardStage::GearUp:
		GroundSpeed = TakeoffSpeed;
		SteerDegrees = 0.0;
		GearCycleFraction = FMath::Lerp(0.0, 1.0, Alpha);
		EngineRPM = MaxRPM;
		bAirborne = true;
		break;

	case EYardStage::GearDown:
		GroundSpeed = TakeoffSpeed;
		SteerDegrees = 0.0;
		// LONGER THAN THE RAISE, at three seconds against two, and not because the cycle is:
		// FractionsAt makes lowering the exact time-reverse of raising. It is the stage you
		// want longest, because the doors lead on extension and that is the order the rig is
		// most often wired backwards.
		GearCycleFraction = FMath::Lerp(1.0, 0.0, Alpha);
		EngineRPM = MaxRPM;
		bAirborne = true;
		break;

	case EYardStage::Touchdown:
		GroundSpeed = FMath::Lerp(TakeoffSpeed, 0.0, Alpha);
		SteerDegrees = 0.0;
		GearCycleFraction = 0.0;
		EngineRPM = FMath::Lerp(MaxRPM, 0.0, Alpha);
		bAirborne = false;
		break;
	}

	// THE LOOP OWNS THE CONFIGURATION WHILE IT IS RUNNING, so the readout says the same thing
	// whichever is in charge: the loop's own gear stages report a travel, exactly as a commanded
	// retraction does, rather than the HUD calling a moving leg "airborne, clean".
	//
	// UNREACHABLE WHILE A COMMAND IS IN FLIGHT: ToggleConfiguration pauses, and a paused loop
	// has already returned above. The two never write this field on the same frame.
	Config = Stage == EYardStage::GearUp
		? EYardConfig::Retracting
		: Stage == EYardStage::GearDown
			? EYardConfig::Extending
			: (bAirborne ? EYardConfig::Airborne : EYardConfig::OnGround);
	ConfigElapsed = 0.0;
}

void FYardMotion::Scrub(EYardChannel Channel, double Delta)
{
	// See the header: pausing is part of the scrub, not the caller's job to remember.
	bPaused = true;

	// A HAND ON A CHANNEL CANCELS A COMMANDED TRAVEL. Two writers to the gear fraction would
	// fight for it every frame, and the drag would be overwritten by the rest of a cycle nobody
	// asked to continue. The leg is left exactly where it stands; which settled state that
	// counts as is decided by the wheels, which the travel has already set.
	if (Config == EYardConfig::Retracting || Config == EYardConfig::Extending)
	{
		Config = bAirborne ? EYardConfig::Airborne : EYardConfig::OnGround;
		ConfigElapsed = 0.0;
	}

	double Min = 0.0;
	double Max = 0.0;
	ChannelRange(Channel, Min, Max);
	const double Next = FMath::Clamp(Value(Channel) + Delta, Min, Max);

	switch (Channel)
	{
	case EYardChannel::GroundSpeed: GroundSpeed = Next; break;
	case EYardChannel::Steer:       SteerDegrees = Next; break;
	case EYardChannel::GearCycle:   GearCycleFraction = Next; break;
	case EYardChannel::EngineRPM:   EngineRPM = Next; break;
	}
}

const TCHAR* FYardMotion::ConfigName(EYardConfig InConfig)
{
	switch (InConfig)
	{
	case EYardConfig::OnGround:   return TEXT("on the wheels");
	case EYardConfig::Retracting: return TEXT("gear retracting");
	case EYardConfig::Airborne:   return TEXT("airborne, clean");
	case EYardConfig::Extending:  return TEXT("gear extending");
	}
	return TEXT("?");
}

double FYardMotion::SecondsOf(EYardStage Stage)
{
	// READ FROM THE ONE STAGE LIST. See the header: a commanded gear travel runs for exactly as
	// long as the one the demo loop shows, because there is only one figure to read.
	for (const FYardStage& Entry : GStages)
	{
		if (Entry.Stage == Stage)
		{
			return Entry.Seconds;
		}
	}
	return 0.0;
}

void FYardMotion::ToggleConfiguration()
{
	// See the header: taking control is the whole of what makes the command stick.
	bPaused = true;

	// EXTENDING COUNTS AS BEING ON THE WAY DOWN, so pressing the key during one sends the leg
	// back up rather than restarting the extension - which is what "toggle" has to mean when
	// there are four states and only two of them are settled.
	const bool bGoingUp = Config == EYardConfig::OnGround || Config == EYardConfig::Extending;

	// SEEDED FROM WHERE THE LEG ACTUALLY IS. Reversing halfway up carries on down from halfway;
	// zeroing the elapsed time would snap it to the end it started from, which at a bench reads
	// as the rig failing rather than as the bench restarting. The progress is measured along the
	// NEW direction, which is why it is one minus the fraction when coming down.
	const double Travel = SecondsOf(bGoingUp ? EYardStage::GearUp : EYardStage::GearDown);
	const double Progress = bGoingUp ? GearCycleFraction : 1.0 - GearCycleFraction;
	ConfigElapsed = FMath::Clamp(Progress, 0.0, 1.0) * Travel;

	Config = bGoingUp ? EYardConfig::Retracting : EYardConfig::Extending;

	// THE WHEELS CHANGE ON THE COMMAND, NOT AT THE END OF THE TRAVEL. Going up, bAirborne is
	// what makes UAirsideAgentAnim decay its own wheel rate over WheelSpinDownSeconds, and that
	// spin-down is the thing worth watching - it has to begin as the legs do, not two seconds
	// after them. Coming down the wheels are given taxi speed again, which is the "ground expand
	// and open" half of the request: something for the wheel animation to actually run on.
	//
	// THIS OVERWRITES A SCRUBBED SPEED, deliberately. The key is a configuration command - put
	// it on the ground with its wheels turning - rather than a request to move one channel, and
	// a version that preserved a hand-set zero would land an aeroplane on dead wheels.
	bAirborne = bGoingUp;
	GroundSpeed = bGoingUp ? 0.0 : TaxiSpeed;
}

void FYardMotion::Reset()
{
	// FIELD BY FIELD RATHER THAN `*this = FYardMotion()`, which would also throw away the
	// tunables above. Reset means "put the aeroplane back on its stand", not "forget how the
	// bench was set up".
	GroundSpeed = 0.0;
	SteerDegrees = 0.0;
	GearCycleFraction = 0.0;
	EngineRPM = 0.0;
	bAirborne = false;
	bPaused = false;
	LoopTime = 0.0;
	Config = EYardConfig::OnGround;
	ConfigElapsed = 0.0;
}

EYardStage FYardMotion::CurrentStage() const
{
	double Alpha = 0.0;
	return StageAt(LoopTime, Alpha);
}

double FYardMotion::Value(EYardChannel Channel) const
{
	switch (Channel)
	{
	case EYardChannel::GroundSpeed: return GroundSpeed;
	case EYardChannel::Steer:       return SteerDegrees;
	case EYardChannel::GearCycle:   return GearCycleFraction;
	case EYardChannel::EngineRPM:   return EngineRPM;
	}
	return 0.0;
}

void FYardMotion::ChannelRange(EYardChannel Channel, double& OutMin, double& OutMax) const
{
	switch (Channel)
	{
	case EYardChannel::GroundSpeed:
		// NO REVERSE END. The bench has no pushback stage, and a negative speed here would
		// look identical to an interpolation running the wrong way - see the loop test.
		OutMin = 0.0;
		OutMax = TakeoffSpeed;
		return;

	case EYardChannel::Steer:
		OutMin = -MaxSteerDegrees;
		OutMax = MaxSteerDegrees;
		return;

	case EYardChannel::GearCycle:
		OutMin = 0.0;
		OutMax = 1.0;
		return;

	case EYardChannel::EngineRPM:
		OutMin = 0.0;
		OutMax = MaxRPM;
		return;
	}

	OutMin = 0.0;
	OutMax = 0.0;
}

double FYardMotion::ChannelStep(EYardChannel Channel) const
{
	// A HUNDREDTH OF EACH CHANNEL'S OWN RANGE, so one notch feels the same whichever channel
	// the caret is on and no figure here has to be retuned when a tunable above changes. The
	// gear is the exception: its range is 0..1 and a hundredth of a cycle is too fine to walk
	// a three-second retraction with, so it gets a fiftieth.
	double Min = 0.0;
	double Max = 0.0;
	ChannelRange(Channel, Min, Max);

	const double Divisor = Channel == EYardChannel::GearCycle ? 50.0 : 100.0;
	return (Max - Min) / Divisor;
}

FAgentMotion FYardMotion::ToAgentMotion(const FGearPerformance& Gear) const
{
	// DEFAULT-CONSTRUCTED AND THEN OVERWRITTEN IN PART. Position, heading, altitude and pitch
	// are left exactly as FAgentMotion declares them, which is how the models stay on their
	// marks - see the struct header, and AirportMgr.View.AnimYard.StaysOnItsMark, which is
	// what fails if a later hand decides an airborne aeroplane ought to rise.
	FAgentMotion Motion;

	Motion.GroundSpeed = GroundSpeed;
	Motion.SteerAngleDegrees = SteerDegrees;
	Motion.EngineRPM = EngineRPM;
	Motion.bEngineRunning = EngineRPM > 0.0;
	Motion.bAirborne = bAirborne;

	// GUARDED ON IsSet() RATHER THAN CALLED UNCONDITIONALLY, and the guard is load-bearing.
	// FractionsAt's unauthored branch answers `bRaising ? 0.0 : 1.0` for the gear, so asking it
	// to raise a fixed-gear airframe returns GEAR STOWED - it is a don't-care path for the
	// model, which checks IsSet() before it ever gets there (FRoadAgent::AdvanceGear), and the
	// bench has to make the same check rather than inherit the answer. Every aircraft in the
	// yard but plane4 is fixed-gear today, so without this the whole row would fold legs it has
	// not got.
	//
	// THE WHOLE POSE, ASSIGNED WHOLE. FractionsAt returns an FGearPose rather than filling two
	// out-parameters (#248), and taking the struct is what makes the bench show the MAIN TRUCK
	// TILT for nothing: a third fraction was added to the model and the yard needed no change
	// to put it on screen. Naming the fields here instead would have quietly dropped it, which
	// is the whole argument for passing the bundle - see CLAUDE.md on one struct per thing.
	if (Gear.IsSet())
	{
		Motion.GearPose = Gear.FractionsAt(GearCycleFraction * Gear.CycleSeconds(), /*bRaising*/ true);
	}

	return Motion;
}
