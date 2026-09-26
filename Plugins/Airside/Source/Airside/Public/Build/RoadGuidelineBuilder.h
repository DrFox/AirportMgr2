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
	 * vehicle (FRoadDesignVehicles - per width tier since 2026-09-25) is what the corner warning
	 * below measures against; every dead-end balloon uses its Default (see FRoadDesignVehicles
	 * for the ruling). Every turn path
	 * this builder lays warns against the same figure (a right-angle corner's takeable
	 * radius), once per ordered arm pair, and used to call
	 * UAirsideSettings::ResolveLargestServiceVehicle() fresh each time. Content/ is resolved
	 * exactly once per rebuild, by URoadSurfacePresenter::Rebuild, and handed down here -
	 * which is also why this file no longer includes Content/AirsideSettings.h at all
	 * (Check-Architecture's Build->Content rule).
	 */
	static void Build(URoadNetwork& Network, const FRoadSolveResult& Solved,
		const FRoadDesignVehicles& DesignVehicles);

	/**
	 * Re-measures one piece a split has made of a turn path - MinRadius, ClearInner/Outer and
	 * their per-sample arrays (FGuidelineEdge's "PER-HALF FIELDS" comment) - against the
	 * pavement at JunctionNode, the same FJunctionPavement construction Build's own per-node
	 * loop uses, then writes the result through URoadNetwork::SetGuidelineEdgeMeasurement.
	 *
	 * A NO-OP, leaving Half exactly as URoadNetwork::SplitGuidelineEdge left it (unmeasured),
	 * when: Half is not a turn path (DerivedFrom set - a lane's Width gates it instead, and
	 * this builder never measures one); Solved has nothing valid for JunctionNode (a solve
	 * that failed there, or a caller with no solve at all - FAnchorLink::Build's Solved
	 * parameter is optional for exactly this reason, issue #324); or Half no longer resolves.
	 *
	 * WRITTEN FOR FAnchorLink::Join (issue #324, follow-up to #288), the one place a stand's
	 * lead-in splits a turn path today - a second caller is welcome, not a layering violation,
	 * since this is Build/ calling Build/. #288 left every split half unmeasured because
	 * re-measuring needs this builder's own MeasureTurn and the junction pavement polygon,
	 * neither reachable from Model/ - so Join is where the gap closes: it reads JunctionNode
	 * off FGuidelineEdge::AtJunction before it ever calls
	 * URoadNetwork::SplitGuidelineEdge, then calls this once per piece the split makes.
	 * JunctionNode IS ITS OWN PARAMETER, rather than something this reads off Half itself,
	 * because a piece cannot always carry AtJunction back out on its own ENDPOINTS the way an
	 * ordinary lane's Origin does: the two-cut sweep's middle piece and a BendArc's interior
	 * pieces have no segment-end node to ask at all (see AtJunction's own comment on
	 * FGuidelineEdge) - the field survives the split on the EDGE itself, which is what makes
	 * asking it once, on the original, before any of that, sufficient.
	 */
	static void MeasureSplitHalf(URoadNetwork& Network, FGuidelineEdgeId Half,
		const FRoadSolveResult& Solved, FRoadNodeId JunctionNode);
};
