#pragma once

#include "CoreMinimal.h"
#include "Tool/RoadBuildTool.h"

/**
 * One grabbable point, and what it belongs to.
 *
 * A CORNER IS NOT A NODE, and squeezing one into a node index is how a wrong index reaches
 * MoveNode and silently moves something else. Two owners and two mutators, so the type
 * carries which - CLAUDE.md's "a phase is an enum, never a set of bools", applied to a
 * handle that would otherwise be an int and a second int that is only sometimes meaningful.
 */
struct FEditHandle
{
	enum class EKind : uint8
	{
		None,

		/** Owner is a road node slot index. Corner is unused. */
		Node,

		/** Owner is an apron slot index; Corner is the index into its outline. */
		ApronCorner,
	};

	EKind Kind = EKind::None;
	int32 Owner = INDEX_NONE;
	int32 Corner = INDEX_NONE;

	bool IsSet() const { return Kind != EKind::None; }
	void Clear() { Kind = EKind::None; Owner = INDEX_NONE; Corner = INDEX_NONE; }

	/** Where it sits on the road plane, or false when whatever owned it has gone. */
	bool PositionIn(const URoadNetwork& Network, FVector2D& Out) const;
};

/**
 * The one tool that runs while the session's mode is Edit - Strategy, like every other
 * IBuildTool, and reached the same way: through FBuildSession::GetActiveTool.
 *
 * IT BUILDS NOTHING, and that is the feature rather than an omission. Editing placed
 * geometry was ruled to require a deliberate act, and the symmetry that settled it cuts
 * both ways: you must not be able to build by accident while reaching for a node, any
 * more than you may move a node while reaching to build. So the mode SUPPRESSES the build
 * tool inside GetActiveTool rather than re-purposing its drags - nine tools each
 * remembering to refuse is nine places that must agree, and eight of them have no edit to
 * offer at all.
 *
 * NOT IN ToolRegistry(), deliberately. It is not picked by a number key and must not
 * appear on the tool row: it is the OTHER AXIS, and a tenth entry in that table would put
 * it in the row whose whole meaning is "which one of these am I holding".
 *
 * WHICH handles are grabbable is not this tool's choice. It reads
 * FToolContext::EditHandles, which MakeContext fills from the lit tool's registry entry -
 * so the Taxiway button lights taxiway nodes and the Apron button lights apron corners,
 * and that mapping lives in the one table rather than in a switch here that would have to
 * agree with it.
 */
class AIRSIDE_API FEditTool : public IBuildTool
{
public:
	virtual FText GetDisplayName() const override;

	/**
	 * Ctrl+click removes the handle under the cursor, healing as the build tools do. A plain
	 * click does nothing: Edit does not build, and it has no selection to make yet.
	 *
	 * CTRL STILL WORKS HERE THOUGH REMOVE IS A MODE THIS ONE EXCLUDES, and the two are not
	 * in conflict: EGestureMode is the STICKY choice, and a HELD key is a different gesture
	 * for a shorter piece of work. MakeContext ORs them, so holding Ctrl inside Edit sets
	 * bRemoveModifier without Edit ceasing to be the mode.
	 */
	virtual void OnClick(const FToolContext& Context) override;

	/** Nothing yet. A drag ends by being released, not cancelled; once a selection exists
	 *  this is where it clears. */
	virtual void OnCancel(const FToolContext& Context) override {}

	virtual void OnDragBegin(const FToolContext& Context) override;
	virtual void OnDrag(const FToolContext& Context) override;
	virtual void OnDragEnd(const FToolContext& Context) override;
	virtual void OnDeactivate(const FToolContext& Context) override;

	virtual void BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const override;

	/**
	 * The DRAG's anchor - what the node in hand may line up with.
	 *
	 * FREE-START SHAPED, and for the real reason rather than by analogy: the thing moving IS
	 * the node, so there is no fixed origin the way a chain has one. The driver puts the
	 * cursor in Origin, and SnapGuide::Arbitrate then measures no direction from a point to
	 * itself, so every angular candidate sits out unaided and exactly the positional ones
	 * remain. See FGuideAnchor::bFreeStart.
	 *
	 * DECLINES WHEN NOTHING IS IN HAND, rather than delegating to the base's free start. With
	 * no drag there is no gesture, and guiding the cursor that is about to GRAB something
	 * would offer to reposition a click that never moves anything.
	 */
	virtual bool DescribeGuideAnchor(const URoadNetwork* Network, IRoadEditTarget* Target,
		FGuideAnchor& Out) const override;

	/**
	 * Idle unless a drag is live.
	 *
	 * It matters that this is honest rather than merely unused - FBuildSession::
	 * CancelActiveGesture reads it, and a tool that claimed to be mid-gesture with nothing
	 * in hand would swallow the first cancel and leave the player pressing escape twice.
	 */
	virtual bool IsIdle() const override { return !Drag.IsSet(); }

	/**
	 * The node in hand, so MakeContext can keep the snap chain off it.
	 *
	 * ONLY FOR A NODE DRAG. An apron corner is not in the road graph and the snap chain has
	 * nothing to exclude for it - answering with its owner index here would exclude an
	 * unrelated NODE that happened to share the number.
	 */
	virtual int32 GetSnapExclusion() const override
	{
		return Drag.Kind == FEditHandle::EKind::Node ? Drag.Owner : INDEX_NONE;
	}

	/** The node a live drag is moving, or INDEX_NONE. For tests and the overlay. */
	int32 GetDragNode() const
	{
		return Drag.Kind == FEditHandle::EKind::Node ? Drag.Owner : INDEX_NONE;
	}

	/** What a live drag is holding. */
	const FEditHandle& GetDragHandle() const { return Drag; }

	/**
	 * Every node slot the lit tool exposes, in index order.
	 *
	 * STATIC, so the preview and the drag cannot disagree about what is grabbable: the one
	 * answer, asked twice. A test can call it directly for the same reason.
	 *
	 * NODES ONLY. An apron corner is not a node and gets its own path rather than being
	 * squeezed into a node index - a wrong index reaching MoveNode is a silent move of the
	 * wrong thing.
	 */
	static void GatherNodeHandles(const FToolContext& Context, TArray<int32>& Out);

	/**
	 * Every handle the lit tool exposes, of whatever kind - what the preview draws and what
	 * a press may grab.
	 *
	 * ONE GATHERER, so those two cannot come to disagree about what is grabbable. The node
	 * accessor above remains because the guide anchor and the snap exclusion are node
	 * questions specifically.
	 */
	static void GatherHandles(const FToolContext& Context, TArray<FEditHandle>& Out);

private:
	/**
	 * What a live drag is holding, or an unset handle.
	 *
	 * A GESTURE, NOT A DRAWING STEP - the distinction FRoadDrawTool's header used to draw
	 * about its own drag, and which travelled here with the code. A drag advances no
	 * progression: it edits geometry that already exists and leaves everything else exactly
	 * as it found it. Modelling it as a state would give every other state a back-pointer
	 * to return to, which is a transition graph invented to fit a pattern rather than to
	 * describe the tool.
	 */
	FEditHandle Drag;
};
