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
	 * How far the propeller may turn this frame, degrees - the real rotation or the most the
	 * frame rate can show, whichever is less. See PropMaxStepPerRepeat for why.
	 *
	 * Static and free of the instance so it can be tested without an actor or a skeleton,
	 * which is the same reason the wheel and propeller arithmetic lives in this class rather
	 * than in the Animation Blueprint at all.
	 */
	static float PropStepDegrees(float RPM, float DeltaSeconds, int32 BladeCount, float MaxStepPerRepeat);

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

	/**
	 * How many blades the modelled propeller has, which is how often it repeats itself.
	 *
	 * A three-blade propeller looks identical every 120 degrees, so THAT is the angle the
	 * frame rate has to resolve - not a full turn. Getting this wrong in either direction
	 * only changes how fast the blades appear to turn, never whether they alias.
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Airside", meta = (ClampMin = "1"))
	int32 PropBladeCount = 3;

	/**
	 * The most of one blade-repeat the propeller may turn in a single frame.
	 *
	 * WHY THE RENDERED PROPELLER IS DELIBERATELY SLOWER THAN THE REAL ONE. At 2200 RPM a
	 * propeller turns 36.7 times a second. No frame rate this game will ever run at can sample
	 * that: the blades alias, and because the alias depends on the frame rate, the apparent
	 * speed changes with it. Measured 2026-09-12 for this airframe - +6.67 rev/s at 90 fps,
	 * DEAD STILL at 110, backwards at 120 and 144. Reported from play as the propeller
	 * changing speed with the camera, which it was: zooming in fills more screen, costs more
	 * to draw, and lands on a different alias.
	 *
	 * Below half a repeat the blades read as turning forwards at any frame rate (Nyquist on
	 * the repeat angle, not on the full turn). A third leaves margin and still looks fast.
	 *
	 * THE MODEL IS NOT TOUCHED. FAgentMotion::EngineRPM stays the real figure and everything
	 * that reasons about the engine still reads it; this is the VIEW choosing a rotation it
	 * can actually show. The honest fix is a blurred disc above PropDiscRPM - see that flag -
	 * and this is what keeps the blades readable until the art for it exists.
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Airside", meta = (ClampMin = "0.01", ClampMax = "0.5"))
	float PropMaxStepPerRepeat = 0.333f;
};
