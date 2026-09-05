#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Model/RoadHandles.h"
#include "Model/RoadNode.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadApron.h"
#include "Model/RoadEntity.h"
#include "RoadNetwork.generated.h"

class URoadProfile;

/**
 * Repository owning the road graph. All mutation goes through this type;
 * from Slice 3 onward only IRoadCommand implementations may call the mutators.
 */
UCLASS(BlueprintType)
class AIRSIDE_API URoadNetwork : public UObject
{
	GENERATED_BODY()

public:
	FRoadNodeId AddNode(const FVector2D& Position);
	bool RemoveNode(FRoadNodeId Node);

	FRoadSegmentId AddSegment(FRoadNodeId A, FRoadNodeId B, const FVector2D& Control, URoadProfile* Profile);
	FRoadSegmentId AddStraightSegment(FRoadNodeId A, FRoadNodeId B, URoadProfile* Profile);
	bool RemoveSegment(FRoadSegmentId Segment);

	/**
	 * Move a live node, keeping the incidence order the solver depends on.
	 *
	 * Moving a node changes the outgoing bearing of every segment that touches it - at BOTH
	 * ends, not just this one - and URoadNetwork's contract is that Incident stays sorted by
	 * that bearing. So this re-sorts the node and every neighbour. Writing Position directly
	 * would leave the lists out of order, and the junction solver walks them assuming they
	 * are: an arm in the wrong slot puts one road's geometry on another road's cut line.
	 *
	 * The stored cut vertices are left stale. The next solve rewrites all of them, and a
	 * partial refresh here would be a second writer of values that must have exactly one.
	 */
	bool SetNodePosition(FRoadNodeId Node, const FVector2D& To);

	const FRoadNode*    GetNode(FRoadNodeId Node) const;
	const FRoadSegment* GetSegment(FRoadSegmentId Segment) const;
	FRoadSegment*       GetSegmentMutable(FRoadSegmentId Segment);

	/** Normalised tangent at AtNode, pointing away from that node along the segment. */
	FVector2D GetOutgoingTangent(FRoadSegmentId Segment, FRoadNodeId AtNode) const;

	FRoadNodeId GetOtherEnd(FRoadSegmentId Segment, FRoadNodeId AtNode) const;

	const TArray<FRoadNode>&    GetNodes()    const { return Nodes; }
	const TArray<FRoadSegment>& GetSegments() const { return Segments; }

	/**
	 * Used by any segment that carries no profile of its own.
	 *
	 * Exists because a segment's own profile did not survive being saved: the fallback
	 * ARoadNetworkActor made on demand lived in the transient package, so every segment in
	 * a reloaded level came back with a null pointer, and a level with four roads in it
	 * rebuilt to four segments and zero triangles.
	 *
	 * A UPROPERTY, so pointing it at an authored asset makes the whole network survive a
	 * round trip. Null is still legal and still means what it meant before.
	 */
	UPROPERTY() TObjectPtr<URoadProfile> DefaultProfile;

	/**
	 * The profile that governs Segment - its own, or DefaultProfile when it has none.
	 *
	 * THE ONLY WAY either the solver or the mesh builder should ask. They previously each
	 * tested Segment->Profile themselves and each treated null as "skip" - the solver by
	 * taking zero half-widths, the builder by dropping the segment - so a null profile
	 * produced a collapsed junction AND no ribbon, from two independent decisions that
	 * happened to agree. Two readers of one fact is how they stop agreeing; this is the
	 * same rule the surface solver and GuidelineGeom already follow.
	 */
	const URoadProfile* ProfileFor(const FRoadSegment& Segment) const;

	/**
	 * If Near sits on a runway, reports the departure from the threshold nearest it.
	 *
	 * WALKS THE WHOLE RUNWAY, not the one segment it lands on. Adding an exit splits a runway,
	 * so by the time it is useful it is several segments - and a roll computed from one piece
	 * would refuse a departure the strip can easily take. The walk follows nodes joining
	 * exactly two continuous segments, which is what an uninterrupted runway looks like from
	 * the graph's point of view.
	 *
	 * A runway is recognised by its PROFILE - see URoadProfile::bContinuousThroughJunctions -
	 * so nothing here needs a runway type or a flag on the segment.
	 *
	 * Direction points from the near threshold toward the far one: the way you depart having
	 * backtracked to that end. False when Near is not on a runway at all.
	 */
	bool RunwayExtentAt(const FVector2D& Near, FVector2D& OutThreshold, FVector2D& OutDirection,
		double& OutLength) const;

	/**
	 * The runway threshold nearest a point, however far away it is.
	 *
	 * THE SAME SEARCH AS RunwayExtentAt WITHOUT THE PROXIMITY TEST, which is exactly the
	 * difference between the two questions. A departure asks "did my taxi end on a runway",
	 * and must hear no everywhere else - that test exists because without it every route
	 * armed a departure at the only runway on the field. An arrival asks "which runway am I
	 * landing on", of a click that is deliberately nowhere near one.
	 *
	 * The threshold returned is the end NEAREST the query and the direction runs away from
	 * it, so an aircraft lands toward the far end - the same convention as a departure, and
	 * the reason both can share the walk.
	 */
	bool NearestRunwayThreshold(const FVector2D& Near, FVector2D& OutThreshold,
		FVector2D& OutDirection, double& OutLength) const;

	/**
	 * Guideline nodes lying on a runway, ordered by distance from its threshold.
	 *
	 * THE EXITS, without needing an exit to be a thing. A runway is continuous through
	 * junctions, so a taxiway joining it already puts a guideline node on the centreline;
	 * asking which nodes lie along the strip therefore finds every way off it, including
	 * ones the player drew after the runway existed.
	 *
	 * MinDistance is what makes the answer useful to an arrival: an exit before the aircraft
	 * can possibly have slowed down is not an exit it can take.
	 */
	TArray<FGuidelineNodeId> RunwayExitNodes(const FVector2D& Threshold,
		const FVector2D& Direction, double Length, double HalfWidth, double MinDistance) const;

	// --- Guideline graph -------------------------------------------------------------
	// A SECOND graph, deliberately in the same object. The build tool must make "draw a
	// taxiway" one atomic undo step spanning pavement and its derived guideline, and
	// Revert must restore handles identically including generation counters; splitting
	// the two graphs across two UObjects makes every composite command a two-phase commit
	// for no gain. Conceptual separation lives in the headers, not in the ownership.

	/**
	 * A guideline node.
	 *
	 * bDerived defaults true, which is right for everything FRoadGuidelineBuilder creates.
	 * Pass false for a node somebody AUTHORED - an entity anchor, a hold-short position -
	 * because the builder's orphan sweep removes idle DERIVED nodes, and an authored node
	 * is idle from the moment it is placed until an edge is drawn to it.
	 */
	FGuidelineNodeId AddGuidelineNode(const FVector2D& Position, bool bDerived = true);

	/** Removes the node AND every edge incident to it. */
	bool RemoveGuidelineNode(FGuidelineNodeId Node);

	/** A and B must both be live, or this returns an unset handle and adds nothing. */
	FGuidelineEdgeId AddGuidelineEdge(FGuidelineEdge&& Edge);
	bool RemoveGuidelineEdge(FGuidelineEdgeId Edge);

	/**
	 * Move an existing edge onto different endpoints, fixing incidence at all four nodes.
	 *
	 * Exists for re-resolution after a rebuild: a hand-authored edge outlives the nodes it
	 * was drawn between, and must be re-pointed at the freshly derived ones rather than
	 * deleted and re-added, which would change its handle and lose the player's edit.
	 */
	bool RelinkGuidelineEdge(FGuidelineEdgeId Edge, FGuidelineNodeId NewA, FGuidelineNodeId NewB);

	const FGuidelineNode* GetGuidelineNode(FGuidelineNodeId Node) const;
	const FGuidelineEdge* GetGuidelineEdge(FGuidelineEdgeId Edge) const;
	FGuidelineEdge*       GetGuidelineEdgeMutable(FGuidelineEdgeId Edge);

	/**
	 * Mutable access to a guideline node.
	 *
	 * The counterpart to GetGuidelineEdgeMutable. Needed because HoldShortFor and
	 * PriorityOverride live on the NODE, and until the build tool can author them there is
	 * otherwise no way for anything - including a test - to write either.
	 */
	FGuidelineNode* GetGuidelineNodeMutable(FGuidelineNodeId Node);

	const TArray<FGuidelineNode>& GetGuidelineNodes() const { return GuidelineNodes; }
	const TArray<FGuidelineEdge>& GetGuidelineEdges() const { return GuidelineEdges; }

	/**
	 * Edges an agent of this class may leave Node along, honouring access AND direction.
	 *
	 * Returns edges, not neighbours, because a caller needs the edge's own width, wingspan
	 * limit and geometry to decide whether to take it.
	 */
	TArray<FGuidelineEdgeId> GetOutgoingGuidelines(FGuidelineNodeId Node, ETraversalClass Class) const;

	// --- Apron surfaces --------------------------------------------------------------
	// Polygon pavement. Deliberately NOT in the segment list: the junction solver walks
	// segments, and an apron has nothing for it to solve.

	FApronId AddApron(FApronSurface&& Apron);
	bool RemoveApron(FApronId Apron);
	const FApronSurface* GetApron(FApronId Apron) const;
	const TArray<FApronSurface>& GetAprons() const { return Aprons; }

	// --- Entities --------------------------------------------------------------------

	/**
	 * Place an entity and resolve every one of Anchors to a guideline node at its world
	 * pose. Returns an unset handle for a null definition.
	 *
	 * Definition is stored on the instance but never dereferenced here - Model/ must not
	 * depend on the Entities layer (RoadEntity.h says so at the top), so the caller resolves
	 * the definition's own Anchors array and hands it in rather than this function reading
	 * Definition->Anchors itself. HasUsableAnchorIds' validation moves with it: the caller
	 * (URoadEditFacade::PlaceStand) checks it before calling, since that check is also a
	 * UEntityDefinition method this layer cannot call.
	 */
	FEntityInstanceId PlaceEntity(UEntityDefinition* Definition,
		TConstArrayView<FEntityAnchor> Anchors, const FVector2D& Position, double Heading);

	/**
	 * Removes the entity, the anchor nodes it owns, and every guideline edge incident to
	 * them - RemoveGuidelineNode cascades. So deleting a stand also deletes the taxi line
	 * drawn into it, which is intended (a lead-in to a deleted stand leads nowhere) but is
	 * destructive and returns nothing describing what went with it. A build tool should
	 * confirm before calling this.
	 */
	bool RemoveEntity(FEntityInstanceId Entity);

	const FEntityInstance* GetEntity(FEntityInstanceId Entity) const;
	const TArray<FEntityInstance>& GetEntities() const { return Entities; }

	/**
	 * World heading of an entity's anchor in radians: the instance's heading composed with
	 * the anchor's own.
	 *
	 * FGuidelineNode carries no heading, so the resolved node cannot answer this and spec
	 * section 6's stop-position marking would have nowhere to learn which way an aircraft
	 * parks. Composed on demand rather than stored on the instance, so it cannot drift from
	 * the instance's own pose - added to FResolvedAnchor::LocalHeading, captured at
	 * placement (see PlaceEntity) rather than read live from the definition, because Model/
	 * cannot call back into UEntityDefinition to do that read.
	 *
	 * Returns false and leaves OutHeading untouched when the entity or the id is unknown.
	 *
	 * NOTE the sum is not wrapped: 7*PI/4 + PI/2 gives 9*PI/4, not PI/4. Every consumer
	 * feeds it to trigonometry, where it makes no difference. A caller comparing two
	 * headings for equality must wrap first.
	 */
	bool GetAnchorWorldHeading(FEntityInstanceId Entity, FName AnchorId, double& OutHeading) const;

	/**
	 * The guideline node an entity's named anchor resolved to, or null.
	 *
	 * By ID, never by index. An instance placed before its definition gained an anchor
	 * simply has no entry for that id and this returns null - a correct answer to "where
	 * does the new cart park on this old stand", where indexing read out of bounds.
	 */
	const FGuidelineNode* GetAnchorNode(FEntityInstanceId Entity, FName AnchorId) const;

	/** The resolved anchor for an id, or null. For callers needing the handle itself. */
	const FResolvedAnchor* FindResolvedAnchor(FEntityInstanceId Entity, FName AnchorId) const;

	/**
	 * Ids of an entity's anchors serving a role, in definition order.
	 *
	 * Role is a CATEGORY, not an identity - a stand has two belt loaders - so this answers
	 * "where can baggage be worked" and the caller picks. Only ids the instance actually
	 * resolved are returned, by construction: this reads FResolvedAnchor::Role, captured at
	 * placement, rather than filtering the definition's own anchors and checking each one
	 * against ResolvedAnchors - so a definition edited after placement cannot hand back an
	 * id that leads nowhere.
	 */
	TArray<FName> GetAnchorIdsForRole(FEntityInstanceId Entity, EServiceRole Role) const;

	/**
	 * Overwrite one resolved anchor's LocalHeading and Role in place. False when Entity or
	 * AnchorId does not resolve to anything.
	 *
	 * A pure data write, taking values rather than a UEntityDefinition, so Model/ still
	 * never calls into the Entities layer - see PlaceEntity's comment. This exists for
	 * UEntityDefinition::RefreshResolvedAnchors (Entities layer) to correct
	 * FResolvedAnchor's snapshot: an instance placed and saved before LocalHeading and Role
	 * existed on this struct loads with LocalHeading == 0.0 and Role == Aircraft (the
	 * UPROPERTY defaults), and nothing else ever writes the real values into it. Not
	 * exposed as a general setter - the caller resolves what the right values ARE by
	 * reading a UEntityDefinition, which is exactly the thing this layer must not do.
	 */
	bool RefreshResolvedAnchor(FEntityInstanceId Entity, FName AnchorId, double LocalHeading, EServiceRole Role);

private:
	void SortIncident(FRoadNodeId Node);

	UPROPERTY() TArray<FRoadNode>    Nodes;
	UPROPERTY() TArray<int32>        NodeFreeList;
	UPROPERTY() TArray<FRoadSegment> Segments;
	UPROPERTY() TArray<int32>        SegmentFreeList;

	/** RunwayExtentAt and NearestRunwayThreshold, which differ only in the proximity test. */
	bool RunwayExtentInternal(const FVector2D& Near, bool bRequireOnRunway,
		FVector2D& OutThreshold, FVector2D& OutDirection, double& OutLength) const;

	UPROPERTY() TArray<FGuidelineNode> GuidelineNodes;
	UPROPERTY() TArray<int32>          GuidelineNodeFreeList;
	UPROPERTY() TArray<FGuidelineEdge> GuidelineEdges;
	UPROPERTY() TArray<int32>          GuidelineEdgeFreeList;

	UPROPERTY() TArray<FApronSurface> Aprons;
	UPROPERTY() TArray<int32>         ApronFreeList;

	UPROPERTY() TArray<FEntityInstance> Entities;
	UPROPERTY() TArray<int32>           EntityFreeList;
};
