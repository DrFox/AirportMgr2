#pragma once

#include "CoreMinimal.h"

/**
 * The lens that makes the airport read as a table-top model: a faint softening of whatever
 * lies far behind the point the camera looks at, the same amount at every zoom.
 *
 * A PLAIN STRUCT, same shape and reason as FSunPath and FBuildCameraRig: numbers in, numbers
 * out, testable with no world. UBuildCameraComponent holds the tunables as UPROPERTYs and
 * writes the result onto its camera's post-process settings every frame.
 *
 * WHY THE SENSOR SIZE IS THE OUTPUT, not the f-stop. UE's cinematic DOF is a real thin lens:
 * the blur of a point at infinity, as a fraction of the frame, is f^2 / (N (s - f) w), and
 * with the focal length f fixed by the field of view and the sensor width w, that shrinks
 * with focus distance s. A real 16 mm lens focused 80 m away blurs the horizon by under a
 * thousandth of a pixel - which is exactly why tilt-shift photographs of real towns need a
 * tilted lens, and why an un-scaled DOF on this camera did nothing visible at 150 m and would
 * have eaten the frame at 6 m. Scaling w with s (and f with it, the FOV being fixed) holds the
 * background blur at one chosen fraction of the frame across the camera's hundredfold range.
 * The f-stop is left alone because the renderer clamps nothing on the sensor but does shape
 * the bokeh from the f-stop (DiaphragmDOFUtils.cpp, FBokehModel::Compile).
 *
 * Spec 2026-09-12-environment-art-direction-design.md section 4.1: a HINT, judged at 20, 150
 * and 600 m. Every default below is a starting guess until a screenshot says otherwise.
 */
struct FMiniatureFocus
{
	/**
	 * Blur diameter of a point at infinity, as a fraction of the frame width.
	 *
	 * 0.01 is 19 px at 1920 wide. At the build camera's steep pitch the far edge of the frame
	 * is only ~1.3x the focus distance away, where blur is (1 - s/d) of this - about a quarter,
	 * so ~4 px. The horizon only shows at close zoom, where the pitch is shallow - and there
	 * the fade below switches the effect off. Judged in PIE 2026-09-30; 0.003 was invisible.
	 */
	double BlurAtInfinity = 0.01;

	/** Aperture. Shapes the bokeh only; the blur amount comes from BlurAtInfinity. */
	double FStop = 4.0;

	/**
	 * THE EFFECT FADES OUT UP CLOSE: none at or inside NoBlurDistanceUu, full from
	 * FullBlurDistanceUu out, smoothstepped between.
	 *
	 * Close in, the camera is near eye level and the thing the player is looking at is often
	 * NOT the screen-centre ground point the lens focuses on - samples/blur.png (2026-09-30)
	 * had a taxiing aircraft just past the focus, blurred, with the foreground smeared too.
	 * A model photograph reads as a model from above; at eye level it just reads as a camera
	 * that missed focus. 30 m is roughly where that aircraft was framed; 80 m is the build
	 * view's start distance, where the effect was first judged.
	 */
	double NoBlurDistanceUu = 3000.0;
	double FullBlurDistanceUu = 8000.0;

	/** BlurAtInfinity after the close-zoom fade, for a lens focused FocusDistanceUu away. */
	double BlurAt(double FocusDistanceUu) const;

	/**
	 * Sensor width in mm that gives BlurAtInfinity when focused FocusDistanceUu away through
	 * a HorizontalFovDegrees lens. Zero for a degenerate input (no blur is the safe failure).
	 */
	double SensorWidthMm(double FocusDistanceUu, double HorizontalFovDegrees) const;

	/**
	 * Blur diameter of a point DistanceUu away, as a fraction of the frame width, for a lens
	 * of SensorWidthMm focused at FocusDistanceUu. The renderer's own thin-lens formula
	 * (DiaphragmDOFUtils.cpp, CircleDofHalfCoc), here so the test can measure the output
	 * rather than re-derive the input.
	 */
	double BlurFraction(double SensorWidthMm, double FocusDistanceUu, double DistanceUu,
		double HorizontalFovDegrees) const;
};
