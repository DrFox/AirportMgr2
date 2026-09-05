#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "AirsideAgentAnim.generated.h"

/**
 * What an aircraft's moving parts are doing, ready for an Animation Blueprint to apply.
 *
 * THE ARITHMETIC IS HERE, NOT IN THE GRAPH. A wheel turns at ground speed over its radius and
 * a propeller at its RPM; both are integrations over time, and both are exactly the kind of
 * thing that is unreadable as Blueprint nodes and untestable once it is there. The graph gets
 * two angles and applies them to two bones.
 *
 * It also means the numbers stay where they were measured: the wheel radius is 0.210 m off the
 * model, on UAircraftType beside the wingspan, rather than typed into a Blueprint where the
 * next person would have no way of knowing it was measured at all.
 *
 * Reads the owning ARoadAgentActor's last motion. The view is still told everything by
 * ARoadNetworkActor - see FAgentMotion - so this decides nothing either.
 */
UCLASS()
class AIRSIDE_API UAirsideAgentAnim : public UAnimInstance
{
	GENERATED_BODY()

public:
	virtual void NativeUpdateAnimation(float DeltaSeconds) override;

	/**
	 * Accumulated propeller rotation, degrees. Apply to the 'prop' bone.
	 *
	 * WRAPPED to 0..360 rather than allowed to run on: at 2000 RPM this gains 12,000 degrees
	 * a second, and a float that has been counting for ten minutes has lost the precision to
	 * express a single degree of it.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Airside")
	float PropAngleDegrees = 0.0f;

	/** Accumulated wheel rotation, degrees. Apply to all three wheel bones. */
	UPROPERTY(BlueprintReadOnly, Category = "Airside")
	float WheelAngleDegrees = 0.0f;

	/** Off the wheels. Stage 2's gear retraction hangs on this. */
	UPROPERTY(BlueprintReadOnly, Category = "Airside")
	bool bAirborne = false;

	/** Speed over the ground, uu per second. Exposed so the graph can blend on it if wanted. */
	UPROPERTY(BlueprintReadOnly, Category = "Airside")
	float GroundSpeed = 0.0f;

	/**
	 * True once the propeller is turning fast enough to read as a disc rather than as blades.
	 *
	 * A Meridian's propeller runs near 2000 RPM, which is 33 turns a second against 60 frames
	 * - so a modelled blade STROBES, and at some speeds appears to stand still or run
	 * backwards. Every flight sim solves this the same way: show blades slowly, a blurred
	 * disc quickly, and cross-fade between them. This is the switch for that; the blur asset
	 * is a separate piece of work and until it exists the blades simply spin.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Airside")
	bool bPropIsDisc = false;

	/**
	 * Main wheel radius, uu. Divides ground speed to give the wheel's turn rate.
	 *
	 * Defaulted to the Meridian's measured 0.210 m. On the anim instance rather than pushed
	 * from the model because an Animation Blueprint is authored per airframe anyway - and a
	 * radius of zero would divide by it, so it is guarded below rather than trusted.
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Airside")
	float MainWheelRadius = 21.0f;

	/** Above this many RPM the blades are replaced by a disc. */
	UPROPERTY(EditDefaultsOnly, Category = "Airside")
	float PropDiscRPM = 400.0f;
};
