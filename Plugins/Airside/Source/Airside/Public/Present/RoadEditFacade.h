#pragma once

#include "CoreMinimal.h"
#include "Model/BuildPurse.h"
#include "Tool/BuildSession.h"
#include "Tool/RoadEditTarget.h"
#include "RoadEditFacade.generated.h"

class ARoadNetworkActor;
class URoadNetwork;
class URoadEditHistory;
class UGroundTraffic;
class FRoadEditScope;
class IBuildPurse;

/**
 * Every graph edit, undo, and query the build tools drive - split out of ARoadNetworkActor
 * by issue #32.
 *
 * Pattern: Facade, same as the actor was before this split (see Tool/RoadEditTarget.h for
 * why the interface it implements exists at all) - this class is simply where the facade's
 * BODY now lives. A UObject because it holds no state of its own that must be saved or
 * GC-traced (Network and History stay on the actor - see below), but Undo/Redo hand back
 * TObjectPtr<URoadNetwork> that only a UObject's reflection keeps safe to return by pointer -
 * a plain C++ class hanging on to one across a frame would be invisible to the collector.
 * Held Transient by the actor for the same reason: it carries no fields of its own that a
 * save would ever need to persist. (It does hold a GC-traced field now, EditorRollbackPoint -
 * a Transient scratch copy an editor-world drag restores from, issue #437 - which is why that
 * field is a UPROPERTY here rather than the strong pointer a plain scope uses: a UObject is
 * destroyed by the collector, and a strong pointer's release does not belong there.)
 *
 * THIS FACADE IS A SUBOBJECT OF ARoadNetworkActor AND NEVER EXISTS WITHOUT ONE - it is not
 * usable standalone, and that is deliberate rather than an oversight. DOES NOT OWN Network
 * OR History: both stay UPROPERTYs on ARoadNetworkActor because they are what the level
 * actually saves, and moving them here would put the saved graph behind a subobject the
 * .umap has never heard of - a bigger, riskier change than this pure refactor is meant to
 * make (see the task's ruling on this point). Existing tool tests still target the actor,
 * which forwards here, rather than constructing this class with NewObject and no owner.
 *
 * Instead this class reaches its owner through the private Actor() accessor
 * (GetTypedOuter<ARoadNetworkActor>(), checked) - Outer is already the actor, because the
 * actor creates this facade with CreateDefaultSubobject, so a second stored pointer would
 * only be a second thing that could disagree with the first. The same path answers
 * GetWorld() (UObject's default walks GetOuter()->GetWorld()), which is what HistoryForEdit
 * needs to tell an editor world from a game one without being handed a world explicitly.
 *
 * PlacementLimits, MinimumRunwayLength and StandDefinition are read the same way: they are
 * level-authored tunables on the actor (RoadBuildController writes PlacementLimits on the
 * actor directly, every frame), not facts this facade owns, so it asks for them rather than
 * caching them.
 *
 * OnChanged replaces the direct RebuildMesh() calls the mutators used to make. The actor owns
 * the presenter that does the rebuilding, and this class must not reach for it - so where the
 * old code rebuilt inline, this one broadcasts instead, and the actor's own RebuildMesh()
 * (bound to OnChanged in the constructor) does the reaching.
 *
 * NotifyChanged is the ONLY place that calls OnChanged.Broadcast() (issue #77). Every
 * scope-committing mutator reaches it through one of two doors: CommitAndNotify, which pairs
 * an FRoadEditScope::Commit() with the notification so neither can happen without the other,
 * or ApplyInteractiveMutation (issue #299) for the three mutators that join a drag already in
 * progress instead of owning a plain scope - see that method's own comment for why a drag
 * needs a door CommitAndNotify cannot be, and MoveNode/MoveApronCorner/MergeNodes's own call
 * sites for how little of each is left once the shared shape moved out. Undo, Redo and
 * ClearNetwork call NotifyChanged directly instead (the first two through AdoptNetwork, see
 * its own comment), because none of them fits either shape: Undo/Redo/ClearNetwork replace
 * Network wholesale rather than mutating through a scope, and ClearNetwork's own scope only
 * records the pre-clear snapshot - the notify has to wait until AFTER Owner.Network is
 * replaced with the fresh one.
 * SetIntermediateHoldingPosition now goes through CommitAndNotify too (issue #179) - it used
 * to commit WITHOUT notifying, on the reasoning that a holding position changes neither
 * pavement nor mesh, which stopped being true once FHoldingPositionMarkingBuilder started
 * painting the flag as a dashed bar in URoadSurfacePresenter::RebuildMarkings. The bare
 * Commit() was the same split-brain issue #77 closed, reopened by a comment nobody updated
 * when the paint layer shipped. UNLIKE every other CommitAndNotify call site, it passes
 * EChangeKind::Markings explicitly rather than taking the default: see that enum's own
 * comment for why Topology - the default, and every other scope-committing mutator's kind -
 * is actively wrong here, not merely more expensive than needed.
 *
 * MoveNode's AND MoveApronCorner's PER-FRAME NOTIFY IS EChangeKind::Geometry, BUT ONLY WHILE
 * AN INTERACTIVE EDIT IS OPEN (issue #165, tightened by review follow-up, #190, and folded
 * into ApplyInteractiveMutation by #299) - see that method's own comment for the full
 * bare-call-trap mechanics this class comment used to carry twice, byte-identical, once per
 * mutator. In short: every earlier drag frame ran the full pipeline, guideline graph and
 * anchor links and plots and traffic included, at frame rate; nothing about a slid position
 * needs any of those rebuilt until the drag actually stops moving nodes around, so
 * EndInteractiveEdit(bKeep=true) fires one EChangeKind::Topology notify of its own once the
 * drag commits, and that single notify is what catches the derived graph up. MergeNodes
 * shares the same helper but NOT this split - see ApplyInteractiveMutation's bChangesGraphShape
 * for why a merge always notifies Topology, drag or no drag.
 *
 * ConnectGuidelines and DisconnectGuideline go through CommitAndNotify too, same as every
 * other scope-committing mutator above (issue #125). They used to open an FRoadEditScope and
 * fall off the end without calling Commit() on it, so a successful link or unlink pushed no
 * undo step and notified nobody - the scope's destructor took the AbandonEdit() branch on a
 * change that had actually happened. Pre-existing, not introduced by issue #77.
 *
 * Before issue #77, half the mutators broadcast and half relied on the TOOL calling
 * RebuildMesh() after a successful edit - a split-brain that let one call site (a REFUSED
 * ConnectNodes in FRoadChainingState::OnClick) rebuild for nothing while real edits elsewhere
 * occasionally got no rebuild at all if a caller forgot.
 *
 * REBUILD BATCHES (BeginRebuildBatch/EndRebuildBatch, driven by FRoadRebuildBatch in
 * Tool/RoadEditTarget.h). Pattern: deferred notification with coalescing - the "suspend
 * layout" shape - applied at NotifyChanged, the one door above, so no mutator changes and none
 * can opt out. While RebuildBatchDepth > 0, NotifyChanged RECORDS its kind
 * (CombineChangeKinds into PendingBatchKind) instead of broadcasting; closing the OUTERMOST
 * batch broadcasts once, with the combined kind, back through NotifyChanged - so OnChanged
 * still has one call site. Nesting is counted; an inner close does nothing but decrement. A
 * batch that recorded nothing (every mutation refused) broadcasts nothing, the same as those
 * refusals would have unbatched. Why it exists: FRigCourseLayout::Lay's ~35 PlaceNode/
 * ConnectNodes each ran the full Topology pipeline, ~1.7 s a course, and 14 tests lay one.
 * How it meets the rest of this class:
 *
 *   - UNDO SCOPES ARE UNTOUCHED. Each mutator in a batch still opens and commits its own
 *     FRoadEditScope, so N mutations are N undo steps. A batch defers REBUILDS, not
 *     history: the Memento snapshots the model, never derived state, so no snapshot differs.
 *     Grouping a bulk edit into one undo step is a different feature (a composite edit on
 *     URoadEditHistory), deliberately not smuggled in here.
 *   - INTERACTIVE EDITS CANNOT BEGIN INSIDE A BATCH. A batch is synchronous - one call on
 *     one frame - and a drag spans frames, repainting Geometry every one of them;
 *     BeginInteractiveEdit inside a batch would freeze the pavement under the cursor until
 *     some later close. So it logs an Error and REFUSES (bInteractiveEditOpen stays false,
 *     EndInteractiveEdit then no-ops, and any MoveNode in between is a bare call that
 *     notifies Topology into the batch - see ApplyInteractiveMutation's bare-call trap).
 *     THE OTHER NESTING IS LEGAL: a batch opened while a drag is already open folds that
 *     frame's notifies and closes before the frame ends; the drag's own EndInteractiveEdit
 *     catch-up is untouched, because bGeometryChangedDuringEdit is still set where it was.
 *   - MergeNodes' ALWAYS-Topology RULE HOLDS: it is the KIND a merge records, and
 *     CombineChangeKinds never weakens a Topology.
 *   - RollBackOpenEdit / AdoptNetwork / Undo / Redo / ClearNetwork IN A BATCH: the network is
 *     swapped (RollBackOpenEdit: restored in place) at once (the model is never deferred), the
 *     notify is folded like any other,
 *     and the close rebuilds from whatever Network is THEN - derived state is a function of
 *     the current network alone, never of the path to it.
 *   - DERIVED STATE IS STALE UNTIL THE CLOSE. ConnectGuidelines, DisconnectGuideline and
 *     SetIntermediateHoldingPosition index the GUIDELINE graph, and FindRoute searches it:
 *     in a batch that has deferred a rebuild they would act on a graph that no longer
 *     matches the model, so each logs a Warning via WarnIfDerivedStale (behaviour otherwise
 *     unchanged - a bulk edit that needs them closes its batch first). SetDriveSide's lane
 *     census log line reads the stale graph the same way.
 *   - RebuildMesh() IS NOT DEFERRED. It reaches the actor's RebuildMeshForChange directly,
 *     not through NotifyChanged, and stays an explicit "rebuild now" - the escape hatch for a
 *     caller that must read derived state mid-batch, at the cost of one rebuild. The
 *     pending kind survives it, so the close still rebuilds.
 *   - EVERY OnChanged LISTENER SEES THE FOLDED NOTIFY, not only the actor:
 *     ARoadBuildController's runway cache is invalidated once, at the close, which is
 *     correct because nothing can read it between two lines of one synchronous call.
 */
UCLASS()
class AIRSIDE_API URoadEditFacade : public UObject, public IRoadEditTarget
{
	GENERATED_BODY()

public:
	/**
	 * See IRoadEditTarget::ResolveProfileFor. THE WIDTH RULE ITSELF (issue #298, moved back from
	 * ARoadNetworkActor, where it sat from 2026-09-20 until the review that became this issue
	 * found it): a CHOSEN WIDTH WINS OVER THE DEFAULT. WidthIndex names one of the content set's
	 * standard widths FOR THIS KIND (the tool cycles it on key-again); INDEX_NONE means
	 * "whatever this kind defaults to". The default for a taxiway is the actor's OWN profile
	 * (Actor().ResolveProfile()), which keeps the content set out of it on purpose, so a player
	 * who never touches the cycle lays exactly the road this level was tuned for. A service
	 * road's is Actor().ResolveServiceRoadProfile().
	 *
	 * A ROAD'S STANDARD WIDTHS (Actor().ResolveWidthProfile) ARE STILL A CONTENT QUESTION, not
	 * this facade's - this composes the RULE (which of three content/level answers a click
	 * gets), not the content lookups themselves, the same division MakeTunables draws between
	 * composing a bundle and resolving what goes in it.
	 *
	 * NOT CONST - see IRoadEditTarget::ResolveProfileFor's own comment; ResolveProfile lazily
	 * fills RuntimeProfile on the actor, a decision that header records deliberately.
	 * ENFORCED BY: Airside.Present.RoadWidthResolution, Airside.Tool.TaxiwayWidth (section 4).
	 */
	virtual URoadProfile* ResolveProfileFor(ERoadKind Kind, int32 WidthIndex) override;

	/**
	 * Fired wherever this class's mutators used to call ARoadNetworkActor::RebuildMesh().
	 *
	 * CARRIES EChangeKind (issue #165), so the listener can skip the derived-graph passes
	 * (guidelines, anchor links, plots, traffic) on a Geometry-only notify - see that enum's
	 * own comment for the Geometry/Topology split, and NotifyChanged below for which mutators
	 * pass which.
	 */
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnNetworkChanged, EChangeKind);
	FOnNetworkChanged OnChanged;

	/**
	 * Fired when the live network is REPLACED WHOLESALE by a change no tool asked for - Undo, Redo, ClearNetwork and a
	 * save-game load (RestoreInPlace) - so a driver deactivates its tool and retires its caches ONCE, in one listener,
	 * rather than beside each of those calls (issue #426). See ENetworkReplace for the two phases and which doors fire
	 * which. OnChanged still fires for all four, and still carries the rebuild - this is the extra fact OnChanged
	 * cannot say: that every slot index a listener holds now names something else.
	 *
	 * NOT FROM RollBackOpenEdit (EndInteractiveEdit's cannot-afford branch, ApplyInteractiveMutation's Verify failure,
	 * #437): a rollback restores the very graph the open edit began on, IN PLACE, so the dragging tool's indices still
	 * hold - and it reaches here from INSIDE that tool's own call, where deactivating the tool would end the drag
	 * re-entrantly.
	 *
	 * BEFORE THIS, each door had its own hand-paired response in ARoadBuildController (OnUndo, OnRedo, OnClearNetwork),
	 * a load had none, and an undo from anywhere else - the settings dialog's Revert - had none either.
	 * ENFORCED BY: Airside.Present.ReplacementIsAnnounced, AirportMgr.Actions.LoadRetiresTheToolAndCaches,
	 * AirportMgr.Actions.UndoFromAnywhereRetiresTheTool
	 */
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnNetworkReplaced, ENetworkReplace);
	FOnNetworkReplaced OnReplaced;

	/**
	 * Fired when a mutator REFUSES A BUILD AT COMMIT for a reason no tool showed first - today only
	 * "cannot afford" (EBuildRefusal). The quote says what, and the purse can price it. The layer that
	 * owns the purse turns this into something the player sees; Airside itself has no UI to say it on.
	 *
	 * NATIVE, NOT DYNAMIC: FBuildQuote is a plain struct (see its own comment) and nothing in Blueprint
	 * binds here directly.
	 * ENFORCED BY: Airside.Present.BuildPurseRefusalIsAnnounced, Check-Architecture rule 32
	 */
	DECLARE_MULTICAST_DELEGATE_TwoParams(FOnBuildRefused, const FBuildQuote& /*Quote*/, EBuildRefusal /*Why*/);
	FOnBuildRefused OnRefused;

	// --- IRoadEditTarget ---------------------------------------------------------------

	virtual const URoadNetwork* GetNetwork() const override;

	virtual int32 PlaceNode(FVector2D Where) override;
	virtual bool ConnectNodes(int32 FromIndex, int32 ToIndex, ERoadKind Kind, int32 WidthIndex,
		EPavement Surface) override;

	/** Forwarded to the actor, which owns the content lookup - see IRoadEditTarget. */
	virtual int32 GetWidthCount(ERoadKind Kind) const override;
	virtual URoadProfile* ResolveWidthProfile(ERoadKind Kind, int32 Index) const override;
	using IRoadEditTarget::ConnectNodes;
	virtual int32 ConnectGuidelines(int32 FromNodeIndex, int32 ToNodeIndex) override;
	virtual bool AddReverseTurn(int32 NodeIndex, int32 FromFarIndex, int32 IntoFarIndex) override;
	virtual bool PlaceRunway(FVector2D From, FVector2D To, URoadProfile* RunwayProfile, const FRunwayFacts& Facts) override;
	using IRoadEditTarget::PlaceRunway;
	virtual bool SetRunwayFacts(int32 SegmentIndex, const FRunwayFacts& Facts) override;
	/** See IRoadEditTarget::UpgradeSegment - SetRunwayFacts' pattern, priced like ConnectNodes. */
	virtual bool UpgradeSegment(int32 SegmentIndex, ERoadKind Kind, int32 WidthIndex, EPavement Surface) override;
	virtual FString WhyUpgradeRefused(int32 SegmentIndex, ERoadKind Kind, int32 WidthIndex, EPavement Surface) const override;
	virtual FString WhyUpgradeSiteRefused(int32 SegmentIndex, ERoadKind Kind, int32 WidthIndex, EPavement Surface) const override;
	virtual FString WhyUpgradeUnaffordable(int32 SegmentIndex, ERoadKind Kind, int32 WidthIndex, EPavement Surface) const override;
	virtual double GetMinimumRunwayLength() const override;
	virtual int32 GetRunwayProfileCount() const override;
	virtual URoadProfile* ResolveRunwayProfile(int32 Index) const override;
	virtual bool DisconnectGuideline(int32 EdgeIndex) override;
	virtual bool SetIntermediateHoldingPosition(int32 NodeIndex, bool bSet) override;

	/**
	 * The airport's drive side, as one undoable edit that re-derives every lane (spec
	 * 2026-09-23 §2). False, pushing no undo step, when it already is Side. NOT on
	 * IRoadEditTarget: no tool sets it - the bar does, through the actor.
	 */
	bool SetDriveSide(EDriveSide Side);

	/**
	 * A module bought for a depot (facility-upgrades spec §3): the network write, a Topology rebuild so
	 * AAirsideBuildingsActor relights the slot, then the undo history CLEARED (R8). Undo is a whole-network
	 * Memento; an undo past this would drop the shed and keep the money, so a purchase is a checkpoint.
	 * NOT ON IRoadEditTarget: no tool buys - the ops runtime's purchase hook does (named loosely:
	 * Airside may not reference AirportOps, Check-Architecture's cross-plugin rule). NOT PRICED here: money is
	 * UFacilityPurchases', posted after this returns true. False, nothing changed, for a non-depot.
	 * ENFORCED BY: Airside.Present.Facility.ModulePurchaseRelightsAndClearsUndo
	 */
	bool AddEntityModule(FEntityInstanceId Entity, EDepotModule Module);

	/**
	 * Remove up to Count of Module from a depot whose plot cannot seat them - the repair's write (#266, owner 2026-09-30:
	 * "there must never be unplaced modules"), and its only one. Returns how many went; 0, nothing changed, for a
	 * non-depot or while a drag is open (AddEntityModule's reason: the history clear below would strand the drag).
	 *
	 * A REPAIR, NOT A PLAYER'S EDIT, so it pushes NO undo step - undoing it would put back a module nothing can stand, which
	 * is the state being repaired. And it CLEARS the history, AddEntityModule's reason from the other side: undo is a
	 * whole-network Memento, and restoring a snapshot from before this would bring the unplaced modules back and keep the
	 * refund, which the repair would then pay a second time. A Topology rebuild, so the presenter's drop count and the
	 * yard agree at once.
	 *
	 * NOT ON IRoadEditTarget (no tool removes a module) and NOT PRICED here: the refund is UFacilityPurchases', posted
	 * after this returns - the ops runtime's removal hook is the one production caller.
	 * ENFORCED BY: Check-Architecture rule 4 row 'module removal', Airside.Present.Facility.UnseatedRemovalRelightsAndLeavesNoUndo,
	 * AirportOps.Present.Facility.RepairRemovesAndRefundsUnseated
	 */
	int32 RemoveUnseatedModules(FEntityInstanceId Entity, EDepotModule Module, int32 Count);
	virtual int32 SplitSegment(int32 SegmentIndex, FVector2D At) override;
	virtual bool DeleteNode(int32 NodeIndex) override;
	virtual bool DeleteSegment(int32 SegmentIndex) override;
	virtual bool MoveNode(int32 NodeIndex, FVector2D To) override;
	virtual bool MergeNodes(int32 KeepIndex, int32 AbsorbIndex) override;
	virtual bool MoveApronCorner(int32 ApronIndex, int32 CornerIndex, FVector2D To) override;
	virtual void BeginInteractiveEdit(const FString& Label) override;
	virtual void EndInteractiveEdit(bool bKeep) override;

	/** See the class comment's REBUILD BATCHES. Use FRoadRebuildBatch rather than these. */
	virtual void BeginRebuildBatch() override;
	virtual void EndRebuildBatch() override;

	/** True between the outermost BeginRebuildBatch and its matching End. */
	bool IsRebuildBatchOpen() const { return RebuildBatchDepth > 0; }

	/**
	 * CACHED on (NodeIndex, Network's EditRevision) (#166). RoadHeal::PlanNodeDeletion
	 * duplicates the whole graph and validates every candidate rejoin against the copy -
	 * real work, and correctly so (see its own header for why the simulation needs a whole
	 * graph) - but FRemoveGesture asks this every frame Ctrl hovers a node, for an answer
	 * that cannot have changed unless the graph did. Recomputed only when NodeIndex differs
	 * from the last call or EditRevision has moved; invalidation is free, because any edit
	 * that would change the answer - including the deletion this same hover is about to
	 * commit - bumps EditRevision itself (URoadNetwork's mutators do; see its own header),
	 * so there is no separate "invalidate the plan cache" call site to forget.
	 */
	virtual FRoadDeletionPlan PlanNodeDeletion(int32 NodeIndex) const override;

	virtual int32 AddApron(const TArray<FVector2D>& Outline) override;
	virtual bool DeleteApron(int32 ApronIndex) override;
	virtual int32 FindApronAt(FVector2D Where) const override;

	virtual int32 PlaceEntity(FVector2D Where, double Heading, EPlaceableEntity Kind) override;
	virtual int32 PlaceEntityInPlot(const TArray<FVector2D>& Outline,
		FVector2D FrontageA, FVector2D FrontageB,
		const TArray<EDepotModule>& Modules, EPlaceableEntity Kind) override;
	using IRoadEditTarget::PlaceStand;
	virtual int32 PlaceStandInPlot(const TArray<FVector2D>& Outline,
		FVector2D EntranceA, FVector2D EntranceB, EPavement Pavement) override;
	virtual FString WhyStandRefused(TArrayView<const FVector2D> Outline, EPavement Pavement) const override;
	virtual FString WhyStandSiteRefused(TArrayView<const FVector2D> Outline) const override;
	virtual FString WhyStandUnaffordable(TArrayView<const FVector2D> Outline, EPavement Pavement) const override;
	virtual uint32 GetEditEpoch() const override { return EditEpoch; }

	/**
	 * See IRoadEditTarget::WhySegmentRefused. Builds the straight shape a click lays (ConnectNodes
	 * is straight only), at the half-width of the profile THIS Kind and WidthIndex resolve to -
	 * not PlacementLimits.NewRoadHalfWidth, the taxiway default whatever is laid - and asks
	 * TaxiwayStrip::JudgeSegment.
	 */
	virtual FString WhySegmentRefused(int32 FromIndex, const FRoadSnapResult& To, ERoadKind Kind, int32 WidthIndex) const override;

	/** See IRoadEditTarget::WhyPlotRefused. PlaceEntityInPlot's own outline refusals, moved here
	 *  whole (same order, same wording), plus the clearance strip in the stand's words. */
	virtual FString WhyPlotRefused(TArrayView<const FVector2D> Outline) const override;

	/**
	 * The one quote a drawn stand is priced at: BuildCost::ForEntity(Definition) plus the pad
	 * it sits on (QuoteForApron(Outline, Pavement)), combined into one "{0} + {1}" What text -
	 * the same shape QuoteForApron's own callers in PlaceEntityInPlot already sum by hand,
	 * pulled out here because WhyStandRefused's afford gate and PlaceStandInPlot's charge both
	 * need EXACTLY this figure and had drifted into two slightly different copies of it (fix
	 * round 1 on this task's own review).
	 *
	 * PUBLIC since shared-pavement Task 8 (was private): a query with no side effect, and the
	 * afford test (Airside.Present.StandPlot.AffordsWithTheChosenPavement) must open a purse
	 * at exactly this figure - a second pricing of the pad in the test is the drift this
	 * function exists to remove.
	 */
	FBuildQuote QuoteStand(const UEntityDefinition& Definition,
		TArrayView<const FVector2D> Outline, EPavement Pavement) const;
	virtual bool DeleteEntity(int32 EntityIndex) override;
	virtual int32 FindEntityAt(FVector2D Where, double Radius) const override;
	virtual const UEntityDefinition* GetEntityDefinition(EPlaceableEntity Kind) const override;
	using IRoadEditTarget::GetStandDefinition;

	/** See IRoadEditTarget::ResolveDepotKits. Forwards to the actor, like ResolveProfileFor
	 *  above - the content lookup itself stays on ARoadNetworkActor with every other Resolve*. */
	virtual TArray<PlotYard::FKitSpec> ResolveDepotKits() const override;

	/**
	 * UpdateGhost, HideGhost, RebuildMesh and DispatchAgent are IRoadEditTarget virtuals
	 * whose real work happens on URoadSurfacePresenter or UAirsideTraffic - neither of which
	 * this facade has a pointer to, by design (it must not reach past Network/History for
	 * anything else - see the class comment). They are still implemented here, because
	 * IRoadEditTarget is a base this class cannot leave abstract, but each one simply asks
	 * the owning actor to do its own job: ARoadNetworkActor's own overrides of these four are
	 * the real forwarders, to the presenter and the traffic object respectively, and this
	 * just reaches them the same way every other query here reaches Network - through Actor().
	 * Nothing calls IRoadEditTarget through a facade-typed pointer today; this exists so
	 * nothing would silently do the wrong thing if that ever changed.
	 */
	virtual void UpdateGhost(int32 FromNodeIndex, const FRoadSnapResult& Snap, bool bValid,
		ERoadKind Kind, int32 WidthIndex) override;
	using IRoadEditTarget::UpdateGhost;
	virtual void HideGhost() override;
	virtual void RebuildMesh() override;
	using IRoadEditTarget::DispatchAgent;
	virtual bool DispatchAgent(const FRoutePlan& Plan, const FAirframe& Airframe,
		ETraversalClass Class) override;
	virtual bool DispatchAgent(const FRoutePlan& Plan, const FVehicle& Vehicle,
		ETraversalClass Class) override;

	virtual bool MakeLiveNodeId(int32 Index, FRoadNodeId& OutId) const override;

	virtual FRoutePlan FindRoute(FGuidelineNodeId Start, FGuidelineNodeId Goal,
		ETraversalClass Class, double Wingspan,
		ERouteErrand Errand = ERouteErrand::PlayerIssued) const override;

	// --- Facade-only members (not part of IRoadEditTarget) ------------------------------

	/** Slot indices of the segments that deleting NodeIndex would take with it. */
	TArray<int32> SegmentsIncidentTo(int32 NodeIndex) const;

	/** Both endpoints of a live segment, on the road plane. False if it is not live. */
	bool GetSegmentEnds(int32 SegmentIndex, FVector2D& OutA, FVector2D& OutB) const;

	/** Index of the nearest live node within Radius of Where, or INDEX_NONE. */
	int32 FindNodeNear(FVector2D Where, double Radius) const;

	/** Discard the whole graph and the mesh built from it. Undoable. */
	void ClearNetwork();

	/**
	 * THE DOOR FOR A SAVE-GAME LOAD (issue #426). Deserialise writes the saved airport INTO the live network - a load
	 * restores in place, so everything holding the network object keeps holding it - and this does everything around
	 * that which a replacement owes, in the one order:
	 *
	 *   OnReplaced(Discarding) - the tool abandons against the graph its indices still name;
	 *   Deserialise(live network);
	 *   ARoadNetworkActor::RepairLoadedNetwork - the repairs a level load gets, which a save game does not;
	 *   ClearHistory - the undo stack holds Mementos of the airport just replaced on purpose;
	 *   AdoptNetwork(the same object) - ghost hidden, OnChanged(Topology), so the mesh, the guideline graph and every
	 *     OnChanged listener (the controller's HasRunway cache) see the loaded airport;
	 *   OnReplaced(Adopted) - the drivers retire their caches.
	 *
	 * BEFORE THIS the load was AirportOps' LoadFromSlot's own hand-ordered sequence - two of the four repairs, a
	 * RebuildMesh that bypassed OnChanged, and no announcement - the fifth spelling of "adopt a network" (#299 closed
	 * four). A CALLBACK, not a Begin/End pair a caller could leave half-open: the OpsSave::Restore that Deserialise
	 * wraps lives in AirportOps, which Airside may not include.
	 *
	 * A FAILED Deserialise skips the repairs and the history clear - nothing was loaded to repair, and the player keeps
	 * their undo - but still rebuilds and still announces Adopted: the tool was already put down, and a listener that
	 * heard Discarding must always hear its end. Returns what Deserialise returned.
	 * ENFORCED BY: Airside.Present.ReplacementIsAnnounced, AirportOps.Present.RuntimeLoad.RunsEveryLoadRepair
	 */
	bool RestoreInPlace(TFunctionRef<bool(URoadNetwork&)> Deserialise);

	/**
	 * Snap and placement tunables, for a driver-supplied view scale, as one bundle - see
	 * FBuildSessionTunables. Moved off ARoadNetworkActor by issue #298: it COMPOSES
	 * PlacementLimits/Snap/GuideSources (level-authored tunables this facade already reads
	 * through Actor(), the same pattern as MinimumRunwayLength/StandDefinition - see the class
	 * comment) with ResolveProfile - a resolve this class is the right place for, not a
	 * level-authored UPROPERTY only an AActor could hold. THE ONE PLACE both drivers assemble
	 * this now: before issue #93, ARoadBuildController filled Tunables.Snap/Limits from its own
	 * seven UPROPERTYs every tick, and URoadBuildEditorTool built a DIFFERENT set from a
	 * view-derived radius, leaving Limits at struct defaults entirely - the same click was
	 * judged by different rules depending on which driver was open.
	 *
	 * ViewWorldWidth > 0 asks for an adaptive ToolPickRadius sized off it (what the editor
	 * tool needs, having no view-distance UPROPERTY of its own to read); 0 leaves
	 * ToolPickRadius at its class default for a caller - the runtime driver - that overwrites
	 * it right after with its own ToolPickRadius view fact. Not const: resolving the
	 * corner-fit half-width goes through Actor().ResolveProfile(), which is deliberately
	 * non-const - see that method's own comment.
	 */
	FBuildSessionTunables MakeTunables(double ViewWorldWidth);

	/**
	 * How many times PlanNodeDeletion has actually run RoadHeal::PlanNodeDeletion (a cache
	 * MISS), for Airside.Present.NetworkActor: two calls with the same node and no edit
	 * between them must move this by exactly one, not two - see PlanNodeDeletion's own
	 * comment for the cache this counts.
	 */
	int32 DeletionPlanComputeCountForTest() const { return DeletionPlanComputeCount; }

	/**
	 * How many times PlaceEntityInPlot has actually run the plot's reservation solve - issue
	 * #182. FPlotPlaceTool::GetSolveCountForTest proves the PREVIEW asked the real evaluator;
	 * this is its opposite number for the COMMIT, so a test can show both the readout's
	 * refusal and the facade's refusal came from the one evaluator actually running, not from
	 * a cached or assumed answer on either side.
	 */
	int32 PlotEvaluatorCountForTest() const { return PlotEvaluatorCount; }

	/**
	 * Wired by ARoadNetworkActor, right after it creates Traffic - the same "reconnect what a
	 * pointer can't" idiom the constructor already uses for OnChanged (see the class comment).
	 * FindRoute calls this instead of reaching Actor().GetTraffic()->GetModel() itself: this
	 * class must not reach past Network/History for anything else (#104), so the ONE place
	 * that knows how to find the traffic model is the actor, same as the ONE place that knows
	 * how to rebuild the mesh is.
	 */
	void SetTrafficModelProvider(TFunction<const UGroundTraffic*()> Provider) { TrafficModelProvider = MoveTemp(Provider); }

	// --- Money ---------------------------------------------------------------------------

	/**
	 * Where the money for a build comes from, or NULL for free.
	 *
	 * A RAW POINTER, not a UPROPERTY: the purse is the ledger, which the ops runtime owns and
	 * which outlives any edit, and this facade is Transient wiring re-made on every attach.
	 * Null is the normal state at design time - URoadBuildEdMode and every tool test build for
	 * nothing, and one test asserts they still can.
	 */
	void SetPurse(IBuildPurse* InPurse) { Purse = InPurse; }
	virtual IBuildPurse* GetPurse() const override { return Purse; }

	/**
	 * What connecting FromIndex to a point would cost, at the profile a click would ACTUALLY
	 * lay - see ResolveProfileFor. The ghost's price, and the reason the tool does not
	 * resolve the profile for itself.
	 */
	virtual FBuildQuote QuoteForConnect(int32 FromIndex, FVector2D To, ERoadKind Kind,
		int32 WidthIndex, EPavement Surface) const override;
	virtual FBuildQuote QuoteForRunway(FVector2D From, FVector2D To, const URoadProfile* Profile,
		EPavement Pavement) const override;

	/** True when there is no purse (design time) or the purse says the player can pay. */
	bool CanAfford(const FBuildQuote& Quote) const;

	/**
	 * CanAfford FOR A COMMIT: the same answer, and when it is no, OnRefused is broadcast so the player
	 * hears it. Every mutator's affordability guard goes through here; CanAfford itself is for the
	 * previews and Why* evaluators, which must stay silent (a ghost is asked every frame).
	 * ENFORCED BY: Check-Architecture rule 32 (refusal-is-announced)
	 */
	bool AffordOrRefuse(const FBuildQuote& Quote);

	/** WhyUpgradeRefused, and - when the refusal is affordability - the quote, so UpgradeSegment can
	 *  announce it. The interface virtual forwards here with null: a preview must stay silent. */
	FString WhyUpgradeRefusedImpl(int32 SegmentIndex, ERoadKind Kind, int32 WidthIndex, EPavement Surface,
		FBuildQuote* OutUnaffordable) const;

	/** WhyUpgradeUnaffordable with the same out-parameter - see WhyUpgradeRefusedImpl, which is the
	 *  two halves' composition (issue #439) and nothing more. */
	FString WhyUpgradeUnaffordableImpl(int32 SegmentIndex, ERoadKind Kind, int32 WidthIndex, EPavement Surface,
		FBuildQuote* OutUnaffordable) const;

	/** WhyStandRefused with the same out-parameter, for PlaceStandInPlot - see WhyUpgradeRefusedImpl.
	 *  The two halves' composition and nothing more, so the tool that asks them one at a time
	 *  (issue #439) and the commit that asks them together cannot be two evaluators. */
	FString WhyStandRefusedImpl(TArrayView<const FVector2D> Outline, EPavement Pavement, FBuildQuote* OutUnaffordable) const;

	/** WhyStandUnaffordable with the same out-parameter - see WhyStandRefusedImpl. */
	FString WhyStandUnaffordableImpl(TArrayView<const FVector2D> Outline, EPavement Pavement,
		FBuildQuote* OutUnaffordable) const;

	// --- Undo ----------------------------------------------------------------------------

	bool Undo();
	bool Redo();
	bool CanUndo() const;
	bool CanRedo() const;
	FString PeekUndoLabel() const;

	/**
	 * The history an edit should snapshot into, or NULL when the editor owns undo.
	 *
	 * Moved verbatim from ARoadNetworkActor - see its own old comment, preserved here: in an
	 * editor world the transaction system already serialises the network on Modify() and
	 * restores it on Ctrl+Z, so this returns null there and FRoadEditScope records no UNDO step; at
	 * runtime, where there is no transaction system, it returns the history.
	 *
	 * NULL MEANS "NO UNDO HISTORY", NOT "NO ROLLBACK" (issue #437). This used to be the switch a
	 * failed edit's revert hung on, so in the editor world - exactly where a refused drop-to-merge
	 * was then committed into the level by the drag transaction - nothing could be reverted. A
	 * scope with no history holds a local snapshot instead (FRoadEditScope), and an interactive
	 * edit keeps EditorRollbackPoint; both restore through URoadNetwork::RestoreFrom.
	 */
	URoadEditHistory* HistoryForEdit();

	/**
	 * Discard every undo step. Harmless (and allocates nothing) when there is no history yet -
	 * see EnsureHistory, which this deliberately does NOT call.
	 *
	 * THE DOOR THIS CLASS SHOULD ALWAYS HAVE HAD (issue #191): AirportOps' load path used to
	 * reach past this facade and call Target->History->Clear() on the actor directly - one
	 * plugin's composition root operating another's undo stack, when
	 * "the history an edit should snapshot into" and everything about it is this facade's own
	 * job (see HistoryForEdit above). A load is a new baseline: the history holds Mementos of
	 * the PRE-load network, and an undo afterwards would revert an airport the player just
	 * replaced on purpose.
	 */
	void ClearHistory();

private:
	/** A live segment's handle from its slot index. See MakeLiveNodeId. */
	bool MakeLiveSegmentId(int32 Index, FRoadSegmentId& OutId) const;

	/**
	 * THE single OnChanged.Broadcast() call site - see the class comment for exactly which
	 * mutators call this directly (Undo and Redo through AdoptNetwork, ClearNetwork) versus
	 * through CommitAndNotify or ApplyInteractiveMutation below. Every scope-committing mutator
	 * reaches one of those three; none call neither today (issue #125 closed the last two that
	 * did, ConnectGuidelines and DisconnectGuideline).
	 *
	 * DEFAULTS TO Topology, which is every call site except MoveNode/MoveApronCorner's
	 * mid-drag notify (issue #165, now ApplyInteractiveMutation's bChangesGraphShape=false
	 * path) and SetIntermediateHoldingPosition's (issue #179): every OTHER scope-committing
	 * mutator through CommitAndNotify changes the graph's shape, and so do Undo/Redo/
	 * ClearNetwork (they replace Network wholesale) and MergeNodes (it removes a node, so it
	 * passes bChangesGraphShape=true to ApplyInteractiveMutation and always gets Topology).
	 * SetIntermediateHoldingPosition passes Markings explicitly, through CommitAndNotify's own
	 * Kind parameter, because it moves nothing and changes no shape either - see EChangeKind's
	 * own comment for why Topology's default would be actively wrong there, not just wasteful.
	 *
	 * INSIDE A REBUILD BATCH this records Kind instead of broadcasting, and EndRebuildBatch's
	 * outermost close calls back in here with the combined kind once depth is zero - see the
	 * class comment's REBUILD BATCHES. Here and not in each door above, so no mutator can
	 * forget to defer.
	 */
	void NotifyChanged(EChangeKind Kind = EChangeKind::Topology);

	/**
	 * THE single OnReplaced.Broadcast() call site, NotifyChanged's shape for the replacement notice. Called by Undo and
	 * Redo (Adopted, after the purse has moved - see Undo/Redo), ClearNetwork and RestoreInPlace (both phases). NOT
	 * FOLDED BY A REBUILD BATCH: a batch defers the REBUILD, and a listener's response to a replacement - putting a tool
	 * down - is not a rebuild, and must happen at the phase it is told, not at a close some lines later.
	 * ENFORCED BY: Airside.Present.ReplacementIsAnnounced
	 */
	void AnnounceReplaced(ENetworkReplace Phase);

	/**
	 * THE FREE DOOR. Edit.Commit() plus NotifyChanged(Kind), in one call so a mutator that
	 * commits an edit cannot forget to notify - which is exactly how ten of these went silent
	 * before issue #77 (see the class comment). Takes the scope by reference rather than being
	 * a method ON FRoadEditScope itself: that type used to live in Tool/RoadEditHistory.h and
	 * had to stay ignorant of this facade's OnChanged, or Tool/ would have depended on
	 * Present/. Issue #191 moved FRoadEditScope to Present/RoadEditHistory.h alongside this
	 * facade, so the two now share a layer - but folding this into a method on the scope is a
	 * design change, not a move, and stayed out of that refactor's scope.
	 *
	 * KIND DEFAULTS TO Topology, same as NotifyChanged itself, for every caller that does not
	 * pass one - which was every caller until SetIntermediateHoldingPosition (issue #179)
	 * needed to pass Markings instead. Threaded through rather than given its own overload:
	 * a second CommitAndNotify would be a second free door, and this issue's whole point is
	 * that a commit and its notification must be one call, not a matching pair a mutator can
	 * get out of step.
	 *
	 * FOR AN EDIT THAT MOVES NO PAVEMENT - placing a bare node, naming a runway, splitting a
	 * segment, unlinking a guideline. Anything that CREATES or DESTROYS surface must use
	 * CommitPurchase or CommitDisposal below instead. Three doors rather than one because
	 * there are three different things to do about money and a single door would have to be
	 * told which anyway - but all three end here, so NotifyChanged still has exactly one call
	 * site, which is what issue #77 was about.
	 */
	void CommitAndNotify(FRoadEditScope& Edit, EChangeKind Kind = EChangeKind::Topology);

	/**
	 * Commit an edit that built something, and take the money for it.
	 *
	 * THE CHARGE HAPPENS AT COMMIT, BUT THE REFUSAL MUST HAPPEN BEFORE THE MUTATION. An
	 * FRoadEditScope that is not committed discards its undo SNAPSHOT; it does NOT roll the
	 * network back by itself (Rollback() is explicit, issue #437). So a caller that let the edit
	 * happen and then found it could not pay would leave the segment built, unpaid for, and with
	 * no undo step for it unless it remembered to roll back - and the player was shown the price
	 * before the click, so the refusal belongs where the preview made it. Every charged mutator
	 * therefore calls CanAfford among its guards, before it opens the scope.
	 *
	 * The charge id is recorded on the pending undo snapshot before the scope's destructor
	 * pushes it, which is what lets Undo reverse exactly what was taken.
	 */
	void CommitPurchase(FRoadEditScope& Edit, const FBuildQuote& Quote);

	/** Commit an edit that tore something out, and credit its scrap value. See IBuildPurse::Credit. */
	void CommitDisposal(FRoadEditScope& Edit, const FBuildQuote& Quote);

	/** What the pavement a segment occupies is worth today, for a charge or a credit. */
	FBuildQuote QuoteForSegment(int32 SegmentIndex) const;

	/**
	 * What every live segment on the network is worth today, summed.
	 *
	 * FOR THE DRAG, which is the one edit that changes how much pavement exists WITHOUT
	 * creating or destroying a segment - see EndInteractiveEdit. Walking the whole graph twice
	 * per drag (once at the start, once at the end) is cheap, and it is the only measure that
	 * cannot miss a length change: a drag can move a node that six segments meet at.
	 */
	FBuildQuote QuoteForAllPavement() const;

	/** What an apron outline is worth today, at the settings' rate, paved with Pavement - unset
	 *  for a bare apron or a depot's plot, which have no pavement of their own (BuildCost::ForApron
	 *  bills an unset line at the rate itself). No default, for ForApron's own reason. */
	FBuildQuote QuoteForApron(TConstArrayView<FVector2D> Outline, TOptional<EPavement> Pavement) const;

	/**
	 * Undo and Redo were the same six lines apart from which of URoadEditHistory's two
	 * methods they called (#103): guard Network/History, run Step, adopt what it returns
	 * through AdoptNetwork (#299) - NOT through CommitAndNotify or ApplyInteractiveMutation,
	 * same as before - see this class's comment on why those two mutators bypass both.
	 */
	bool Travel(TFunctionRef<URoadNetwork*(URoadEditHistory&, URoadNetwork&)> Step);

	/**
	 * THE COMMON TAIL of every place this facade throws the live network away and takes a
	 * REPLACEMENT rather than mutating it in place - spelled four times before issue #299 (a
	 * regression of #103's own Travel, which only Undo/Redo ever joined): Travel itself
	 * (Undo/Redo, adopting a Memento), the two revert sites (EndInteractiveEdit's
	 * cannot-afford branch and ApplyInteractiveMutation's Verify-failure branch, each undoing a
	 * whole open edit at once because AbandonEdit only drops the snapshot and does not put
	 * anything back - both now through RollBackOpenEdit below, which restores IN PLACE and
	 * reaches this with the network it already has, the assignment a no-op and the ghost hide
	 * and notify the point), and ClearNetwork (adopting a fresh, empty one). Byte for byte:
	 * `Owner.Network = X; HideGhost(); NotifyChanged();`.
	 *
	 * AND A LOAD, the fifth spelling (#426): RestoreInPlace adopts the SAME object it just deserialised into. The
	 * pointer write is then a no-op and the rest is exactly the tail a load owes - which is the point: a load was the
	 * one replacement that went round this door, calling RebuildMesh directly, so no OnChanged listener heard it.
	 * Announcing the replacement (OnReplaced) is NOT part of this tail: RollBackOpenEdit must not - see OnReplaced.
	 *
	 * HIDES THE GHOST because the preview may be describing a node that no longer exists in the
	 * replacement, and its cache (IsGhostCacheHit) compares only the cursor and the start node -
	 * neither of which a network swap changes, so a stale ghost would survive it and read as a
	 * road that is there until the next mouse move proves otherwise.
	 *
	 * NOTIFIES Topology, NotifyChanged's own default - every caller here replaces the graph
	 * wholesale, which is the largest shape change there is.
	 */
	void AdoptNetwork(URoadNetwork& NewNetwork);

	/**
	 * Put the live network back to the state the OPEN edit began in, and catch everything that
	 * derives from it up - the revert door (issue #437) for a failed interactive edit
	 * (ApplyInteractiveMutation's Verify) and an unaffordable drag (EndInteractiveEdit) alike.
	 *
	 * WORLD-BLIND, which is the point. Use is the history's pending edit when there is a history
	 * (a game world); otherwise the restore comes from EditorRollbackPoint, the copy taken when
	 * the drag began. Both restore IN PLACE through URoadNetwork::RestoreFrom, so the object the
	 * editor's transaction Modify()d is the one that comes back, and the two worlds cannot
	 * disagree about which fields do or how the revision clocks move.
	 *
	 * NOTIFIES (through AdoptNetwork, with the network it already has) where FRoadEditScope::
	 * Rollback does not: a drag has already told the presenter about every frame it moved, so the
	 * pavement on screen is the failed edit's and only a Topology notify puts the restored one
	 * back. False, notifying nothing and changing nothing, when there is nothing to restore from.
	 */
	bool RollBackOpenEdit(URoadEditHistory* Use);

	/**
	 * THE COMMON SHAPE MoveNode, MoveApronCorner and MergeNodes drifted into three copies of
	 * (issue #299, a regression of #77 back): guard the network, join a drag ALREADY in
	 * progress or open a tiny edit of its own, run Mutate, then Commit/Abandon/Revert and
	 * notify - all of it byte-identical apart from the label and, in MergeNodes, a second
	 * guard that had already drifted onto the first (`bOwnsEdit && Use != nullptr`, when
	 * bOwnsEdit alone already implies Use is non-null).
	 *
	 * JOINS A DRAG ALREADY IN PROGRESS, so the whole drag is one undo step; on its own it is
	 * one edit of its own. IsEditing is what tells the two apart - see BeginInteractiveEdit/
	 * EndInteractiveEdit, which bracket a drag across many calls to this.
	 *
	 * MUTATE RETURNING FALSE MEANS NOTHING WAS TOUCHED, so ABANDON is right and Revert would be
	 * wrong - see URoadEditHistory::RollbackEdit on the distinction. No notify either: a refusal
	 * changes nothing to rebuild for.
	 *
	 * VERIFY EXISTS FOR MergeNodes ALONE TODAY: MoveNode and MoveApronCorner judge their move
	 * BEFORE calling this (their own guards refuse without mutating), so they pass none and
	 * get the default, which always accepts. A merge cannot be judged until it exists -
	 * RoadPlacement::NodeCornersFit reads a node's CURRENT arms, which do not exist as one set
	 * until the merge has happened - so its Verify runs AFTER Mutate and, on a false answer,
	 * REVERTS RATHER THAN REFUSES: AbandonEdit would leave the merged (and now un-cornerable)
	 * graph standing with no undo step for it, where RollBackOpenEdit restores the state the WHOLE
	 * open edit started from - not merely this call's own slice of it, which is right when this
	 * call joined a drag already in progress (drop-to-merge does exactly that). The revert is
	 * not gated on bOwnsEdit, for the same reason: a merge that joined someone else's edit must
	 * still be able to unwind the whole thing.
	 *
	 * AND NOT GATED ON THERE BEING A HISTORY (issue #437). It was - `Use != nullptr` - so in the
	 * editor world, where HistoryForEdit() is null by design, a merge whose Verify failed stayed
	 * merged: MergeNodes logged "the whole edit is reverted", nothing was, and the editor's drag
	 * transaction then committed the result into the level. EditorRollbackPoint is what a
	 * history-less world restores from - the copy taken when the drag began, or, for a bare call
	 * (no drag open), one this call takes itself and lets go on the way out.
	 * ENFORCED BY: Airside.Present.MergeRefusedIntoStripInEditorWorld,
	 * Airside.Present.MergeCornerRefusalInEditorWorld.
	 *
	 * bChangesGraphShape SELECTS WHICH NOTIFY A SUCCESS GETS, and the two answers disagree on
	 * purpose:
	 *
	 *   - false (MoveNode, MoveApronCorner): GEOMETRY ONLY WHILE AN INTERACTIVE EDIT IS STILL
	 *     OPEN - THE BARE-CALL TRAP (review follow-up on #165). bInteractiveEditOpen true means
	 *     this call joined a drag BeginInteractiveEdit started and EndInteractiveEdit has not
	 *     yet closed - the ONLY case where something downstream (EndInteractiveEdit's own
	 *     Topology notify) is guaranteed to catch the derived graph up later. Anything else has
	 *     no EndInteractiveEdit coming and must do the whole job itself, Topology, right here: a
	 *     BARE call (bOwnsEdit was true - this very call opened and closed its own tiny history
	 *     edit, with no surrounding BeginInteractiveEdit at all) never set the flag in the first
	 *     place. bInteractiveEditOpen, NOT `Use != nullptr && Use->IsEditing()` (issue #190) -
	 *     that test was always false in an editor world, where HistoryForEdit() is a deliberate
	 *     no-op (see its own comment), so an editor-mode drag notified Topology on every frame
	 *     regardless of URoadBuildEdMode's own Begin/EndInteractiveEdit calls bracketing it
	 *     exactly as PIE's do.
	 *
	 *   - true (MergeNodes): ALWAYS Topology, drag or no drag. A merge removes a node - the
	 *     graph's SHAPE changed, not merely a position - and drop-to-merge runs from inside an
	 *     already-open drag (FEditTool::OnDragEnd calls it before EndInteractiveEdit), so
	 *     bInteractiveEditOpen reads true at exactly the moment a merge succeeds. Deferring to
	 *     EndInteractiveEdit's catch-up the way a plain drag frame does would not be WRONG - the
	 *     same Topology notify would still land, one call later - but it would fire TWICE (once
	 *     here mislabelled Geometry, once more at EndInteractiveEdit), a second rebuild the
	 *     graph's shape change does not owe.
	 */
	bool ApplyInteractiveMutation(const TCHAR* Label, TFunctionRef<bool(URoadNetwork&)> Mutate,
		bool bChangesGraphShape = false,
		TFunctionRef<bool(const URoadNetwork&)> Verify = [](const URoadNetwork&) { return true; });

	/**
	 * DeleteApron, DeleteEntity and DisconnectGuideline were the same shape apart from which
	 * slot-map Remove they called (#103, folded in on review once #134 gave
	 * DisconnectGuideline its own CommitAndNotify): guard the network and the doomed handle,
	 * open an edit scope, Remove, CommitAndNotify. bDoomed is evaluated by the caller, which
	 * is the one that knows how to turn its own index into its own handle type - and, for
	 * DisconnectGuideline, checks its own extra derived-edge refusal first.
	 */
	bool DeleteSlot(bool bDoomed, const TCHAR* Label, TFunctionRef<bool(URoadNetwork&)> Remove,
		const FBuildQuote& Quote = FBuildQuote());

	/**
	 * THE ONE EVALUATOR PlaceEntityInPlot judges a plot against - issue #182. Runs
	 * PlotLayoutFor(Layout)->Solve(Site, Specs), the IDENTICAL call
	 * FPlotPlaceTool::ReservationFor makes for the ghost and the readout, so a commit cannot
	 * disagree with the preview that led to it the way PlotFit::FitBays - a 4 m x 12 m bay
	 * grid with its own point-in-polygon test - used to.
	 *
	 * TAKES Outline/FrontageA/FrontageB AS GIVEN, not corrected for winding: PlotYard's own
	 * functions read the interior side off the outline's signed area (PlotYard::InwardOf), so
	 * they answer the same for a plot wound either way as long as FrontageA/FrontageB travel
	 * with it - exactly the property PlaceEntityInPlot already relied on for PlotFit. Kind
	 * resolves the definition (for EPlotLayout) and nothing else; Modules plays no part in
	 * what a plot can HOLD, only in what it starts pre-built with.
	 *
	 * BUMPS PlotEvaluatorCount ON EVERY CALL, unlike FPlotPlaceTool's own memo: nothing here
	 * is asked twice in a row the way a hover frame asks the tool, so there is no repeat call
	 * worth short-circuiting - see PlotEvaluatorCountForTest for what the count is FOR.
	 */
	PlotYard::FReservation ReserveForPlot(TArrayView<const FVector2D> Outline,
		FVector2D FrontageA, FVector2D FrontageB, EPlaceableEntity Kind) const;

	IBuildPurse* Purse = nullptr;

	/** What the pavement was worth when the current interactive drag began. See EndInteractiveEdit. */
	double PavementValueAtDragStart = 0.0;

	/**
	 * PlanNodeDeletion's cache (#166) - see its own header comment. MUTABLE because
	 * PlanNodeDeletion is const (it is a query, not a mutator: IRoadEditTarget's other
	 * const methods have never had to cache anything, but this one's answer is expensive
	 * and this class is where "expensive" is measured, not RoadHeal, which must still be
	 * callable from a query with no side effects of its own).
	 */
	mutable int32 LastDeletionPlanNode = INDEX_NONE;
	mutable uint32 LastDeletionPlanRevision = 0;
	mutable bool bHasLastDeletionPlan = false;
	mutable FRoadDeletionPlan LastDeletionPlan;
	mutable int32 DeletionPlanComputeCount = 0;

	/** See PlotEvaluatorCountForTest. MUTABLE for the same reason DeletionPlanComputeCount is:
	 *  ReserveForPlot is logically a query, callable from a const context, that must still
	 *  count how many times it actually ran. */
	mutable int32 PlotEvaluatorCount = 0;

	/**
	 * Whether MoveNode or MoveApronCorner actually moved something during the CURRENT
	 * interactive edit - cleared in BeginInteractiveEdit, set by ApplyInteractiveMutation's
	 * Geometry notify (issue #299 moved the setter off MoveNode/MoveApronCorner themselves).
	 *
	 * WHAT THIS GUARDS (issue #165 follow-up review). Before #165, every MoveNode/
	 * MoveApronCorner notify ran the whole pipeline, so it did not matter whether the edit
	 * that owned a drag was later kept or abandoned: the derived graph was always fresh.
	 * After #165 a drag frame notifies Geometry only, so EndInteractiveEdit is the ONLY place
	 * left that can catch the derived graph up - and it needs to know whether there is
	 * anything to catch up. Read in exactly two places:
	 *   - bKeep=false (abandoned): AbandonEdit only drops the undo snapshot, it does NOT put
	 *     the nodes back, so a drag that moved something and was then abandoned (Escape) would
	 *     leave guidelines/anchor links/plots/traffic pointed at pre-drag positions FOREVER
	 *     with no flag here to say so - nothing else will ever notify Topology for that edit.
	 *   - bKeep=true with nothing moved (a click-release that opened and closed an edit
	 *     without a single successful move): firing a Topology notify anyway would be a full
	 *     rebuild that never happened before #165, for no reason.
	 * NOT read on the CanAfford-revert branch inside bKeep=true - that branch already
	 * notifies Topology itself via RollBackOpenEdit, unconditionally, because a reverted drag always
	 * changed something (the charge check only runs after a real move).
	 */
	bool bGeometryChangedDuringEdit = false;

	/**
	 * Whether an interactive edit is open RIGHT NOW - set in BeginInteractiveEdit, cleared in
	 * EndInteractiveEdit - issue #190.
	 *
	 * TRACKED INDEPENDENTLY OF URoadEditHistory::IsEditing(), which MoveNode and
	 * MoveApronCorner used to test instead (`Use != nullptr && Use->IsEditing()`) to decide
	 * Geometry vs Topology. That test is FALSE FOR EVERY EDITOR-MODE DRAG: HistoryForEdit()
	 * is a deliberate no-op in an editor world (see its own comment - "the editor's
	 * transaction system does the Memento's job already"), so `Use` is null there and
	 * `Use->IsEditing()` could never have been true, whatever URoadBuildEdMode's own
	 * Begin/EndInteractiveEdit calls were doing. Every editor-mode drag frame therefore
	 * notified Topology - the full derived-graph rebuild #165 exists to skip - and
	 * EndInteractiveEdit's own early-return on `History == nullptr` meant nothing ever fired
	 * the one catch-up notify a real drag needs either. This bool answers "is a drag open"
	 * on its own terms, true in both worlds for exactly the span BeginInteractiveEdit and
	 * EndInteractiveEdit bracket, so the split applies wherever a drag does.
	 */
	bool bInteractiveEditOpen = false;

	/**
	 * THE STATE THE OPEN INTERACTIVE EDIT BEGAN IN, in a world with no undo history (issue #437) -
	 * what RollBackOpenEdit restores from when HistoryForEdit() is null. Taken by
	 * BeginInteractiveEdit for the span of a drag; ApplyInteractiveMutation takes its own, and lets
	 * it go on the way out, for a bare call made with no drag open. Null in a game world, where the
	 * history's pending snapshot is the same thing, and between drags.
	 *
	 * A UPROPERTY, TRANSIENT: a snapshot referenced only by a raw pointer is collectable
	 * mid-drag, and one the level SAVED would write a transient-package object into the .umap.
	 * Its outer is the transient package (FRoadEditScope::SnapshotForRollback), not this facade,
	 * for the same reason. ClearHistory drops it: a load is a new baseline, and a point taken
	 * before one would restore the OLD airport over the loaded one.
	 */
	UPROPERTY(Transient)
	TObjectPtr<URoadNetwork> EditorRollbackPoint;

	/**
	 * How many rebuild batches are open - see the class comment's REBUILD BATCHES. A COUNT,
	 * not a bool: ARigTestCourse::BuildCourse (game module) opens one around the loop AND the
	 * yard, and each of their Lay()s opens its own, so a bool would close at the first inner
	 * guard and rebuild twice.
	 */
	int32 RebuildBatchDepth = 0;

	/**
	 * The combined kind of every notify an open batch has folded (CombineChangeKinds), unset
	 * when it has folded none - a TOptional rather than a kind plus a bool, since the two could
	 * otherwise disagree about whether a rebuild is owed.
	 */
	TOptional<EChangeKind> PendingBatchKind;

	/** How many notifies the open batch has folded, for the close's log line only. */
	int32 FoldedNotifyCount = 0;

	/**
	 * Bumped by every NotifyChanged, batched or not - see IRoadEditTarget::GetEditEpoch. A
	 * session clock, not saved. It is not reset by AdoptNetwork or ClearNetwork: a swap is
	 * itself an edit, and a counter that could go back to a value it once held would let a
	 * memo keyed on it match a network it never saw.
	 */
	uint32 EditEpoch = 0;

	/**
	 * Warn that Who is about to read the guideline graph while a batch has deferred the rebuild
	 * that would bring it up to date - see the class comment's REBUILD BATCHES. Silent outside a
	 * batch, and inside one that has folded nothing yet (the graph is still current then).
	 */
	void WarnIfDerivedStale(const TCHAR* Who) const;

	/**
	 * The actor this facade edits, found through Outer rather than stored a second time.
	 *
	 * A REFERENCE, not a pointer every caller has to null-check: this facade REQUIRES an
	 * owning actor (see the class comment) and is created only by
	 * ARoadNetworkActor::ARoadNetworkActor via CreateDefaultSubobject, so a null Outer here
	 * is a construction error, not a state normal control flow should route around.
	 * checkf-ed rather than silently tolerated, so that error is loud at the first call
	 * instead of surfacing later as a null-network read that looks like an empty level.
	 * CreateDefaultSubobject sets Outer to the constructing actor, so GetTypedOuter is
	 * exactly as reliable as a cached pointer would be and cannot go stale independently of
	 * it.
	 */
	ARoadNetworkActor& Actor() const;

	URoadNetwork& EnsureNetwork();
	URoadEditHistory& EnsureHistory();

	/** See SetTrafficModelProvider. Returns null before Traffic has dispatched anything, or
	 *  in an editor world with no running simulation - FindRoute already treats a null model
	 *  as "no occupancy to weight against", same as it did reaching Actor() directly. */
	TFunction<const UGroundTraffic*()> TrafficModelProvider;
};
