#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"
#include "Model/RoadTraffic.h"
#include "RoadGuideline.generated.h"

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
struct AIRSIDE_API FGuidelineEndRef
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

/**
 * The kinds of holding position, from the ground markings (spec 2026-09-07).
 *
 * A RUNWAY-holding position is infrastructure: two solid and two dashed lines across every
 * taxiway that meets a runway, derived by FRoadGuidelineBuilder at the taxiway's end. An
 * INTERMEDIATE one is a single dashed line at a taxiway junction where ATC may hold traffic;
 * the player places it. "Hold short" is the instruction given AT one of these, not a kind
 * of marking - it belongs to M3's sequencer, and nothing here means "hold" by itself.
 */
UENUM()
enum class EHoldingPositionKind : uint8
{
	None,
	Runway,
	Intermediate,
};

/**
 * A point on the guideline graph where something happens.
 *
 * Nodes exist at junctions, crossings, holding positions and entity anchors - NOT at
 * a fixed interval. Spec 3: a node every N metres has nothing to say to anybody, and the
 * parent spec's R9 subdivision was justified by a pathing benefit that moved to this
 * graph when the two graphs were separated.
 */
USTRUCT()
struct AIRSIDE_API FGuidelineNode
{
	GENERATED_BODY()

	UPROPERTY() FVector2D Position = FVector2D::ZeroVector;

	/** Maintained by URoadNetwork. Never edit from outside it. */
	UPROPERTY() TArray<FGuidelineEdgeId> Incident;

	/**
	 * What kind of holding position this node is, if any. Spec 2026-09-07. One enum, not
	 * two flags: Runway iff HoldingPositionFor is set, and URoadNetwork keeps it so.
	 */
	UPROPERTY() EHoldingPositionKind HoldingPosition = EHoldingPositionKind::None;

	/** The runway a Runway holding position protects (any segment of its chain). Unset otherwise. */
	UPROPERTY() FRoadSegmentId HoldingPositionFor;

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
struct AIRSIDE_API FGuidelineEdge
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
	 * The entity whose SERVICE LOOP or anchor spur this edge is. Unset for everything else,
	 * which is almost every edge.
	 *
	 * Provenance, exactly as DerivedFrom is for a road's own guidelines, and needed for the
	 * same reason turned inside out: a stand's lane is ITSELF a vehicle guideline, so a link
	 * search that did not know which edges were the searcher's own would join a lane to
	 * itself four metres away, report every stand connected, and route no truck anywhere.
	 *
	 * The LINK from a lane to a road deliberately does NOT carry this. It is a lead-in like
	 * any other, and leaving it unowned is exactly what lets
	 * URoadNetwork::IsServiceNodeConnected tell a lane that reaches a road from one that
	 * only ever reaches itself.
	 */
	UPROPERTY() FEntityInstanceId ServiceLoopOwner;

	/**
	 * True for a SPUR - the stub from a service anchor to the ring - and false for a side of
	 * the ring itself. Meaningless unless ServiceLoopOwner is set.
	 *
	 * STATED, not inferred. It was read off the endpoints - "a spur touches an anchor node" -
	 * which is true of a spur nobody has split and false the moment a second anchor spurs
	 * onto the first, leaving an inner piece with an anchor node at neither end. That piece
	 * then read as a ring side, took a link of its own, and the truck drove from the road
	 * across the aeroplane to reach it.
	 *
	 * It has to live on the EDGE rather than in the builder's result, because the result is
	 * re-gathered from the graph on every later pass - see FServiceLoopBuild::Build's opening
	 * block - and a pass that did not lay the lane has nothing else to tell the two apart.
	 */
	UPROPERTY() bool bServiceSpur = false;

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

/**
 * A player-placed hold bar, stored by IDENTITY so it survives the rebuild. Spec §6.
 *
 * The flag itself lives on FGuidelineNode::HoldingPositionFor, and every derived node is thrown
 * away and re-made by FRoadGuidelineBuilder on each road edit - so the flag alone is a
 * CACHE, and this is the source it is rebuilt from. At is the same key the builder's own
 * Ends map uses, which is what lets the mark be resolved through the map the builder
 * already computes rather than by hunting for a coincident node.
 */
USTRUCT()
struct AIRSIDE_API FHoldingPositionMark
{
	GENERATED_BODY()

	/**
	 * Which derived node: the same key the builder's Ends map uses.
	 *
	 * INTERMEDIATE positions only, since 2026-09-07. Runway-holding positions are derived
	 * on every build from the junction itself and need no source of truth beyond the graph.
	 */
	UPROPERTY() FGuidelineEndRef At;
};
