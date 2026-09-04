#pragma once

#include "CoreMinimal.h"

class URoadNetwork;
struct IToolPreviewSink;

/**
 * Draws the guideline graph - the routes agents follow, not the pavement they sit on.
 *
 * THE ONLY EMITTER of EPreviewStyle::Guideline. It used to live inside FRouteTool, which
 * meant the routing graph was visible only while the route tool was selected: you could not
 * see what you were building for while you were building it, and a defect at the
 * road/guideline boundary was invisible until someone happened to press 4.
 *
 * Kept OUT of any tool for that reason. A tool draws what its gesture is doing; this draws
 * context that is true whatever the gesture. Putting it back inside a tool would re-create
 * exactly the coupling that hid the problem.
 *
 * Free function rather than a class because it has no state - it is a projection of the
 * network onto a sink, and giving it an instance would invite one to start caching.
 */
namespace GuidelineOverlay
{
	/**
	 * Every alive guideline edge and node, in ROAD PLANE coordinates naming a meaning.
	 *
	 * UNFILTERED by traffic class, deliberately. This answers "what routing graph exists",
	 * which is true regardless of who is asking; whether a particular class may USE an edge
	 * is the route tool's question, and its hover marker already refuses to light up a node
	 * the current class cannot reach.
	 *
	 * Edges are sampled through GuidelineGeom::Sample and nothing else. That function is the
	 * single evaluator for this graph - the same array the search costs, this draws, and a
	 * follower walks - so a cube cannot leave the line the player was shown. A second
	 * sampler here would break that silently, and only on bends.
	 */
	AIRSIDE_API void Draw(const URoadNetwork& Network, IToolPreviewSink& Sink);
}
