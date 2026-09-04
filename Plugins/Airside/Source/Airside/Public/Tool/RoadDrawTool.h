#pragma once

#include "CoreMinimal.h"
#include "Tool/RoadBuildTool.h"

/**
 * One step of drawing a road - State, per design spec 7.2.
 *
 * A handler returns the state that replaces it, or an empty pointer to stay where it is.
 * Polymorphic states rather than an enum and a switch because the states carry DIFFERENT
 * DATA: chaining holds a pending node and whether it created it, and idle holds nothing.
 * An enum would put that data on the tool, alive in every state, which is how a flag ends
 * up being read in a state that never set it.
 *
 * BuildPreview is const for the reason spec 7.2 gives: a state cannot mutate the network
 * while drawing what it would do.
 */
struct AIRSIDE_API IRoadDrawState
{
	virtual ~IRoadDrawState() = default;

	virtual TUniquePtr<IRoadDrawState> OnClick(const FToolContext& Context) = 0;
	virtual TUniquePtr<IRoadDrawState> OnCancel(const FToolContext& Context) = 0;
	virtual void BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const = 0;

	virtual bool IsIdle() const = 0;

	/** The node a segment would run from, or INDEX_NONE. Read by the ghost. */
	virtual int32 GetPendingNode() const { return INDEX_NONE; }
};

/** Nothing part-drawn. A click puts down the start of a road. */
class AIRSIDE_API FRoadIdleState : public IRoadDrawState
{
public:
	virtual TUniquePtr<IRoadDrawState> OnClick(const FToolContext& Context) override;
	virtual TUniquePtr<IRoadDrawState> OnCancel(const FToolContext& Context) override;
	virtual void BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const override;
	virtual bool IsIdle() const override { return true; }
};

/**
 * A start node is down; the next click runs a segment to wherever it lands.
 *
 * Remembers whether THIS chain created the start node, because cancelling removes the node
 * the chain dropped and must not remove one that was already there.
 */
class AIRSIDE_API FRoadChainingState : public IRoadDrawState
{
public:
	FRoadChainingState(int32 InFrom, bool bInCreated) : From(InFrom), bCreated(bInCreated) {}

	virtual TUniquePtr<IRoadDrawState> OnClick(const FToolContext& Context) override;
	virtual TUniquePtr<IRoadDrawState> OnCancel(const FToolContext& Context) override;
	virtual void BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const override;
	virtual bool IsIdle() const override { return false; }
	virtual int32 GetPendingNode() const override { return From; }

private:
	int32 From = INDEX_NONE;
	bool bCreated = false;
};

/**
 * Drawing and editing roads: place, chain, split, delete, and drag a node about.
 *
 * Dragging is deliberately NOT a state. The states model the DRAWING PROGRESSION - what a
 * click means next - and a drag advances none of it: it edits geometry that already exists
 * and leaves the chain exactly as it found it. Making it a state would give every other
 * state a back-pointer to return to, which is a transition graph invented to fit a pattern
 * rather than to describe the tool.
 */
class AIRSIDE_API FRoadDrawTool : public IBuildTool
{
public:
	FRoadDrawTool();

	virtual FText GetDisplayName() const override;

	virtual void OnClick(const FToolContext& Context) override;
	virtual void OnCancel(const FToolContext& Context) override;
	virtual void OnDragBegin(const FToolContext& Context) override;
	virtual void OnDrag(const FToolContext& Context) override;
	virtual void OnDragEnd(const FToolContext& Context) override;
	virtual void Tick(const FToolContext& Context) override;
	virtual void OnDeactivate(const FToolContext& Context) override;
	virtual void BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const override;
	virtual bool IsIdle() const override;

	/** The node a segment would run from, or INDEX_NONE. For tests and the ghost. */
	int32 GetPendingNode() const;

private:
	/** Ctrl+click: remove whatever the snap chain resolved. */
	void Remove(const FToolContext& Context);

	/** What a Ctrl+click would take, and what it would put back. */
	void PreviewRemoval(const FToolContext& Context, IToolPreviewSink& Sink) const;

	TUniquePtr<IRoadDrawState> State;

	/** Node held by an in-progress drag, or INDEX_NONE. A gesture, not a drawing step. */
	int32 DragNode = INDEX_NONE;
};
