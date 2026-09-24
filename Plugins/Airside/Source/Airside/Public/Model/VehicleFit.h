#pragma once

#include "CoreMinimal.h"

struct FGuidelineEdge;
struct FVehicle;

/**
 * Whether a vehicle's body fits an edge (spec 2026-09-23 §6) - the ONE rule route search
 * gates vehicles on, beside the wingspan rule it gates aircraft on.
 *
 *   A lane: the widest body plus WidthMargin each side within the edge's Width.
 *   A curve: its measured MinRadius no tighter than the steering lock, the steady-state
 *   envelope (VehicleSweep) able to hold it, and that envelope inside the measured
 *   clearances.
 *
 * UNMEASURED GATES NOTHING: Width 0, MinRadius 0 or a clearance of -1 - hand-drawn edges,
 * straight lanes, balloons over grass, anything saved before this existed - and a vehicle
 * with no measured body (BodyWidth 0) is checked for its lock only.
 */
namespace VehicleFit
{
	/** Kept clear each side of a body in a lane, uu: mirrors and the wobble of a real driver. */
	constexpr double WidthMargin = 15.0;

	AIRSIDE_API bool Fits(const FGuidelineEdge& Edge, const FVehicle& Vehicle);
}
