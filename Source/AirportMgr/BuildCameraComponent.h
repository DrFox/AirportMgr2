#pragma once

#include "CoreMinimal.h"
#include "BuildCameraRig.h"
#include "Components/ActorComponent.h"
#include "BuildCameraComponent.generated.h"

class ARoadNetworkActor;
class ACameraActor;

/**
 * Owns the build driver's camera: the orbiting rig you build with, and the second rig that
 * rides an aircraft while watching it - see FBuildCameraRig's own comment for why one type
 * serves both.
 *
 * Pulled out of ARoadBuildController by issue #94, where the class carried seven concerns
 * (view rig, watch rig, four widget classes, a testing override, placement) as roughly 40
 * UPROPERTYs and the .cpp to match: CreateBuildCamera, UpdateView, ZoomBy/ZoomIn/ZoomOut and
 * ToggleWatchAgent duplicated their tunables and their reset logic once per mode
 * (ApplyViewLimits vs ApplyWatchLimits; two near-identical resets in CreateBuildCamera and
 * ToggleWatchAgent), and a `bWatchingAgent ? ... : ...` ternary sat at every call site that
 * needed "the rig actually driving the camera" - except one. CursorOnRoadPlane's
 * MaxPlaceDistanceFactor cap read CurrentView.Distance unconditionally, even while watching,
 * because nothing forced every such reader through one function. ActiveRig() is that
 * function now.
 *
 * Everything about WHICH KEY does what, and WHICH AGENT to watch when none is selected,
 * stays on the controller - that is Session/Selection-reading policy this component has no
 * business owning, and the log lines that explain a refusal stay there too, since they name
 * facts (a selected aircraft, a flight landed by key 7) this component knows nothing about.
 * This component knows only: two rigs, their limits, and how to ease one of them towards a
 * target and apply it to a spawned ACameraActor.
 */
UCLASS(ClassGroup = "Airside", meta = (BlueprintSpawnableComponent))
class AIRPORTMGR_API UBuildCameraComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UBuildCameraComponent();

	// --- Build view tunables -----------------------------------------------------------

	/** Limits and starting pose of the orbiting build view. */
	UPROPERTY(EditAnywhere, Category = "Airside|View")
	FCameraRigLimits ViewLimits;

	/** Fraction the view distance changes per mouse-wheel notch. */
	UPROPERTY(EditAnywhere, Category = "Airside|View", meta = (ClampMin = "0.01", ClampMax = "0.9"))
	double ZoomStep = 0.15;

	/**
	 * Pan speed, in view distances per second - shared with the watch rig.
	 *
	 * Not uu per second: the view spans a hundredfold range, and a fixed speed crawls when
	 * zoomed out and overshoots when zoomed in. As a fraction of the view, a pan crosses the
	 * same amount of screen at every zoom.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|View", meta = (ClampMin = "0.0"))
	double PanRate = 0.9;

	/** Rotation speed on Q and E, in degrees per second. Shared with the watch rig. */
	UPROPERTY(EditAnywhere, Category = "Airside|View", meta = (ClampMin = "0.0"))
	double RotateRate = 90.0;

	/** Seconds the view takes to settle after an input. Zero snaps. Shared with the watch rig. */
	UPROPERTY(EditAnywhere, Category = "Airside|View", meta = (ClampMin = "0.0"))
	double CameraLag = 0.12;

	/** Horizontal field of view, in degrees. */
	UPROPERTY(EditAnywhere, Category = "Airside|View", meta = (ClampMin = "20.0", ClampMax = "150.0"))
	double FieldOfView = 75.0;

	// --- Watch camera -------------------------------------------------------------------
	//
	// A second camera MODE rather than a second camera: the build rig is a top-down thing
	// for laying pavement, and watching a take-off from it shows a dot getting smaller. This
	// orbits the aircraft instead, and hands back the moment there is nothing to watch.
	//
	// It is the SAME rig type as the build view, kept in the aircraft's frame (see
	// FBuildCameraRig::InFrame), so the wheel, WASD and Q/E do in watch mode exactly what
	// they do while building: zoom, slide the look-at point, orbit. Two rigs rather than one
	// re-aimed, so leaving watch mode lands on the build view where it was left.

	/**
	 * Limits and starting pose of the camera that rides an aircraft - see ToggleWatchAgent.
	 *
	 * NOT FCameraRigLimits' own defaults - those are the BUILD view's numbers (600/60000
	 * uu, 30/70 degrees, 8000 uu, yaw 0), which read as a camera parked 8000 uu out at yaw 0
	 * instead of 1550 uu off the aircraft's right wing. Set explicitly in the constructor to
	 * the values this replaced: WatchMinDistance 800, WatchMaxDistance 20000,
	 * WatchMinPitchDegrees 10, WatchMaxPitchDegrees 60, WatchStartDistance 1550,
	 * WatchStartYaw -75.
	 *
	 * 1550 is 15.5 m: wide enough to frame a 13 m wingspan and close enough to read the
	 * attitude, which is the whole point of watching a rotation. -75 looks across the
	 * aircraft from off its right wing and a little behind, which is the pose the fixed
	 * watch camera had: side-on enough to read pitch, angled enough to see the nose. The
	 * pitch range is lower than the build rig's: the build rig's floor keeps the road plane
	 * readable, while this one wants to be near eye level beside an aircraft.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Watch")
	FCameraRigLimits WatchLimits;

	/** Height of the look-at point above the aircraft's origin, uu. Its origin is the
	 *  main-gear axle; 150 is about the fuselage centreline. */
	UPROPERTY(EditAnywhere, Category = "Airside|Watch")
	double WatchFocusHeight = 150.0;

	/**
	 * Furthest WASD may slide the look-at point from the aircraft, uu.
	 *
	 * Without a leash, W held for a few seconds carries the focus off into the grass and the
	 * aircraft leaves the frame, with nothing on screen to say which way it went.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Watch", meta = (ClampMin = "0.0"))
	double WatchMaxFocusOffset = 2000.0;

	// --- Actions -------------------------------------------------------------------------

	/**
	 * Spawns the orbiting camera above Target and makes it Owner's view target.
	 *
	 * Owner rather than reading GetOwner() and casting: a component's owner is an AActor,
	 * not necessarily an APlayerController, and this needs SetViewTarget specifically -
	 * spelling out the type the caller already has is cheaper than a cast this class would
	 * have to fail out of gracefully for no real caller.
	 */
	void CreateBuildCamera(APlayerController& Owner, const ARoadNetworkActor& Target);

	/**
	 * Reads WASD/QE (Right/Forward/Turn, each already resolved to [-1,1] by the caller, which
	 * owns input) into whichever rig is active, eases it towards its target and applies the
	 * result to the spawned camera actor. Hands the watch rig back to the build view the
	 * moment Target has nothing left to watch, logging that it did.
	 */
	void UpdateView(float DeltaTime, double Right, double Forward, double Turn, ARoadNetworkActor* Target);

	/** Mouse wheel: moves whichever rig is active in or out; the pitch follows the distance. */
	void ZoomBy(double Notches);

	/**
	 * Starts riding PreferredAgentId, or stops riding whatever is currently watched. Returns
	 * false only when asked to START and PreferredAgentId cannot be found - the caller owns
	 * the refusal log line, since it also names why (select an aircraft, or land one) in
	 * terms this component has no business knowing.
	 */
	bool ToggleWatchAgent(const ARoadNetworkActor& Target, int32 PreferredAgentId);

	bool IsWatchingAgent() const { return bWatchingAgent; }
	int32 GetWatchAgentId() const { return WatchAgentId; }

	/**
	 * Whichever rig is actually driving the camera right now - the build view, or the watch
	 * rig while riding an agent. THE ONE PLACE that ternary is asked now - see this class's
	 * own comment for the reader that used to skip it.
	 *
	 * ROAD-PLANE COORDINATES ONLY WHILE NOT WATCHING. While watching, this is the watch
	 * rig, and its Focus is a leash offset in the AIRCRAFT'S OWN FRAME (see
	 * FBuildCameraRig::InFrame) - not a point on the road plane. A caller that wants "where
	 * the player is looking, as a road-plane position regardless of which camera is on
	 * screen" - key 7's landing spot, say - wants ViewFocus() instead. Distance is safe from
	 * either rig, which is why CursorOnRoadPlane's placement cap uses ActiveRig().Distance.
	 */
	const FBuildCameraRig& ActiveRig() const { return bWatchingAgent ? WatchCurrent : CurrentView; }

	/**
	 * Where the BUILD VIEW is looking, in road-plane coordinates, regardless of whether the
	 * watch rig is what is actually on screen right now.
	 *
	 * Issue #94 review: ActiveRig().Focus is the wrong answer for "aim key 7 here" while
	 * watching, because the watch rig's Focus is not a road-plane point at all - see
	 * ActiveRig()'s own comment. The build view keeps its own focus the whole time it is
	 * hidden, which is what lets leaving watch mode land the camera exactly where it was.
	 */
	const FVector2D& ViewFocus() const { return TargetView.Focus; }

private:
	/** Orbiting camera spawned on possession; the view target while building AND while
	 *  watching - the SAME actor serves both, only the rig driving it changes. */
	UPROPERTY(Transient)
	TObjectPtr<ACameraActor> BuildCamera;

	/** Where the input says the build view should be, and where it actually is. Separate so
	 *  a wheel notch eases in rather than cutting - see FBuildCameraRig::EaseToward. */
	FBuildCameraRig TargetView;
	FBuildCameraRig CurrentView;

	/** Where the watch rig is asked to be, and where it is; relative to the aircraft - see
	 *  FBuildCameraRig::InFrame. Two rigs rather than one re-aimed, so leaving watch mode
	 *  lands on the build view where it was left. */
	FBuildCameraRig WatchTarget;
	FBuildCameraRig WatchCurrent;

	/** True while the camera is riding with an agent. */
	bool bWatchingAgent = false;

	/** The agent the watch camera rides. */
	int32 WatchAgentId = 0;
};
