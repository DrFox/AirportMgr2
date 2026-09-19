#pragma once

#include "CoreMinimal.h"
#include "Build/RoadMeshSink.h"

class URoadNetwork;

/** How many of each marking one Build painted, for the census log line and the tests. */
struct AIRSIDE_API FRunwayMarkingCensus
{
	int32 Runways = 0;
	int32 ThresholdStripes = 0;
	int32 DesignatorStrokes = 0;
	int32 CentrelineDashes = 0;
	int32 AimingPointBars = 0;
	int32 TouchdownStripes = 0;
	int32 SideStripes = 0;
	int32 GrassMarkers = 0;
	/** Filled by BuildRubber, never by Build - the two write different buffers. */
	int32 RubberPatches = 0;
};

/**
 * The PAINT of a runway: the ICAO marking set its width, surface and approach class
 * dictate, as quads for a marking component of its own - the same mesh trick as
 * FHoldingPositionMarkingBuilder (UV1 = 0 paints a quad solid MarkingColor), drawn
 * through a material instance whose MarkingColor is white.
 *
 * Everything is DERIVED, per chain, from the threshold, direction and length that
 * RunwayExtentAt reports, the width the profile gives and the facts on the segment.
 * Nothing is stored: move a runway and its paint follows, exactly as its designator
 * does (see RunwayDesignator).
 *
 * Constants are ICAO Annex 14 vol. I chapter 5 figures in uu, scaled where the standard
 * is given per width. A Build/ class beside the holding-position builder, for the same
 * reason it is not part of FRoadMeshBuilder: a marking that lands on a road vertex must
 * not weld to it. Two surfaces meet; they are not one surface.
 */
struct AIRSIDE_API FRunwayMarkingBuilder
{
	// --- Threshold stripes ("piano keys"): 30 m long, 1.8 m wide, 6 m from the threshold.
	static constexpr double StripeLength = 3000.0;
	static constexpr double StripeWidth = 180.0;
	static constexpr double StripeStart = 600.0;
	/** The outermost stripe's outer edge sits this far inside the pavement edge. */
	static constexpr double StripeInset = 300.0;

	// --- Designation: two digits this tall, the same gap past the stripes as they had past the threshold.
	static constexpr double DigitHeight = 900.0;
	static constexpr double DigitGapAfterStripes = 600.0;
	/** Gap between the two digits, as a fraction of a digit's width. */
	static constexpr double DigitSpacing = 0.4;

	// --- Centreline: 30 m on, 20 m off, from designation to far designation.
	static constexpr double DashOn = 3000.0;
	static constexpr double DashOff = 2000.0;
	static constexpr double CentrelineWidthNarrow = 45.0;
	static constexpr double CentrelineWidthWide = 90.0;
	/** Runways at least this wide take the wide centreline. */
	static constexpr double WideRunway = 4500.0;
	static constexpr double DashGapAfterDigits = 600.0;

	// --- Aiming point: two bars starting 400 m in (300 m on a runway under 1200 m), 6 m wide, 18 m apart.
	static constexpr double AimingPointAt = 40000.0;
	static constexpr double AimingPointAtShort = 30000.0;
	static constexpr double AimingPointShortRunway = 120000.0;
	static constexpr double AimingPointLength = 4500.0;
	static constexpr double AimingPointLengthNarrow = 3000.0;
	static constexpr double AimingPointNarrowRunway = 3000.0;
	static constexpr double AimingPointWidth = 600.0;
	static constexpr double AimingPointGap = 1800.0;

	// --- Touchdown zone: pairs at 150 m intervals, one, two then three stripes a side.
	static constexpr double TouchdownPairsAt[3] = { 15000.0, 30000.0, 45000.0 };
	static constexpr double TouchdownStripeLength = 2250.0;
	static constexpr double TouchdownStripeWidth = 180.0;
	static constexpr double TouchdownStripeGap = 150.0;

	// --- Side stripes: 0.9 m continuous at each edge.
	static constexpr double SideStripeWidth = 90.0;

	// --- Tyre rubber. NOT a marking, and the distinction is the whole design: paint is
	// applied to a plan, rubber is deposited by use. It is therefore not in the standard,
	// and its extent is instead ANCHORED to the markings that describe where aircraft are
	// meant to touch down - which is where they do touch down, and so where the rubber is.
	//
	// From the far edge of the piano keys to the far edge of the last touchdown pair. Both
	// ends are expressions rather than figures so that moving a marking moves the rubber
	// with it; a number typed here would be a second source of truth for the same fact.
	static constexpr double RubberStart = StripeStart + StripeLength;
	static constexpr double RubberEnd = TouchdownPairsAt[2] + TouchdownStripeLength;
	/**
	 * How far across the strip a rubber band reaches, as a fraction of the half width.
	 *
	 * TWO bands per end, one either side of the centreline, because that is where main gear
	 * tracks run - rubber is heaviest under the wheels and there is a visible clean strip
	 * down the middle of a real runway. One band across the full width would be the thing
	 * every stylised airport gets wrong.
	 */
	static constexpr double RubberAcrossFraction = 0.55;

	// --- Grass: 0.6 m markers every 60 m along both edges, 3 m squares at the corners.
	static constexpr double GrassMarker = 60.0;
	static constexpr double GrassMarkerSpacing = 6000.0;
	static constexpr double GrassCorner = 300.0;

	/** Threshold stripe count for a total width, nearest of the five ICAO widths. */
	static int32 ThresholdStripeCount(double TotalWidth);

	/** The centreline dash width for a total width. */
	static double CentrelineWidth(double TotalWidth);

	/**
	 * Append the markings of every runway in Network to Out, in the road plane at Z.
	 * Returns how many runways were painted; Census, when given, says what was painted.
	 *
	 * Each marking is a quad of four consecutive vertices (MarkingQuads::AddQuad), so a
	 * test can measure them one at a time. Markings that would cross the strip's midpoint
	 * - a touchdown pair or an aiming point on a short runway - are omitted rather than
	 * overlapped with the far end's (spec §8: paint what fits).
	 */
	static int32 Build(const URoadNetwork& Network, double Z, FRoadMeshBuffers& Out,
		FRunwayMarkingCensus* Census = nullptr);

	/**
	 * Append the tyre rubber of every paved runway in Network to Out, in the road plane at
	 * Z. Returns how many patches were laid; Census, when given, has RubberPatches set.
	 *
	 * A SEPARATE BUFFER FROM Build, and that is not an implementation detail. The rubber is
	 * drawn dark where the paint is drawn white, so it needs its own component and its own
	 * material instance. It also must not share Build's buffer for a second reason: the
	 * marking tests identify glyph strokes BY EXCLUSION - any quad that is none of the
	 * standard rectangles - so a rubber quad in that buffer would silently be counted as a
	 * designator stroke.
	 *
	 * Census is NOT reset here, unlike in Build: the two are meant to be called with the
	 * same census and each owns its own fields.
	 *
	 * Two quads per end, either side of the centreline. A band that would cross the strip's
	 * midpoint is CUT AT THE MIDPOINT, not dropped - and that is where rubber parts company
	 * with the markings beside it. A touchdown pair that will not fit is omitted, because a
	 * marking is specified and half of one is not it. Rubber has no standard and no
	 * specified length; a short runway does not have less rubber, it has a shorter patch.
	 *
	 * Grass strips get none: rubber on grass is a rut, not a stain, and that is a different
	 * feature.
	 */
	static int32 BuildRubber(const URoadNetwork& Network, double Z, FRoadMeshBuffers& Out,
		FRunwayMarkingCensus* Census = nullptr);
};
