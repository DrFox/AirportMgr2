#pragma once

#include "CoreMinimal.h"
#include "Tool/RoadBuildTool.h"

/**
 * What a closing-polygon gesture actually builds.
 *
 * EXTRACTED FROM FApronDrawTool when the fuel depot became a drawn plot rather than a
 * stamped box. The gesture is identical - click each corner, click the first again to
 * close, ctrl on open ground to remove - and only three things differ between an apron and
 * a plot: what a closed outline commits to, what ctrl removes, and what the doomed preview
 * outlines. Those three are this interface; everything else is shared.
 *
 * ONE STATE MACHINE AND NOT TWO, deliberately. A second copy of the closing rule and the
 * self-crossing rule would be two lists that must agree, and they would drift the first
 * time either was tuned - the player would meet one closing gesture on aprons and a subtly
 * different one on depots without ever being told why.
 */
struct AIRSIDE_API IOutlineTarget
{
	virtual ~IOutlineTarget() = default;

	/**
	 * Commit the closed outline. False leaves the outline IN PROGRESS rather than throwing
	 * it away, so a refusal does not cost the player eight corners of work.
	 */
	virtual bool Commit(const FToolContext& Context, const TArray<FVector2D>& Corners) const = 0;

	/** Ctrl on open ground with nothing part-drawn: remove whatever is under the cursor. */
	virtual bool RemoveUnderCursor(const FToolContext& Context) const = 0;

	/** Outline what RemoveUnderCursor would take, if anything is under the cursor. */
	virtual void PreviewRemoval(const FToolContext& Context, IToolPreviewSink& Sink) const = 0;
};

/**
 * One step of drawing a closed outline - State, alongside FRoadDrawTool's.
 *
 * The same argument for objects rather than an enum applies, and more strongly: outlining
 * carries a growing list of corners and idle carries nothing. On an enum that list would
 * live on the tool, present and readable in a state that never touched it.
 *
 * THE TARGET IS PASSED IN rather than held. A state that stored a reference to a member of
 * the tool that owns it would be binding to storage the tool's own constructor has not
 * reached yet; passing it per call has no such puzzle and keeps the states pure.
 */
struct AIRSIDE_API IOutlineDrawState
{
	virtual ~IOutlineDrawState() = default;

	virtual TUniquePtr<IOutlineDrawState> OnClick(
		const FToolContext& Context, const IOutlineTarget& Target) = 0;
	virtual TUniquePtr<IOutlineDrawState> OnCancel(
		const FToolContext& Context, const IOutlineTarget& Target) = 0;
	virtual void BuildPreview(const FToolContext& Context, const IOutlineTarget& Target,
		IToolPreviewSink& Sink) const = 0;

	virtual bool IsIdle() const = 0;

	/** Corners placed so far. Empty unless outlining. For tests. */
	virtual TArrayView<const FVector2D> GetCorners() const { return TArrayView<const FVector2D>(); }
};

/** Nothing part-drawn. A click puts down the first corner. */
class AIRSIDE_API FOutlineIdleState : public IOutlineDrawState
{
public:
	virtual TUniquePtr<IOutlineDrawState> OnClick(
		const FToolContext& Context, const IOutlineTarget& Target) override;
	virtual TUniquePtr<IOutlineDrawState> OnCancel(
		const FToolContext& Context, const IOutlineTarget& Target) override;
	virtual void BuildPreview(const FToolContext& Context, const IOutlineTarget& Target,
		IToolPreviewSink& Sink) const override;
	virtual bool IsIdle() const override { return true; }
};

/**
 * Corners are going down. Clicking the first one again closes the outline.
 *
 * The outline lives HERE and not in the model, because a part-drawn shape is not built
 * yet - putting it in the graph would make every intermediate click an undoable edit and
 * leave a half-polygon behind if the player wandered off.
 */
class AIRSIDE_API FOutlineDrawingState : public IOutlineDrawState
{
public:
	explicit FOutlineDrawingState(const FVector2D& First) { Corners.Add(First); }

	virtual TUniquePtr<IOutlineDrawState> OnClick(
		const FToolContext& Context, const IOutlineTarget& Target) override;
	virtual TUniquePtr<IOutlineDrawState> OnCancel(
		const FToolContext& Context, const IOutlineTarget& Target) override;
	virtual void BuildPreview(const FToolContext& Context, const IOutlineTarget& Target,
		IToolPreviewSink& Sink) const override;
	virtual bool IsIdle() const override { return false; }
	virtual TArrayView<const FVector2D> GetCorners() const override { return Corners; }

private:
	/** True when the cursor is near enough to the first corner to close on it. */
	bool WouldClose(const FToolContext& Context) const;

	/** True when running an edge to Where would cross one already placed. */
	bool WouldCross(const FVector2D& Where) const;

	TArray<FVector2D> Corners;
};

/**
 * A tool driven by the closing-polygon gesture. Subclasses supply a name and a target.
 *
 * GetOutlineTarget is a virtual rather than a constructor argument for the lifetime reason
 * on IOutlineDrawState: a base cannot be handed a reference to a member of the subclass
 * that has not been constructed yet.
 */
class AIRSIDE_API FOutlineDrawTool : public IBuildTool
{
public:
	FOutlineDrawTool();

	virtual void OnClick(const FToolContext& Context) override;
	virtual void OnCancel(const FToolContext& Context) override;
	virtual void OnDeactivate(const FToolContext& Context) override;
	virtual void BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const override;
	virtual bool IsIdle() const override;

	/** Corners placed so far. For tests. */
	TArrayView<const FVector2D> GetCorners() const;

	/**
	 * The last corner placed, and the edge it grew from. See IBuildTool::DescribeGuideAnchor.
	 *
	 * ON THE BASE, not on FApronDrawTool: what makes an anchor here is the OUTLINE gesture, which
	 * is this class's whole job, and a second outline tool would want the same answer. The
	 * anchor names "this edge" rather than "the apron" for the same reason - the base does not
	 * know what its outline will become.
	 *
	 * A BOUNDARY DRAG. Every corner is on the shape's own limit, so there is no pavement either
	 * side of it and the half-widths stay zero - see EDragPoint.
	 */
	virtual bool DescribeGuideAnchor(const URoadNetwork* Network, IRoadEditTarget* Target,
		FGuideAnchor& Out) const override;

	/**
	 * YES - start an outline flush with a road edge, or in line with an apron already down.
	 *
	 * ON THE BASE for the reason DescribeGuideAnchor is: the argument is about the OUTLINE
	 * gesture, which is this class's whole job, and a second outline tool would want the same
	 * answer. Ruled 2026-09-20 for the apron, which is the only subclass there is.
	 *
	 * CONSUMED IN FOutlineIdleState: its click takes the guided cursor and its preview draws
	 * the dashed line. See IBuildTool::WantsFreeStartGuides on why that half is not optional.
	 */
	virtual bool WantsFreeStartGuides() const override { return true; }

protected:
	virtual const IOutlineTarget& GetOutlineTarget() const = 0;

private:
	TUniquePtr<IOutlineDrawState> State;
};
