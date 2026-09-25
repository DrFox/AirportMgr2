#pragma once

// SPLIT OUT OF Model/RoadEntity.h BY #176. FAgentMotion is what the VIEW needs - a pose and
// what it is doing - and nothing about where an entity sits on the apron or how an airframe
// moves, which is why it changes for a third, unrelated set of reasons: a new bone to drive,
// not a new anchor or a new power setting. ARoadAgentActor takes just this now, instead of
// the whole of RoadEntity.h.

// Model/GearPose.h AND Model/BodyPose.h ARE THE ONLY HEADERS THIS INCLUDES BEYOND
// CoreMinimal.h, and GearPose.h was split out of Airframe.h expressly so that stayed true -
// see its own header comment. A pose is what the view needs; a performance is not. BodyPose.h
// joined it on 2026-09-25 on the same terms: a pose, and nothing else.

#include "CoreMinimal.h"
#include "Model/BodyPose.h"
#include "Model/GearPose.h"
#include "AgentMotion.generated.h"

/**
 * Where one link of a tow is this frame (see FTowLink): the point it couples at, its axle, and
 * the way it faces. Road-plane XY, uu; Heading radians from +X, axle toward hitch.
 *
 * WHAT THE VIEW PLACES A TRAILER BY - hitch and heading - and it is the model's answer, not
 * one the actor works out: a view that derived the trailer's yaw from two frames of the cab
 * would be a second trailer model beside VehicleSweep::StepChain.
 */
USTRUCT()
struct AIRSIDE_API FTowPose
{
	GENERATED_BODY()

	UPROPERTY() FVector2D Hitch = FVector2D::ZeroVector;
	UPROPERTY() FVector2D Axle = FVector2D::ZeroVector;
	UPROPERTY() double Heading = 0.0;
};

/**
 * Everything the VIEW needs to show an agent: where it is, and what it is doing.
 *
 * SetPose used to take a position, a heading, a surface height, an altitude and a pitch, and
 * animation wants three more - how fast the wheels are turning, whether the propeller is
 * running, whether the gear is still on the ground. Eight arguments in a row is a signature
 * nobody can call correctly, and the two that mattered were already easy to swap.
 *
 * It also keeps the view dumb, which is the point. ARoadAgentActor decides nothing: a wheel
 * turns at GroundSpeed over its radius because that is arithmetic, not judgement, and every
 * value here is one the model already holds - see FRouteFollower and FTakeoffRun.
 */
USTRUCT(BlueprintType)
struct AIRSIDE_API FAgentMotion
{
	GENERATED_BODY()

	/** Road-plane XY. */
	UPROPERTY() FVector2D Position = FVector2D::ZeroVector;

	/** Radians, yaw from +X. */
	UPROPERTY() double Heading = 0.0;

	/** Above the road surface, uu. Zero on the wheels. */
	UPROPERTY() double Altitude = 0.0;

	/** Nose-up, degrees. */
	UPROPERTY() double PitchDegrees = 0.0;

	/**
	 * The point the airframe pitches ABOUT, local X, uu. The main gear.
	 *
	 * X ALONE, deliberately: the pivot is the CONTACT PATCH, on the ground directly below
	 * the axle, and the axle's own height is discarded. A main-gear bone sits at the hub -
	 * one wheel radius up, 68.6 uu on plane2 - and pitching about that would drag the tyre
	 * through the tarmac. The view supplies the zero; see ARoadAgentActor::SetMotion.
	 *
	 * WITHOUT IT THE PIVOT IS THE ORIGIN, and that only looked right while origins sat
	 * mid-fuselage. plane2's origin is its nose gear (UAircraftType's local space), so a
	 * nose-up attitude about the origin levers everything aft of it into the ground - at 8
	 * degrees of flare the mains ended up 63 uu under the tarmac, which is exactly how it
	 * was reported from play.
	 *
	 * An aeroplane on the ground pitches about its MAIN GEAR, and in the air about a centre
	 * of gravity that sits close to it, so the mains serve both. Zero - an unmeasured
	 * airframe, or one whose origin already IS the mains, like the Piper - pitches about
	 * the origin exactly as it always did.
	 */
	UPROPERTY() double PitchPivotX = 0.0;

	/**
	 * Speed over the ground, uu per second. SIGNED: negative means going backwards.
	 *
	 * THE SIGN IS THE VIEW'S ONLY SOURCE OF DIRECTION. UAirsideAgentAnim::WheelStepDegrees is
	 * RadiansToDegrees(GroundSpeed / Radius) and reads nothing else, so while this was a
	 * magnitude a reversing vehicle rolled its wheels forwards - reported from play on
	 * 2026-09-20 against the fuel truck, and true of every aircraft pushback before it.
	 * FRoadAgent::DescribeMotion negates the two phases that go backwards; the run structs
	 * themselves keep magnitudes, so the direction is decided once.
	 *
	 * ANYTHING ASKING "IS IT STOPPED" MUST COMPARE THE MAGNITUDE. `<= 0.0` was that question
	 * once and is now also true of a truck backing into a bay at full crawl.
	 *
	 * DRIVES THE WHEELS ON THE GROUND ONLY (#107 item 8). It stays meaningful once airborne -
	 * a departure keeps climbing, not stopping - but UAirsideAgentAnim no longer reads it
	 * there: a wheel that stopped the instant the aircraft lifted off used to snap from
	 * spinning to still in one frame, which is the one thing real wheels visibly do not do,
	 * so the view now decays its OWN last-known rate toward zero over WheelSpinDownSeconds
	 * instead (see UAirsideAgentAnim::WheelStepDegrees) rather than integrating this figure
	 * further once bAirborne is true.
	 */
	UPROPERTY() double GroundSpeed = 0.0;

	/** The propeller turns. True whenever the aircraft is under way at all. */
	UPROPERTY() bool bEngineRunning = false;

	/**
	 * How fast the propeller is actually turning, RPM.
	 *
	 * NOT derivable from bEngineRunning, which is why both are here. That flag is what the
	 * engine has been COMMANDED to do; this is where the propeller has got to, and between a
	 * shutdown and a stopped prop there are nine seconds where they disagree.
	 */
	UPROPERTY() double EngineRPM = 0.0;

	/** Off the wheels. Stage 2's gear retraction hangs on this. */
	UPROPERTY() bool bAirborne = false;

	/**
	 * The steered wheel's deflection, degrees, signed the way Heading turns.
	 *
	 * THE CONTROL INPUT, not a description of the motion: this is the angle the follower
	 * steered with, and the yaw the aircraft took came out of it. An earlier design had the
	 * view derive this from the yaw rate for the animation's sake, which would have been a
	 * second model of the same thing - and the two would have drifted the first time either
	 * was retuned.
	 *
	 * Zero on a pivot-steered vehicle, which has no steered wheel to draw.
	 */
	UPROPERTY() double SteerAngleDegrees = 0.0;

	/**
	 * Where the undercarriage has got to - gear, bay doors and main truck. See FGearPose,
	 * which names each fraction, says which way round it counts, and carries the reasons.
	 *
	 * ONE STRUCT AND NOT THREE DOUBLES, since 2026-09-21. It was two loose doubles until truck
	 * tilt made a third, and three values that are always written together by one evaluator
	 * are one thing - CLAUDE.md's rule, and the same argument FAirframe itself is an instance
	 * of.
	 *
	 * A DEFAULT-CONSTRUCTED POSE IS A PARKED AEROPLANE, which is also the bind pose of every
	 * rig in the fleet, so a vehicle and an airframe that declared no gear both get the
	 * answer that rotates no bone at all.
	 */
	UPROPERTY() FGearPose GearPose;

	/**
	 * One pose per link of the tow, tractor end first; EMPTY for anything rigid, which is every
	 * aircraft and every vehicle but the rig and the drawbar trailer. Filled by
	 * FRoadAgent::DescribeMotion from the axles the agent steps.
	 *
	 * NOT A UPROPERTY, AND INLINE FOR TWO. This struct is copied several times per agent per
	 * sub-step - DescribeMotion returns it, LastMotion keeps it, OutMotion and the traffic
	 * loop's local take it again (RoadAgent.cpp and GroundTraffic.cpp, 2026-09-24) - and a
	 * plain TArray would make each copy of a towing vehicle's motion a heap allocation. UHT
	 * refuses an allocator on a UPROPERTY, and nothing needs this reflected: the motion is
	 * rewritten by every Advance and never saved. A rigid agent's empty array copies for free
	 * either way.
	 */
	TArray<FTowPose, TInlineAllocator<2>> Tow;

	/**
	 * Where a ground vehicle's working parts have got to - lift, platform, towbar. See
	 * FBodyPose, which says why it counts from the other end to FGearPose.
	 *
	 * A DEFAULT-CONSTRUCTED POSE IS STOWED, the bind pose of every vehicle rig, so an aircraft
	 * and a vehicle that has no working parts both get the answer that moves no bone.
	 */
	UPROPERTY() FBodyPose BodyPose;
};
