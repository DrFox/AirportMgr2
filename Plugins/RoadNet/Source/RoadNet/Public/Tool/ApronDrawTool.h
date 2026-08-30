#pragma once

#include "CoreMinimal.h"
#include "Tool/RoadBuildTool.h"

/**
 * One step of drawing an apron - State, alongside FRoadDrawTool's.
 *
 * The same argument for objects rather than an enum applies, and more strongly: outlining
 * carries a growing list of corners and idle carries nothing. On an enum that list would
 * live on the tool, present and readable in a state that never touched it.
 */
struct ROADNET_API IApronDrawState
{
	virtual ~IApronDrawState() = default;

	virtual TUniquePtr<IApronDrawState> OnClick(const FToolContext& Context) = 0;
	virtual TUniquePtr<IApronDrawState> OnCancel(const FToolContext& Context) = 0;
	virtual void BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const = 0;

	virtual bool IsIdle() const = 0;

	/** Corners placed so far. Empty unless outlining. For tests. */
	virtual TArrayView<const FVector2D> GetCorners() const { return TArrayView<const FVector2D>(); }
};

/** Nothing part-drawn. A click puts down the first corner. */
class ROADNET_API FApronIdleState : public IApronDrawState
{
public:
	virtual TUniquePtr<IApronDrawState> OnClick(const FToolContext& Context) override;
	virtual TUniquePtr<IApronDrawState> OnCancel(const FToolContext& Context) override;
	virtual void BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const override;
	virtual bool IsIdle() const override { return true; }
};

/**
 * Corners are going down. Clicking the first one again closes the outline.
 *
 * The outline lives HERE and not in the model, because a part-drawn apron is not pavement
 * yet - putting it in the graph would make every intermediate click an undoable edit and
 * leave a half-polygon behind if the player wandered off.
 */
class ROADNET_API FApronOutliningState : public IApronDrawState
{
public:
	explicit FApronOutliningState(const FVector2D& First) { Corners.Add(First); }

	virtual TUniquePtr<IApronDrawState> OnClick(const FToolContext& Context) override;
	virtual TUniquePtr<IApronDrawState> OnCancel(const FToolContext& Context) override;
	virtual void BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const override;
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
 * Drawing an apron: a polygon of pavement with no cross-section and no junctions.
 *
 * Its own tool rather than a mode of the road tool, because almost nothing is shared - an
 * apron has no profile, no bands, no solve, and its input is a closing polygon rather than
 * a chain. Selected with 2; roads are 1.
 */
class ROADNET_API FApronDrawTool : public IBuildTool
{
public:
	FApronDrawTool();

	virtual FText GetDisplayName() const override;

	virtual void OnClick(const FToolContext& Context) override;
	virtual void OnCancel(const FToolContext& Context) override;
	virtual void OnDeactivate(const FToolContext& Context) override;
	virtual void BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const override;
	virtual bool IsIdle() const override;

	/** Corners placed so far. For tests. */
	TArrayView<const FVector2D> GetCorners() const;

private:
	TUniquePtr<IApronDrawState> State;
};
