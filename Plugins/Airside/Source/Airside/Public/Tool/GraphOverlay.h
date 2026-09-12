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
 * not just the one that happens to be selected.
 *
 * Split into DescribeNodes/DescribeStands rather than one unconditional call like
 * GuidelineOverlay::Draw's: ARoadBuildHUD keeps bDrawNodes and bDrawStands as INDEPENDENT
 * toggles, and a single combined function would force them to rise and fall together the
 * first time either one drew something the other flag was meant to hide. Describe() calls
 * both, for the one caller that has no such toggle at all - RoadBuildEditorTool's viewport,
 * which always draws everything.
 *
 * Written to replace THREE independent renderings of the same two facts that had drifted:
 * ARoadBuildHUD::DrawNodes/DrawStands (its own StubColour/EndColour/JunctionColour degree
 * test, its own footprint/anchor transform) and RoadBuildEditorTool::DrawPersistentState
 * (its own Refused/Snap/Heal degree test, StandPreview::Describe for the stand itself but no
 * anchor ring at all). A defect fixed in one never reached the other two - see issue #95.
 *
 * Free functions, like GuidelineOverlay: none of the three carry state of their own, only a
 * projection of the network onto a sink.
 */
namespace GraphOverlay
{
	/**
	 * Every alive road node, in ROAD PLANE coordinates naming a meaning.
	 *
	 * Degree becomes EPreviewStyle::NodeStub/NodeThrough/NodeJunction - context, not intent,
	 * exactly like GuidelineOverlay's own ::Guideline.
	 */
	AIRSIDE_API void DescribeNodes(const URoadNetwork& Network, IToolPreviewSink& Sink);

	/**
	 * Every placed, alive entity, in ROAD PLANE coordinates naming a meaning.
	 *
	 * Footprint, service points and fixtures come from StandPreview::Describe - the SAME
	 * function a placement tool's own preview calls, so a placed stand and an in-progress
	 * one read as the same object rather than the two this was filed to fix. Each RESOLVED
	 * anchor (the guideline node a vehicle will actually route to, not the definition's
	 * local offset StandPreview also draws) becomes EPreviewStyle::ServiceAnchor. See the
	 * .cpp for why the entity's own EPreviewStyle::StandPose marker is emitted AFTER that
	 * call rather than before it.
	 */
	AIRSIDE_API void DescribeStands(const URoadNetwork& Network, IToolPreviewSink& Sink);

	/** DescribeNodes then DescribeStands - what a caller with no per-feature toggle wants. */
	AIRSIDE_API void Describe(const URoadNetwork& Network, IToolPreviewSink& Sink);
}
