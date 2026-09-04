#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"
#include "Tool/RoadBuildTool.h"

/** Whether a hand-drawn link may be made, and if not, why not. */
enum class EGuidelineLink : uint8
{
	Valid,

	/** Nothing usable under the first click. */
	NoStart,

	/** Both ends are the same node. A link from a node to itself connects nothing. */
	SameNode,

	/** A live edge already runs between these two nodes. */
	AlreadyJoined,
};

/**
 * Draws a routing connection the derivation never made.
 *
 * The graph is derived from pavement, so it can only ever connect what pavement connects.
 * A stand whose lead-in missed, a taxiway across an apron, two networks meant to meet -
 * none of those are expressible by drawing more road, and all of them are one edge.
 *
 * Two clicks and no more, so this carries a bool rather than the IRoadDrawState machine the
 * road and apron tools use - the second click always completes the link.
 *
 * The edge it creates is bDerived == false, which is what makes the builder step aside for
 * it, and carries the ENDPOINTS' identities so it is re-attached to each fresh derivation
 * rather than left pointing at nodes nothing else uses. See FGuidelineEndRef: an edge that
 * stored raw handles survived every rebuild and was connected to none of them.
 */
class ROADNET_API FGuidelineDrawTool : public IBuildTool
{
public:
	virtual FText GetDisplayName() const override;

	virtual void OnClick(const FToolContext& Context) override;
	virtual void OnCancel(const FToolContext& Context) override;
	virtual void OnDeactivate(const FToolContext& Context) override;
	virtual void BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const override;

	virtual bool IsIdle() const override { return !bHasStart; }

	/** Whether From may be linked to To. Answerable before anything is built. */
	static EGuidelineLink Validate(const URoadNetwork& Network,
		FGuidelineNodeId From, FGuidelineNodeId To);

	/** Short player-facing reason, for the overlay. */
	static const TCHAR* Describe(EGuidelineLink Result);

private:
	/**
	 * The guideline node under the cursor - ANY alive one.
	 *
	 * Deliberately not RouteSearch::FindNearestNode, which filters by traversal class and
	 * requires the node already be incident to a usable edge. An isolated node is invisible
	 * to that, and an isolated node is exactly the one you are here to connect.
	 */
	FGuidelineNodeId PickNode(const FToolContext& Context) const;

	bool bHasStart = false;
	FGuidelineNodeId StartNode;
};
