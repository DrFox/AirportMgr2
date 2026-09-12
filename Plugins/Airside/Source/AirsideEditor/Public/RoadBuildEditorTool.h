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

	virtual void Setup() override;
	virtual void Shutdown(EToolShutdownType ShutdownType) override;
	virtual void Render(IToolsContextRenderAPI* RenderAPI) override;

	/** Escape. Drops a road chain or a half-drawn apron; see FRoadBuildEdModeCommands. */
	void CancelGesture();

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
	static const int32 InsertModifierId = 2;
	virtual void OnUpdateModifierState(int ModifierID, bool bIsOn) override;

private:
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
	 */
	FBuildSession& Sess() { return SharedSession != nullptr ? *SharedSession : OwnSession; }
	const FBuildSession& Sess() const { return SharedSession != nullptr ? *SharedSession : OwnSession; }

	/** Set by the builder from URoadBuildEdMode::GetSession. Null only outside the mode. */
	FBuildSession* SharedSession = nullptr;

	/**
	 * The fallback, used only when this tool was built without a mode - which no shipping
	 * path does. Kept rather than asserting so a tool constructed in isolation still works,
	 * and because a null session would crash where a private one merely loses state nobody
	 * outside the mode is keeping.
	 */
	FBuildSession OwnSession;

	UPROPERTY()
	TObjectPtr<ARoadNetworkActor> Target;

	// Press, travel, release - the same click-or-drag question the PlayerController asks,
	// asked again here because it is a fact about the mouse rather than about the tool.
	// FBuildGesture (Tool/BuildGesture.h) is the recogniser itself, shared with
	// ARoadBuildController - see issue #92; this class keeps only the ITF-specific parts
	// (transactions, ray/plane resolution).
	FBuildGesture Gesture;

	bool bRemoveHeld = false;
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
	 * World width the viewport currently spans at the cursor, refreshed each Render.
	 *
	 * Everything the preview measures - marker size, how close counts as "on" a point -
	 * is a fraction of this rather than a fixed number of uu. A tolerance in world units
	 * is either unusably tight zoomed out or absurdly loose zoomed in.
	 */
	double ViewWorldWidth = 10000.0;
};
