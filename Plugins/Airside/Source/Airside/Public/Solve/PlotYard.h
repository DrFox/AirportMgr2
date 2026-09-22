#pragma once

#include "CoreMinimal.h"

/**
 * Where each module STANDS in a drawn plot.
 *
 * Dependency-free, like every other Solve/ header: CoreMinimal.h and nothing else. In
 * particular it never sees EDepotModule, which lives in Model/RoadEntity.h - this layer
 * takes FOOTPRINTS and hands back STANDS, and mapping a module to its footprint is
 * UPlotPresenter's job on the other side of the seam. That is the same split FitBays makes
 * by taking an outline rather than an entity, and it is what keeps these tests free of a
 * world, an actor and NewObject.
 *
 * WHY NOT PlotFit. PlotFit answers "how many bays fit against this edge", which is a
 * question about the PLOT. This answers "where does each thing stand", which is a question
 * about its CONTENTS, and the two have different inputs and different failure modes. They
 * are separate files so that a change to one cannot quietly alter the other.
 */
namespace PlotYard
{
	/**
	 * How far apart two modules must stand, uu. 1 m.
	 *
	 * NOT a tolerance: modules that touch read as one building, which is precisely the
	 * "stamped" look this whole file exists to remove. Placement feel, judged in PIE - if it
	 * needs tuning it becomes a UAirsideSettings knob rather than being retyped here.
	 */
	inline constexpr double ClearanceUu = 100.0;

	/**
	 * The lane kept clear from the gate into the plot, uu - and the fence's gate gap, which is
	 * the same number. 8 m.
	 *
	 * WAS 6.2 m, "the fuel truck's length", until 2026-09-22 - the length of a truck that no
	 * longer ships. With 8.5 m of truck turning out of it, the body swept through the gate
	 * posts (PIE: "gets stuck trying to turn out through the gate"). 10 m was tried and cost
	 * too much: seven plot tests failed, small plots lost their gates. 8 m costs the 12 m
	 * Tier 1 plot its gate (FenceLayout wants 13 m of frontage), which the user accepted.
	 *
	 * NOT THE WHOLE FIX: the route out still turns at R=206 uu against a steering lock of
	 * R>=699 uu, so the truck crabs through the turn (LogAirsideTraffic "Route asks for R=206
	 * uu"); the wider gap gives the crabbing body room to clear the fence.
	 *
	 * A DEPOT THE TRUCK CANNOT LEAVE LOOKS PERFECTLY CORRECT FROM EVERY ANGLE - the same
	 * failure the fence's gate gap exists to prevent, and the reason that gap is counted
	 * rather than eyeballed.
	 */
	inline constexpr double GateCorridorUu = 800.0;

	/**
	 * Candidate poses tried per module before it is dropped.
	 *
	 * BOUNDED, because UPlotPresenter::RebuildFrom runs on every graph change and an
	 * unbounded search would make laying a road stutter on an airport full of depots.
	 *
	 * 64 RATHER THAN 24, which was the first figure and was too few: candidates are cheap
	 * only because LayOut samples from the region a module can actually occupy, and even
	 * then a well-filled yard rejects most of them on the modules already standing.
	 */
	inline constexpr int32 MaxTries = 64;

	/**
	 * How far a heading may wander off its quarter turn, radians. ~12 degrees.
	 *
	 * A QUARTER TURN PLUS A FEW DEGREES, never a uniform circle: uniform rotation reads as
	 * debris after an explosion, and this reads as something parked in a hurry, which is the
	 * feeling being bought.
	 */
	inline constexpr double HeadingJitterRadians = 0.21;

	/**
	 * Depths tried when standing a module against the back fence, deepest first.
	 *
	 * A SCAN RATHER THAN A BINARY SEARCH, because "does it fit at this depth" is not
	 * monotonic: the four-point gesture can draw a concave quad, where a module may fit deep,
	 * foul a notch mid-way, and fit again nearer the road. A bisection would happily land in
	 * the notch. Thirty-two containment tests on one module, once per rebuild, is nothing.
	 */
	inline constexpr int32 BackFenceProbes = 32;

	struct FFootprint
	{
		/** Along the module's own +X, which faces away from the road. */
		double LengthUu = 0.0;
		double WidthUu = 0.0;

		/**
		 * Stood against the plot's BACK boundary rather than sampled into the yard.
		 *
		 * POSITION ONLY - the name says where it stands and nothing about which way it
		 * points, deliberately. It was called bFrontsTheGate, which reads as either, and was
		 * built as a position: the shed stood IN the gateway (PIE, 2026-09-17, "the position
		 * of the shed should not be in the gate, it should be further back").
		 *
		 * The HEADING is separate and is not sampled either: a footprint flagged here is laid
		 * square to the frontage, +X away from the road, so its opening faces the yard and the
		 * gate beyond it. See UEntityDefinition::BuildFuelDepot, whose comment records that
		 * this convention was once stated the wrong way round.
		 */
		bool bAgainstTheBackFence = false;
	};

	struct FStand
	{
		FVector2D Centre = FVector2D::ZeroVector;
		/** Radians. +X faces away from the road, as UEntityDefinition::BuildFuelDepot states. */
		double Heading = 0.0;
		bool bPlaced = false;
	};

	struct FYard
	{
		/**
		 * One per footprint given, IN THE ORDER GIVEN; a dropped one has bPlaced false.
		 *
		 * NOT COMPACTED to the ones that fit. The caller knows which module it asked about
		 * only by index, and compacting would silently re-associate a pump's stand with a
		 * tank - a depot drawing the wrong box in the wrong place, with nothing to say so.
		 */
		TArray<FStand> Stands;

		/** How many more of the caller's sample footprint would still fit. */
		int32 RoomForMore = 0;

		/** Derived, never stored: a second count is a second thing to keep in agreement. */
		int32 DroppedCount() const
		{
			int32 Count = 0;
			for (const FStand& Stand : Stands)
			{
				if (!Stand.bPlaced) { ++Count; }
			}
			return Count;
		}
	};

	/**
	 * One kind of thing a plot can reserve room for.
	 *
	 * STILL NOT EDepotModule. A spec is a footprint and two integers, and mapping a kit to
	 * one stays UPlotPresenter's job on the other side of the seam - the same split FitBays
	 * makes by taking an outline rather than an entity, and what keeps these tests free of a
	 * world, an actor and NewObject.
	 */
	struct FKitSpec
	{
		/** ONE module's, never a run's. Reserve multiplies it by the run length itself. */
		FFootprint Footprint;

		/**
		 * Clear ground beyond the footprint, uu. X towards the gate, Y to either side.
		 *
		 * STILL NO KIT IN Solve/. A spec is rectangles and integers; which asset they came
		 * from stays on the other side of the seam.
		 */
		FVector2D ApronUu = FVector2D::ZeroVector;

		/**
		 * How often this kit comes up in the fill cycle.
		 *
		 * IT SETS A CEILING, NOT THE PLAYER'S STRATEGY. They buy in whatever order they like
		 * up to the ceiling, so a generous weight costs nothing and a mean one silently
		 * forbids a build the player wanted.
		 */
		int32 ReserveWeight = 1;

		/** Modules of this kit grouped into one stand at one heading. 1 = never grouped. */
		int32 RunCap = 1;

		/**
		 * Beyond the bays at EACH end of a run, uu - a modular building's end caps. Zero for
		 * anything that is not assembled from parts.
		 */
		double RunEndUu = 0.0;

		/**
		 * The width a run of Length modules stands on, caps included.
		 *
		 * THE ONE MULTIPLICATION. Five sites did "Footprint.WidthUu * RunLength" by hand - the
		 * sampler, the bands strategy, the tool's outline twice and the presenter - and adding
		 * end caps to four of them would have drawn a shed wider than the ground it reserved.
		 * ENFORCED BY: Check-Architecture rule 14, which fails on the hand-written product.
		 */
		double RunWidthUu(int32 Length) const
		{
			return Footprint.WidthUu * Length + RunEndUu * 2.0;
		}
	};

	struct FReservedStand : FStand
	{
		/** Into the Kits array Reserve was given. */
		int32 KitIndex = INDEX_NONE;

		/** Modules this stand holds. 1 unless the kit groups into runs. */
		int32 RunLength = 1;
	};

	/**
	 * Everything a plot has room for, decided once.
	 *
	 * ONLY WHAT IT PLACED. Unlike FYard, which reports a module it could not fit, a
	 * reservation has nothing to refuse - it chose the list. A stand here is a promise that
	 * the module fits, which is what lets the player be shown ghosted slots and charged for
	 * filling them.
	 */
	struct FReservation
	{
		/** In placement order, which is the fill cycle's order. */
		TArray<FReservedStand> Stands;

		/**
		 * True when a Stand's Centre is the centre of its footprint PLUS its apron, rather
		 * than the footprint alone.
		 *
		 * ONE FLAG, TWO READERS: UPlotPresenter::RebuildFrom offsets the drawn object back by
		 * half the apron only when this is set, and FPlotPlaceTool::BuildPreview outlines the
		 * reserved ground at footprint-plus-apron size only when this is set. PlotYard::Reserve
		 * (the scatter strategy) never reads ApronUu at all - grep confirms - so its stands are
		 * centred on the bare footprint; applying either reader's offset there drew a box half
		 * an apron outside the ground the sampler actually fenced off (issue #193). Only
		 * UFuelYardBandsStrategy claims apron ground as part of a stand's rectangle, so only it
		 * sets this true.
		 */
		bool bStandsIncludeApron = false;

		/**
		 * How many modules of one kit this plot can hold.
		 *
		 * DERIVED, never stored beside Stands: a second count is a second thing to keep in
		 * agreement, for the same reason FYard::DroppedCount is derived. Sums RunLength
		 * rather than counting stands, because a three-bay run IS three modules.
		 */
		int32 CeilingFor(int32 KitIndex) const
		{
			int32 Count = 0;
			for (const FReservedStand& Stand : Stands)
			{
				if (Stand.KitIndex == KitIndex) { Count += Stand.RunLength; }
			}
			return Count;
		}
	};

	/**
	 * The four corners of a stand, in order, inset by PlotFit::CornerInsetUu.
	 *
	 * PUBLIC because the tests assert containment and non-overlap with it, and a test that
	 * computed corners its own way would be checking its own arithmetic rather than the
	 * solver's. One derivation, two consumers.
	 */
	AIRSIDE_API void StandCorners(const FStand& Stand, const FFootprint& Footprint,
		TArray<FVector2D>& OutCorners);

	/**
	 * The inward normal of the frontage: which way is INTO the plot.
	 *
	 * PUBLIC because a prescriptive layout needs the plot's own frame before it can place
	 * anything, and deriving it a second time in Build/ would be a second opinion about which
	 * way a depot faces - the failure BuildFuelDepot's comment records having already made.
	 *
	 * Read off the polygon's winding rather than assumed counter-clockwise: a plot stored the
	 * other way round would otherwise aim every module out across the road.
	 */
	AIRSIDE_API FVector2D InwardOf(TArrayView<const FVector2D> Outline,
		FVector2D FrontageA, FVector2D FrontageB);

	/**
	 * Do these two stands' rectangles intersect? Separating axis.
	 *
	 * PUBLIC for the same reason StandCorners is: a caller that computed overlap its own way
	 * would be checking its own arithmetic rather than the solver's. One derivation, now
	 * three consumers - the sampler, the band layout and the tests.
	 */
	AIRSIDE_API bool StandsOverlap(const FStand& A, const FFootprint& FootprintA,
		const FStand& B, const FFootprint& FootprintB);

	/**
	 * Lay the footprints out in the plot.
	 *
	 * Gate is where the fence is left open - URoadEditFacade::PlaceEntityInPlot puts the
	 * entity's pose there, so it is the entity's Position. Seed makes the result repeatable;
	 * see the design doc section 5 for why that is a requirement and not a nicety.
	 */
	AIRSIDE_API FYard LayOut(TArrayView<const FVector2D> Outline,
		FVector2D FrontageA, FVector2D FrontageB, FVector2D Gate,
		TArrayView<const FFootprint> Footprints, int32 Seed,
		const FFootprint& RoomForFootprint);

	/**
	 * Fill the plot, and report what fits.
	 *
	 * THE PLOT'S CAPACITY IS WHAT THIS PLACED, not a number derived beside it. A budget in
	 * bays or square metres would assert that a packing exists without ever proving one -
	 * against a sampler that keeps a gate corridor clear, holds ClearanceUu between modules
	 * and jitters headings, packing into a quad the player drew freehand. It would be right
	 * nearly always, and the occasional wrong is the expensive kind: the player spent money
	 * and got a module that could not be placed.
	 *
	 * THE SAME SAMPLER LayOut USES. The layout shown while the outline is dragged IS the
	 * layout that gets built, because there is one piece of code that decides where a thing
	 * stands. A second evaluator would be a preview quietly describing a different depot.
	 *
	 * Gate is where the fence is left open, as LayOut has it. Seed makes the result
	 * repeatable, which here is load-bearing rather than merely nice: nothing about a
	 * reservation is saved, so the same plot must solve the same way every rebuild.
	 */
	AIRSIDE_API FReservation Reserve(TArrayView<const FVector2D> Outline,
		FVector2D FrontageA, FVector2D FrontageB, FVector2D Gate,
		TArrayView<const FKitSpec> Kits, int32 Seed);
}
