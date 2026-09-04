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
/**
 * WHICH derived endpoint a node is, so it can be found again after regeneration.
 *
 * The guideline graph is rebuilt wholesale on every road edit, and AddGuidelineNode never
 * deduplicates - it allocates a new slot every time. A handle held across a rebuild
 * therefore points at a node the new derivation no longer uses, while a fresh coincident
 * node takes its place. Anything hand-authored must say WHAT its endpoint is, not WHERE it
 * currently lives.
 *
 * These three fields are exactly FRoadGuidelineBuilder's own EndKey, which is what lets a
 * player edge be re-resolved through the same map the builder already computes. Same move
 * as FResolvedAnchor: carry the id you resolved FROM.
 */
USTRUCT()
struct ROADNET_API FGuidelineEndRef
{
	GENERATED_BODY()

	UPROPERTY() FRoadSegmentId Segment;

	/** Which end of that segment - A, or B. */
	UPROPERTY() bool bEndA = true;

	UPROPERTY() int32 GuidelineIndex = 0;

	bool IsSet() const { return Segment.IsSet(); }

	bool operator==(const FGuidelineEndRef& Other) const
	{
		return Segment == Other.Segment && bEndA == Other.bEndA
			&& GuidelineIndex == Other.GuidelineIndex;
	}
};

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

	/**
	 * True while this node is owned by the derivation and may be swept when it falls idle.
	 *
	 * Edges carry provenance and nodes did not, so the orphan sweep used to remove ANY
	 * idle node - including one the build tool had placed but not yet drawn an edge to,
	 * which is the natural authoring order.
	 */
	UPROPERTY() bool bDerived = true;

	/**
	 * Which segment end this node was derived FOR, or unset.
	 *
	 * Carried so a hand-drawn edge can record what it attached to rather than merely which
	 * slot happened to hold it. Unset on anchor and pose nodes: those are non-derived, are
	 * never swept, and keep their handles across every rebuild already.
	 */
	UPROPERTY() FGuidelineEndRef Origin;

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
	 * Which of the source profile's declared guidelines this came from. INDEX_NONE for a
	 * turn path or a hand-drawn edge.
	 *
	 * Without it, DerivedFrom alone cannot tell a regeneration that a slot is already
	 * covered by an edge the player has edited - so it derives over the top and the
	 * airport ends up with two guidelines where the player drew one.
	 */
	UPROPERTY() int32 DerivedGuidelineIndex = INDEX_NONE;

	/**
	 * True while this edge is still owned by its surface and may be regenerated.
	 *
	 * Flips to false on first manual edit and never flips back on its own. Regeneration
	 * must then leave it alone, because regenerating it would silently discard the edit.
	 */
	UPROPERTY() bool bDerived = true;

	/**
	 * For a HAND-AUTHORED edge, what its two ends are - not where they currently sit.
	 *
	 * Filled from the clicked nodes' Origin, and re-resolved after every derivation. Unset
	 * for a derived edge (which is regenerated anyway) and for a link between two anchor
	 * nodes (whose handles are already stable), and an unset ref means "leave this end
	 * alone" rather than "detach it".
	 */
	UPROPERTY() FGuidelineEndRef EndRefA;
	UPROPERTY() FGuidelineEndRef EndRefB;

	UPROPERTY() int32 Generation = 0;
	UPROPERTY() bool  bAlive = false;
};
