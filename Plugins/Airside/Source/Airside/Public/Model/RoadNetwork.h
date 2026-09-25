#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Model/RoadHandles.h"
#include "Model/RoadNode.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadApron.h"
#include "Model/RoadEntity.h"
#include "Model/TrafficOccupancy.h"
#include "RoadNetwork.generated.h"

class URoadProfile;

/** FRoadNetworkSolver's per-arm result (Solve/JunctionSolver.h) - forward-declared rather
 *  than included so the many TUs that only touch the road graph, not the solver, do not
 *  gain a Solve/ include through this header. WriteSegmentEndSolve takes it by reference,
 *  and only URoadNetwork.cpp's body needs the full definition. */
struct FJunctionArmResult;

/**
 * Repository owning the road graph. All mutation goes through this type; URoadEditFacade is
 * the one caller the build tools use, and it snapshots the graph into URoadEditHistory (a
 * Memento, not the Command layer design spec 7.3 once specified - see URoadEditHistory's own
 * comment) before each edit, which is what undo replays against. This type itself enforces
 * nothing about who calls it; the facade is the boundary by convention.
 */
UCLASS(BlueprintType)
class AIRSIDE_API URoadNetwork : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Super::PostLoad() then EnsureStandOutlines() - see that function. Only override this
	 * class has: a level saved before outlines existed (or before stands could be point-
	 * placed at all) loads with live IsStand() entities carrying no Outline, and nothing
	 * else in the load path would ever give them one.
	 */
	virtual void PostLoad() override;

	FRoadNodeId AddNode(const FVector2D& Position);
	bool RemoveNode(FRoadNodeId Node);

	FRoadSegmentId AddSegment(FRoadNodeId A, FRoadNodeId B, const FVector2D& Control, URoadProfile* Profile);
	FRoadSegmentId AddStraightSegment(FRoadNodeId A, FRoadNodeId B, URoadProfile* Profile);
	bool RemoveSegment(FRoadSegmentId Segment);

	/**
	 * Replace Doomed with two straight segments meeting at a new node placed At. Unset if
	 * Doomed is not live or At is too close to either of its ends to leave a real road.
	 *
	 * Shared by URoadEditFacade::SplitSegment (the real edit) and URoadSurfacePresenter's
	 * ghost preview deliberately: two implementations of the same surgery is precisely how a
	 * preview comes to show something the click will not do, and that failure is invisible -
	 * the ghost looks plausible either way. Lives here rather than only in the facade (#104):
	 * a Model/ test (RunwayFactsTest) and the presenter's ghost preview both used to reach
	 * into Present/URoadEditFacade for a static helper that is genuinely graph surgery and
	 * touches nothing Present/ owns - Model/ was this operation's home from the start, and
	 * issue #32's split just never got around to it (see the facade's now-removed comment on
	 * the old SplitSegmentIn for why it was deferred).
	 */
	FRoadNodeId SplitSegment(FRoadSegmentId Doomed, const FVector2D& At);

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

	/**
	 * Fold Absorb into Keep: every arm of Absorb becomes an arm of Keep, and Absorb dies.
	 *
	 * THE MERGE HAS NO VERB IN THE UI. Dropping one node onto another is the whole gesture,
	 * exactly as drawing within the snap radius reuses a node rather than making a second
	 * one - see ERoadSnapKind::Node, "clicking reuses it, which is how a junction is
	 * closed". This is that same idea for a node that already exists, and it is what fixes
	 * the close pairs that made vehicles crawl: a pair a few metres apart is a corner
	 * FSpeedProfile rightly refuses to take at speed, and there was no way to remove it.
	 *
	 * THREE CASES PER ARM, and the middle one is the interesting one:
	 *   - the arm's far end IS Keep      -> it collapses; the arm is removed
	 *   - Keep already reaches that end  -> the WIDER profile survives, the other is removed
	 *   - otherwise                      -> the Absorb end is repointed to Keep
	 *
	 * WIDEST WINS by URoadProfile::GetTotalWidth, the same measure the width cycle reports.
	 * On an exact tie the arm already on Keep survives, being the edit that touches less.
	 * Ground geometry is sized for the largest aircraft admitted, so collapsing a stub must
	 * never silently narrow a route something was cleared for.
	 *
	 * CONTROL POINTS COME WITH THE ENDPOINT, shifted by half its displacement - the rule
	 * SetNodePosition above already applies, and for the reason its comment gives: direction
	 * is derived from Control, so an arm repointed without it keeps aiming at where its end
	 * used to be. Half the displacement is how far the chord's midpoint travels, which
	 * leaves a straight segment exactly straight.
	 *
	 * NO LEGALITY JUDGEMENT HERE. This is graph surgery, like RemoveNode and SplitSegment
	 * beside it; whether the resulting corners FIT is a placement question and belongs to
	 * the facade, which owns FRoadPlacementLimits - see URoadEditFacade::MergeNodes.
	 */
	bool MergeNodes(FRoadNodeId Keep, FRoadNodeId Absorb);

	/**
	 * Bumped by every node/segment mutator - AddNode, RemoveNode, AddSegment, RemoveSegment,
	 * SplitSegment, SetNodePosition, MergeNodes (#166). Scoped to the ROAD graph only, unlike
	 * GuidelineRevision below: the two callers this exists for - the ghost preview's cached
	 * GhostNetwork and URoadEditFacade's cached FRoadDeletionPlan - both derive entirely from
	 * nodes and segments, and a guideline-only edit (a holding bar placed, an edge relinked)
	 * leaves every node and segment exactly where it was, so it must not invalidate either
	 * cache. Not a UPROPERTY - a session clock, not state, same as GuidelineRevision.
	 */
	uint32 GetEditRevision() const { return EditRevision; }

	/** Which side of a two-lane road traffic keeps to. See EDriveSide. */
	EDriveSide GetDriveSide() const { return DriveSide; }

	/**
	 * Sets the airport's drive side. False, changing nothing, when it already is Side - so a
	 * caller can tell a no-op from an edit and not commit an undo step that does nothing.
	 * Guidelines are NOT re-derived here: the network is the model and the builder derives,
	 * so the caller rebuilds exactly as it does after any other edit (URoadEditFacade does).
	 */
	bool SetDriveSide(EDriveSide Side);

	/**
	 * Overwrite every node/segment/guideline/apron/entity array from Source, leaving handles
	 * (index and generation) identical to what DuplicateObject would have produced - a plain
	 * TArray assignment per field, copying the same data DuplicateObject's reflection walk
	 * copies (#166).
	 *
	 * EXISTS so a caller that needs a disposable scratch graph every frame - the ghost
	 * preview is the one that mattered - can keep ONE URoadNetwork alive for its own
	 * lifetime and refresh it here instead of allocating (and orphaning, to the next GC) a
	 * fresh UObject each time. DefaultProfile is copied too and named explicitly here: it
	 * is the one field a caller actually depends on (ProfileFor's fallback), and the exact
	 * thing a straight "copy the arrays" pass would be tempted to leave out.
	 *
	 * NOT a full UObject clone - Outer, flags and anything Blueprint-visible are untouched -
	 * so this is for a SCRATCH object already constructed for the purpose, never a
	 * replacement for DuplicateObject where the whole UObject identity matters (undo's
	 * snapshot, for one, still uses DuplicateObject deliberately).
	 */
	void CopyFrom(const URoadNetwork& Source);

	const FRoadNode*    GetNode(FRoadNodeId Node) const;
	const FRoadSegment* GetSegment(FRoadSegmentId Segment) const;

	/** The handle for a live slot index, for callers walking GetNodes() by index. Unset if dead. */
	FRoadNodeId NodeIdAt(int32 Index) const;

	/** The handle for a live slot index, for callers walking GetSegments() by index. Unset if dead. */
	FRoadSegmentId SegmentIdAt(int32 Index) const;

	/**
	 * A segment's straight-line ends, or false when the segment or either node has gone.
	 *
	 * ONE HOME, not the three this used to have - a copy each in SnapGuideChain.cpp
	 * (GuideSegmentEnds) and PlotPlaceTool.cpp (SegmentEnds), plus the collision the unity
	 * build made of the second name (#192). Both callers wanted exactly this: the model is
	 * where a fact about a segment belongs, not a tool-local helper reaching into it.
	 */
	bool SegmentEnds(FRoadSegmentId Segment, FVector2D& OutA, FVector2D& OutB) const;

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
	 * THE ONLY WAY the solver, the mesh builder or the guideline builder should ask. They previously each
	 * tested Segment->Profile themselves and each treated null as "skip" - the solver by
	 * taking zero half-widths, the builder by dropping the segment - so a null profile
	 * produced a collapsed junction AND no ribbon, from two independent decisions that
	 * happened to agree. Two readers of one fact is how they stop agreeing; this is the
	 * same rule the surface solver and GuidelineGeom already follow.
	 */
	const URoadProfile* ProfileFor(const FRoadSegment& Segment) const;

	/**
	 * The profile rule, in one place: a runway is a segment whose profile is continuous
	 * through junctions. Every runway query below asked this inline; the traffic model asks
	 * it per claim, and two spellings of one rule is how a taxiway ends up a runway.
	 */
	bool IsRunwaySegment(FRoadSegmentId Segment) const;

	// --- Runway reads: forwarders. See Model/RunwayQuery.h for what each answers and why -
	// the repository grew a second responsibility deriving these from its own graph, so the
	// logic moved beside IsRunwaySegment/ProfileFor's callers rather than living inside the
	// class it reads (issue #105 item 7). Old name and signature, so nothing calling these
	// changes.
	TArray<FRoadSegmentId> RunwayChain(FRoadSegmentId Seed) const;
	TArray<FRoadSegmentId> RunwayChainOrSeed(FRoadSegmentId Seed) const;

	/**
	 * RunwayChainOrSeed(Seed), each segment wrapped as the FTrafficResource the occupancy
	 * table reads - the chain-to-resources conversion ArrivalPlanner and RouteSearch's
	 * IsRunwayHeld each spelled out over their own loop before asking FTrafficOccupancy::
	 * IsAnyHeld the chain-held question (#103). NOT moved to RunwayQuery with the reads
	 * below (#105 item 7): it landed after that move started, and FTrafficResource is a
	 * Model/ concept the same repository already depends on either way, so there is no
	 * layering reason to move it and every reason to leave a #103/#105 merge with one less
	 * conflict to resolve by hand.
	 */
	TArray<FTrafficResource> RunwaySurfaces(FRoadSegmentId Seed) const;

	FRunwayFacts RunwayFactsFor(FRoadSegmentId Seed) const;
	bool IsGuidelineNodeOnRunway(FGuidelineNodeId Node, FRoadSegmentId Seed,
		double* OutChainHalfWidth = nullptr) const;
	/** IsGuidelineNodeOnRunway(Node, Seed), against a chain already walked - added beside
	 *  IsPointOnRunway's own Chain overload for the same caller (issue #170): an
	 *  FRunwayChainCache entry answers both without walking RunwayChain(Seed) twice. */
	bool IsGuidelineNodeOnRunway(FGuidelineNodeId Node, const TArray<FRoadSegmentId>& Chain,
		double* OutChainHalfWidth = nullptr) const;
	bool IsPointOnRunway(const FVector2D& Position, FRoadSegmentId Seed,
		double* OutChainHalfWidth = nullptr) const;
	bool IsPointOnRunway(const FVector2D& Position, const TArray<FRoadSegmentId>& Chain,
		double* OutChainHalfWidth = nullptr) const;
	bool RunwayExtentAt(const FVector2D& Near, FRunwayEnd& OutEnd) const;
	bool NearestRunwayThreshold(const FVector2D& Near, FRunwayEnd& OutEnd) const;
	TArray<FGuidelineNodeId> RunwayExitNodes(FRoadSegmentId Seed, const FVector2D& Threshold,
		const FVector2D& Direction, double MinDistance) const;

	/**
	 * Write Facts onto EVERY segment of RunwayChain(Seed). False, and nothing written,
	 * when Seed is not a live runway - a taxiway has no surface class to set.
	 *
	 * The chain rather than the one segment, because the facts are the strip's: a runway
	 * split by two exits is three segments and one runway, and a tool that classified the
	 * segment it clicked would leave the halves past each exit disagreeing with it.
	 *
	 * A MUTATOR, so it stays here rather than moving to RunwayQuery with its reads.
	 */
	bool SetRunwayFacts(FRoadSegmentId Seed, const FRunwayFacts& Facts);

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
	 * Pass false for a node somebody AUTHORED - an entity anchor, a holding position -
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

	/**
	 * Splits Edge at curve parameter T (GuidelineGeom::Split), replacing it with two edges
	 * that together trace the original curve. Both halves copy every field of the original
	 * (AllowedTraffic, StandGeometryOwner, DerivedFrom, ...) except A/B/Control, so provenance
	 * survives the split exactly as it did at each of this method's five former call sites.
	 *
	 * Within WeldTolerance of an existing endpoint, no split happens: OutNode is that
	 * endpoint and Edge is left alone, reported back as OutTail (weld to A) or OutHead (weld
	 * to B) so a caller chaining splits can keep walking the same edge. This is the "reuse
	 * the endpoint" guard every copy of this surgery used to duplicate by hand.
	 *
	 * Returns false, and leaves every output unset, if Edge does not resolve. Chain two
	 * calls through OutTail (re-deriving T for the remainder) for a three-way split - see
	 * FRoadGuidelineBuilder and FAnchorLink's two-cut sweep.
	 */
	bool SplitGuidelineEdge(FGuidelineEdgeId Edge, double T, double WeldTolerance,
		FGuidelineNodeId& OutNode, FGuidelineEdgeId& OutHead, FGuidelineEdgeId& OutTail);

	const FGuidelineNode* GetGuidelineNode(FGuidelineNodeId Node) const;
	const FGuidelineEdge* GetGuidelineEdge(FGuidelineEdgeId Edge) const;

	/**
	 * THE graph-edge call of GuidelineGeom::Sample. RouteSearch, NodeReach, GuidelineOverlay,
	 * AnchorLink, AnchorLinkFinder and StandLaneBuild each used to fetch A/B themselves and
	 * call it directly - a dozen near-identical bodies for "look up both ends, sample the
	 * curve between them."
	 *
	 * NO EXCEPTION REMAINS, since 2026-09-16. One did (PR #137 review): FAnchorLink::Join
	 * sampled a curve captured BEFORE a lane split that had already replaced its edge with two
	 * new pieces, so there was no live FGuidelineEdgeId left to look up. A stand declares its
	 * entries now, so nothing is split before that point and Join calls this like everyone
	 * else - see the comment at its own call.
	 *
	 * bFromB walks the curve from B to A instead of A to B by swapping the endpoints handed
	 * to GuidelineGeom::Sample, not by sampling then reversing the array: a quadratic Bezier
	 * evaluated backwards is the same curve (Sample(B,C,A) at t equals Sample(A,C,B) at 1-t),
	 * so swapping the ends is all the reversal a caller walking TOWARD A needs.
	 *
	 * False, Out left whatever it was, if Edge does not resolve or either end is dead.
	 */
	bool SampleGuideline(FGuidelineEdgeId Edge, TArray<FVector2D>& Out, bool bFromB = false) const;

	/**
	 * How many times SampleGuideline has actually run, for #171: RouteSearch caches each
	 * edge's length on the edge itself (FGuidelineEdge::Length) precisely so a route search
	 * never has to call this per relaxation, and a test brackets a RouteSearch::Find with this
	 * to prove the cache is being read rather than silently bypassed. Counts every call
	 * regardless of caller, the same convention as UBuildSession::MakeContextCallCountForTest -
	 * a test reads the delta across the operation it is measuring, not the raw total.
	 */
	int32 SampleGuidelineCallCountForTest() const { return SampleGuidelineCalls; }

	/**
	 * How many times FAnchorLink::Resolve has dispatched to an ILinkFinder, for #177: Gather's
	 * declared-entry loop used to run this exact search too, on every entry, purely to fill a
	 * map (FEntryReach's Contact/Distance/bReaches) that nothing downstream ever read - so every
	 * such link paid for two full scans of the guideline graph where Build's own scan was the
	 * only one anything used. A test brackets a Build with this and compares the delta against
	 * the number of pending links (from a separate Gather call), the same convention as
	 * SampleGuidelineCallCountForTest: counts every dispatch regardless of caller, so a test
	 * reads the delta across the operation it is measuring, not the raw total.
	 */
	int32 AnchorLinkFindCallCountForTest() const { return AnchorLinkFindCalls; }

	/**
	 * Bumped once per FAnchorLink::Resolve call - see AnchorLinkFindCallCountForTest. Not
	 * folded into Resolve itself because Resolve is FAnchorLink's static, not a member here;
	 * this is the one line on the Network side of that call, the same seam SampleGuideline's own
	 * counter sits on.
	 */
	void NoteAnchorLinkFind() const { ++AnchorLinkFindCalls; }

	const TArray<FGuidelineNode>& GetGuidelineNodes() const { return GuidelineNodes; }
	const TArray<FGuidelineEdge>& GetGuidelineEdges() const { return GuidelineEdges; }

	/**
	 * Bumped by every guideline mutation - node or edge added, removed or relinked - so a
	 * table derived from the graph (FNodeReachCache) can tell it is stale without walking
	 * it. Not saved: it dates a graph within one session, and a loaded graph starts at zero
	 * with no derived table alive to fool. Node POSITIONS are not covered because nothing
	 * moves a guideline node in place; the builder makes fresh ones. If that changes, the
	 * mover bumps this too.
	 */
	uint32 GetGuidelineRevision() const { return GuidelineRevision; }

	/**
	 * The capped bend widenings the last TRACING solve warned about, by a key of the bend and its
	 * figures (FRoadNetworkSolver::SolveAll). Kept so the rig course - laid once, rebuilt on every
	 * Topology edit - warns about a bend once until that bend's geometry changes, not every
	 * rebuild (re-review of 8de90a45). The solver replaces it wholesale each tracing solve, so a
	 * bend that is fixed and later capped again warns again. Transient: a session's log, not state.
	 * ENFORCED BY: Airside.Build.BendLanes.CappedWideningWarnsOncePerGeometry
	 */
	TSet<uint32>& CappedWideningsWarned() { return CappedWideningsWarnedKeys; }

	/**
	 * The handle for a live slot index, for callers walking GetGuidelineNodes() by index.
	 * Unset for a dead or out-of-range slot, so a caller cannot build a handle to a node
	 * that RoadSlot::IsValid would then reject.
	 */
	FGuidelineNodeId GuidelineNodeIdAt(int32 Index) const;

	/** The handle for a live slot index, for callers walking GetGuidelineEdges() by index. Unset if dead. */
	FGuidelineEdgeId GuidelineEdgeIdAt(int32 Index) const;

	// --- Holding-position bars ---------------------------------------------------------------

	/**
	 * Place or clear an INTERMEDIATE holding position at a guideline node. False when refused.
	 *
	 * TWO WRITES, deliberately: the kind on the node (what the overlay and, from M3, the
	 * sequencer read) and a mark keyed by the node's Origin (what survives the next rebuild,
	 * since FRoadGuidelineBuilder throws every derived node away). The mark is the SOURCE
	 * and the node the cache; keeping them in one function is what stops a position existing
	 * in only one of the two.
	 *
	 * Refuses a dead node, and a node that is a RUNWAY holding position: those are derived
	 * from the junction and are not the player's to place or clear (spec 2026-09-07).
	 */
	bool SetIntermediateHoldingPosition(FGuidelineNodeId Node, bool bSet);

	/**
	 * Flags a node as a RUNWAY holding position protecting Protects, WITHOUT a mark.
	 *
	 * ForTest because in play the builder derives these from the junction; a test that
	 * hand-builds its guidelines (every M2 traffic fixture) has no junction to derive from
	 * and needs the flag the traffic rules read. Refuses a dead node or a non-runway.
	 */
	bool SetRunwayHoldingPositionForTest(FGuidelineNodeId Node, FRoadSegmentId Protects);

	const TArray<FHoldingPositionMark>& GetHoldingPositionMarks() const { return HoldingPositionMarks; }

	/**
	 * Drop marks whose node identity or whose protected runway no longer exists.
	 *
	 * PUBLIC because FRoadGuidelineBuilder calls it immediately before re-applying the rest
	 * - but the invariant belongs to this class, not to the builder, which is why the
	 * builder asks rather than filtering the array itself. Liveness is by GENERATION, not
	 * by index: slots are recycled, and a mark left naming a recycled slot would silently
	 * move a bar onto whatever road took the index over.
	 */
	void PruneHoldingPositionMarks();

	/**
	 * The runway a hold bar at this node would protect, or unset.
	 *
	 * The node's own incident derived edges first, then each neighbour's - ONE HOP, and no
	 * further. A taxiway's end node at a runway junction is joined to the runway's own
	 * centreline nodes by TURN PATHS, which carry no DerivedFrom, so the runway edge is
	 * exactly one hop away and cannot be seen from the node itself. Two hops would let a
	 * bar be placed a whole taxiway segment back from the runway it claims to guard.
	 */
	FRoadSegmentId RunwayNearGuidelineNode(FGuidelineNodeId Node) const;

	/**
	 * Edges an agent of this class may leave Node along, honouring access AND direction.
	 *
	 * Returns edges, not neighbours, because a caller needs the edge's own width, wingspan
	 * limit and geometry to decide whether to take it.
	 */
	TArray<FGuidelineEdgeId> GetOutgoingGuidelines(FGuidelineNodeId Node, ETraversalClass Class) const;

	/**
	 * Same rule as GetOutgoingGuidelines - access AND direction - visited edge by edge with no
	 * TArray at all (#171). RouteSearch's inner loop calls this once per node EXPANSION, and a
	 * node can be expanded from several arms before the search finishes, so GetOutgoingGuidelines'
	 * fresh array on every one of those calls was an allocation the search paid for and threw
	 * away before the next relaxation. GetOutgoingGuidelines is now a thin forwarder onto this,
	 * kept for the tests that genuinely want the array - GroundTrafficRebuild moved onto this
	 * function directly at #190, once it was the last production caller still paying for one it
	 * threw away on the same match-and-break shape RouteSearch had already been fixed for - one
	 * incidence-and-direction rule, not two copies of it that could drift apart.
	 */
	void ForEachOutgoingGuideline(FGuidelineNodeId Node, ETraversalClass Class,
		TFunctionRef<void(FGuidelineEdgeId)> Visit) const;

	/**
	 * True when Node has line on it that leads OFF the service geometry it belongs to.
	 *
	 * "Does this anchor have an edge on it" used to be the same question, and stopped being
	 * it the moment stands grew SERVICE LANES: a hydrant is a waypoint ON its stand's lane,
	 * so the count is true for a stand in the middle of a field. This walks only the edges
	 * marked FGuidelineEdge::StandGeometryOwner - the stand's own lane - and reports whether
	 * the component they reach touches anything that is not one of them.
	 *
	 * A node with no service geometry at all is answered by its own first edge, so a depot's
	 * pose and a hand-drawn node both give the obvious answer without a walk.
	 */
	bool IsServiceNodeConnected(FGuidelineNodeId Node) const;

	/**
	 * True when Entity's pose has ANY line on it at all - placed, but with no road within
	 * its lead-in reach otherwise. SHALLOW, deliberately, unlike IsServiceNodeConnected's
	 * walk: a depot only needs a route search to start from somewhere, so "has an edge"
	 * is the whole question, asked the same way ChooseDepot's classifying loop and its
	 * refusal log's depot count used to ask it separately (#103).
	 */
	bool IsDepotJoined(const FEntityInstance& Entity) const;

	// --- Apron surfaces --------------------------------------------------------------
	// Polygon pavement. Deliberately NOT in the segment list: the junction solver walks
	// segments, and an apron has nothing for it to solve.

	FApronId AddApron(FApronSurface&& Apron);
	bool RemoveApron(FApronId Apron);
	const FApronSurface* GetApron(FApronId Apron) const;

	/**
	 * Move one corner of a live apron's outline. False for a dead apron or an index off the
	 * end; the outline is otherwise written as given.
	 *
	 * NO VALIDITY JUDGEMENT HERE, deliberately - this is graph surgery, like RemoveApron
	 * beside it. Whether the resulting polygon is SIMPLE is a placement question and the
	 * facade's, which refuses before calling this at all: see URoadEditFacade::MoveApronCorner
	 * and the note in SetIntermediateHoldingPosition on why a guard inside a scope that
	 * cannot roll back is the wrong place for one.
	 */
	bool SetApronCorner(FApronId Apron, int32 CornerIndex, const FVector2D& To);
	const TArray<FApronSurface>& GetAprons() const { return Aprons; }

	/** The handle for a live slot index, for callers walking GetAprons() by index. Unset if dead. */
	FApronId ApronIdAt(int32 Index) const;

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
	 *
	 * DesignWingspan is stored on the instance for the capability summary; the caller reads
	 * it from the definition for the same Model/-must-not-see-Entities/ reason as Anchors.
	 * Defaulted so the many test callers that never cared about size keep compiling.
	 *
	 * PoseRole travels the same way and for the same reason: the caller reads
	 * UEntityDefinition::PoseRole, which this layer may not. It decides which class of
	 * guideline the pose's lead-in may join - see FEntityInstance::PoseRole. Defaulted to
	 * Aircraft so every caller written before the fuel slice keeps meaning what it meant.
	 *
	 * Trucks is the third and last such capture - see FEntityInstance::Trucks for why a
	 * fourth would become a struct instead.
	 */
	FEntityInstanceId PlaceEntity(UEntityDefinition* Definition,
		TConstArrayView<FEntityAnchor> Anchors, const FVector2D& Position, double Heading,
		double DesignWingspan = 0.0, EServiceRole PoseRole = EServiceRole::Aircraft,
		int32 Trucks = 0);

	/**
	 * Place from a full description, including a drawn plot and the modules filling it.
	 *
	 * THIS OVERLOAD HOLDS THE LOGIC and the signature above forwards to it, not the other
	 * way round - the refactor contract's rule that every interface stays reachable at its
	 * old name, as a forwarder if the logic moved. Roughly thirty call sites use the old
	 * form, almost all of them tests, and churning them is not this slice's work.
	 */
	FEntityInstanceId PlaceEntity(const FEntityPlacement& Placement);

	/**
	 * Give every alive IsStand() entity with Outline.Num() < 3 the Code C box its pose
	 * implies (StandBox::BoxAt) - a stand drawn with a real letter, or one placed through
	 * PlaceEntity above since Task 6, already has 4+ points and is skipped. Returns how many
	 * it changed; logs LogAirside when that is more than zero.
	 *
	 * EXISTS FOR LEGACY SAVE DATA - a level saved before a stand's outline was given at
	 * placement at all. PostLoad calls this so "a stand has an outline" holds everywhere,
	 * which is what lets WhyStandRefused's overlap check (URoadEditFacade) test every stand
	 * by outline alone rather than special-casing an un-plotted one.
	 *
	 * ALSO SETS DesignWingspan TO IcaoCode::DesignSpanForLetter(EIcaoCode::C) WHEN IT IS 0 -
	 * an old stand had no captured wingspan at all, and is now being pinned to a real letter
	 * for the first time, so it is pinned on both axes together rather than gaining an
	 * outline that implies a letter its admission rule does not honour. NOT DesignWingspan(C)
	 * ITSELF (Ruling 5): MaxWingspanForLetter(C) is the boundary the NEXT letter's row starts
	 * at, and reads back as "D" - DesignSpanForLetter sits one uu under it, which is what
	 * makes the round trip land on C.
	 *
	 * DELIBERATELY NOT the same write PlaceEntity's own outline-giving makes (see its call
	 * site) - a freshly point-placed stand keeps DesignWingspan == 0, "unknown, admits any
	 * aircraft", which StandChoiceTest's whole module depends on unchanged. Only a stand
	 * loaded with no outline AT ALL - one that predates Task 6 - is old enough to have never
	 * had a chance to be admission-limited, and this is the one place that changes.
	 */
	int32 EnsureStandOutlines();

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

	/** The handle for a live slot index, for callers walking GetEntities() by index. Unset if dead. */
	FEntityInstanceId EntityIdAt(int32 Index) const;

	/**
	 * Index of the live entity whose PoseNode is Node, or INDEX_NONE.
	 *
	 * MEMOISED against GuidelineRevision, not a linear scan any more (issue #190): "the
	 * inspector asks once per frame for one node, and there are tens of stands" stopped being
	 * true once FClaimPass::ClaimGoalNode started calling this for every agent every
	 * substep - the inspector was never the busy caller. See PoseNodeIndex.
	 */
	int32 FindEntityIndexByPoseNode(FGuidelineNodeId Node) const;

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
	 * The first anchor id for a role, or NAME_None - what UFuelService::FuelAnchorOf actually
	 * needs every tick and GetAnchorIdsForRole above never was (issue #190): that heap-
	 * allocates a TArray<FName> BY VALUE for a caller that reads element 0 and stops, once
	 * per waiting aircraft per tick for as long as the fleet stays saturated. Same ordering
	 * rule as GetAnchorIdsForRole - only ids the instance actually resolved, in definition
	 * order - so the two agree on which id "first" means.
	 */
	FName FirstAnchorIdForRole(FEntityInstanceId Entity, EServiceRole Role) const;

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

	/**
	 * Overwrite an entity's own FEntityInstance::PoseRole. False when Entity is not live.
	 *
	 * THE SIBLING OF RefreshResolvedAnchor, and it exists for the same reason: the pose role
	 * is a placement-time SNAPSHOT of a UEntityDefinition field, and a snapshot needs a
	 * moment it gets refreshed at. An entity placed and saved before the field existed loads
	 * with the UPROPERTY default (Aircraft) and nothing else ever corrects it - harmless for
	 * every stand, and a depot stuck routing aeroplanes to its truck bay.
	 *
	 * A pure data write taking a VALUE rather than a UEntityDefinition, so Model/ still never
	 * calls into the Entities layer. UEntityDefinition::RefreshResolvedAnchors is the caller,
	 * because it is the one place both layers are known at once.
	 */
	bool SetEntityPoseRole(FEntityInstanceId Entity, EServiceRole PoseRole);

	/**
	 * Re-point an entity at Definition. False for a dead entity.
	 *
	 * A PURE POINTER WRITE, which is all Model/ may do with a UEntityDefinition (forward
	 * declared; never dereferenced here). ARoadNetworkActor::RebindStandDefinitions is the
	 * caller: a drawn D/E/F stand's definition is never saved, so every load re-points it.
	 * Anchors and trucks are NOT re-captured - a rebind lands on the definition the stand was
	 * committed from, whose anchors the saved ResolvedAnchors already are.
	 */
	bool SetEntityDefinition(FEntityInstanceId Entity, UEntityDefinition* Definition);

	// --- Narrow mutators replacing the raw *Mutable accessors (#191) -------------------
	// GetSegmentMutable, GetGuidelineEdgeMutable and GetGuidelineNodeMutable used to be
	// public, which let a caller write one field of a multi-field fact and leave the rest
	// stale: FRoadSegment's solver-only TrimA/LeftCutA/RightCutA/bSolvedA one at a time,
	// FGuidelineNode's Origin (three FGuidelineEndRef fields) or HoldingPosition without
	// its HoldingPositionFor, or an FGuidelineEdge's A/B with Incident left unfixed -
	// RelinkGuidelineEdge exists for exactly that repair, and a raw write could skip it.
	// The three accessors are private now; every caller that used to reach through them
	// writes one whole fact through one of these instead.

	/**
	 * Write Solve as Segment's A or B end - TrimA/B, the LeftCut/RightCut vertices and
	 * bSolvedA/B together, the four fields FRoadNetworkSolver::SolveNodeInto used to set
	 * one at a time through a raw FRoadSegment*. FJunctionArmResult (Solve/), not a
	 * bespoke struct: it is already exactly what SolveNodeInto holds per arm
	 * (CutDistance/LeftCut/RightCut), so this takes it directly rather than unpacking it
	 * into one bundle only to repack it into another here.
	 *
	 * False, and nothing written, for a dead segment.
	 */
	bool WriteSegmentEndSolve(FRoadSegmentId Segment, bool bEndA, const FJunctionArmResult& Solve);

	/**
	 * Clear one end's bSolvedA/B after a failed solve - see FRoadSegment::bSolvedA's own
	 * comment: TrimA/B and the cut vertices are left exactly as a previous solve wrote
	 * them, because a mesh builder reads them ONLY when bSolved says to, and zeroing them
	 * here would be a second writer of values SolveNodeInto did not just recompute.
	 *
	 * False for a dead segment.
	 */
	bool ClearSegmentEndSolve(FRoadSegmentId Segment, bool bEndA);

	/**
	 * Overwrite a guideline node's Origin - which segment end it was derived for, or which
	 * end a hand-drawn edge attached to (FGuidelineEndRef's three fields together).
	 * FRoadGuidelineBuilder is the only caller: it records this the moment it makes a
	 * node, and a raw FGuidelineNode* let it (or anything) write Segment and bEndA but
	 * leave GuidelineIndex from the slot's previous life.
	 *
	 * False for a dead node.
	 */
	bool SetGuidelineNodeOrigin(FGuidelineNodeId Node, const FGuidelineEndRef& Origin);

	/**
	 * Overwrite a guideline node's HoldingPosition/HoldingPositionFor pair - the two
	 * fields FGuidelineNode::HoldingPosition's own comment says must agree ("Runway iff
	 * HoldingPositionFor is set"). A raw FGuidelineNode* let a caller write one and not
	 * the other; this writes both or neither.
	 *
	 * UNLIKE SetIntermediateHoldingPosition, this does not touch HoldingPositionMarks -
	 * FRoadGuidelineBuilder manages marks itself (PruneHoldingPositionMarks,
	 * GetHoldingPositionMarks) while re-deriving every node's flag from scratch each
	 * rebuild, so a mark side-effect here would be a second writer of the same fact
	 * during that pass.
	 *
	 * False for a dead node.
	 */
	bool SetGuidelineNodeHoldingPosition(FGuidelineNodeId Node, EHoldingPositionKind Kind, FRoadSegmentId For);

private:
	/**
	 * The outline half of EnsureStandOutlines' rule, shared with PlaceEntity(const
	 * FEntityPlacement&) - ONE PLACE that turns a pose into a Code C box, per Task 6's own
	 * "implement it once". Writes Instance.Outline and returns true only for a live IsStand()
	 * entity whose Outline.Num() < 3; a depot, or a stand that already carries a drawn
	 * outline (any letter), is left alone and this returns false.
	 *
	 * DOES NOT TOUCH DesignWingspan - see EnsureStandOutlines' own comment on why that write
	 * stays out of the placement path. Static: it needs nothing from a live network, only the
	 * instance handed to it, so both callers can use it on an FEntityInstance& they already
	 * have without a redundant handle round trip.
	 */
	static bool GiveStandOutlineIfMissing(FEntityInstance& Instance);

	void SortIncident(FRoadNodeId Node);

	/** Kept private (#191) - SplitSegment, SetNodePosition and SetRunwayFacts above still
	 *  call this directly for fields (Control, Runway) the narrow mutators above do not
	 *  cover; everything outside this class goes through those instead. */
	FRoadSegment* GetSegmentMutable(FRoadSegmentId Segment);

	/** Kept private (#191) - no member function writes an edge's fields directly any more
	 *  (RelinkGuidelineEdge fixes A/B and Incident together, inline); only
	 *  FRoadNetworkTestAccess reaches through this now, for a field no production caller
	 *  ever writes. */
	FGuidelineEdge* GetGuidelineEdgeMutable(FGuidelineEdgeId Edge);

	/** Kept private (#191) - SetGuidelineNodeOrigin and SetGuidelineNodeHoldingPosition are
	 *  this class's own writers of it now (SetIntermediateHoldingPosition and
	 *  SetRunwayHoldingPositionForTest go through the latter); FRoadNetworkTestAccess reaches
	 *  through this directly for PriorityOverride, which none of them cover (see
	 *  FGuidelineNode::PriorityOverride's own comment). */
	FGuidelineNode* GetGuidelineNodeMutable(FGuidelineNodeId Node);

	/** Kept private, same reason as the two above: no production placement path writes an
	 *  entity's Outline after PlaceEntity yet (a stand's own drawn outline is later work in
	 *  this SDD slice); FRoadNetworkTestAccess::SetEntityOutlineForTest reaches through this
	 *  to build that case ahead of the tool that will draw one. */
	FEntityInstance* GetEntityMutable(FEntityInstanceId Entity);

	friend struct FRoadNetworkTestAccess;

	UPROPERTY() TArray<FRoadNode>    Nodes;
	UPROPERTY() TArray<int32>        NodeFreeList;
	UPROPERTY() TArray<FRoadSegment> Segments;
	UPROPERTY() TArray<int32>        SegmentFreeList;

	UPROPERTY() TArray<FGuidelineNode> GuidelineNodes;
	UPROPERTY() TArray<int32>          GuidelineNodeFreeList;
	UPROPERTY() TArray<FGuidelineEdge> GuidelineEdges;
	UPROPERTY() TArray<int32>          GuidelineEdgeFreeList;

	/** See GetGuidelineRevision. Plain, not a UPROPERTY - it is a session clock, not state. */
	uint32 GuidelineRevision = 0;

	/** See CappedWideningsWarned. */
	TSet<uint32> CappedWideningsWarnedKeys;

	/** See GetEditRevision. Plain, not a UPROPERTY - a session clock, not state. */
	uint32 EditRevision = 0;

	/**
	 * A UPROPERTY, so it saves with the airport and undo's DuplicateObject snapshot carries
	 * it. Right by default: a network saved before 2026-09-23 loads right-hand.
	 */
	UPROPERTY() EDriveSide DriveSide = EDriveSide::Right;

	/**
	 * SAVED, not transient: this is the only durable record that a bar was ever placed.
	 *
	 * The network is what the level serialises and what undo snapshots (URoadEditHistory
	 * duplicates this object), so a Transient array here would lose every bar on save and
	 * on the first Ctrl+Z.
	 */
	UPROPERTY() TArray<FHoldingPositionMark> HoldingPositionMarks;

	UPROPERTY() TArray<FApronSurface> Aprons;
	UPROPERTY() TArray<int32>         ApronFreeList;

	UPROPERTY() TArray<FEntityInstance> Entities;
	UPROPERTY() TArray<int32>           EntityFreeList;

	/**
	 * FindEntityIndexByPoseNode's index, memoised the same discipline FNodeReachCache and
	 * FRunwayChainCache use against GuidelineRevision - brought inside this class rather than
	 * a separate cache struct because there is only ever one Entities array to be stale
	 * against, so the "which network was this built for" check those two need does not apply.
	 *
	 * NOT MAINTAINED BY PlaceEntity/RemoveEntity BY HAND, which is what the issue that added
	 * this proposed: that is a second place this index could drift from Entities, exactly the
	 * kind of duplication FindEntityIndexByPoseNode used to be safe from by reading Entities
	 * directly. Every writer of PoseNode - PlaceEntity's AddGuidelineNode, RemoveEntity's
	 * RemoveGuidelineNode - already bumps GuidelineRevision, so the mismatch is free to detect
	 * and the rebuild below is the only place this map is ever written.
	 *
	 * MUTABLE: FindEntityIndexByPoseNode is const, and rebuilding this is memoisation of
	 * Entities, not a decision - the array being memoised changed a revision ago, not now.
	 */
	mutable TMap<FGuidelineNodeId, int32> PoseNodeIndex;

	/** GuidelineRevision PoseNodeIndex was built against. MAX_uint32 so the very first call
	 *  always rebuilds rather than matching a network that happens to start at revision 0. */
	mutable uint32 PoseNodeIndexRevision = MAX_uint32;

	/** See SampleGuidelineCallCountForTest. mutable for the same reason ContextBuildCountForTest
	 *  is on UBuildSession: SampleGuideline is const and this counts real work it did, not a
	 *  decision. Not a UPROPERTY - a session counter, not state. */
	mutable int32 SampleGuidelineCalls = 0;

	/** See AnchorLinkFindCallCountForTest. mutable for the same reason SampleGuidelineCalls is:
	 *  NoteAnchorLinkFind is called from a const Resolve and this counts real dispatch it did,
	 *  not a decision. Not a UPROPERTY - a session counter, not state. */
	mutable int32 AnchorLinkFindCalls = 0;
};

/**
 * Writes RoadNetworkTest.cpp, RoadGuidelineBuilderTest.cpp and GroundTrafficTest.cpp make
 * that no production caller makes: standing in for a player's manual edit of a derived
 * guideline edge, and setting a node's per-node traffic priority override (spec 5.4, on
 * FGuidelineNode::PriorityOverride) before any build tool can author it (#191).
 *
 * ONE friend struct rather than either three public ForTest methods on URoadNetwork itself
 * or leaving GetGuidelineEdgeMutable/GetGuidelineNodeMutable public for everyone - see
 * FGroundTrafficTestAccess (Model/GroundTraffic.h) for the same argument made once there:
 * a test constructs this wrapper around the network it wants to drive, and nothing else
 * can reach the fields below. Stateless and cheap to construct per call; it holds nothing
 * but the reference.
 */
struct AIRSIDE_API FRoadNetworkTestAccess
{
	explicit FRoadNetworkTestAccess(URoadNetwork& InNetwork) : Network(InNetwork) {}

	/** Simulate a player's manual edit of a derived edge - RoadGuidelineBuilderTest's
	 *  "edited edge survives regeneration" case. False for a dead edge. */
	bool MarkGuidelineEdgeEditedForTest(FGuidelineEdgeId Edge, double MaxWingspan);

	/** Write PriorityOverride directly - see FGuidelineNode's own comment. False for a
	 *  dead node. */
	bool SetGuidelineNodePriorityOverrideForTest(FGuidelineNodeId Node, TArray<ETraversalClass> PriorityOverride);

	/** Write Outline directly onto an already-placed entity - simulating a drawn stand
	 *  (Airside.Present.PlotPresenter.StandOutlineIsNotADepot) before any production path can
	 *  draw one. False for a dead entity. */
	bool SetEntityOutlineForTest(FEntityInstanceId Entity, TArray<FVector2D> Outline);

private:
	URoadNetwork& Network;
};
