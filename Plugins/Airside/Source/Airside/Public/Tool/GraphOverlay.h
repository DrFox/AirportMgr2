#pragma once

#include "CoreMinimal.h"

class URoadNetwork;
struct IToolPreviewSink;

/**
 * Draws the COMMITTED graph - live road nodes and placed entities - the model already holds.
 *
 * Companion to GuidelineOverlay::Draw: that one draws the derived routing graph, this one
 * draws the road graph and whatever is placed on it. Both exist for the same reason - a fact
 * about the airport that is true whatever tool is active must be visible under every tool,
 * not just the one that happens to be selected - and both are unconditional, with no
 * per-feature toggle inside the overlay itself: see GuidelineOverlay.h for why.
 *
 * Written to replace THREE independent renderings of the same two facts that had drifted:
 * ARoadBuildHUD::DrawNodes/DrawStands (its own StubColour/EndColour/JunctionColour degree
 * test, its own footprint/anchor transform) and RoadBuildEditorTool::DrawPersistentState
 * (its own Refused/Snap/Heal degree test, StandPreview::Describe for the stand itself but no
 * anchor ring at all). A defect fixed in one never reached the other two - see issue #95.
 *
 * Free function, like GuidelineOverlay: it has no state of its own, only a projection of the
 * network onto a sink.
 */
namespace GraphOverlay
{
	/**
	 * Every alive road node and placed entity, in ROAD PLANE coordinates naming a meaning.
	 *
	 * Node degree becomes EPreviewStyle::NodeStub/NodeThrough/NodeJunction - context, not
	 * intent, exactly like GuidelineOverlay's own ::Guideline. A placed entity's committed
	 * pose is EPreviewStyle::StandPose; its footprint, service points and fixtures come from
	 * StandPreview::Describe - the SAME function a placement tool's own preview calls, so a
	 * placed stand and an in-progress one read as the same object rather than the two this
	 * was filed to fix - and each RESOLVED anchor (the guideline node a vehicle will actually
	 * route to, not the definition's local offset StandPreview also draws) becomes
	 * EPreviewStyle::ServiceAnchor.
	 */
	AIRSIDE_API void Describe(const URoadNetwork& Network, IToolPreviewSink& Sink);
}
