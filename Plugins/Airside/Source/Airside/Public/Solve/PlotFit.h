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

	/**
	 * A plot's slots: Width bays across the frontage, Depth rows back from it.
	 *
	 * ROW-MAJOR AND FRONT ROW FIRST. Slots[0 .. Width-1] is the row that fronts the road and
	 * takes the modules; everything after it is expansion space the player buys into later.
	 * The presenter fills in this order, so the order is part of the contract rather than an
	 * accident of the loop - a row-major slip would put a shed in the back yard.
	 */
	struct FPlotGrid
	{
		TArray<FPlotBay> Slots;
		int32 Width = 0;
		int32 Depth = 0;
	};

	/**
	 * Lay Width x Depth slots against the frontage edge A->B, interior on its LEFT.
	 *
	 * NO POLYGON AND NO CONTAINMENT TEST, unlike FitBays below: a rectangle built out from
	 * its own edge cannot fall outside itself. That is the whole reason a staged gesture is
	 * cheaper than a freeform one - the shape is known before the geometry is asked about.
	 *
	 * THE CALLER OWNS THE INTERIOR SIDE. FitBays has to discover it from the polygon's
	 * winding because the player drew that polygon; here the gesture built the frontage from
	 * a road and the side the cursor was on, so there is nothing to discover.
	 */
	AIRSIDE_API FPlotGrid BuildGrid(FVector2D FrontageA, FVector2D FrontageB,
		int32 Width, int32 Depth);

	/**
	 * The plot's own boundary, counter-clockwise and implicitly closed.
	 *
	 * COUNTER-CLOCKWISE IS NOT COSMETIC. The pad is triangulated by the same ear-clipper an
	 * apron uses, which orients its triangles from the winding, and the surface is not
	 * two-sided - so a clockwise outline renders as no concrete at all. That shipped on
	 * 2026-09-15 and took a screenshot to find.
	 */
	AIRSIDE_API TArray<FVector2D> GridOutline(FVector2D FrontageA, FVector2D FrontageB,
		int32 Width, int32 Depth);

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
