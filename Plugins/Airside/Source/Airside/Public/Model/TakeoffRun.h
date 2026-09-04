#pragma once

#include "CoreMinimal.h"
#include "Model/RoadEntity.h"
#include "TakeoffRun.generated.h"

/** Where a departure has got to. */
UENUM()
enum class ETakeoffPhase : uint8
{
	/** Turning onto the runway heading. An aircraft backtracks and turns before it rolls. */
	LineUp,

	/** Full power, still on the wheels, accelerating toward rotation speed. */
	Roll,

	/** At Vr: the nose is coming up and the aircraft is leaving the ground. */
	Rotate,

	/** Established, holding the climb attitude. */
	Climb,

	/** Above the clear altitude. The agent has nothing left to do here. */
	Clear
};

/**
 * A departure: line up, roll, rotate, climb, gone.
 *
 * A SECOND MOTION PHASE, deliberately not part of FRouteFollower. The follower walks a route
 * and says so; a take-off is not a route - it is a straight line at full power with a rotation
 * in the middle of it and no polyline to consult. Growing the follower to do both would have
 * made "where is this agent" a question with two answers depending on a mode flag inside it.
 *
 * The agent switches: the follower brings it to the threshold, this takes it from there.
 *
 * World-free like the follower. It is a threshold, a direction, and two structs of numbers,
 * so a whole departure can be flown in a loop with no world - see Airside.Model.TakeoffRun.
 *
 * IT USES THE SAME MinTaxiSpeed RULE while lining up, because the reason has not changed: a
 * wheeled aircraft cannot yaw without rolling, and an aircraft turning onto a runway is still
 * a wheeled aircraft.
 */
USTRUCT()
struct AIRSIDE_API FTakeoffRun
{
	GENERATED_BODY()

	UPROPERTY() ETakeoffPhase Phase = ETakeoffPhase::Clear;

	/** The threshold the roll starts from, in road-plane XY. */
	UPROPERTY() FVector2D Threshold = FVector2D::ZeroVector;

	/** Unit vector from the threshold toward the far end. The departure heading. */
	UPROPERTY() FVector2D Direction = FVector2D(1.0, 0.0);

	/** Runway available, uu. */
	UPROPERTY() double RunwayLength = 0.0;

	UPROPERTY() FGroundPerformance Ground;
	UPROPERTY() FClimbPerformance Climb;

	/** How far down the runway, uu. Keeps increasing after rotation - the aircraft flies on. */
	UPROPERTY() double Travelled = 0.0;

	UPROPERTY() double Speed = 0.0;

	/** Radians, slewed at the airframe's turn rate while lining up. */
	UPROPERTY() double Heading = 0.0;

	/** Above the runway surface, uu. */
	UPROPERTY() double Altitude = 0.0;

	/** Nose-up, degrees. */
	UPROPERTY() double Pitch = 0.0;

	/**
	 * Arms a departure. False, and logs, when this runway cannot take this aircraft.
	 *
	 * THE REFUSAL IS THE POINT of returning a bool: the roll needed to reach Vr is
	 * v^2 / 2a, which is a published figure for every airframe, and a strip shorter than it
	 * is one the aircraft cannot leave. Rolling anyway and running off the end would be a
	 * simulation of an accident, not of a departure.
	 */
	bool Start(const FVector2D& InThreshold, const FVector2D& InDirection, double InRunwayLength,
		const FGroundPerformance& InGround, const FClimbPerformance& InClimb, double InHeading);

	/**
	 * Flies one frame. False once the departure is over, leaving the outputs untouched.
	 *
	 * Same contract as FRouteFollower::Advance, and for the same reason: a caller that
	 * ignores the return value leaves its aircraft where it was rather than at the origin.
	 */
	bool Advance(double DeltaSeconds, FVector2D& OutPosition, double& OutHeading,
		double& OutAltitude, double& OutPitch);

	bool HasCleared() const { return Phase == ETakeoffPhase::Clear; }

	/** The ground roll this airframe needs to reach rotation speed, uu. v^2 / 2a. */
	static double RequiredRoll(const FGroundPerformance& InGround);
};
