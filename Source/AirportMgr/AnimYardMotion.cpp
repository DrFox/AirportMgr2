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
}

void FYardMotion::Scrub(EYardChannel Channel, double Delta)
{
	// See the header: pausing is part of the scrub, not the caller's job to remember.
	bPaused = true;

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

void FYardMotion::ToggleAirborne()
{
	// See the header: taking control is the whole of what makes the toggle stick.
	bPaused = true;
	bAirborne = !bAirborne;
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
	// FractionsAt's unauthored branch answers `bRaising ? 0.0 : 1.0`, so asking it to raise a
	// fixed-gear airframe returns GEAR STOWED - it is a don't-care path for the model, which
	// checks IsSet() before it ever gets there (FRoadAgent::AdvanceGear), and the bench has to
	// make the same check rather than inherit the answer. Every aircraft in the yard but
	// plane4 is fixed-gear today, so without this the whole row would fold legs it has not got.
	if (Gear.IsSet())
	{
		Gear.FractionsAt(GearCycleFraction * Gear.CycleSeconds(), /*bRaising*/ true,
			Motion.GearDownFraction, Motion.BayDoorOpenFraction);
	}

	return Motion;
}
