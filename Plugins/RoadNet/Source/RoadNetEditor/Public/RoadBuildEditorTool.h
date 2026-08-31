#pragma once

#include "CoreMinimal.h"
#include "InteractiveTool.h"
#include "InteractiveToolBuilder.h"
#include "BaseBehaviors/BehaviorTargetInterfaces.h"
#include "Tool/RoadBuildTool.h"
#include "RoadBuildEditorTool.generated.h"

class ARoadNetworkActor;

/** Which IBuildTool a builder should wrap. */
UENUM()
enum class ERoadBuildToolKind : uint8
{
	Road,
	Apron,
	Stand,
};

/**
 * Makes one adapter around one build tool.
 *
 * A single builder class parameterised by kind rather than three near-identical ones: what
 * differs between them is one line, and three classes to say it would be three places for
 * the wiring to drift.
 */
UCLASS()
class URoadBuildEditorToolBuilder : public UInteractiveToolBuilder
{
	GENERATED_BODY()

public:
	UPROPERTY()
	ERoadBuildToolKind Kind = ERoadBuildToolKind::Road;

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
	void SetKind(ERoadBuildToolKind InKind) { Kind = InKind; }

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

	ERoadBuildToolKind Kind = ERoadBuildToolKind::Road;

	/** The shared behaviour. Owned here; the framework owns this object. */
	TUniquePtr<IBuildTool> Build;

	UPROPERTY()
	TObjectPtr<ARoadNetworkActor> Target;

	// Press, travel, release - the same click-or-drag question the PlayerController asks,
	// asked again here because it is a fact about the mouse rather than about the tool.
	bool bPressed = false;
	bool bDragging = false;
	FVector2D PressScreen = FVector2D::ZeroVector;

	bool bRemoveHeld = false;
	bool bInsertHeld = false;

	/** Last cursor position on the plane, for previews between events. */
	FVector2D HoverPosition = FVector2D::ZeroVector;
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
