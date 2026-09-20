#pragma once

#include "CoreMinimal.h"
#include "Tool/RoadBuildTool.h"

/**
 * The one tool that runs while the session's mode is Edit - Strategy, like every other
 * IBuildTool, and reached the same way: through FBuildSession::GetActiveTool.
 *
 * IT BUILDS NOTHING, and that is the feature rather than an omission. Editing placed
 * geometry was ruled to require a deliberate act, and the symmetry that settled it cuts
 * both ways: you must not be able to build by accident while reaching for a node, any
 * more than you may move a node while reaching to build. So the mode SUPPRESSES the build
 * tool inside GetActiveTool rather than re-purposing its drags - nine tools each
 * remembering to refuse is nine places that must agree, and eight of them have no edit to
 * offer at all.
 *
 * NOT IN ToolRegistry(), deliberately. It is not picked by a number key and must not
 * appear on the tool row: it is the OTHER AXIS, and a tenth entry in that table would put
 * it in the row whose whole meaning is "which one of these am I holding".
 *
 * WHICH handles are grabbable is not this tool's choice. It reads
 * FToolContext::EditHandles, which MakeContext fills from the lit tool's registry entry -
 * so the Taxiway button lights taxiway nodes and the Apron button lights apron corners,
 * and that mapping lives in the one table rather than in a switch here that would have to
 * agree with it.
 */
class AIRSIDE_API FEditTool : public IBuildTool
{
public:
	virtual FText GetDisplayName() const override;

	/** Nothing. Edit does not build - see the class comment. */
	virtual void OnClick(const FToolContext& Context) override {}

	/** Nothing yet. A drag is ended by releasing it, not by cancelling; once a selection
	 *  exists this is where it clears. */
	virtual void OnCancel(const FToolContext& Context) override {}

	virtual void BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const override;

	/**
	 * Always idle for now: nothing is part-drawn because nothing is drawn.
	 *
	 * It matters that this is true rather than merely unused - FBuildSession::
	 * CancelActiveGesture reads it, and an edit tool that claimed to be mid-gesture would
	 * swallow the first cancel and leave the player pressing escape twice to put the mode
	 * down. A drag makes this answer conditional; a drag is a gesture, not a drawing step.
	 */
	virtual bool IsIdle() const override { return true; }
};
