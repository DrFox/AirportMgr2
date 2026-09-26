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
 * save would ever need to persist.
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

	// --- IRoadEditTarget ---------------------------------------------------------------

	virtual const URoadNetwork* GetNetwork() const override;

	virtual int32 PlaceNode(FVector2D Where) override;
	virtual bool ConnectNodes(int32 FromIndex, int32 ToIndex, ERoadKind Kind, int32 WidthIndex) override;

	/** Forwarded to the actor, which owns the content lookup - see IRoadEditTarget. */
	virtual int32 GetWidthCount(ERoadKind Kind) const override;
	virtual URoadProfile* ResolveWidthProfile(ERoadKind Kind, int32 Index) const override;
	using IRoadEditTarget::ConnectNodes;
	virtual int32 ConnectGuidelines(int32 FromNodeIndex, int32 ToNodeIndex) override;
	virtual bool PlaceRunway(FVector2D From, FVector2D To, URoadProfile* RunwayProfile, const FRunwayFacts& Facts) override;
	using IRoadEditTarget::PlaceRunway;
	virtual bool SetRunwayFacts(int32 SegmentIndex, const FRunwayFacts& Facts) override;
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
	virtual int32 SplitSegment(int32 SegmentIndex, FVector2D At) override;
	virtual bool DeleteNode(int32 NodeIndex) override;
	virtual bool DeleteSegment(int32 SegmentIndex) override;
	virtual bool MoveNode(int32 NodeIndex, FVector2D To) override;
	virtual bool MergeNodes(int32 KeepIndex, int32 AbsorbIndex) override;
	virtual bool MoveApronCorner(int32 ApronIndex, int32 CornerIndex, FVector2D To) override;
	virtual void BeginInteractiveEdit(const FString& Label) override;
	virtual void EndInteractiveEdit(bool bKeep) override;

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
		FVector2D EntranceA, FVector2D EntranceB) override;
	virtual FString WhyStandRefused(TArrayView<const FVector2D> Outline) const override;
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
		int32 WidthIndex) const override;
	virtual FBuildQuote QuoteForRunway(FVector2D From, FVector2D To, const URoadProfile* Profile) const override;

	/** True when there is no purse (design time) or the purse says the player can pay. */
	bool CanAfford(const FBuildQuote& Quote) const;

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
	 * restores it on Ctrl+Z, so this returns null there and FRoadEditScope becomes a no-op; at
	 * runtime, where there is no transaction system, it returns the history.
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
	 */
	void NotifyChanged(EChangeKind Kind = EChangeKind::Topology);

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
	 * network back. So a caller that let the edit happen and then found it could not pay would
	 * leave the segment built, unpaid for, and with no undo step for it. Every charged mutator
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

	/** What an apron outline is worth today, at the settings' rate. */
	FBuildQuote QuoteForApron(TConstArrayView<FVector2D> Outline) const;

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
	 * (Undo/Redo, adopting a Memento), the two RevertEdit sites (EndInteractiveEdit's
	 * cannot-afford branch and ApplyInteractiveMutation's Verify-failure branch, each undoing a
	 * whole open edit at once because AbandonEdit only drops the snapshot and does not put
	 * anything back), and ClearNetwork (adopting a fresh, empty one). Byte for byte:
	 * `Owner.Network = X; HideGhost(); NotifyChanged();`.
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
	 * wrong - see URoadEditHistory::RevertEdit on the distinction. No notify either: a refusal
	 * changes nothing to rebuild for.
	 *
	 * VERIFY EXISTS FOR MergeNodes ALONE TODAY: MoveNode and MoveApronCorner judge their move
	 * BEFORE calling this (their own guards refuse without mutating), so they pass none and
	 * get the default, which always accepts. A merge cannot be judged until it exists -
	 * RoadPlacement::NodeCornersFit reads a node's CURRENT arms, which do not exist as one set
	 * until the merge has happened - so its Verify runs AFTER Mutate and, on a false answer,
	 * REVERTS RATHER THAN REFUSES: AbandonEdit would leave the merged (and now un-cornerable)
	 * graph standing with no undo step for it, where RevertEdit hands back the state the WHOLE
	 * open edit started from - not merely this call's own slice of it, which is right when this
	 * call joined a drag already in progress (drop-to-merge does exactly that). The revert is
	 * unconditional on Use != nullptr, NOT gated on bOwnsEdit, for the same reason: a merge
	 * that joined someone else's edit must still be able to unwind the whole thing.
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

	/**
	 * The one quote a drawn stand is priced at: BuildCost::ForEntity(Definition) plus the pad
	 * it sits on (QuoteForApron(Outline)), combined into one "{0} + {1}" What text - the same
	 * shape QuoteForApron's own callers in PlaceEntityInPlot already sum by hand, pulled out
	 * here because WhyStandRefused's afford gate and PlaceStandInPlot's charge both need
	 * EXACTLY this figure and had drifted into two slightly different copies of it (fix round
	 * 1 on this task's own review).
	 */
	FBuildQuote QuoteStand(const UEntityDefinition& Definition,
		TArrayView<const FVector2D> Outline) const;

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
	 * notifies Topology itself via RevertEdit, unconditionally, because a reverted drag always
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
