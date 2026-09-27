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
}
