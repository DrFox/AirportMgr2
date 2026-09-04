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

	/**
	 * Where the aircraft leaves the runway, uu from the threshold.
	 *
	 * IT KEEPS ROLLING AT TAXI SPEED UNTIL IT GETS THERE. Braking to taxi speed and stopping
	 * being "vacated" was the first version, and it puts the aircraft wherever the physics
	 * ran out - which is not where the taxiway is. The follower then starts at the exit node
	 * and the aircraft jumps to it.
	 *
	 * Zero means "as soon as it has slowed down", which is what a probe wants.
	 */
	UPROPERTY() double VacateAt = 0.0;

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
		const FApproachPerformance& InApproach, double InVacateAt = 0.0);

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
	 * Runway needed PAST THE THRESHOLD to stop at taxi speed, uu.
	 *
	 * MEASURED BY FLYING IT, not estimated. This flies a whole arrival on a runway long
	 * enough that it cannot be refused, and reports where it vacated - so the figure the
	 * refusal uses and the distance the aircraft actually covers are the same number by
	 * construction, and cannot drift apart.
	 *
	 * That is the point. The closed form it replaces charged the flare's whole horizontal
	 * run against the runway, but a flare from thirty feet on a three-degree slope BEGINS
	 * about 172 m before the threshold, over the approach lights. It demanded 649 m of a
	 * model that uses 297 m, which refused every runway on a 500 m field - and the refusal
	 * read as "pressing 7 does nothing".
	 *
	 * Cheap enough to do per dispatch: a few thousand steps of arithmetic on three structs,
	 * with no world and no allocation.
	 */
	static double RequiredLandingDistance(const FGroundPerformance& InGround,
		const FClimbPerformance& InClimb, const FApproachPerformance& InApproach);

	/**
	 * The safety factor on that measurement.
	 *
	 * A refusal wants to err toward refusing: accepting a runway and running off the end is
	 * the failure worth avoiding, and a landing is flown to a touchdown zone rather than to
	 * the numbers. Applied where the figure is USED rather than inside the measurement, so
	 * the measurement stays a measurement.
	 */
	static constexpr double LandingMargin = 1.25;

private:
	/** Arms without the runway-length check, so RequiredLandingDistance can fly a probe. */
	bool Begin(const FVector2D& InThreshold, const FVector2D& InDirection, double InRunwayLength,
		const FGroundPerformance& InGround, const FClimbPerformance& InClimb,
		const FApproachPerformance& InApproach, double InVacateAt);
};
