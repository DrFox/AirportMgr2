#pragma once

#include "CoreMinimal.h"
#include "Solve/JunctionSolver.h"

struct FVehicle;

/**
 * THE INSIDE OF A BEND, WIDENED TO WHAT ITS DESIGN VEHICLE SWEEPS (user ruling 2026-09-25: "bend
 * lanes follow the pavement concentrically; inside widened only by the measured remainder").
 *
 * The lanes round a two-arm bend are arcs about the inner fillet's centre (GuidelineGeom::
 * BendLane). An articulated trailer on the inner lane still cuts inside that fillet - 247 uu
 * past the Wide tier's inner edge, measured - and on past the cut onto the leaving arm, because a
 * trailer converges on its tractor's line along a tractrix, long after the turn ends. So the
 * design vehicle is DRIVEN round every lane's arc here (VehicleSweep::Drive, the router's own
 * pursuit), every body point is put against the inner edge - the arm's straight edge, the fillet,
 * the other arm's straight edge, as one length u along it - and the edge is pushed out into the
 * grass by the deepest reach plus Margin at each u, with Taper either side so the widened edge
 * leans back onto the unwidened one instead of stepping. The arms are cut back far enough to hold
 * it, and the rim that replaces the fillet's arc is handed to FJunctionSolver::SolveBoundary.
 *
 * A PLAIN PATTERN, NOT A NEW SURFACE: the widened rim is the junction polygon's own boundary
 * between the same two cut vertices the fillet ran between, so the ribbon welds to it bitwise as
 * it welded to the arc, and the mesh builder paves it with the same bands and materials.
 */
namespace BendWidening
{
	/** One lane of one arm at the bend: its offset in the arm's OUTGOING frame (+ = PerpCCW of the arm's tangent). */
	struct FLane
	{
		double Lateral = 0.0;
		bool bArrives = false;
		bool bLeaves = false;
	};

	/**
	 * Kept between the design vehicle's body and the widened edge, uu: VehicleFit::WidthMargin's
	 * 15 (what a lane keeps clear each side of a body - mirrors, and a real driver's wobble) plus
	 * the builder's 10 uu clearance march (MeasureTurn), which reads the edge up to a step short -
	 * so the router, judging on those clearances, still sees the body on the tarmac.
	 */
	constexpr double Margin = 25.0;

	/**
	 * How steeply the widened edge leans back onto the unwidened one: 1 across per 4 along. The
	 * trailer's own reach already fades over metres; this only closes the envelope where it stops
	 * so the edge never steps. Gentler costs arm length a short segment does not have - the rig
	 * course's 30 m arms are already cut to their allowance (2026-09-25).
	 */
	constexpr double Taper = 0.25;

	/** The envelope's resolution along the inner edge, uu. */
	constexpr double Bin = 25.0;

	/** What a bend's inside needs, from Measure. */
	struct FWidening
	{
		/** The inner corner: FJunctionResult::Corners index, = the arm whose LEFT edge it starts on. */
		int32 Corner = INDEX_NONE;
		/** Per arm (FJunctionInput::Arms index), the cut the widening needs; 0 = none beyond the fillet's. */
		double NeededCut[2] = { 0.0, 0.0 };
		/** The deepest the design vehicle left the unwidened tarmac, uu - the "measured remainder". */
		double Deepest = 0.0;
		/** The widening along the inner edge: at U[i] (uu from the fillet's first tangent point, negative back up the first arm), W[i] into the grass. */
		TArray<double> U;
		TArray<double> W;
		/** How many lane turns were actually DRIVEN (the rest the steady-state envelope ruled out). */
		int32 Drives = 0;
	};

	/**
	 * Drives Body round every concentric lane turn of a solved two-arm bend - arriving lanes of
	 * one arm to leaving lanes of the other, where both sit the same distance off the inner edge
	 * (GuidelineGeom::BendLane) - and measures what the inside needs. False when nothing leaves
	 * the tarmac by more than -Margin, or the bend is not one this applies to (not two arms, a
	 * straight or invalid corner, no concentric lanes, a bodiless vehicle). Lanes[i] are
	 * Input.Arms[i]'s. A turn that folds the tow is skipped: the router refuses it anyway.
	 */
	bool Measure(const FJunctionInput& Input, const FJunctionResult& Result, const TArray<FLane> (&Lanes)[2],
		const FVehicle& Body, FWidening& Out);

	/**
	 * The rim that replaces the inner fillet's arc, for the cuts Result now HAS (re-solved with
	 * NeededCut as a floor, which the arms' allowance may have capped): InOut.W is clamped to lean
	 * to nothing at each cut, and OutShortfall is how much of the envelope the cap cost - how far
	 * the design vehicle will still leave the tarmac.
	 */
	void Rim(const FJunctionInput& Input, const FJunctionResult& Result, FWidening& InOut,
		TArray<FVector2D>& OutRim, double& OutShortfall);
}
