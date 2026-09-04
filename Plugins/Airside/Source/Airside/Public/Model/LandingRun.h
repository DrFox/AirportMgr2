#pragma once

#include "CoreMinimal.h"
#include "Model/RoadEntity.h"
#include "LandingRun.generated.h"

/** Where an arrival has got to. */
UENUM()
enum class ELandingPhase : uint8
{
	/** On the glideslope at Vref, descending toward the threshold. */
	Approach,

	/** Below the flare height: the nose is coming up and the descent is being arrested. */
	Flare,

	/** On the wheels, braking toward taxi speed. */
	Rollout,

	/** Down to taxi speed and off the runway's business. The follower takes it from here. */
	Vacated
};

/**
 * An arrival: approach, flare, touch down, brake, vacate.
 *
 * THE MIRROR OF FTakeoffRun, and deliberately a separate struct from it for the same reason
 * FTakeoffRun is separate from FRouteFollower: an arrival is not a route. It is a straight
 * line at a fixed descent angle with a flare in the middle of it and no polyline to consult.
 *
 * The handover runs the other way round from a departure. A departure is follower then
 * take-off - taxi to the runway, then fly. An arrival is landing then follower - fly the
 * approach, then taxi to the stand.
 *
 * World-free like both of them: a threshold, a direction, and three structs of numbers, so a
 * whole arrival can be flown in a loop with no world. See Airside.Model.LandingRun.
 *
 * TOUCHDOWN IS NOT DECLARED, it falls out of the flare - the exact mirror of lift-off falling
 * out of the rotation. See FApproachPerformance for the symmetry, and note that both ends of
 * it are the same function, FClimbPerformance::RequiredAngleAt.
 */
USTRUCT()
struct AIRSIDE_API FLandingRun
{
	GENERATED_BODY()

	UPROPERTY() ELandingPhase Phase = ELandingPhase::Vacated;

	/** The landing threshold, in road-plane XY. Travelled is measured FROM here. */
	UPROPERTY() FVector2D Threshold = FVector2D::ZeroVector;

	/** Unit vector from the threshold toward the far end. The landing heading. */
	UPROPERTY() FVector2D Direction = FVector2D(1.0, 0.0);

	/** Runway available beyond the threshold, uu. */
	UPROPERTY() double RunwayLength = 0.0;

	UPROPERTY() FGroundPerformance Ground;
	UPROPERTY() FClimbPerformance Climb;
	UPROPERTY() FApproachPerformance Approach;

	/**
	 * Distance along the runway centreline from the THRESHOLD, uu.
	 *
	 * NEGATIVE while on final, which is the whole reason it is signed: an aircraft on a
	 * three-mile final is three miles short of the threshold, and giving it a separate
	 * "distance to run" would be a second answer to where it is.
	 */
	UPROPERTY() double Travelled = 0.0;

	UPROPERTY() double Speed = 0.0;

	/** Radians. Fixed on the runway heading throughout - an arrival is already lined up. */
	UPROPERTY() double Heading = 0.0;

	/** Above the runway surface, uu. */
	UPROPERTY() double Altitude = 0.0;

	/** Nose-up, degrees. */
	UPROPERTY() double Pitch = 0.0;

	/**
	 * Arms an arrival. False, and logs, when this runway cannot take this aircraft.
	 *
	 * THE REFUSAL IS THE POINT, exactly as it is for a departure: a strip shorter than the
	 * flare plus the braking roll is one this aircraft cannot stop on, and landing anyway
	 * would be a simulation of an overrun. The user asked for a refusal rather than a
	 * go-around, which is a second flight phase and doubles this.
	 */
	bool Start(const FVector2D& InThreshold, const FVector2D& InDirection, double InRunwayLength,
		const FGroundPerformance& InGround, const FClimbPerformance& InClimb,
		const FApproachPerformance& InApproach);

	/**
	 * Flies one frame. False once the arrival is over, leaving the outputs untouched.
	 *
	 * Same contract as FRouteFollower::Advance and FTakeoffRun::Advance, and for the same
	 * reason: a caller that ignores the return value leaves its aircraft where it was rather
	 * than at the origin.
	 */
	bool Advance(double DeltaSeconds, FVector2D& OutPosition, double& OutHeading,
		double& OutAltitude, double& OutPitch);

	bool HasVacated() const { return Phase == ELandingPhase::Vacated; }

	/** True once the wheels are down - the gear stays out, but the aircraft is no longer flying. */
	bool IsOnGround() const
	{
		return Phase == ELandingPhase::Rollout || Phase == ELandingPhase::Vacated;
	}

	/**
	 * Runway needed from the threshold to stop at taxi speed, uu.
	 *
	 * Two parts, the mirror of FTakeoffRun::RequiredRoll's two: the air distance from the
	 * flare height to touchdown, and then the braking roll. Both over-estimate, which is the
	 * safe direction for a figure a refusal is based on - the alternative is an aircraft
	 * that accepts a runway and runs off the end of it.
	 */
	static double RequiredLandingDistance(const FGroundPerformance& InGround,
		const FApproachPerformance& InApproach);
};
