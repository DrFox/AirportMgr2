#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Model/RoadHandles.h"
#include "Model/RoadNode.h"
#include "Model/RoadGuideline.h"
#include "RoadNetwork.generated.h"

class URoadProfile;

/**
 * Repository owning the road graph. All mutation goes through this type;
 * from Slice 3 onward only IRoadCommand implementations may call the mutators.
 */
UCLASS()
class ROADNET_API URoadNetwork : public UObject
{
	GENERATED_BODY()

public:
	FRoadNodeId AddNode(const FVector2D& Position);
	bool RemoveNode(FRoadNodeId Node);

	FRoadSegmentId AddSegment(FRoadNodeId A, FRoadNodeId B, const FVector2D& Control, URoadProfile* Profile);
	FRoadSegmentId AddStraightSegment(FRoadNodeId A, FRoadNodeId B, URoadProfile* Profile);
	bool RemoveSegment(FRoadSegmentId Segment);

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

	FGuidelineNodeId AddGuidelineNode(const FVector2D& Position);

	/** Removes the node AND every edge incident to it. */
	bool RemoveGuidelineNode(FGuidelineNodeId Node);

	/** A and B must both be live, or this returns an unset handle and adds nothing. */
	FGuidelineEdgeId AddGuidelineEdge(FGuidelineEdge&& Edge);
	bool RemoveGuidelineEdge(FGuidelineEdgeId Edge);

	const FGuidelineNode* GetGuidelineNode(FGuidelineNodeId Node) const;
	const FGuidelineEdge* GetGuidelineEdge(FGuidelineEdgeId Edge) const;
	FGuidelineEdge*       GetGuidelineEdgeMutable(FGuidelineEdgeId Edge);

	const TArray<FGuidelineNode>& GetGuidelineNodes() const { return GuidelineNodes; }
	const TArray<FGuidelineEdge>& GetGuidelineEdges() const { return GuidelineEdges; }

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
};
