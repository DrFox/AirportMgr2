#pragma once

#include "CoreMinimal.h"

/**
 * The world grid: axis-aligned lines at every multiple of a step, through the world origin.
 *
 * DEPENDENCY-FREE like every Solve/ header. The step is a plain uu figure here rather than the
 * reflected EGridStep, because UHT cannot see a Solve/ enum (see GuideArbiter.h's ERelation) and
 * FSnapGuideSettings is the one place the figures live - this only does the arithmetic.
 *
 * WORLD, NOT PER-ROAD. The per-segment bay grid in PlotGesture::AnchorAt is phased from each
 * segment's own A end, which is why two stands of equal depth off two segments did not line up
 * (2026-09-27 report; world-grid-snap design). A grid shared by every tool must have one phase.
 *
 * EVERY STEP <= 0 IS "NO GRID", answered by each function rather than checked by every caller,
 * so Off cannot divide by zero anywhere.
 */
namespace GridSnap
{
	/**
	 * The nearest grid point. Point unchanged when StepUu <= 0.
	 *
	 * TO NEAREST, via RoundToDouble - never an int cast, which truncates toward zero and would
	 * put every point west or south of the origin one line out.
	 */
	AIRSIDE_API FVector2D Quantise(const FVector2D& Point, double StepUu);

	/**
	 * The point on the line Origin + t * Direction that lies on a grid line (X = k * Step or
	 * Y = k * Step) and is nearest the foot of Near on that line.
	 *
	 * BOTH FAMILIES, nearest wins: a diagonal line crosses X and Y lines alternately, and taking
	 * one family only would double the spacing a player sees the point jump by. A line parallel
	 * to one family crosses only the other.
	 *
	 * False, Out untouched, for StepUu <= 0 or a zero Direction - CLAUDE.md's out-parameter rule.
	 */
	AIRSIDE_API bool NearestCrossingAlong(const FVector2D& Origin, const FVector2D& Direction,
		const FVector2D& Near, double StepUu, FVector2D& Out);

	/**
	 * NearestCrossingAlong limited to t in [TMin, TMax] along the unit direction - the nearest
	 * crossing to Near's foot that lies inside the range, or false (Out untouched) when the range
	 * holds none. What a plot needs: an anchor must stay on its segment and a frontage must be at
	 * least the minimum, and rounding to the unrestricted nearest broke both (review, 2026-09-27).
	 */
	AIRSIDE_API bool NearestCrossingInRange(const FVector2D& Origin, const FVector2D& Direction,
		const FVector2D& Near, double StepUu, double TMin, double TMax, FVector2D& Out);

	/** One drawable stretch of a grid line. */
	struct FPiece
	{
		FVector2D From = FVector2D::ZeroVector;
		FVector2D To = FVector2D::ZeroVector;

		/** Every fifth line from the origin, so a 1 m grid can still be counted in fives. */
		bool bMajor = false;
	};

	/**
	 * Every grid line crossing the disc, clipped to it, cut into pieces no longer than one step.
	 *
	 * PIECES, NOT WHOLE CHORDS, because ARoadBuildHUD::Line drops a line outright when either
	 * end projects more than 64 px off-screen - a 120 m chord would vanish whenever the disc ran
	 * past the view's edge, while one-cell pieces lose only the cells that are actually off it.
	 *
	 * Out is reset. Empty for StepUu <= 0 or RadiusUu <= 0.
	 */
	AIRSIDE_API void PiecesInDisc(const FVector2D& Centre, double RadiusUu, double StepUu,
		TArray<FPiece>& Out);

	/**
	 * A grid TURNED TO A THING - grid-follows-snap design (2026-09-28). The functions above are
	 * the world grid; a frame is the same arithmetic in rotated coordinates.
	 *
	 * ONE LINE ALONG THE THING'S CENTRELINE, THE CROSS LINES PHASED FROM THE WORLD ORIGIN - the
	 * design's option 1. Along() puts Origin at the foot of the world origin on the thing's line,
	 * so both families pass through it: the thing lies on a line (a stand's depth off it is a
	 * round figure), and the cross lines are the same for every segment of one road and every
	 * road parallel to it (the 2026-09-27 report the world grid fixed stays fixed). Rejected: the
	 * world grid rotated about the origin (the road sits between lines - odd depths, the diagonal
	 * complaint turned); an origin at the segment's own end (a phase per segment - that report).
	 *
	 * AXIS FOLDED INTO [0, 90) DEGREES. A square grid is the same after a quarter turn, so a road
	 * drawn A->B and one drawn B->A, or at 30 and at 120 degrees, must give ONE frame - and
	 * SameLines compares them exactly, which only an exact fold makes possible.
	 */
	struct AIRSIDE_API FGridFrame
	{
		FVector2D Origin = FVector2D::ZeroVector;

		/** Unit, folded - see above. */
		FVector2D Axis = FVector2D(1.0, 0.0);

		/** uu; <= 0 is "no grid", as for every function here. */
		double StepUu = 0.0;

		/** The world grid: what every function above computes. */
		static FGridFrame World(double StepUu);

		/** The grid along the line through Point in Direction. A zero Direction is World. */
		static FGridFrame Along(const FVector2D& Point, const FVector2D& Direction, double StepUu);

		bool IsOn() const { return StepUu > 0.0; }

		/**
		 * Origin zero and Axis (1,0): the frame overloads then call the world functions DIRECTLY
		 * rather than mapping - the bitwise-today contract, not an optimisation. A map through
		 * (1,0) is exact for finite values but can flip the sign of a zero, and "World is
		 * today" was promised without that footnote.
		 * ENFORCED BY: Airside.Solve.GridSnap.WorldFrameIsBitwiseToday
		 */
		bool IsWorldAligned() const;

		/** Exactly the same lines: origin, axis and step compared bitwise. */
		bool SameLines(const FGridFrame& Other) const;

		/** The folded axis's angle from +X, [0, 90). For the log line, not for arithmetic. */
		double AxisDegrees() const;
	};

	/** Quantise in Frame's coordinates. Point unchanged when the frame is off. */
	AIRSIDE_API FVector2D Quantise(const FVector2D& Point, const FGridFrame& Frame);

	/** NearestCrossingAlong against Frame's two families. Same refusals, same out-parameter rule. */
	AIRSIDE_API bool NearestCrossingAlong(const FVector2D& Origin, const FVector2D& Direction,
		const FVector2D& Near, const FGridFrame& Frame, FVector2D& Out);

	/** NearestCrossingInRange against Frame. T is along the unit Direction, as there. */
	AIRSIDE_API bool NearestCrossingInRange(const FVector2D& Origin, const FVector2D& Direction,
		const FVector2D& Near, const FGridFrame& Frame, double TMin, double TMax, FVector2D& Out);

	/** PiecesInDisc for Frame's lines; major every fifth line from Frame.Origin. */
	AIRSIDE_API void PiecesInDisc(const FVector2D& Centre, double RadiusUu, const FGridFrame& Frame,
		TArray<FPiece>& Out);
}
