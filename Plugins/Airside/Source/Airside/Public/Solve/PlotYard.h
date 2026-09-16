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
	 * The lane kept clear from the gate into the plot, uu. 6.2 m, the fuel truck's length.
	 *
	 * A DEPOT THE TRUCK CANNOT LEAVE LOOKS PERFECTLY CORRECT FROM EVERY ANGLE - the same
	 * failure the fence's gate gap exists to prevent, and the reason that gap is counted
	 * rather than eyeballed.
	 */
	inline constexpr double GateCorridorUu = 620.0;

	/**
	 * Candidate poses tried per module before it is dropped.
	 *
	 * BOUNDED AND SMALL. UPlotPresenter::RebuildFrom runs on every graph change, so an
	 * unbounded search would make laying a road stutter on an airport full of depots.
	 */
	inline constexpr int32 MaxTries = 24;

	/**
	 * How far a heading may wander off its quarter turn, radians. ~12 degrees.
	 *
	 * A QUARTER TURN PLUS A FEW DEGREES, never a uniform circle: uniform rotation reads as
	 * debris after an explosion, and this reads as something parked in a hurry, which is the
	 * feeling being bought.
	 */
	inline constexpr double HeadingJitterRadians = 0.21;

	struct FFootprint
	{
		/** Along the module's own +X, which faces away from the road. */
		double LengthUu = 0.0;
		double WidthUu = 0.0;

		/**
		 * Square to the frontage and nearest the gate, rather than sampled.
		 *
		 * The shed, because a truck drives out of it. Its heading is FUNCTIONAL and is the
		 * one thing here that may not be turned for looks.
		 */
		bool bFrontsTheGate = false;
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
	 * The four corners of a stand, in order, inset by PlotFit::CornerInsetUu.
	 *
	 * PUBLIC because the tests assert containment and non-overlap with it, and a test that
	 * computed corners its own way would be checking its own arithmetic rather than the
	 * solver's. One derivation, two consumers.
	 */
	AIRSIDE_API void StandCorners(const FStand& Stand, const FFootprint& Footprint,
		TArray<FVector2D>& OutCorners);

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
}
