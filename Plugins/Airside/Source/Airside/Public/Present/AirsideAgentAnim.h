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
	 * How far the propeller may turn this frame, degrees.
	 *
	 * RPM IS CAPPED TO DisplayCapRPM FIRST (#107 item 7), and THAT capped rate is what gets
	 * integrated every frame - not the per-frame step directly. Capping the step itself (the
	 * ONLY thing this did before) made the apparent speed proportional to frame rate whenever
	 * it bound: a fixed degrees-per-FRAME limit times a higher frame rate is a higher
	 * degrees-per-SECOND rate, which is exactly the "propeller speeds up when I zoom in"
	 * report - zooming in cost more to draw, landed on a lower frame rate, and lowered the
	 * apparent RPM. Capping the RATE instead makes the shown speed the same number whatever
	 * the frame rate is, PROVIDED the frame rate is fast enough that the guard below does not
	 * additionally bind - see PropMaxStepPerRepeat and DisplayCapRPM's own comments for what
	 * "fast enough" means for the default figures.
	 *
	 * Static and free of the instance so it can be tested without an actor or a skeleton,
	 * which is the same reason the wheel and propeller arithmetic lives in this class rather
	 * than in the Animation Blueprint at all.
	 */
	static float PropStepDegrees(float RPM, float DeltaSeconds, int32 BladeCount, float MaxStepPerRepeat,
		float DisplayCapRPM);

	/**
	 * How far the wheel turns this frame, degrees, and updates InOutRateDegPerSec - the
	 * wheel's own persistent turn rate - for next frame's call.
	 *
	 * ON THE GROUND (bAirborne false) the rate is read straight off GroundSpeed every frame,
	 * v = wr: the tyre has no inertia of its own, it is driven by contact with the tarmac.
	 *
	 * AIRBORNE (#107 item 8), InOutRateDegPerSec DECAYS TOWARD ZERO over SpinDownSeconds
	 * instead of being read from GroundSpeed at all - which is what FAgentMotion::GroundSpeed's
	 * own header always said should happen ("a wheel that stopped the instant the aircraft
	 * lifted off would snap from spinning to still in one frame, which is the one thing real
	 * wheels visibly do not do") and what this contradicted by gating on !bAirborne outright: a
	 * ~12,000 deg/s wheel at rotation stopped dead in a single frame. The law is the flare's
	 * own (h' = -h/tau, FLandingRun::Advance) applied to the rate instead of a height.
	 *
	 * Static and free of the instance so it can be tested without an actor or a skeleton, the
	 * same reason PropStepDegrees is. InOutRateDegPerSec is the caller's own persistent state
	 * (WheelRateDegPerSec below) rather than a member here, because a static method has none.
	 */
	static float WheelStepDegrees(float GroundSpeed, float Radius, bool bAirborne, float DeltaSeconds,
		float SpinDownSeconds, float& InOutRateDegPerSec);

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

	/**
	 * The wheel's own turn rate, degrees per second - carried between frames so it can decay
	 * smoothly once airborne instead of being re-derived from GroundSpeed every frame. See
	 * WheelStepDegrees. Not read by anything else; BlueprintReadOnly only for the same reason
	 * PropAngleDegrees etc. are - so it is visible for debugging in the Animation Blueprint.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Airside")
	float WheelRateDegPerSec = 0.0f;

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

	/**
	 * How long the wheels take to spin down to a stop once airborne, seconds (#107 item 8).
	 *
	 * NOT ZERO: the gear stays out (Stage 2's retraction is separate work - see bAirborne),
	 * and a wheel that stopped the instant the aircraft left the ground would snap from
	 * spinning to still in one frame, contradicting FAgentMotion::GroundSpeed's own header.
	 * 2 seconds is long enough to read as inertia rather than another snap, short enough that
	 * the wheels have visibly stopped well before a climbing aircraft is far from the camera.
	 * See WheelStepDegrees for the decay law.
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Airside", meta = (ClampMin = "0.0"))
	float WheelSpinDownSeconds = 2.0f;

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
	 * A GUARD NOW, NOT THE SHAPER (#107 item 7) - DisplayCapRPM below is what keeps the
	 * apparent speed down at ordinary frame rates; this only still binds at a frame rate slow
	 * enough to hitch, the same fallback it always was before DisplayCapRPM existed. At 2200
	 * RPM a propeller turns 36.7 times a second, which no frame rate this game runs at can
	 * sample without aliasing - this is what stops that aliasing from ever reading as
	 * backwards, whatever rate binds it.
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

	/**
	 * The propeller is never SHOWN turning faster than this RPM, whatever FAgentMotion::
	 * EngineRPM actually says (#107 item 7).
	 *
	 * WHY: PropStepDegrees used to clamp the per-frame STEP alone (PropMaxStepPerRepeat), a
	 * fixed degrees-per-FRAME ceiling - so degrees-per-SECOND, the apparent speed, rose with
	 * the frame rate whenever that ceiling bound: 200/400/800 apparent RPM at 30/60/120 fps
	 * for this airframe's cruise RPM and blade count, measured 2026-09-12. Reported from play
	 * as the propeller changing speed with the camera, which it was - zooming in cost more to
	 * draw and landed the frame rate on a different point along that same ramp.
	 *
	 * CHOSEN FOR 60 FPS - THIS FIGURE'S OWN FLOOR for full effect, not a project-wide
	 * constant (Airside.Model.Traffic.SubstepCeilingCoversTheSpeedLadder picks a different,
	 * lower one - 30 fps - for the substep ceiling; each figure's floor is authored against
	 * what THAT figure needs, not shared): 400 RPM at three blades is 40 degrees a frame at
	 * 60 fps, which is exactly
	 * PropMaxStepPerRepeat's own 0.333 x 120 degree repeat - the guard does not additionally
	 * bind at 60 fps or above, so the shown speed is this exact figure at every ordinary frame
	 * rate, not merely bounded by it. Below 60 fps the guard binds again and the apparent
	 * speed sags toward whatever the hitching frame rate allows, the same fallback that
	 * applied everywhere before this cap existed - a slower prop during a hitch, never a
	 * backwards one.
	 *
	 * A DIFFERENT BLADE COUNT OR STEP GUARD ON ANOTHER AIRFRAME'S ANIM BLUEPRINT WANTS ITS OWN
	 * FIGURE HERE, the same way it wants its own PropMaxStepPerRepeat - this is authored per
	 * instance rather than derived from the other two, because deriving it would silently
	 * change a working figure the moment either of them was retuned for an unrelated reason.
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Airside", meta = (ClampMin = "1.0"))
	float PropDisplayCapRPM = 400.0f;
};
