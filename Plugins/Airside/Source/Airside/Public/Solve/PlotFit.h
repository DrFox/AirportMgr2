#pragma once

#include "CoreMinimal.h"

/**
 * Fitting module bays into a drawn plot, against the edge that faces the road.
 *
 * Dependency-free, like every other Solve/ header: CoreMinimal.h and nothing else. In
 * particular NOT FTransform2D, which lives in Math/TransformCalculus2D.h and is not pulled
 * in by CoreMinimal.h - a bay is a centre and a heading, which is all a placement needs,
 * and reaching for a transform type would cost this header its world-free tests.
 *
 * WHICH EDGE IS THE FRONTAGE IS NOT ASKED HERE. Finding the nearest road needs the network,
 * which this layer may not see, so the caller picks the edge and hands it in. See
 * URoadEditFacade::FindFrontageEdge. That split is the whole reason this file can be tested
 * with no world, no actor and no NewObject.
 */
namespace PlotFit
{
	/**
	 * 4 m. The Tier 1 depot on the concept sheet is three of these, which is where the
	 * number came from - see the design doc §3.2. Everything else keys off it, so moving it
	 * moves what every plot can hold.
	 */
	inline constexpr double BayWidthUu = 400.0;

	/** 8 m, the concept sheet's site depth. ONE ROW ONLY: a plot drawn deeper is yard, not
	 *  a second rank of bays. That keeps the fit arithmetic instead of a packing solver. */
	inline constexpr double BayDepthUu = 800.0;

	/**
	 * How far a corner probe is pulled in from the bay's own corner before asking whether
	 * it is inside the plot, uu. 1 cm.
	 *
	 * NOT A TOLERANCE FUDGE - it is load-bearing. A plot exactly three bays wide puts every
	 * bay corner exactly ON the outline, and a winding-number test is undefined on the
	 * boundary: it answers by floating-point coin flip, differently on another machine.
	 * Probing just inside asks the question that was actually meant - "is this bay within
	 * the plot" - rather than "is this point on its edge".
	 */
	inline constexpr double CornerInsetUu = 1.0;

	enum class EPlotRefusal : uint8
	{
		None,
		/** Less than one bay of frontage, or too shallow for one bay to stand in. */
		TooSmall,
		/** The caller found no road-facing edge to fit against. */
		NoFrontage
	};

	struct FPlotBay
	{
		FVector2D Centre = FVector2D::ZeroVector;

		/**
		 * Radians. +X points AWAY from the frontage, because the truck drives out of the
		 * back of the installation - see UEntityDefinition::BuildFuelDepot, whose comment
		 * records that this was once stated the wrong way round.
		 */
		double Heading = 0.0;
	};

	struct FPlotFit
	{
		TArray<FPlotBay> Bays;
		bool bFits = false;
		EPlotRefusal Why = EPlotRefusal::None;
	};

	// A GRID OF SLOTS AND A RECTANGLE BUILDER lived here until 2026-09-17: FPlotGrid,
	// BuildGrid and GridOutline. They served the rectangle gesture, and the four-point
	// gesture that replaced it has no rows and no bays - a plot is four corners the player
	// placed, and its contents are laid out by Solve/PlotYard rather than fitted to a grid.
	// See docs/superpowers/specs/2026-09-16-four-point-plot-gesture-design.md.
	//
	// DELETED RATHER THAN LEFT. A solver nothing calls is a thing the next reader has to
	// disprove the importance of before they can change anything near it.
	//
	// FitBays below is NOT dead: URoadEditFacade::PlaceEntityInPlot still calls it, for
	// better and worse - see that spec's section 8 on the second evaluator it represents.

	/**
	 * Lay bays along the frontage edge, inside Outline.
	 *
	 * Outline is a simple polygon, closed implicitly - the last point joins the first and
	 * the array does NOT repeat it, the same contract FApronSurface::Outline states.
	 *
	 * FrontageA -> FrontageB must be given in the outline's own winding order, which is what
	 * URoadEditFacade::FindFrontageEdge returns. EITHER WINDING IS ACCEPTED: the interior
	 * side is derived from the polygon's signed area rather than assumed counter-clockwise,
	 * because a plot whose clicks happened to run the other way round would otherwise aim
	 * every bay out of the plot and across the road.
	 */
	AIRSIDE_API FPlotFit FitBays(TArrayView<const FVector2D> Outline,
		FVector2D FrontageA, FVector2D FrontageB);
}
