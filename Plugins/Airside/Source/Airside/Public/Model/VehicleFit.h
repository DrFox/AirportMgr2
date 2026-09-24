#pragma once

#include "CoreMinimal.h"

struct FGuidelineEdge;
struct FVehicle;
class URoadNetwork;

/**
 * Whether a vehicle's body fits an edge (spec 2026-09-23 §6) - the ONE rule route search
 * gates vehicles on, beside the wingspan rule it gates aircraft on.
 *
 *   A lane: the widest body plus WidthMargin each side within the edge's Width.
 *   A curve: its measured MinRadius no tighter than the steering lock, and the vehicle
 *   SIMULATED along the curve's own samples (VehicleSweep::Trace) without jack-knifing and
 *   with its swept width, at every sample, inside the tarmac's width there.
 *
 * SWEPT WIDTH AGAINST TARMAC WIDTH, not inside against inside (2026-09-24): the line is the
 * lane's centre, and a real rig turning on the near side swings OUT into the other lane to
 * keep its trailer off the kerb. Holding it to its own lane's line refused turns real rigs
 * make every day. The cost, stated: the claim model does not reserve the other lane, so an
 * oncoming vehicle can overlap a rig mid-turn.
 *
 * UNMEASURED GATES NOTHING: Width 0, MinRadius 0, no per-sample clearances - hand-drawn edges,
 * straight lanes, balloons over grass, anything saved before this existed - and a vehicle
 * with no measured body (BodyWidth 0) is checked for its lock only.
 */
namespace VehicleFit
{
	/** Kept clear each side of a body in a lane, uu: mirrors and the wobble of a real driver. */
	constexpr double WidthMargin = 15.0;

	AIRSIDE_API bool Fits(const FGuidelineEdge& Edge, const FVehicle& Vehicle, const URoadNetwork& Network);
}
