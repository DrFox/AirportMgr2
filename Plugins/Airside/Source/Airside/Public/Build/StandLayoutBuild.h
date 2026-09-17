#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"
#include "Solve/IcaoCode.h"

class URoadNetwork;

/**
 * Puts each placed entity's SERVICE LAYOUT - the parking bays and the four legs of every
 * vehicle's visit - into the guideline graph.
 *
 * WHY THE LAYOUT LOOKS AS IT DOES is UEntityDefinition::ServiceBays's business; this is only
 * where it reaches the graph. What matters here is that all of it is DERIVED, so it is swept
 * and remade by the ordinary rebuild, follows the stand when the stand moves, and goes when the
 * stand goes - the same lifecycle as any other derived edge, and the reason no instance stores
 * a layout and no saved level needs migrating.
 *
 * IT TRANSFORMS; IT DOES NOT SOLVE, and that is the whole of the 2026-09-17 redesign. The lane
 * this replaces measured a shape, rounded its corners to a radius, clamped the ones that did
 * not fit and laid whatever came out - a shape decided by a disagreement between two figures,
 * which passed its tests and crabbed in PIE four times running. The template arrives already
 * solved and already proven drivable for every vehicle, and placement is a rotation and a
 * translation. A transform preserves curvature, so a verified template cannot be made
 * undrivable by being put somewhere. Everything that measured, rounded or clamped is deleted.
 *
 * RUNS BEFORE FAnchorLink, which joins each entry and exit to a road. The layout has to exist
 * before anything can link it.
 *
 * IDEMPOTENT, because the two callers cannot promise the sweep ran in between: the presenter
 * always rebuilds first, and a test may call FAnchorLink twice to check a second pass adds
 * nothing. An entity that already owns live layout edges is left alone rather than given a
 * second layout, detected from FGuidelineEdge::StandGeometryOwner - the same mark the link
 * search uses to avoid joining a layout to itself.
 */
struct AIRSIDE_API FStandLayoutBuild
{
	/**
	 * Physical width given to a lane nobody paints, uu.
	 *
	 * A number rather than a profile lookup because there is no surface here to read one from:
	 * the layout is invisible by design, and FGuidelineEdge::Width drives marking geometry and
	 * clearance, neither of which this has.
	 *
	 * IT LIVES IN IcaoCode and this forwards to it. A stand's MINIMUM width is derived from the
	 * lane - a stand has to hold one down each side, because nothing may cross under the
	 * aeroplane - and a figure that sizes the table cannot be owned by something downstream of
	 * the table.
	 */
	static double LaneWidth() { return IcaoCode::ServiceLaneWidth(); }

	/** What one pass laid, and what the link search needs to know about it. */
	struct FResult
	{
		/**
		 * Every node of every layout now in the graph.
		 *
		 * FAnchorLink excludes these as link TARGETS, exactly the way it excludes anchor
		 * nodes: a layout is itself a vehicle guideline, so without this a layout would join
		 * itself four metres away, every stand would read as connected, and no truck would
		 * ever route anywhere.
		 */
		TSet<FGuidelineNodeId> Nodes;

		/** Per entity, the edges of its layout - what the link search measures FROM. */
		TMap<FEntityInstanceId, TArray<FGuidelineEdgeId>> Layouts;

		/**
		 * Per entity, the nodes a road may join: one ENTRY per service bay and one EXIT per
		 * side the stand has bays on.
		 *
		 * WHERE A ROAD MAY JOIN, said by the asset rather than measured off the graph. The ring
		 * this descends from was searched for its nearest approach to a road and cut wherever
		 * that fell, which put entrances on bends, clustered three of them along one edge, and
		 * needed a whole side-walking apparatus to undo.
		 *
		 * ONE ENTRY PER BAY, ruled 2026-09-17: a vehicle never threads past a parked one to
		 * reach its own slot. The exits are shared per side because a vehicle leaves along its
		 * lane, so one way out per side is one junction per side rather than one per service.
		 *
		 * ONE NODE PER ENTRY, unlike the lane's two. An entry is authored on a STRAIGHT - the
		 * 45 degree run in from the back edge - so there is a node exactly at it, where the
		 * lane's entries sat on rounded corners whose own point carried no node at all.
		 *
		 * NO KEY AT ALL for a definition that declares no bay, rather than an empty array:
		 * "this stand has no entries" is a real and reportable state, and a caller's Find() is
		 * the question, so the map only answers when it has an answer.
		 */
		TMap<FEntityInstanceId, TArray<FGuidelineNodeId>> Entries;

		int32 LayoutsBuilt = 0;
	};

	static FResult Build(URoadNetwork& Network);
};
