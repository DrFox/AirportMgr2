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
 *
 * NOT AIRSIDE_API, deliberately (issue #191): nothing outside this module names IRoadDrawState,
 * FRoadIdleState or FRoadChainingState - only FRoadDrawTool itself, below, is a public seam
 * (built by RoadBuildController and the editor tool) and keeps its export.
 */
struct IRoadDrawState
{
	virtual ~IRoadDrawState() = default;

	virtual TUniquePtr<IRoadDrawState> OnClick(const FToolContext& Context) = 0;
	virtual TUniquePtr<IRoadDrawState> OnCancel(const FToolContext& Context) = 0;
	virtual void BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const = 0;

	virtual bool IsIdle() const = 0;

	/** The node a segment would run from, or INDEX_NONE. Read by the ghost. */
	virtual int32 GetPendingNode() const { return INDEX_NONE; }

	/**
	 * Which standard taxiway width this state's next click lays, INDEX_NONE for the
	 * level's default. See FRoadDrawTool::WidthIndex, which owns the choice.
	 *
	 * ON THE BASE, unlike Kind, and the difference is that this one CHANGES: cycling the
	 * width mid-chain has to reach the part-drawn state, which the tool can only do
	 * through this pointer. Kind is fixed at construction and never needs reaching.
	 */
	int32 WidthIndex = INDEX_NONE;
};

/** Nothing part-drawn. A click puts down the start of a road. */
class FRoadIdleState : public IRoadDrawState
{
public:
	/** Kind is carried by the STATE as well as by the tool because a state builds its own
	 *  successor, and the successor must lay the same cross-section this one started. */
	explicit FRoadIdleState(ERoadKind InKind = ERoadKind::Taxiway, int32 InWidthIndex = INDEX_NONE)
		: Kind(InKind) { WidthIndex = InWidthIndex; }

	virtual TUniquePtr<IRoadDrawState> OnClick(const FToolContext& Context) override;
	virtual TUniquePtr<IRoadDrawState> OnCancel(const FToolContext& Context) override;
	virtual void BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const override;
	virtual bool IsIdle() const override { return true; }

private:
	ERoadKind Kind = ERoadKind::Taxiway;
};

/**
 * A start node is down; the next click runs a segment to wherever it lands.
 *
 * Remembers whether THIS chain created the start node, because cancelling removes the node
 * the chain dropped and must not remove one that was already there.
 */
class FRoadChainingState : public IRoadDrawState
{
public:
	FRoadChainingState(int32 InFrom, bool bInCreated, ERoadKind InKind = ERoadKind::Taxiway,
		int32 InWidthIndex = INDEX_NONE)
		: From(InFrom), bCreated(bInCreated), Kind(InKind) { WidthIndex = InWidthIndex; }

	virtual TUniquePtr<IRoadDrawState> OnClick(const FToolContext& Context) override;
	virtual TUniquePtr<IRoadDrawState> OnCancel(const FToolContext& Context) override;
	virtual void BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const override;
	virtual bool IsIdle() const override { return false; }
	virtual int32 GetPendingNode() const override { return From; }

private:
	int32 From = INDEX_NONE;
	bool bCreated = false;

	/** See FRoadIdleState::Kind. */
	ERoadKind Kind = ERoadKind::Taxiway;

};

/**
 * Drawing roads: place, chain, split and delete.
 *
 * IT NO LONGER DRAGS A NODE. It did, and any press-and-travel over one reshaped the road
 * with no way to decline - mid-chain, a slightly-moved click on a junction moved the
 * junction instead of continuing from it. Editing placed geometry is a deliberate act now
 * and lives in FEditTool, behind the Edit mode. The argument for why a drag was not one of
 * the states below travelled with the code, to FEditTool::DragNode.
 */
class AIRSIDE_API FRoadDrawTool : public IBuildTool
{
public:
	/**
	 * ONE TOOL, TWO REGISTRY ENTRIES - key 1 lays a taxiway and key 9 lays a service road.
	 *
	 * Drawing a road and drawing a taxiway are the SAME gesture with the same states, the
	 * same snap chain and the same removal rules; only the cross-section differs. A second
	 * class would be a copy of two hundred lines that must agree with this one for ever,
	 * which is the duplication CLAUDE.md's "lists that must agree are ONE list" exists to
	 * prevent - applied here to behaviour rather than to a table.
	 */
	explicit FRoadDrawTool(ERoadKind InKind = ERoadKind::Taxiway);

	virtual FText GetDisplayName() const override;

	virtual void OnClick(const FToolContext& Context) override;
	virtual void OnCancel(const FToolContext& Context) override;
	virtual void Tick(const FToolContext& Context) override;
	virtual void OnDeactivate(const FToolContext& Context) override;
	virtual void BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const override;
	virtual bool IsIdle() const override;

	/** The node a segment would run from, or INDEX_NONE. For tests and the ghost. */
	int32 GetPendingNode() const;

	/** Which standard width the next click lays, or INDEX_NONE for the level's default. */
	int32 GetWidthIndex() const { return WidthIndex; }

	/** Selecting this tool while it is already active cycles the taxiway width - the same
	 *  gesture FRunwayTool::OnReselect gives runways. */
	virtual void OnReselect(const FToolContext& Context) override;

	/**
	 * The node the chain is drawing FROM, and the direction of the segment already arriving
	 * there - design section 3's "the incoming segment's direction, and its perpendicular".
	 *
	 * ASKS THE GRAPH, because FRoadChainingState holds only the node it draws from and never
	 * the one before it. See IBuildTool::DescribeGuideAnchor on why the network is a parameter
	 * where the context deliberately is not.
	 */
	virtual bool DescribeGuideAnchor(const URoadNetwork* Network, IRoadEditTarget* Target,
		FGuideAnchor& Out) const override;

	/**
	 * YES - start a road in line with an existing one, or a matching gap from a pair of them.
	 *
	 * BOTH REGISTRY ENTRIES, taxiway and service road: the argument is about the gesture and
	 * one class serves both. Already consumed, as it happens - a free click goes down at
	 * RoadGuidedSnap(Context).Position through ResolveToNode, and BuildPreview draws the dashed
	 * line whatever state the tool is in. See IBuildTool::WantsFreeStartGuides on why saying
	 * yes is only half the work.
	 */
	virtual bool WantsFreeStartGuides() const override { return true; }

private:
	/** Ctrl+click: remove whatever the snap chain resolved. */
	void Remove(const FToolContext& Context);

	/** What a Ctrl+click would take, and what it would put back. */
	void PreviewRemoval(const FToolContext& Context, IToolPreviewSink& Sink) const;

	TUniquePtr<IRoadDrawState> State;

	/** Which cross-section this tool lays. Fixed at construction by the registry entry that
	 *  made it - a tool is picked, never transitioned into, so this never changes. */
	ERoadKind Kind = ERoadKind::Taxiway;

	/**
	 * Which standard taxiway width the next click lays, or INDEX_NONE for the level's own
	 * default.
	 *
	 * ON THE TOOL, not on FToolContext, for the reason ERoadKind gives about itself: it is
	 * a fact about the TOOL the player selected rather than about the gesture, and a
	 * context field would let two tools disagree about it.
	 *
	 * STARTS UNSET so a player who never presses the key again lays exactly the road this
	 * level was tuned for - see ARoadNetworkActor::ResolveProfile, whose comment records
	 * what happened the last time a default was quietly overridden.
	 *
	 * A SERVICE ROAD NEVER SETS IT: that kind has one authored cross-section, so the cycle
	 * refuses rather than laying a taxiway's width on a lane meant for vans.
	 */
	int32 WidthIndex = INDEX_NONE;
};
