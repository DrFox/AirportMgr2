#pragma once

#include "CoreMinimal.h"

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

	/** Pitch at MinDistance, in degrees below horizontal. Near eye level. */
	double MinPitch = 30.0;

	/** Pitch at MaxDistance. 90 would be straight down. */
	double MaxPitch = 70.0;

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
