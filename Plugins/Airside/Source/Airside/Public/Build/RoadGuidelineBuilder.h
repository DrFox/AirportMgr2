#pragma once

#include "CoreMinimal.h"
#include "Build/RoadNetworkSolver.h"
#include "Model/RoadHandles.h"

class URoadNetwork;
struct FRoadDesignVehicles;

/**
 * Derives the guideline graph from a solved surface network.
 *
 * A segment contributes one edge per guideline its profile declares. A junction
 * contributes one edge per ORDERED pair of distinct arms - the parent spec's 5.8 turn
 * paths - expressed as ordinary guideline edges so pathfinding never special-cases a
 * junction.
 *
 * Endpoints are shared by HANDLE. A segment edge and the turn paths that continue it
 * reference the same FGuidelineNodeId, which is what makes the graph connected; nothing
 * here depends on two positions being bitwise equal, unlike the surface mesh next door.
 */
struct AIRSIDE_API FRoadGuidelineBuilder
{
	/**
	 * Rebuilds every DERIVED guideline in Network from Solved.
	 *
	 * Edges with bDerived == false are left untouched, along with the nodes they need.
	 *
	 * DesignVehicles IS REQUIRED, not resolved in here - issue #190. Each profile's design
	 * vehicle (FRoadDesignVehicles - per width tier since 2026-09-25) sizes that road's dead-end
	 * balloon and the corner warning below. Every turn path
	 * this builder lays warns against the same figure (a right-angle corner's takeable
	 * radius), once per ordered arm pair, and used to call
	 * UAirsideSettings::ResolveLargestServiceVehicle() fresh each time. Content/ is resolved
	 * exactly once per rebuild, by URoadSurfacePresenter::Rebuild, and handed down here -
	 * which is also why this file no longer includes Content/AirsideSettings.h at all
	 * (Check-Architecture's Build->Content rule).
	 */
	static void Build(URoadNetwork& Network, const FRoadSolveResult& Solved,
		const FRoadDesignVehicles& DesignVehicles);
};
