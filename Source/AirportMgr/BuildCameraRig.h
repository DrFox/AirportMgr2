#pragma once

#include "CoreMinimal.h"
#include "BuildCameraRig.generated.h"

/**
 * The limits and starting pose one camera MODE resets to - see FBuildCameraRig::Reset.
 *
 * Pulled out of ARoadBuildController's ViewLimits/WatchLimits (issue #94): the controller
 * held these as two parallel sets of UPROPERTYs - MinViewDistance/MinPitchDegrees/... beside
 * WatchMinDistance/WatchMinPitchDegrees/... - which is the same six numbers typed twice,
 * copied onto a rig by two near-identical functions (ApplyViewLimits/ApplyWatchLimits), and
 * reset by two near-identical blocks (CreateBuildCamera's setup, ToggleWatchAgent's). One
 * struct, one UBuildCameraComponent holding two instances of it, and one Reset that both
 * call is what stops the third copy that was always one edit away.
 */
USTRUCT(BlueprintType)
struct FCameraRigLimits
{
	GENERATED_BODY()

	/**
	 * Closest the camera may come, in uu.
	 *
	 * The build view's default (600) is sized to sit beside a vehicle; the watch view's
	 * default is set explicitly higher on UBuildCameraComponent::WatchLimits - see that
	 * field's own comment for why the two modes do not share one number here.
	 */
	UPROPERTY(EditAnywhere, meta = (ClampMin = "1.0"))
	double MinDistance = 600.0;

	/** Furthest the camera may pull back, in uu. */
	UPROPERTY(EditAnywhere, meta = (ClampMin = "1.0"))
	double MaxDistance = 60000.0;

	/**
	 * Pitch at MinDistance, in degrees below horizontal. Near eye level.
	 *
	 * 12, NOT 30, AND THE HORIZON IS THE REASON. At UBuildCameraComponent's FieldOfView of
	 * 75 degrees horizontal, a 16:9 frame is 46.7 degrees tall, so the top of the screen is
	 * 23.3 degrees above centre - and 21:9 is 18.2. A pitch of 30 therefore put the horizon
	 * SIX AND A HALF DEGREES OFF THE TOP OF THE SCREEN at every aspect ratio the game will
	 * see: fully zoomed in, there was no sky at all. 12 sits it about halfway up the top
	 * half at 16:9 and keeps it on screen at 21:9.
	 *
	 * This reverses the floor's old justification, which was that it "keeps the road plane
	 * readable". That reasoning holds for LAYING AN AIRPORT OUT, and it is untouched: pitch
	 * is a function of distance, so the moment the view pulls back at all it climbs toward
	 * MaxPitch again. The floor only applies at the very closest zoom, and at the closest
	 * zoom the player is looking at one aircraft or one building rather than reading the
	 * plan - which is exactly when a horizon is worth more than a plan view.
	 *
	 * Sanity check on the height it implies: at MinDistance 600 the camera sits
	 * 600 * sin(12) = 125 uu above the plane, which is 1.25 m. Eye level, as the summary
	 * line says, rather than the 3 m that 30 degrees gave.
	 */
	UPROPERTY(EditAnywhere, meta = (ClampMin = "1.0", ClampMax = "89.0"))
	double MinPitch = 12.0;

	/**
	 * Pitch at MaxDistance. 90 would be straight down, and is deliberately not offered:
	 * control rotation renormalises unpredictably at the poles, and a view that flat loses
	 * every cue about relief that the angle exists to provide.
	 */
	UPROPERTY(EditAnywhere, meta = (ClampMin = "1.0", ClampMax = "89.0"))
	double MaxPitch = 70.0;

	/** Camera-to-focus distance the rig resets to, in uu. */
	UPROPERTY(EditAnywhere, meta = (ClampMin = "1.0"))
	double StartDistance = 8000.0;

	/** Yaw the rig resets to, in degrees. */
	UPROPERTY(EditAnywhere)
	double StartYaw = 0.0;

	/**
	 * Road-plane point the rig resets its focus to.
	 *
	 * THE MISSING THIRD OF THE STARTING POSE. StartDistance and StartYaw were here from the
	 * start and Reset hard-coded the focus to the world origin beside them, which is fine for
	 * an airport - the origin is where you start building - and wrong for anything laid out
	 * somewhere else. The model yard's bench aims at its own row of aircraft, and had no way
	 * to say so.
	 *
	 * DEFAULTS TO THE ORIGIN, which is exactly what Reset did before, so no existing caller
	 * changes behaviour - see Airside.View.BuildCameraRig.ApplyLimitsAndReset, which pins it.
	 */
	UPROPERTY(EditAnywhere)
	FVector2D StartFocus = FVector2D::ZeroVector;
};

/**
 * Orbit camera for the build tool: a focus point on the road plane, a distance from it,
 * and a yaw. Everything else is derived.
 *
 * Pitch is NOT stored. It is a function of distance, so pulling back tilts the view
 * towards the vertical for laying an airport out, and zooming in tilts it down towards
 * eye level beside a vehicle. Storing it would let the two disagree, and there is no
 * meaning to a pitch that does not match the distance it was chosen for.
 *
 * Deliberately a plain struct: no UObject, no component, no world. It computes a
 * transform from three numbers and knows nothing about cameras, input or actors, which
 * is what keeps it readable next to a build tool that has plenty to do already.
 */
struct FBuildCameraRig
{
	// --- State -----------------------------------------------------------------------

	/** Point on the road plane the camera looks at and orbits around. */
	FVector2D Focus = FVector2D::ZeroVector;

	/** Camera-to-focus distance in uu. This is the zoom. */
	double Distance = 8000.0;

	/**
	 * Compass direction the camera looks along, in degrees.
	 *
	 * Deliberately NOT normalised to [0, 360). Rotating past a wrap point and then easing
	 * towards the target would take the long way round the circle, or spin, and every
	 * version of that bug is a special case in the interpolation. An unbounded angle makes
	 * the interpolation plain linear and the bug unrepresentable; FRotator does not care.
	 */
	double Yaw = 0.0;

	// --- Limits ----------------------------------------------------------------------

	/** Closest the camera may come. Sized so a vehicle fills a useful part of the screen. */
	double MinDistance = 600.0;

	double MaxDistance = 60000.0;

	/** Pitch at MinDistance, in degrees below horizontal. Near eye level.
	 *  MIRRORS FCameraRigLimits::MinPitch and must keep agreeing with it - ApplyLimits
	 *  overwrites this on any rig that has been given limits, so a disagreement shows only
	 *  on one that has not, which is the hardest kind to notice. See that field for why 12. */
	double MinPitch = 12.0;

	/** Pitch at MaxDistance. 90 would be straight down. */
	double MaxPitch = 70.0;

	/** Copy Min/MaxDistance and Min/MaxPitch from Limits onto this rig, so a details-panel
	 *  edit takes effect on the live view - the one function ApplyViewLimits and
	 *  ApplyWatchLimits used to be separately (issue #94). */
	void ApplyLimits(const FCameraRigLimits& Limits);

	/** ApplyLimits, then snap Focus to StartFocus, Distance to StartDistance (clamped) and
	 *  Yaw to StartYaw - what CreateBuildCamera's setup and ToggleWatchAgent's "reset on
	 *  every entry" block each used to spell out separately (issue #94). StartFocus defaults
	 *  to the origin, which is the value this hard-coded until the model yard needed to aim
	 *  somewhere else. */
	void Reset(const FCameraRigLimits& Limits);

	// --- Derived ---------------------------------------------------------------------

	/**
	 * Pitch for the current distance, in degrees below horizontal.
	 *
	 * Interpolated on the LOGARITHM of distance, not on distance itself. The zoom is
	 * geometric - each notch multiplies - and over a hundredfold range a linear blend
	 * leaves the pitch within a couple of degrees of MinPitch for almost the whole useful
	 * band, then swings through forty degrees at the very end. On a log scale every notch
	 * changes the pitch by the same amount, which is the thing that reads as smooth.
	 */
	double PitchDegrees() const;

	/** Where the camera sits, for a road plane at PlaneZ. */
	FVector CameraLocation(double PlaneZ) const;

	FRotator CameraRotation() const;

	/**
	 * This rig re-expressed in world space, treating its Focus and Yaw as RELATIVE to a
	 * frame at Origin facing HeadingDegrees: Focus.X is ahead of the nose, Focus.Y is off
	 * the right wing, Yaw is measured from the heading. Limits and distance carry across.
	 *
	 * The watch camera keeps its state in the aircraft's frame and projects with this each
	 * frame. Storing the state in world space instead would mean re-deriving "beside the
	 * aircraft" after every degree of turn, and easing a world-space focus towards a moving
	 * aircraft lags behind it - which reads as the camera failing to keep up, not as
	 * smoothing. In the relative frame the aircraft's own motion is rigid and only the
	 * player's inputs are eased, which is the split that feels right.
	 */
	FBuildCameraRig InFrame(const FVector2D& Origin, double HeadingDegrees) const;

	// --- Input -----------------------------------------------------------------------

	/** Multiply the distance by (1 + Step) per notch, and clamp. */
	void Zoom(double Step, double Notches);

	/**
	 * Slide the focus across the road plane in the CAMERA's basis, not the world's.
	 *
	 * Right and Forward are in [-1, 1]. Once the view can rotate, panning has to follow
	 * the screen: W moving north regardless of which way the camera faces is the thing
	 * that makes a rotatable camera feel broken.
	 *
	 * Rate is in view-distances per second, so a pan crosses the same fraction of the
	 * screen however far out the view is. A speed in uu per second would crawl when zoomed
	 * out and fly when zoomed in.
	 */
	void Pan(double Right, double Forward, double Rate, double DeltaTime);

	void Rotate(double Degrees);

	/**
	 * Ease this rig towards Target, frame-rate independently. Lag <= 0 snaps.
	 *
	 * Zoom moves in geometric notches and pitch moves with it, so without this a single
	 * wheel click jumps both the distance and the angle - which reads as a cut rather than
	 * a camera move.
	 */
	void EaseToward(const FBuildCameraRig& Target, double Lag, double DeltaTime);
};
