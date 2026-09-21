#pragma once

#include "CoreMinimal.h"
#include "Model/AgentMotion.h"

struct FGearPerformance;

/**
 * A value the bench can drag by hand.
 *
 * NOT bAirborne, which is a toggle rather than a range and lives on its own key. A channel is
 * something with two ends and a step between them; a flag dragged through a range would be a
 * slider with two positions and a caret you have to stop on.
 */
enum class EYardChannel : uint8
{
	GroundSpeed,
	Steer,
	GearCycle,
	EngineRPM,
};

/** One leg of the demo loop. See FYardMotion::Stages, which is the list these name. */
enum class EYardStage : uint8
{
	RollOut,
	SteerLeft,
	SteerRight,
	SteerCentre,
	SlowToStop,
	TakeoffRoll,
	Climb,
	GearUp,
	GearDown,
	Touchdown,
};

/** A stage and how long it runs. */
struct FYardStage
{
	EYardStage Stage = EYardStage::RollOut;

	/** Seconds. */
	double Seconds = 0.0;
};

/**
 * What the model yard is telling every rig to do - the bench's whole simulation.
 *
 * WORLD-FREE AND ACTOR-FREE, deliberately, the same split UAirsideAgentAnim's static helpers
 * make and for the same reason: everything here is arithmetic over time, which is exactly the
 * kind of thing that is unreadable inside an actor's Tick and untestable once it is there.
 * AAnimYard owns one of these and does nothing but convert what it says into an FAgentMotion
 * per subject.
 *
 * ONE PRODUCER, TWO WAYS IN. Advance walks the demo loop; Scrub drags a single channel. Both
 * write the SAME fields, so the canned loop and the hand-scrub can never show two different
 * things - which they would the moment a scrub kept its value anywhere else.
 *
 * IT DRIVES PARTS AND NOT AIRCRAFT. Position, heading, altitude and pitch are never set: the
 * yard's models animate on their marks, because a bench whose subjects taxied away would empty
 * itself in ten seconds. That is the one deliberate difference from the airport, where the
 * same FAgentMotion carries a pose as well - see ToAgentMotion.
 */
struct AIRPORTMGR_API FYardMotion
{
	// --- Tunables -----------------------------------------------------------------------
	//
	// HERE RATHER THAN AS UPROPERTYs ON AAnimYard. A figure copied onto the actor would be a
	// second source of truth for the same number, and Content/'s rule (one resolver per
	// default) is the same rule. They are public, so a future actor property can be a
	// forwarder into one of them rather than a copy of it.

	/** What the roll-out and steering stages taxi at, uu per second. 8 m/s, a brisk taxi. */
	double TaxiSpeed = 800.0;

	/**
	 * What the take-off roll reaches, uu per second.
	 *
	 * FAST ENOUGH TO ALIAS, on purpose: 50 m/s against plane4's 0.21 m wheel is ~13,600 deg/s,
	 * which is the regime UAirsideAgentAnim::WheelStepDegrees exists to keep readable. A bench
	 * that never left taxi speed would never show the case the arithmetic was written for.
	 */
	double TakeoffSpeed = 5000.0;

	/**
	 * How far the steering sweeps each way, degrees.
	 *
	 * 60 is past any real nosewheel limit and that is the point - a bench looks for the angle
	 * at which a rig breaks, so it has to ask for more than the model ever will.
	 */
	double MaxSteerDegrees = 60.0;

	/** Taxi RPM. Below UAirsideAgentAnim::PropDiscRPM's 400 would show blades only. */
	double IdleRPM = 700.0;

	/** Take-off RPM. Above the disc threshold, so both sides of bPropIsDisc are on screen. */
	double MaxRPM = 2200.0;

	// --- Channel state ------------------------------------------------------------------

	/** uu per second. Never negative here: the bench does not reverse. */
	double GroundSpeed = 0.0;

	/** Nosewheel deflection, degrees, signed the way FAgentMotion::SteerAngleDegrees is. */
	double SteerDegrees = 0.0;

	/**
	 * How far through a RETRACTION, 0 down and locked, 1 stowed.
	 *
	 * A FRACTION OF THE CYCLE AND NOT A PAIR OF FRACTIONS, so that dragging it walks the real
	 * curve - doors and all - rather than a straight line the bench invented. ToAgentMotion
	 * hands it to FGearPerformance::FractionsAt, which calls itself THE ONE EVALUATOR and
	 * gives the reason: a second one lets the doors the player sees disagree with the doors
	 * the model thinks it opened. A bench is the likeliest place for that second one to get
	 * written, which is why Airside.View.AnimYard.GearUsesTheOneEvaluator pins it.
	 *
	 * LOWERING NEEDS NO SEPARATE STATE. FractionsAt documents that lowering is the exact
	 * time-reverse of raising, so scrubbing this DOWN is a gear extension, on the same curve.
	 */
	double GearCycleFraction = 0.0;

	/** RPM. See FAgentMotion::EngineRPM - the real figure, which the view caps for display. */
	double EngineRPM = 0.0;

	/** Off the wheels. A toggle, not a channel - see EYardChannel. */
	bool bAirborne = false;

	// --- Loop state ---------------------------------------------------------------------

	/** Frozen. Set by Scrub as well as by the pause key - see Scrub. */
	bool bPaused = false;

	/** Where the demo loop has got to, seconds, wrapped to LoopSeconds(). */
	double LoopTime = 0.0;

	// --- The list -----------------------------------------------------------------------

	/**
	 * THE STAGE LIST, and the only place the loop's shape is written down.
	 *
	 * The HUD names the current stage from this same array rather than from a parallel switch,
	 * because a list and a list of names for it are two lists that must agree - see CLAUDE.md
	 * on where a list is CONSUMED. Airside.View.AnimYard.LoopMovesEveryChannel walks every
	 * entry and checks the loop actually lands on it.
	 */
	static TArrayView<const FYardStage> Stages();

	/** Every scrubbable channel, in the order Tab walks them. */
	static TArrayView<const EYardChannel> Channels();

	/** How long one pass of the demo loop takes, seconds. The sum of Stages(). */
	static double LoopSeconds();

	static const TCHAR* StageName(EYardStage Stage);
	static const TCHAR* ChannelName(EYardChannel Channel);

	// --- Driving ------------------------------------------------------------------------

	/**
	 * Walk the demo loop forward. A no-op while paused.
	 *
	 * RECOMPUTES EVERY CHANNEL FROM THE WRAPPED LOOP TIME rather than integrating each one
	 * forward. Integration would make the state depend on the frame rate and on which stages
	 * happened to be visited, and a bench that shows something slightly different at 30 fps
	 * than at 120 is a bench you cannot compare two rigs on.
	 */
	void Advance(double DeltaSeconds);

	/**
	 * Drag one channel by Delta, clamped to that channel's ends, and PAUSE.
	 *
	 * PAUSING IS PART OF THE SCRUB and not the caller's job to remember: without it the next
	 * Advance overwrites the value that was just dragged, and the rig snaps back while you are
	 * still looking at it. One rule in one place.
	 */
	void Scrub(EYardChannel Channel, double Delta);

	/** Back to parked, loop running from the top. */
	void Reset();

	/** Which stage the loop is in. Meaningless while paused, in the sense that it is not moving. */
	EYardStage CurrentStage() const;

	double Value(EYardChannel Channel) const;
	void ChannelRange(EYardChannel Channel, double& OutMin, double& OutMax) const;

	/** One notch of this channel, in its own units. The controller scales this for a fine drag. */
	double ChannelStep(EYardChannel Channel) const;

	/**
	 * What to hand ARoadAgentActor::SetMotion.
	 *
	 * TAKES THE SUBJECT'S OWN GEAR FIGURES rather than holding a set here, because the travel
	 * and door times are a fact about one airframe - plane4's are the fleet's only authored
	 * ones - and the bench drives ten rigs from one set of channels. The channel is normalised
	 * and each subject converts it with its own cycle length, which is the same split
	 * FAgentMotion::GearDownFraction already makes between the model and the rig.
	 */
	FAgentMotion ToAgentMotion(const FGearPerformance& Gear) const;
};
