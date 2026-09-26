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

	/**
	 * What this edge offers a VEHICLE'S BODY, measured by FRoadGuidelineBuilder on the same
	 * samples the follower walks (spec 2026-09-23 §6), so route search can gate a vehicle
	 * without re-deriving any geometry:
	 *   MinRadius  - the tightest radius the curve delivers; 0 for a straight edge.
	 *   ClearInner / ClearOuter - distance from the line to the edge of the PAVEMENT, toward
	 *                and away from the curve's centre, the minimum over the samples.
	 * -1 means UNMEASURED and gates nothing: straight lanes (their Width gates them), dead-end
	 * balloons (over grass, ruled), and anything hand-drawn or saved before this existed.
	 */
	UPROPERTY() double MinRadius = 0.0;
	UPROPERTY() double ClearInner = -1.0;
	UPROPERTY() double ClearOuter = -1.0;

	/**
	 * The same clearances PER SAMPLE, one entry per point GuidelineGeom::Sample gives this
	 * curve (2026-09-24). VehicleFit simulates the vehicle along those samples
	 * (VehicleSweep::Trace) and asks, at each one, whether the body's reach either side fits
	 * the tarmac there - which a single minimum cannot answer, because a trailer cuts in late
	 * and a turn is widest in the middle. Empty means unmeasured (as -1 above).
	 *
	 * PER-HALF FIELDS, with MinRadius/ClearInner/ClearOuter above and EndRefA/EndRefB below:
	 * URoadNetwork::SplitGuidelineEdge's list of what does NOT survive a split unchanged,
	 * because each describes the curve (or an end of it) rather than either whole piece:
	 *   MinRadius, ClearInner, ClearOuter, ClearInnerAt, ClearOuterAt - measured over the
	 *     ORIGINAL curve's samples; a re-sampled half has the same sample COUNT (a fixed
	 *     subdivision) but different geometry, so a naive copy passes VehicleFit's
	 *     Path.Num()-vs-Num() guard while judging the half against the whole curve's numbers
	 *     (#288). Reset to the unmeasured defaults above BY THIS FUNCTION, not re-measured
	 *     here: that needs FRoadGuidelineBuilder::MeasureTurn and the junction pavement
	 *     polygon the original numbers were marched against, neither reachable from Model/.
	 *     RE-MEASURED A MOMENT LATER, when the split is a turn path (#324, follow-up to
	 *     #288): FAnchorLink::Join is the one caller that splits a turn path (a stand's
	 *     lead-in landing on one), reads AtJunction below straight off the un-split edge
	 *     (surviving the split unchanged, unlike the fields above - see AtJunction's own
	 *     comment) and calls FRoadGuidelineBuilder::MeasureSplitHalf on each resulting piece
	 *     right after, through URoadNetwork::SetGuidelineEdgeMeasurement - so the unmeasured
	 *     state this function leaves is transient within that one call rather than surviving
	 *     to the next VehicleFit::Judge. A split OUTSIDE FAnchorLink::Join (a test, or a lane
	 *     split, which has no AtJunction to re-measure against) still leaves the halves
	 *     unmeasured exactly as before - this function itself never re-measures.
	 *   EndRefA / EndRefB - only the end that MOVED (onto the new split node) is cleared; the
	 *     end that did not move keeps referring to what it always did.
	 * ENFORCED BY: Airside.Model.GuidelineGraph's split-clearances case (this function leaves
	 * both halves unmeasured); Airside.Build.AnchorLinkReMeasuresSplitTurnPath (FAnchorLink::
	 * Join's re-measure puts real numbers back before VehicleFit::Judge ever sees the split).
	 */
	UPROPERTY() TArray<float> ClearInnerAt;
	UPROPERTY() TArray<float> ClearOuterAt;

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
	 * DerivedFrom's COUNTERPART FOR A TURN PATH (issue #324): the road-graph junction node
	 * this piece was laid at. Unset for a lane (DerivedFrom names it instead) and for a
	 * dead-end balloon (laid over grass by ruling - TryBuildDeadEndBalloon never sets this,
	 * because there is no pavement polygon to re-measure a split piece of one against).
	 *
	 * SET ONCE, ON THE SHARED Turn TEMPLATE, before FRoadGuidelineBuilder::Build splits it into
	 * however many pieces its ETurnShape needs (Chord: one; TaperS: two; BendArc: several) -
	 * every piece is a copy of that template, so EVERY piece carries it, including a BendArc's
	 * INTERIOR pieces, whose own endpoints have no FGuidelineNode::Origin at all (that node is
	 * a pure geometry waypoint the arc's own construction added, not a segment end). Origin
	 * lookup was tried first and rejected for exactly that reason - it answers "which segment
	 * end" a node was derived for, and a BendArc's interior split has no such end to ask.
	 *
	 * READ BY FAnchorLink::Join to re-measure a split piece (MeasureSplitHalf's own comment) -
	 * needs no per-half handling in SplitGuidelineEdge unlike MinRadius/Clear* above: it names
	 * WHICH JUNCTION, true of both halves alike, not a fact about the curve that trimming
	 * invalidates.
	 */
	UPROPERTY() FRoadNodeId AtJunction;

	/**
	 * True while this edge is still owned by its surface and may be regenerated.
	 *
	 * Flips to false on first manual edit and never flips back on its own. Regeneration
	 * must then leave it alone, because regenerating it would silently discard the edit.
	 */
	UPROPERTY() bool bDerived = true;

	/**
	 * The entity whose SERVICE LANE this edge is part of. Unset for everything else, which is
	 * almost every edge.
	 *
	 * Provenance, exactly as DerivedFrom is for a road's own guidelines, and needed for the
	 * same reason turned inside out: a stand's lane is ITSELF a vehicle guideline, so a link
	 * search that did not know which edges were the searcher's own would join a lane to
	 * itself four metres away, report every stand connected, and route no truck anywhere.
	 *
	 * THE MARK IS WHAT ANSWERS "DOES THIS HYDRANT REACH A ROAD".
	 * URoadNetwork::IsServiceNodeConnected walks the component this mark spans and reports
	 * whether it touches anything that is NOT marked; FuelService.cpp:131 and :472 are its
	 * callers. Delete the mark and every stand standing alone in an empty field reads as
	 * connected, every fuel job is accepted, and no truck arrives.
	 *
	 * The LINK from a lane to a road deliberately does NOT carry this. It is a lead-in like
	 * any other, and leaving it unowned is exactly what makes that walk work.
	 */
	UPROPERTY() FEntityInstanceId StandGeometryOwner;

	/**
	 * True on the edges of a service bay's REVERSE leg - the back-out the vehicle makes once it
	 * has finished at a service point.
	 *
	 * IT SAYS WHICH LIMIT JUDGES THE EDGE, and that is the whole of it. A reversing vehicle
	 * pivots about its FIXED axle, so it holds L/tan(lock) where forward driving needs
	 * L/sin(lock) - 361 uu against 510 for the 6.2 m bowser, about 30% tighter. A
	 * reverse leg is therefore LEGITIMATELY tighter than the forward limit, and a test that
	 * swept every laid edge past FSpeedProfile's forward rule would refuse the one manoeuvre
	 * the layout was designed around.
	 *
	 * A FLAG ON THE EDGE rather than a lookup through StandGeometryOwner back to the bay,
	 * because the consumer is a walk over edges that has an FGuidelineEdge and no idea which
	 * of a stand's four legs it came from.
	 */
	UPROPERTY() bool bReverseLeg = false;


	// bStandApproach IS DELETED, 2026-09-16. It marked an edge that APPROACHED a stand's lane
	// rather than being part of the cycle - the SPUR from a service anchor into the old ring -
	// and was meaningless without StandGeometryOwner beside it, since it said which of an
	// OWNED edge's two kinds this was.
	//
	// NOTHING CAN SET IT ANY MORE, which is why it goes rather than waiting for a writer. An
	// anchor is a waypoint ON the lane now, so there is no stub; and the road link a declared
	// entry casts deliberately carries no owner at all - see the paragraph above, where being
	// unowned is what lets IsServiceNodeConnected tell a lane that reaches a road from one that
	// only reaches itself. A UPROPERTY every reader gets false from is a false statement in the
	// data model, and the next session would spend an hour looking for the code that was
	// supposed to set it.
	//
	// WHAT IT PROTECTED IS NOT LOST, and that was the argument for keeping it: it was read off
	// the ENDPOINTS once - "an approach touches an anchor node" - which was true of a spur
	// nobody had split and false the moment a second anchor joined the first, leaving an inner
	// piece with an anchor node at neither end; that piece read as part of the cycle, took a
	// link of its own, and a truck drove from the road across the aeroplane to reach it. The
	// same question is answered by StandGeometryOwner now, which IS written and load-bearing:
	// everything the lane builder lays carries it and nothing else does.

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

	/**
	 * Sampled length of Control's curve between A and B, in uu. Written by
	 * URoadNetwork::AddGuidelineEdge, RelinkGuidelineEdge and SplitGuidelineEdge - the only
	 * three places this edge's endpoints or Control can change - from GuidelineGeom::Length,
	 * which samples exactly as SampleGuideline itself does (#171).
	 *
	 * A CACHE, not a second measurement: "the guideline graph samples ONCE" (CLAUDE.md) means
	 * one evaluator produces the number the search costs, the overlay draws and the follower
	 * walks, not that the number is recomputed by hand at every one of those call sites. Before
	 * this existed, RouteSearch's EdgeCost re-sampled and re-measured this same curve on EVERY
	 * relaxation of every edge - a node can be relaxed several times before Closed catches it -
	 * so a route search paid for the same polyline again and again for geometry that cannot
	 * have changed since the edge was last written.
	 */
	UPROPERTY() double Length = 0.0;

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
