#pragma once

#include "CoreMinimal.h"
#include "Tool/RoadBuildTool.h"

/**
 * What a Ctrl gesture takes, and the picture of it - together, because they must agree.
 *
 * ONE ANSWER TO "WHAT WOULD THIS REMOVE", shared by FRoadDrawTool and FEditTool. Both offer
 * the gesture and a copy in each is the duplication CLAUDE.md's "lists that must agree are
 * ONE list" exists to prevent, applied to behaviour rather than to a table. The failure it
 * prevents is specific and invisible: a preview drawn by one routine and a deletion
 * performed by another look plausible separately and disagree the day either is edited -
 * exactly what URoadNetwork::SplitSegment's own comment records about the ghost.
 *
 * Describe ASKS THE MODEL for the plan rather than working one out, so the heal drawn and
 * the heal performed are the same computation, refusal included.
 */
namespace RemoveGesture
{
	/**
	 * Draw what a Ctrl gesture at Context.Snap would take, and what it would put back.
	 *
	 * Silent on open ground: nothing to remove is the correct outcome, not a refusal.
	 */
	AIRSIDE_API void Describe(const FToolContext& Context, IToolPreviewSink& Sink);

	/**
	 * Take it. True when something was removed, so a caller with state of its own knows
	 * whether to reconsider it - FRoadDrawTool's chain may have been running from the node
	 * that just went.
	 */
	AIRSIDE_API bool Apply(const FToolContext& Context);
}
