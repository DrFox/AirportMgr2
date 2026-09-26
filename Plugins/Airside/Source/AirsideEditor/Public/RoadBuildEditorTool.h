#pragma once

#include "CoreMinimal.h"
#include "InteractiveTool.h"
#include "InteractiveToolBuilder.h"
#include "BaseBehaviors/BehaviorTargetInterfaces.h"
#include "Tool/BuildGesture.h"
#include "Tool/BuildSession.h"
#include "Tool/RoadBuildTool.h"
#include "RoadBuildEditorTool.generated.h"

class ARoadNetworkActor;

/**
 * One label FViewportPreviewSink collected this frame - issue #304. A PrimitiveDrawInterface
 * (Render's own sink) draws geometry, not text, so a tool's refusal reason used to have nowhere
 * to land in the editor viewport at all (FViewportPreviewSink::Label was "deliberately
 * nothing"), though PIE's ARoadBuildHUD has always drawn the identical call. See
 * URoadBuildEditorTool::PendingLabels, the one place a frame's worth of these is cached.
 */
struct FEditorPreviewLabel
{
	FVector2D At = FVector2D::ZeroVector;
	FString Text;
	EPreviewStyle Style = EPreviewStyle::Pending;
};

/**
 * Makes one adapter around one build tool, named by its index into ToolRegistry() rather
 * than a `Kind` enum - see issue #33. A single builder class parameterised by index rather
 * than one per tool: what differs between them is one number, and one class per tool would
 * be one more place for the registry and the editor to drift, which is the class of bug
 * this whole table exists to make impossible.
 */
UCLASS()
class URoadBuildEditorToolBuilder : public UInteractiveToolBuilder
{
	GENERATED_BODY()

public:
	UPROPERTY()
	int32 ToolIndex = 0;

	virtual bool CanBuildTool(const FToolBuilderState& SceneState) const override { return true; }
	virtual UInteractiveTool* BuildTool(const FToolBuilderState& SceneState) const override;
};

/**
 * Thin adapter: the InteractiveTools framework on the outside, an IBuildTool on the inside.
 *
 * It exists because UEdMode has no raw input hooks - no InputKey, no MouseMove, no Render -
 * and routes viewport input through the ITF input router instead. So ITF is not optional
 * for an editor mode that wants clicks, whatever one thinks of the framework.
 *
 * It stays THIN on purpose. Every decision about what a gesture means belongs to the
 * IBuildTool, which is shared with the runtime PlayerController and knows about neither
 * driver. This class does three jobs and no more:
 *
 *   turn an ITF ray into a road-plane position and a snap result
 *   decide whether a press became a click or a drag
 *   draw whatever the tool describes, in the viewport rather than on a HUD
 *
 * UNDO IS THE EDITOR'S. Each gesture opens a transaction and calls Modify() on the network
 * before touching it, so Ctrl+Z is the editor's own undo. The runtime Memento history is
 * deliberately switched off in an editor world - two undo stacks fighting over one graph is
 * a far worse surprise than either alone.
 */
UCLASS()
class URoadBuildEditorTool : public UInteractiveTool, public IClickDragBehaviorTarget, public IHoverBehaviorTarget
{
	GENERATED_BODY()

public:
	void SetToolIndex(int32 InToolIndex) { ToolIndex = InToolIndex; }

	/**
	 * The mode's session, handed over at build time so tool state outlives this instance.
	 *
	 * ITF builds a new tool object per activation; the session must not be rebuilt with it
	 * or a runway's chosen width dies the moment the tool is picked - see
	 * URoadBuildEdMode::GetSession for the whole account.
	 */
	void SetSharedSession(FBuildSession* InSession) { SharedSession = InSession; }

	/** Which session this instance is actually driving. For a test that it is the mode's. */
	const FBuildSession* SessionForTest() const { return SharedSession; }

	/** Whether the nine-tool fallback session has actually been built - issue #190. For a
	 *  test that an activation handed a SharedSession never allocates OwnSession at all,
	 *  rather than building it and simply not reading it. Reads the flag directly instead of
	 *  calling Sess(), which would allocate it just to answer the question. */
	bool OwnSessionAllocatedForTest() const { return OwnSession.IsValid(); }

	/** The press/drag/release recogniser this instance drives. For a test that ITF's click
	 *  and drag callbacks actually reach FBuildGesture, rather than a copy nothing calls -
	 *  same precedent as SessionForTest, see issue #92. */
	const FBuildGesture& GestureForTest() const { return Gesture; }

	/** Whether Ctrl/Shift are currently reported down by the behaviours OnUpdateModifierState
	 *  hears from - see that method. Read by URoadBuildEdMode::StartToolAction so a reselect
	 *  (the tool's own key pressed again) carries the SAME modifier a fresh click would, which
	 *  PIE has always done by reading live key state for every SelectTool call - issue
	 *  #191/#92-#93; before this the editor's reselect context was bare Target with both
	 *  false, so Ctrl+the runway key never reached FRunwayTool::OnReselect's NextApproach
	 *  branch here the way it does in play. */
	bool IsRemoveModifierHeld() const { return bRemoveHeld; }
	bool IsInsertModifierHeld() const { return bInsertHeld; }

	/** Points this instance at InTarget without going through Setup's ResolveTarget, which
	 *  needs a live UInteractiveToolManager/world neither BuildGestureCompositionTest nor the
	 *  horizon-cap test below has - same precedent as ARoadBuildController::SetTargetForTest. */
	void SetTargetForTest(ARoadNetworkActor* InTarget) { Target = InTarget; }

	/** RayToPlane, exposed so a test can drive the horizon cap without a real
	 *  IToolsContextRenderAPI to call Render() through - see SetViewCentreDistanceForTest. */
	bool RayToPlaneForTest(const FRay& Ray, FVector2D& OutPosition) const { return RayToPlane(Ray, OutPosition); }

	/** Stands in for what Render() measures every frame it actually runs - see
	 *  ViewCentreDistance's own comment. */
	void SetViewCentreDistanceForTest(double Distance) { ViewCentreDistance = Distance; }

	/**
	 * Stands in for what Render() and DrawHUD() each ask of the session, every frame either
	 * actually runs: both call MakeHoverContext() and nothing else that could rebuild a
	 * context - see Render's BuildPreview call and DrawHUD's BuildReadout call. A real
	 * IToolsContextRenderAPI/FSceneView/FCanvas needs a live viewport this headless harness
	 * does not have, same precedent as SetViewCentreDistanceForTest standing in for Render's
	 * own measurement. Airside.Editor.HoverFrameBuildsOneContext drives OnUpdateHover for real
	 * and this twice more, in Render's and DrawHUD's place, to measure issue #303's cache
	 * across all three without needing to fake UE's renderer.
	 */
	void HoverFrameContextForTest() const { MakeHoverContext(); }

	/**
	 * Stands in for what Render() does to populate PendingLabels every frame it actually runs -
	 * same precedent as SetViewCentreDistanceForTest/HoverFrameContextForTest: a real
	 * IToolsContextRenderAPI/FPrimitiveDrawInterface needs a live viewport this headless harness
	 * does not have. Takes the active tool EXPLICITLY rather than through Sess().GetActiveTool(),
	 * so a counting spy - no production IBuildTool exposes a BuildPreview call count -
	 * can stand in for it; Airside.Editor.RenderCachesLabelsOnce is what actually counts,
	 * proving DrawHUD's read of PendingLabels (CollectPreviewLabelTextForTest below) costs no
	 * further calls - the bug review round 2 of issue #304 found (a SECOND, independent
	 * BuildPreview running from DrawHUD every frame, discarding what Render's own call had
	 * already produced).
	 */
	void CachePreviewLabelsForTest(IBuildTool& ActiveTool);

	/**
	 * The TEXT of every label DrawHUD would draw this frame, straight from PendingLabels -
	 * issue #304's own composition test (a too-short runway drag must put "too short" in this
	 * set) reads this after CachePreviewLabelsForTest (or a real Render) has filled the cache.
	 * Positions and styles are deliberately left out: the point under test is that the text
	 * REACHES the editor at all.
	 */
	TArray<FString> CollectPreviewLabelTextForTest() const;

	virtual void Setup() override;
	virtual void Shutdown(EToolShutdownType ShutdownType) override;
	virtual void Render(IToolsContextRenderAPI* RenderAPI) override;
	virtual void DrawHUD(FCanvas* Canvas, IToolsContextRenderAPI* RenderAPI) override;

	/** Escape. Drops a road chain or a half-drawn apron; see FRoadBuildEdModeCommands. */
	void CancelGesture();

	/**
	 * Ctrl+Z / Ctrl+Y in the editor. Tells the active build tool to abandon whatever it had
	 * part-drawn - the graph an undo or redo just changed may no longer hold the node or
	 * segment it was chaining from.
	 *
	 * ISSUE #191/#92-#93: ARoadBuildController::OnUndo/OnRedo have always called
	 * Tool->OnDeactivate for exactly this reason; this mode had no FEditorUndoClient at all
	 * until now (URoadBuildEdMode::PostUndo/PostRedo call this), so an editor Ctrl+Z that
	 * removed a node FRoadDrawTool was chaining from left it still holding one. Same shape as
	 * Shutdown's own deactivate block above, kept separate rather than shared: Shutdown also
	 * owns a mid-drag transaction this call has no business touching (GEditor's own undo
	 * transaction is what got the mode here, not a drag this instance is mid-way through).
	 */
	void DeactivateOnUndo();

	/**
	 * Enter. Commits whatever the active tool has staged - see FRoadBuildEdModeCommands::Build
	 * and issue #185.
	 *
	 * REACHABLE AT ANY MOMENT, like CancelGesture: this class decides nothing about whether a
	 * commit makes sense right now, because IBuildTool::OnCommit already ignores the call in
	 * every stage but the one it means something in (FPlotPlaceTool::OnCommit checks its own
	 * Stage) - the same contract ARoadBuildController::OnBuild relies on in PIE. A second
	 * guard here would be a second copy of a rule the tool already owns.
	 */
	void CommitGesture();

	/**
	 * Runs Verb.Apply against the shared session at the last known cursor - the editor's own
	 * door onto BuildVerbRegistry(), the same shape CancelGesture/CommitGesture already are for
	 * the two verbs that predate it (issue #304). Called from URoadBuildEdMode's mapped
	 * commands for Remove/Insert/Edit, which is what makes the sticky EGestureMode trio
	 * reachable in the editor for the first time - GetActiveTool() already returns FEditTool the
	 * moment the session's mode is Edit (FBuildSession::GetActiveTool's own comment), so nothing
	 * else has to change for a drag or a merge to reach it once the mode itself does.
	 */
	void ApplyVerb(const FBuildVerbRegistration& Verb);

	/**
	 * Draws the graph that already exists - nodes by degree, stands by heading.
	 *
	 * The runtime HUD has always done this; the editor never did, which is why existing
	 * nodes could not be seen, moved or removed, and why a snap had nothing visible to
	 * attach to. The tool's own preview draws on top of this.
	 */
	void DrawPersistentState(IToolPreviewSink& Sink) const;

	// --- IClickDragBehaviorTarget ------------------------------------------------------
	virtual FInputRayHit CanBeginClickDragSequence(const FInputDeviceRay& PressPos) override;
	virtual void OnClickPress(const FInputDeviceRay& PressPos) override;
	virtual void OnClickDrag(const FInputDeviceRay& DragPos) override;
	virtual void OnClickRelease(const FInputDeviceRay& ReleasePos) override;
	virtual void OnTerminateDragSequence() override;

	// --- IHoverBehaviorTarget ----------------------------------------------------------
	virtual FInputRayHit BeginHoverSequenceHitTest(const FInputDeviceRay& PressPos) override;
	virtual void OnBeginHover(const FInputDeviceRay& DevicePos) override {}
	virtual bool OnUpdateHover(const FInputDeviceRay& DevicePos) override;
	virtual void OnEndHover() override {}

	/** Modifier ids, so the behaviours can report ctrl and shift back to us. */
	static const int32 RemoveModifierId = 1;

	/** Alt: suspend every guide for this drag. Third, because Remove is 1 and Insert is 2. */
	static const int32 SuspendModifierId = 3;
	static const int32 InsertModifierId = 2;
	virtual void OnUpdateModifierState(int ModifierID, bool bIsOn) override;

private:
	/**
	 * RAII around GEditor->BeginTransaction/EndTransaction, calling Modify() on the actor
	 * and its network together - the shape OnClickDrag, OnClickRelease and CancelGesture
	 * each wrote out by hand (#103), one of them (OnClickDrag) with an early return between
	 * the two that could otherwise leave a transaction open. EndTransaction fires from the
	 * destructor rather than from a matching call every path through the caller has to
	 * remember to reach.
	 *
	 * Bodies live in the .cpp, where ARoadNetworkActor is a complete type - only the
	 * pointer is named here, which is all a declaration needs.
	 */
	struct FScopedRoadBuildTransaction
	{
		FScopedRoadBuildTransaction(const FText& SessionName, ARoadNetworkActor* InTarget);
		~FScopedRoadBuildTransaction();

		/** Undo-stack no-op instead of committing - the mid-drag Escape case. */
		void Cancel();

		FScopedRoadBuildTransaction(const FScopedRoadBuildTransaction&) = delete;
		FScopedRoadBuildTransaction& operator=(const FScopedRoadBuildTransaction&) = delete;

	private:
		bool bCancelled = false;
	};

	/** Where a ray meets the road plane. False when it is parallel or points away. */
	bool RayToPlane(const FRay& Ray, FVector2D& OutPosition) const;

	/** Everything the tool needs to judge this position, built fresh each event. */
	FToolContext MakeContext(const FInputDeviceRay& At) const;

	/**
	 * Context for the last known cursor, for callers that have no ray - Render, cancel,
	 * deactivate.
	 *
	 * These used to pass a default-constructed FRay as a "no ray" sentinel. FRay defaults
	 * its direction to (0,0,1), so it hit the road plane at the world origin and reported
	 * SUCCESS, and every preview was drawn against (0,0) while the model stayed correct.
	 */
	FToolContext MakeHoverContext() const;

	/** The shared body of both: everything that follows from a plane position. */
	FToolContext MakeContextAt(const FVector2D& Plane) const;

	/** The network actor in the editor world, created if the level has none. */
	ARoadNetworkActor* ResolveTarget() const;

	int32 ToolIndex = 0;

	/**
	 * The mode's session, or this instance's own when none was supplied.
	 *
	 * Session.Tools holds every registry entry, of which only the one at ToolIndex is ever
	 * asked for: wasteful in tool COUNT, cheap in reality, since these are small state
	 * machines with nothing expensive to construct. The alternative - a second, editor-only
	 * way to make just one - is exactly the kind of second copy issue #33 exists to remove.
	 *
	 * OwnSession IS LAZY (issue #190): ITF builds a new UObject per activation, and every
	 * shipping path hands one of THESE a SharedSession before Sess() is ever called - so a
	 * value-typed OwnSession used to construct a full nine-tool FBuildSession (FRoadDrawTool
	 * x2, FApronDrawTool, FStandPlotTool, FGuidelineDrawTool, FRunwayTool, FHoldingPointTool,
	 * FPlotPlaceTool, FSelectTool) on EVERY activation and throw it away unread the moment
	 * SharedSession was set. Building it only the one time Sess() is actually called with no
	 * SharedSession - a tool used outside the mode - keeps the fallback this comment already
	 * argued for, at the cost it was supposed to have.
	 */
	FBuildSession& Sess()
	{
		if (SharedSession != nullptr)
		{
			return *SharedSession;
		}
		if (!OwnSession.IsValid())
		{
			OwnSession = MakeUnique<FBuildSession>();
		}
		return *OwnSession;
	}
	const FBuildSession& Sess() const
	{
		if (SharedSession != nullptr)
		{
			return *SharedSession;
		}
		if (!OwnSession.IsValid())
		{
			OwnSession = MakeUnique<FBuildSession>();
		}
		return *OwnSession;
	}

	/** Set by the builder from URoadBuildEdMode::GetSession. Null only outside the mode. */
	FBuildSession* SharedSession = nullptr;

	/**
	 * The fallback, used only when this tool was built without a mode - which no shipping
	 * path does. Kept rather than asserting so a tool constructed in isolation still works,
	 * and because a null session would crash where a private one merely loses state nobody
	 * outside the mode is keeping.
	 *
	 * TUniquePtr, not a value member - see Sess()'s own comment. Mutable so the const overload
	 * can build it too: a first call to Sess() const with no SharedSession is exactly as much
	 * a "first use" as the non-const overload's, and there is no way to know which overload a
	 * caller will reach for first.
	 */
	mutable TUniquePtr<FBuildSession> OwnSession;

	UPROPERTY()
	TObjectPtr<ARoadNetworkActor> Target;

	// Press, travel, release - the same click-or-drag question the PlayerController asks,
	// asked again here because it is a fact about the mouse rather than about the tool.
	// FBuildGesture (Tool/BuildGesture.h) is the recogniser itself, shared with
	// ARoadBuildController - see issue #92; this class keeps only the ITF-specific parts
	// (transactions, ray/plane resolution).
	FBuildGesture Gesture;

	/**
	 * Owns the one transaction a drag opens in OnClickDrag's DragBegan branch, closed
	 * (destroyed, committing) in OnClickRelease's DragEnd branch or cancelled in
	 * OnTerminateDragSequence - the only one of the three transactions whose lifetime
	 * spans more than one call, so it needs somewhere to live between them.
	 */
	TUniquePtr<FScopedRoadBuildTransaction> DragTransaction;

	bool bRemoveHeld = false;
	bool bSuspendHeld = false;
	bool bInsertHeld = false;

	/**
	 * Whether RayToPlane resolved a real hover position this session.
	 *
	 * The position itself lives on FBuildSession (RecordPlaneHit/LastPlaneHit), shared with
	 * ARoadBuildController rather than kept as this class's own HoverPosition - the two used
	 * to be separate copies of the same fallback (issue #92). This bool is NOT part of that
	 * fallback: it gates whether Render has ever had a real hover to draw a preview at, which
	 * FBuildSession's shared position has no equivalent of before the first mouse move.
	 */
	bool bHoverValid = false;

	/**
	 * What the active tool's own BuildPreview call described this frame, as text - issue #304
	 * review round 2. FILLED BY Render (from the SAME BuildPreview call it already makes to
	 * draw markers/lines - see Render's own comment), never by DrawHUD: a second, independent
	 * BuildPreview from DrawHUD is exactly the doubled cost that comment fixes.
	 * CachePreviewLabelsForTest fills it the identical way for a headless test that cannot call
	 * Render for real. RESET, not left stale, whenever bHoverValid is false - see both fillers'
	 * own bodies.
	 */
	TArray<FEditorPreviewLabel> PendingLabels;

	/**
	 * World width the viewport currently spans at the cursor, refreshed each Render.
	 *
	 * Everything the preview measures - marker size, how close counts as "on" a point -
	 * is a fraction of this rather than a fixed number of uu. A tolerance in world units
	 * is either unusably tight zoomed out or absurdly loose zoomed in.
	 */
	double ViewWorldWidth = 10000.0;

	/**
	 * Distance from the camera to the road plane AT THE VIEW CENTRE, refreshed each Render -
	 * the same number Render already computed to derive ViewWorldWidth and used to discard
	 * (issue #191/#92-#93). RayToPlane multiplies this by RoadGeom::DefaultMaxPlaceDistanceFactor
	 * to cap a click the same way ARoadBuildController::CursorOnRoadPlane caps one against
	 * BuildCameraComp->ActiveRig().Distance - before this fix RayToPlane passed
	 * TNumericLimits<double>::Max() unconditionally, so a near-horizon click that PIE refused
	 * landed kilometres out here instead.
	 *
	 * NEGATIVE MEANS "NO MEASUREMENT YET": before the first Render call (the very first frame
	 * this tool is active) or while the current view is orthographic (every ray shares the
	 * camera's own direction there, so the horizon runaway this guards against cannot happen -
	 * see Render's own comment), RayToPlane must stay uncapped, exactly as it always was.
	 */
	double ViewCentreDistance = -1.0;
};
