#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Model/RoadHandles.h"
#include "Model/RoadNode.h"
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

private:
	void SortIncident(FRoadNodeId Node);

	UPROPERTY() TArray<FRoadNode>    Nodes;
	UPROPERTY() TArray<int32>        NodeFreeList;
	UPROPERTY() TArray<FRoadSegment> Segments;
	UPROPERTY() TArray<int32>        SegmentFreeList;
};
