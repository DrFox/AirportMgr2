#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"
#include "Solve/RoadGeom.h"

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

	/**
	 * TaxiwayEndFloor where the runway ENDS at the node rather than passing through it. Both
	 * tangents point away from the node, RunwayTangent into the runway. A taxiway leaving
	 * AWAY from the runway's body has the slab behind it, so it is clear once its far edge is
	 * past the runway's end line - T * tan(off the axis) - or past its side, whichever comes
	 * first. One running back alongside the body is the side case, TaxiwayEndFloor.
	 *
	 * The side formula alone read an in-line taxiway as one that never clears the strip and
	 * floored it at 10 degrees: (H + T) / sin 10 = 81 m down a 30 m taxiway, so the holding
	 * position stood in the grass and aircraft crabbed off the pavement to it
	 * (samples/colours.png, 2026-10-01).
	 */
	AIRSIDE_API double TaxiwayEndFloorAtRunwayEnd(double RunwayHalfWidth, double TaxiwayHalfWidth,
		const FVector2D& RunwayTangent, const FVector2D& TaxiwayTangent);

	/**
	 * How far off the runway's extended centreline a taxiway leaving its end may run and still
	 * be solved as straight through (IsInLineAtRunwayEnd). 10 degrees, set 2026-10-01: the
	 * reported layout was 1.95 degrees off, grid and direction snaps leave a few, and at 10 the
	 * runway's end edge, squared to the taxiway, moves under 1.8 m on a 20 m runway.
	 * THE VALUE IS RoadGeom::InLineDegrees since 2026-10-02 (taxiway names read it from Model/).
	 * ENFORCED BY: Airside.Solve.InLineIsOneDefinition
	 */
	constexpr double InLineEndDegrees = RoadGeom::InLineDegrees;

	/**
	 * A runway ending at a node whose one other arm is a taxiway carrying on within
	 * InLineEndDegrees of its line. Such a node is SOLVED AS STRAIGHT THROUGH (the runway's
	 * end edge squared to the taxiway) because as a corner it has no answer: the two edges are
	 * the runway's and the taxiway's, offset by the difference of their half widths and
	 * nearly parallel, so they meet kilometres away - 17 km at the reported 1.95 degrees - and
	 * the flare, following an arc that barely turns, asks for a radius larger still. The node
	 * failed and paved nothing, or paved a one-sided wedge (samples/colours.png).
	 * Tangents point away from the node.
	 */
	AIRSIDE_API bool IsInLineAtRunwayEnd(const FVector2D& RunwayTangent, const FVector2D& TaxiwayTangent);
}
