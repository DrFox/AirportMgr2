#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"
#include "Model/RoadTraffic.h"
#include "RoadGuideline.generated.h"

/**
 * A point on the guideline graph where something happens.
 *
 * Nodes exist at junctions, crossings, hold-short positions and entity anchors - NOT at
 * a fixed interval. Spec 3: a node every N metres has nothing to say to anybody, and the
 * parent spec's R9 subdivision was justified by a pathing benefit that moved to this
 * graph when the two graphs were separated.
 */
USTRUCT()
struct ROADNET_API FGuidelineNode
{
	GENERATED_BODY()

	UPROPERTY() FVector2D Position = FVector2D::ZeroVector;

	/** Maintained by URoadNetwork. Never edit from outside it. */
	UPROPERTY() TArray<FGuidelineEdgeId> Incident;

	/** Set when this node requires clearance; names the surface it protects. Spec 5.5. */
	UPROPERTY() FRoadSegmentId HoldShortFor;

	/**
	 * Overrides the default class priority at this node. Empty - the overwhelmingly
	 * common case - means TraversalPriority applies. Spec 5.4.
	 */
	UPROPERTY() TArray<ETraversalClass> PriorityOverride;

	UPROPERTY() int32 Generation = 0;
	UPROPERTY() bool  bAlive = false;
};

/**
 * A line an agent is told to follow.
 *
 * NOTE, because the surface model next door does the opposite: this graph has NO bitwise
 * weld contract. Endpoints are shared by HANDLE, not by coincident position, so guideline
 * geometry may be recomputed freely and no seam can open. Do not import the surface
 * model's ==-on-position discipline here.
 */
USTRUCT()
struct ROADNET_API FGuidelineEdge
{
	GENERATED_BODY()

	UPROPERTY() FGuidelineNodeId A;
	UPROPERTY() FGuidelineNodeId B;

	/** Quadratic Bezier control point, as FRoadSegment. Equals the midpoint when straight. */
	UPROPERTY() FVector2D Control = FVector2D::ZeroVector;

	/** Who may use this. Defaults to nobody - see FTrafficMask. */
	UPROPERTY() FTrafficMask AllowedTraffic;

	UPROPERTY() EGuidelineDir Direction = EGuidelineDir::Bidirectional;

	/**
	 * Physical extent of the path in uu, driving marking geometry and clearance.
	 *
	 * NOT a capacity. Abreast concurrency is structural - two lanes are two guidelines -
	 * and flow-versus-single-file is a property of the traversal class, not of the edge.
	 * A four-metre service road could hold two vans abreast and never does. Spec 5.3.
	 */
	UPROPERTY() double Width = 0.0;

	/** 0 means unlimited. Spec 5.6. */
	UPROPERTY() double MaxWingspan = 0.0;

	/** The surface this was derived from; unset when hand-drawn. */
	UPROPERTY() FRoadSegmentId DerivedFrom;

	/**
	 * True while this edge is still owned by its surface and may be regenerated.
	 *
	 * Flips to false on first manual edit and never flips back on its own. Regeneration
	 * must then leave it alone, because regenerating it would silently discard the edit.
	 */
	UPROPERTY() bool bDerived = true;

	UPROPERTY() int32 Generation = 0;
	UPROPERTY() bool  bAlive = false;
};
