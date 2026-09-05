#pragma once

#include "CoreMinimal.h"
#include "Tool/RoadEditTarget.h"
#include "RoadEditFacade.generated.h"

class ARoadNetworkActor;
class URoadNetwork;
class URoadEditHistory;

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
 * DOES NOT OWN Network OR History. Both stay UPROPERTYs on ARoadNetworkActor: they are what
 * the level actually saves, and moving them here would put the saved graph behind a subobject
 * the .umap has never heard of - a bigger, riskier change than this pure refactor is meant to
 * make (see the task's ruling on this point). Instead this class reaches its owner through
 * GetTypedOuter<ARoadNetworkActor>() - Outer is already the actor, because the actor creates
 * this facade with CreateDefaultSubobject, so a second stored pointer would only be a second
 * thing that could disagree with the first. The same path answers GetWorld() (UObject's
 * default walks GetOuter()->GetWorld()), which is what HistoryForEdit needs to tell an editor
 * world from a game one without being handed a world explicitly.
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
	virtual bool ConnectNodes(int32 FromIndex, int32 ToIndex) override;
	virtual int32 ConnectGuidelines(int32 FromNodeIndex, int32 ToNodeIndex) override;
	virtual bool PlaceRunway(FVector2D From, FVector2D To, URoadProfile* RunwayProfile) override;
	virtual double GetMinimumRunwayLength() const override;
	virtual bool DisconnectGuideline(int32 EdgeIndex) override;
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

	virtual int32 PlaceStand(FVector2D Where, double Heading) override;
	virtual bool DeleteEntity(int32 EntityIndex) override;
	virtual int32 FindEntityAt(FVector2D Where, double Radius) const override;
	virtual const UEntityDefinition* GetStandDefinition() const override;

	/**
	 * UpdateGhost, HideGhost, RebuildMesh and DispatchAgent are IRoadEditTarget virtuals
	 * whose real work happens on URoadSurfacePresenter or UAirsideTraffic - neither of which
	 * this facade has a pointer to, by design (it must not reach past Network/History for
	 * anything else - see the class comment). They are still implemented here, because
	 * IRoadEditTarget is a base this class cannot leave abstract, but each one simply asks
	 * the owning actor to do its own job: ARoadNetworkActor's own overrides of these four are
	 * the real forwarders, to the presenter and the traffic object respectively, and this
	 * just reaches them the same way every other query here reaches Network - through Owner().
	 * Nothing calls IRoadEditTarget through a facade-typed pointer today; this exists so
	 * nothing would silently do the wrong thing if that ever changed.
	 */
	virtual void UpdateGhost(int32 FromNodeIndex, const FRoadSnapResult& Snap, bool bValid) override;
	virtual void HideGhost() override;
	virtual void RebuildMesh() override;
	virtual bool DispatchAgent(const FRoutePlan& Plan, const FAirframe& Airframe) override;

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
	 * The split surgery itself, against any network.
	 *
	 * Shared by the real edit (SplitSegment, above) and URoadSurfacePresenter's ghost preview
	 * deliberately: two implementations of the same surgery is precisely how a preview comes
	 * to show something the click will not do, and that failure is invisible - the ghost looks
	 * plausible either way. Static rather than moved to Model/URoadNetwork: it is genuinely
	 * graph surgery and a free function on URoadNetwork was the brief's preferred home for it,
	 * but this refactor's blast radius is the Present/ split named in issue #32, and Model/ is
	 * explicitly out of scope for it - adding a new public Model/ entry point is a second,
	 * unrelated design decision this task should not fold in silently. A static here costs the
	 * presenter one extra include and nothing else.
	 */
	static FRoadNodeId SplitSegmentIn(URoadNetwork& Net, FRoadSegmentId Doomed, const FVector2D& At);

private:
	/** A live segment's handle from its slot index. See MakeLiveNodeId. */
	bool MakeLiveSegmentId(int32 Index, FRoadSegmentId& OutId) const;

	/**
	 * The actor this facade edits, found through Outer rather than stored a second time.
	 *
	 * CreateDefaultSubobject sets Outer to the constructing actor, so GetTypedOuter is exactly
	 * as reliable as a cached pointer would be and cannot go stale independently of it. Never
	 * null for a facade actually driving an actor; the only caller that could hand this a null
	 * owner is a test constructing URoadEditFacade with NewObject and no actor, which none of
	 * the existing tests do - they still target ARoadNetworkActor, which forwards here.
	 */
	ARoadNetworkActor* Owner() const;

	URoadNetwork& EnsureNetwork();
	URoadEditHistory& EnsureHistory();
};
