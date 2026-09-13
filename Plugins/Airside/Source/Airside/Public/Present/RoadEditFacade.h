#pragma once

#include "CoreMinimal.h"
#include "Model/BuildPurse.h"
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
 * scope-committing mutator EXCEPT MoveNode reaches it through CommitAndNotify, which pairs
 * an FRoadEditScope::Commit() with the notification so neither can happen without the
 * other. Undo, Redo and ClearNetwork call NotifyChanged directly instead, because none of
 * them fits that shape: Undo/Redo replace Network wholesale rather than mutating through a
 * scope, and ClearNetwork's own scope only records the pre-clear snapshot - the notify has
 * to wait until AFTER Owner.Network is replaced with the fresh one. MoveNode also calls
 * NotifyChanged directly, and on every successful call rather than once: a drag joins one
 * scope-free edit across many frames (BeginInteractiveEdit/EndInteractiveEdit), and needs a
 * rebuild each frame it actually moves, not only when the drag ends.
 * SetIntermediateHoldingPosition commits its scope WITHOUT notifying, by design - a holding
 * position changes neither pavement nor mesh.
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
	/** Fired wherever this class's mutators used to call ARoadNetworkActor::RebuildMesh(). */
	DECLARE_MULTICAST_DELEGATE(FOnNetworkChanged);
	FOnNetworkChanged OnChanged;

	// --- IRoadEditTarget ---------------------------------------------------------------

	virtual const URoadNetwork* GetNetwork() const override;

	virtual int32 PlaceNode(FVector2D Where) override;
	virtual bool ConnectNodes(int32 FromIndex, int32 ToIndex, ERoadKind Kind, int32 WidthIndex) override;

	/** Forwarded to the actor, which owns the content lookup - see IRoadEditTarget. */
	virtual int32 GetTaxiwayProfileCount() const override;
	virtual URoadProfile* ResolveTaxiwayProfile(int32 Index) const override;
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
	virtual int32 SplitSegment(int32 SegmentIndex, FVector2D At) override;
	virtual bool DeleteNode(int32 NodeIndex) override;
	virtual bool DeleteSegment(int32 SegmentIndex) override;
	virtual bool MoveNode(int32 NodeIndex, FVector2D To) override;
	virtual void BeginInteractiveEdit(const FString& Label) override;
	virtual void EndInteractiveEdit(bool bKeep) override;
	virtual FRoadDeletionPlan PlanNodeDeletion(int32 NodeIndex) const override;

	virtual int32 AddApron(const TArray<FVector2D>& Outline) override;
	virtual bool DeleteApron(int32 ApronIndex) override;
	virtual int32 FindApronAt(FVector2D Where) const override;

	virtual int32 PlaceEntity(FVector2D Where, double Heading, EPlaceableEntity Kind) override;
	using IRoadEditTarget::PlaceStand;
	virtual bool DeleteEntity(int32 EntityIndex) override;
	virtual int32 FindEntityAt(FVector2D Where, double Radius) const override;
	virtual const UEntityDefinition* GetEntityDefinition(EPlaceableEntity Kind) const override;
	using IRoadEditTarget::GetStandDefinition;

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

	virtual bool MakeLiveNodeId(int32 Index, FRoadNodeId& OutId) const override;

	virtual FRoutePlan FindRoute(FGuidelineNodeId Start, FGuidelineNodeId Goal,
		ETraversalClass Class, double Wingspan) const override;

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

private:
	/** A live segment's handle from its slot index. See MakeLiveNodeId. */
	bool MakeLiveSegmentId(int32 Index, FRoadSegmentId& OutId) const;

	/**
	 * THE single OnChanged.Broadcast() call site - see the class comment for exactly which
	 * mutators call this directly (Undo, Redo, ClearNetwork, MoveNode) versus through
	 * CommitAndNotify below, and which two currently call neither (issue #125).
	 */
	void NotifyChanged();

	/**
	 * THE FREE DOOR. Edit.Commit() plus NotifyChanged(), in one call so a mutator that commits
	 * an edit cannot forget to notify - which is exactly how ten of these went silent before
	 * issue #77 (see the class comment). Takes the scope by reference rather than being a
	 * method ON FRoadEditScope itself: that type lives in Tool/RoadEditHistory.h and must not
	 * know about this facade's OnChanged, or Tool/ would depend on Present/.
	 *
	 * FOR AN EDIT THAT MOVES NO PAVEMENT - placing a bare node, naming a runway, splitting a
	 * segment, unlinking a guideline. Anything that CREATES or DESTROYS surface must use
	 * CommitPurchase or CommitDisposal below instead. Three doors rather than one because
	 * there are three different things to do about money and a single door would have to be
	 * told which anyway - but all three end here, so NotifyChanged still has exactly one call
	 * site, which is what issue #77 was about.
	 */
	void CommitAndNotify(FRoadEditScope& Edit);

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

	/** What an apron outline is worth today, at the settings' rate. */
	FBuildQuote QuoteForApron(TConstArrayView<FVector2D> Outline) const;

	/**
	 * Undo and Redo were the same six lines apart from which of URoadEditHistory's two
	 * methods they called (#103): guard Network/History, run Step, adopt what it returns,
	 * hide the ghost, NotifyChanged directly - NOT through CommitAndNotify, same as before -
	 * see this class's comment on why those two mutators bypass it.
	 */
	bool Travel(TFunctionRef<URoadNetwork*(URoadEditHistory&, URoadNetwork&)> Step);

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

	IBuildPurse* Purse = nullptr;

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
