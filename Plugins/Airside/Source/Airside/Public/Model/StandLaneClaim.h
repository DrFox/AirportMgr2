#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"

class URoadNetwork;
struct FRoutePlan;

/**
 * ONE SERVICE VEHICLE ON A STAND'S LANES AT A TIME (#540, owner ruling 2026-10-03: "Roads are 2 lane in the game;
 * it's only on the stand that they are not 2 lane"). A stand's contact spur and the link onto the road are ONE
 * two-way strip, so a vehicle leaving and one arriving can only meet head-on there; the claim makes the second
 * wait on the ROAD lane, a gap short of the node it would turn in at, until the first has left.
 *
 * THE RESOURCE IS FTrafficResource::OfStandLanes, held through the same occupancy table and claim pass as every
 * edge, node and runway - so it is released on every exit the table already knows: the vehicle driving off the
 * lanes (the per-tick ReleaseExcept), its agent retired (ReleaseAll), a replan (ReleaseReservations keeps only
 * what it stands on), a load (the table is rebuilt). FClaimPass asks these three questions and nothing more;
 * they live here so the pass does not grow a fourth concern inline.
 * ENFORCED BY: AirportOps.Fuel.StandLanes.OneVehicleAtATime, AirportOps.Fuel.StandLanes.ReleasedWhenTheHolderIsRetired
 */
namespace StandLaneClaim
{
	/** The stand whose lanes Plan's step Index is part of (FGuidelineEdge::StandLanesOf), or unset. */
	AIRSIDE_API FEntityInstanceId Of(const URoadNetwork& Network, const FRoutePlan& Plan, int32 Index);

	/**
	 * The stand whose lanes step Index + 1 ENTERS from step Index, or unset - where the door claim goes, asked
	 * before Index's end node so a refusal stops the vehicle short of that node on the road.
	 */
	AIRSIDE_API FEntityInstanceId EnteredAfter(const URoadNetwork& Network, const FRoutePlan& Plan, int32 Index);

	/** One line when a vehicle's occupied stand lanes changed across a claim pass: left the old, entered the new. */
	AIRSIDE_API void LogChange(int32 AgentId, FEntityInstanceId Before, FEntityInstanceId After);
}
