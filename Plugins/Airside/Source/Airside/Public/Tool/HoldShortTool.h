#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"
#include "Tool/RoadBuildTool.h"

/**
 * Places the hold bar a taxiing aircraft stops at before a runway. One click, no state.
 *
 * THE ONLY TOOL THAT EDITS A NODE RATHER THAN PAVEMENT. Every other one adds or removes
 * surface - a segment, an apron, a stand - and the guideline graph follows. This one adds
 * nothing to build: it flags a node that the derivation already made, so the airport looks
 * identical afterwards and only the traffic model reads any difference. That is why it is
 * idle by construction (IsIdle() is always true) and has nothing to cancel: there is no
 * part-drawn chain a second click could complete, and a first click is also the last.
 *
 * WHICH RUNWAY THE BAR PROTECTS IS ASKED OF THE NETWORK, NOT DECIDED HERE.
 * URoadNetwork::RunwayNearGuidelineNode is a fact about the graph - the one-hop walk over
 * derived edges past the turn paths that join a taxiway end to a runway centreline - and
 * facts about the graph belong to the graph, where they can be tested with no world, no
 * actor and no tool at all (Airside.Build.HoldShortSurvivesRebuild does exactly that). A
 * copy of that walk in here would be a second answer to the same question, and the two
 * would drift the first time the derivation changed.
 *
 * A SECOND CLICK CLEARS, rather than ctrl-click or a remove mode. One gesture means one
 * thing on this tool, so there is no sticky modifier to forget you left on - and the bar is
 * a toggle in the player's head ("is this junction guarded?"), which is exactly what a
 * click-to-flip reads as. The remove modifier the road tool uses is deliberately ignored.
 */
class AIRSIDE_API FHoldShortTool : public IBuildTool
{
public:
	virtual FText GetDisplayName() const override;

	virtual void OnClick(const FToolContext& Context) override;

	/** Nothing is part-drawn, so cancel only takes the refusal message off the screen. */
	virtual void OnCancel(const FToolContext& Context) override;

	virtual void BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const override;

	/** Always: a click completes in one step, so there is never a gesture in progress. */
	virtual bool IsIdle() const override { return true; }

	/**
	 * Why the last click was refused, or empty. Shown by the preview at the cursor.
	 *
	 * Kept on the tool rather than logged and forgotten, because "I clicked and nothing
	 * happened" is indistinguishable from "the click missed" unless the reason is on screen
	 * where the click was made. It is ALSO logged - see OnClick - so a report of it reaching
	 * AirportMgr.log can be checked without a screenshot.
	 */
	FString LastRefusal;

private:
	/** The guideline node under the cursor an aircraft could actually use, if any. */
	FGuidelineNodeId PickNode(const FToolContext& Context) const;
};
