#pragma once

#include "CoreMinimal.h"
#include "Model/RouteSearch.h"
#include "Tool/RoadBuildTool.h"

/**
 * Routing: click a start, click a destination, watch a cube drive it.
 *
 * The only tool that BUILDS NOTHING. Every other one edits the airport; this one asks it a
 * question.
 *
 * It used to draw the guideline graph itself, which made the thing being asked about
 * visible ONLY here - so a route drawn over an otherwise invisible graph showed that a path
 * existed while hiding every reason it went that way, and nothing else could see the graph
 * at all. GuidelineOverlay owns that now and draws it under every tool; this one draws only
 * its own query.
 *
 * Two states and no more, so this carries a bool rather than the IRoadDrawState machine the
 * road and apron tools use: a second click always completes the query, and there is no
 * third step that could mean something new.
 *
 * The plan outlives the gesture on purpose. After dispatching, the route stays drawn until
 * the next query, because the useful thing to look at is the line the cube is driving.
 */
class ROADNET_API FRouteTool : public IBuildTool
{
public:
	virtual FText GetDisplayName() const override;

	virtual void OnClick(const FToolContext& Context) override;
	virtual void OnCancel(const FToolContext& Context) override;
	virtual void OnDeactivate(const FToolContext& Context) override;
	virtual void BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const override;

	virtual bool IsIdle() const override { return !bHasStart; }

	/**
	 * What is being routed. Aircraft, because the destination that matters is a stand.
	 *
	 * Not read from the clicked node: a node admitting both aircraft and vans cannot say
	 * which one you meant, and guessing from the graph would make the same click mean
	 * different things on different taxiways.
	 */
	ETraversalClass Class = ETraversalClass::Aircraft;

private:
	/** The guideline node under the cursor that this class could use, if any. */
	FGuidelineNodeId PickNode(const FToolContext& Context) const;


	bool bHasStart = false;

	FGuidelineNodeId StartNode;

	/** The last query's answer, kept so a failure stays on screen with its reason. */
	FRoutePlan LastPlan;
};
