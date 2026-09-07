#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"

class URoadNetwork;

/**
 * THE ONE PLACE the exit-arc geometry at a runway junction is decided, because two builders
 * need the same numbers: the junction solver's input (the flare fillet's radius follows the
 * arc) and the guideline builder (the arc's tangent length IS the set-back). Two copies of
 * "min(ExitLength, 45 percent of every arm)" would be two numbers that must agree about one
 * arc - see CLAUDE.md, "lists that must agree are ONE list".
 *
 * Spec: docs/superpowers/specs/2026-09-06-runway-exit-arcs-design.md, §3.4 as amended
 * 2026-09-07 (symmetric) and §12 (the flare).
 */
namespace ExitGeometry
{
	/** The share of an arm's node-to-node length a set-back may take. */
	constexpr double ArmShare = 0.45;

	/**
	 * The fillet a runway-junction corner gets when the arc through it asks for less, uu:
	 * the acute side of an exit, where the only turn is the hairpin. Small on purpose. The
	 * profile's default (15 m) there costs a stub taxiway most of its cut allowance - a 45
	 * degree corner needs 2.4 radii of tangent - and the solver then shrinks EVERY radius at
	 * the node to fit, flare included, which put the swept band off the pavement on the
	 * obtuse side. The flare is the corner that matters; this one is a kerb.
	 */
	constexpr double AcuteCornerRadius = 300.0;

	/**
	 * The tangent length of every exit arc at this node, uu - the same on the runway side
	 * and the taxiway side (symmetric). Zero when the node is not MIXED (at least one
	 * continuous arm and at least one that is not) or when its runway authors no
	 * ExitLength: the arcs are off and the ends stay at their cut lines.
	 *
	 * Arms is the node's incident segments in the solver's order; NodeIndex names the node.
	 */
	AIRSIDE_API double NodeExitLength(const URoadNetwork& Network, int32 NodeIndex,
		const TArray<FRoadSegmentId>& Arms);

	/**
	 * The fillet radius that follows an exit arc through a corner, uu, or 0 when the
	 * default is at least as large.
	 *
	 * An arc of tangent length L through a corner of angle Theta (the wedge between the two
	 * arms' outgoing tangents) turns by PI - Theta and has radius L * tan(Theta / 2). The
	 * pavement's inner edge must sit a taxiway's half width inside that: on the obtuse
	 * corner of a rapid exit that is a wide flare (60 m tangents at 150 degrees: 224 m arc,
	 * 212 m fillet); on the acute corner the same formula gives less than a default fillet,
	 * so the default stands. One formula, no side-picking.
	 */
	AIRSIDE_API double FlareRadius(double TangentLength, double CornerAngle, double TaxiwayHalfWidth);

	/**
	 * The least a taxiway's guideline end may sit from the junction node, uu: where the
	 * taxiway's far edge clears the runway slab, so a holding position on it is never on
	 * the asphalt however wide the flare pavement grows. AxisAngle is the angle between the
	 * taxiway's and the runway's axes, either way round; below ten degrees it is treated as
	 * ten, because a taxiway that shallow never clears the strip by this measure at all.
	 */
	AIRSIDE_API double TaxiwayEndFloor(double RunwayHalfWidth, double TaxiwayHalfWidth, double AxisAngle);
}
