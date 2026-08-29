#pragma once

#include "CoreMinimal.h"
#include "Build/RoadNetworkSolver.h"
#include "Model/RoadHandles.h"

class URoadNetwork;

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
struct ROADNET_API FRoadGuidelineBuilder
{
	/**
	 * Rebuilds every DERIVED guideline in Network from Solved.
	 *
	 * Edges with bDerived == false are left untouched, along with the nodes they need.
	 */
	static void Build(URoadNetwork& Network, const FRoadSolveResult& Solved);
};
