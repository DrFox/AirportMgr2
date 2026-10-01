#pragma once

#include "CoreMinimal.h"

/**
 * The lens that makes the airport read as a table-top model: when the camera is down near
 * eye level looking across the field, whatever lies well beyond the near ground goes soft.
 *
 * A PLAIN STRUCT, same shape and reason as FSunPath and FBuildCameraRig: numbers in, numbers
 * out, testable with no world. UBuildCameraComponent holds the tunables as UPROPERTYs and
 * writes the result onto its camera's post-process settings every frame.
 *
 * REVERSED 2026-10-01. The first version (2026-09-30) focused on the screen-centre ground
 * point, held the blur at one fraction of the frame at EVERY zoom, and faded it out only at
 * close range. That was the wrong way round: at max zoom an aircraft is ~20 px long and a
 * ~4 px blur everywhere off the centre band smeared it, while the close, near-horizontal
 * view - where the owner meant the effect to live - was fully sharp. What was asked for:
 * "blur when sitting at max zoom-in where you are looking horizontally at aircraft at a
 * distance; those in close distance (<50 m) remain focused; it doesn't apply as you zoom
 * out into top view". So now:
 *   - the focus is a FIXED distance (SharpDistanceUu), not the look-at point;
 *   - everything NEARER than it is sharp because r.DOF.Kernel.MaxForegroundRadius is 0 in
 *     DefaultEngine.ini - a thin lens blurs near things MORE than far ones, so without that
 *     cap the 6 m foreground of the closest view would be the blurriest thing on screen;
 *   - the blur is at full strength only at close zoom and fades to none as the view climbs
 *     toward plan (FullBlurZoomUu .. NoBlurZoomUu, on the rig's camera-to-focus distance).
 *
 * WHY THE SENSOR SIZE IS THE OUTPUT, not the f-stop. UE's cinematic DOF is a real thin lens:
 * the blur of a point at infinity, as a fraction of the frame, is f^2 / (N (s - f) w), and
 * with the focal length f fixed by the field of view and the sensor width w, that is tiny for
 * any real lens at tens of metres - which is why tilt-shift photographs of real towns need a
 * tilted lens. Solving for w gives the chosen blur at infinity directly. The f-stop is left
 * alone because the renderer clamps nothing on the sensor but does shape the bokeh from the
 * f-stop (DiaphragmDOFUtils.cpp, FBokehModel::Compile).
 *
 * Spec 2026-09-12-environment-art-direction-design.md section 4.1: a HINT. Every default
 * below is a starting guess until a screenshot says otherwise.
 */
struct FMiniatureFocus
{
	/**
	 * Blur diameter of a point at infinity, as a fraction of the frame width, at full
	 * strength. 0.01 is 19 px at 1920 wide; a point at twice SharpDistanceUu gets half that.
	 * Kept from 2026-09-30, where 0.003 was judged invisible.
	 */
	double BlurAtInfinity = 0.01;

	/** Aperture. Shapes the bokeh only; the blur amount comes from BlurAtInfinity. */
	double FStop = 4.0;

	/**
	 * Focus distance, uu. Everything nearer is sharp (see the foreground cap above); beyond
	 * it the blur grows toward BlurAtInfinity as 1 - s/d. 50 m is the owner's figure.
	 */
	double SharpDistanceUu = 5000.0;

	/**
	 * THE EFFECT LIVES AT CLOSE ZOOM: full strength at or inside FullBlurZoomUu of
	 * camera-to-focus distance, none from NoBlurZoomUu out, smoothstepped between.
	 *
	 * Chosen by pitch, which FBuildCameraRig derives from distance on a log curve (12 deg at
	 * 600 uu, 70 at 60000): 1500 uu is ~23 deg, still looking ACROSS the field; 4000 uu is
	 * ~36 deg, where the view has started to look DOWN on the plan, and the plan is for
	 * reading, not for atmosphere (zoomed-out readability rulings, 2026-09-27).
	 */
	double FullBlurZoomUu = 1500.0;
	double NoBlurZoomUu = 4000.0;

	/** BlurAtInfinity after the zoom fade, for a camera ZoomUu from its focus point. */
	double BlurAt(double ZoomUu) const;

	/**
	 * Sensor width in mm that gives BlurAt(ZoomUu) through a HorizontalFovDegrees lens
	 * focused at SharpDistanceUu. Zero means no lens: zoomed out past the fade, or a
	 * degenerate input (no blur is the safe failure).
	 */
	double SensorWidthMm(double ZoomUu, double HorizontalFovDegrees) const;

	/**
	 * Blur diameter of a point DistanceUu away, as a fraction of the frame width, for a lens
	 * of SensorWidthMm focused at FocusDistanceUu. The renderer's own thin-lens formula
	 * (DiaphragmDOFUtils.cpp, CircleDofHalfCoc), here so the test can measure the output
	 * rather than re-derive the input. It does NOT apply the foreground cap: a point nearer
	 * than the focus returns the uncapped value the renderer then clamps to zero.
	 */
	double BlurFraction(double SensorWidthMm, double FocusDistanceUu, double DistanceUu,
		double HorizontalFovDegrees) const;
};
