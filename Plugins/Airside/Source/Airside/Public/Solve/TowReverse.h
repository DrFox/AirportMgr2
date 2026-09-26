#pragma once

#include "CoreMinimal.h"
#include "Solve/VehicleSweep.h"

/**
 * A TOW CHAIN BACKING ALONG A LINE, SOLVED ONCE (spec 2026-09-26 §1). Dependency-free, like the
 * rest of Solve/: Model/FTowReverseRun plays the samples back, Model/VehicleFit judges a route
 * with the same call, so the router and the agent cannot disagree about a reverse.
 *
 * SOLVED, NOT TRACKED - the rule FReverseRun's header gives for a rigid vehicle, and it holds
 * harder here: a trailer backing up is unstable in its hitch angle, which is why a real driver
 * steers the "wrong" way. A controller is still needed to solve the manoeuvre, but it runs HERE,
 * ahead of time, over a line both ends of which are known, and its output is checked before a
 * single frame plays. Live tracking at runtime (gains tuned against a moving target, a heading
 * error that grows every frame) was rejected by ruling, 2026-09-26.
 *
 * THE LINE IS THE LEADING AXLE'S. Going backwards the rearmost trailer axle leads, so a reverse
 * leg's polyline is where THAT axle goes - the same rule as a rigid vehicle, whose leading axle
 * is its fixed one.
 *
 * ONE JOINT, BY RULING (2026-09-26): a drawbar trailer's turntable is LOCKED while reversing, as
 * real turntable dollies have a lock for, so the utility tow backs as one rigid link from the
 * utility's hitch to the trailer's rear axle. Forward, both joints still swing. A chain with two
 * free joints is the classically unbackable full trailer; the lock is what makes it one joint.
 *
 * THE CONTROLLER, CASCADED:
 *  - outer: pure pursuit of the trailer axle along the line, in its own direction of travel,
 *    gives the curvature the trailer should be on; the steady-state hitch angle for that
 *    curvature is the reference;
 *  - inner: the tractor's curvature is solved from the exact hitch-angle kinematics so the
 *    angle's error decays at a chosen rate (feedback linearisation), clamped to the lock.
 * Kinematics, per unit of tractor fixed-axle travel s (a the hitch's offset ahead of the fixed
 * axle, L hitch to trailer axle, phi = tractor heading - trailer heading):
 *    dphi/ds = kappaF - (sin(phi) + a kappaF cos(phi)) / L
 * ENFORCED BY: Airside.Solve.TowReverse.SteadyStateHolds (the steady state this implies)
 */
namespace TowReverse
{
	/** Why a solve failed, for the log and for VehicleFit's verdict. */
	enum class ERefusal : uint8
	{
		None,
		/** The line has no direction, or no length. */
		BadLine,
		/** The leading axle strayed more than MaxLineError from the line - at the start, or on the way. */
		OffLine,
		/** A locked joint is more than TurntableLockDegrees off straight: the lock cannot engage. */
		TurntableBent,
		/** The hitch passed the critical angle: no steering recovers it from there. */
		Jackknife,
		/** It reached the end of the line off the end pose (position or heading). */
		MissedEnd,
		/** The step budget ran out - a line that loops, or a controller that never arrives. */
		NoArrival
	};

	/** One solved pose. The FULL chain, so the view poses every real link, towbar included. */
	struct FSample
	{
		/** The tractor's fixed axle and heading (radians from +X). */
		FVector2D Fixed = FVector2D::ZeroVector;
		double Heading = 0.0;
		/** One axle per link of the FULL chain (VehicleFit::BodyOf's), a locked towbar posed on the locked line. */
		TArray<FVector2D, TInlineAllocator<2>> Axles;
		/**
		 * Road-wheel angle, degrees, atan(Wheelbase * tractor curvature) in the FORWARD sense -
		 * the convention FReverseRun::SteerDegrees states: going backwards the wheels turn
		 * opposite the way the back of the vehicle swings.
		 */
		double SteerDegrees = 0.0;
		/** Tractor heading minus the (locked) trailer's, radians, signed. */
		double HitchRadians = 0.0;
		/** The leading axle's distance along the line, uu - what playback interpolates by. */
		double Along = 0.0;
	};

	struct FInput
	{
		/** The FULL chain, as VehicleFit::BodyOf gives it. ReverseBody is taken inside. */
		VehicleSweep::FBody Body;
		double MaxSteerRadians = 0.0;
		/** Where the leading axle goes, in travel order. */
		TArray<FVector2D> Line;
		/** The tractor's fixed axle and heading (radians), and the FULL chain's axles, now. */
		FVector2D Fixed = FVector2D::ZeroVector;
		double Heading = 0.0;
		TArray<FVector2D> Axles;
	};

	struct FSolution
	{
		ERefusal Refusal = ERefusal::None;
		/** The number the refusal is about - uu for OffLine/MissedEnd position, degrees otherwise. */
		double Figure = 0.0;
		/** The limit it was judged against, in Figure's unit. */
		double Limit = 0.0;
		FVector2D Where = FVector2D::ZeroVector;
		TArray<FSample> Samples;
		double WorstHitchRadians = 0.0;
		double WorstLineError = 0.0;
		double EndPositionError = 0.0;
		double EndHeadingErrorDegrees = 0.0;

		bool IsValid() const { return Refusal == ERefusal::None && Samples.Num() >= 2; }
		/** One line naming the refusal, its figure, its limit and where - or the worst figures of a success. */
		AIRSIDE_API FString Describe() const;
	};

	/**
	 * The body a reverse is solved for: link 0 keeps its joint, every later link is merged
	 * rigidly into it (the turntable lock). One link in, one link out; a rigid body, none.
	 * ENFORCED BY: Airside.Solve.TowReverse.ReverseBodyMergesTheTurntable
	 */
	AIRSIDE_API VehicleSweep::FBody ReverseBody(const VehicleSweep::FBody& Body);

	/**
	 * The largest hitch angle, radians, from which full lock still turns the angle back -
	 * sin(phi) + a kMax cos(phi) <= L kMax, from the kinematics above - capped at
	 * VehicleSweep::MaxHitchRadians, where StepChain calls the chain folded. DERIVED, not typed:
	 * the rig and the locked utility both come out at the cap on 2026-09-26 (short tractors,
	 * long links), which is the answer rather than a coincidence to hard-code.
	 */
	AIRSIDE_API double CriticalHitchRadians(const VehicleSweep::FBody& Reverse, double MaxSteerRadians);

	/**
	 * The hitch angle, radians, that holds the trailer axle on a circle of forward curvature
	 * Kappa (signed, 1/uu) - circle geometry: Rt = 1/|k|, the tractor's fixed axle on
	 * sqrt(Rt^2 + L^2 - a^2), phi = atan(L/Rt) - atan(a/Rf).
	 */
	AIRSIDE_API double SteadyHitchRadians(const VehicleSweep::FBody& Reverse, double Kappa);

	/** Solves the whole reverse. Never partial: a refusal carries no playable samples. */
	AIRSIDE_API FSolution Solve(const FInput& In);

	/** The lock engages within this of straight, and snaps the joint to straight as it does. */
	inline constexpr double TurntableLockDegrees = 3.0;
	/** uu the leading axle may stray from its line anywhere on the way. */
	inline constexpr double MaxLineError = 30.0;
	/** The end pose: the leading axle within this of the line's end, facing within MaxEndHeadingDegrees. */
	inline constexpr double MaxEndPositionError = 20.0;
	inline constexpr double MaxEndHeadingDegrees = 3.0;
	/** The reference is held this far inside the critical angle, so the inner loop always has lock to spare. */
	inline constexpr double ReferenceShare = 0.8;
}
