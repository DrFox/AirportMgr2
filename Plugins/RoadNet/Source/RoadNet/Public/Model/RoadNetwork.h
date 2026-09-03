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
class ROADNET_API URoadNetwork : public UObject
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
	 * Place an entity and resolve every anchor its definition declares to a guideline node
	 * at that anchor's world pose. Returns an unset handle for a null definition.
	 */
	FEntityInstanceId PlaceEntity(UEntityDefinition* Definition, const FVector2D& Position, double Heading);

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
	 * parks. Composed on demand rather than stored, so it cannot drift from the instance's
	 * pose - and read from the DEFINITION by id, so editing an anchor's heading in the
	 * asset is picked up by instances already placed.
	 *
	 * Returns false and leaves OutHeading untouched when the entity, the definition or the
	 * id is unknown.
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
	 * resolved are returned, so a definition edited after placement cannot hand back an id
	 * that leads nowhere.
	 */
	TArray<FName> GetAnchorIdsForRole(FEntityInstanceId Entity, EServiceRole Role) const;

private:
	void SortIncident(FRoadNodeId Node);

	UPROPERTY() TArray<FRoadNode>    Nodes;
	UPROPERTY() TArray<int32>        NodeFreeList;
	UPROPERTY() TArray<FRoadSegment> Segments;
	UPROPERTY() TArray<int32>        SegmentFreeList;

	UPROPERTY() TArray<FGuidelineNode> GuidelineNodes;
	UPROPERTY() TArray<int32>          GuidelineNodeFreeList;
	UPROPERTY() TArray<FGuidelineEdge> GuidelineEdges;
	UPROPERTY() TArray<int32>          GuidelineEdgeFreeList;

	UPROPERTY() TArray<FApronSurface> Aprons;
	UPROPERTY() TArray<int32>         ApronFreeList;

	UPROPERTY() TArray<FEntityInstance> Entities;
	UPROPERTY() TArray<int32>           EntityFreeList;
};
