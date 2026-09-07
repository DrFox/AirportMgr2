#pragma once

#include "CoreMinimal.h"
#include "Tool/RoadBuildTool.h"

/**
 * Placing aircraft stands: press to set the stop position, drag to aim it, release.
 *
 * NO state machine, and that is a decision rather than an omission. The road and apron
 * tools have states because their drawing PROGRESSES - a click means something different
 * depending on what has already been clicked, and each step carries different data. Placing
 * a stand is one gesture with one outcome: there is no second click that means something
 * new, so a state machine here would have exactly one state and would be describing
 * ceremony rather than behaviour.
 *
 * What the tool does hold is transient DRAG state, which is the same distinction the road
 * tool draws: a gesture in progress is not a step in a drawing.
 */
class AIRSIDE_API FStandPlaceTool : public IBuildTool
{
public:
	/**
	 * ONE TOOL, TWO REGISTRY ENTRIES - key 3 places a stand and key 0 places a fuel depot.
	 *
	 * The gesture is identical: press to set the pose, drag to aim it, release, click to
	 * commit; Ctrl+click removes whatever is under the cursor. Only the DEFINITION differs,
	 * and that is resolved by the facade from a kind (see EPlaceableEntity), never named
	 * here. A second class would be a copy of this one that must agree with it for ever -
	 * the duplication CLAUDE.md's "lists that must agree are ONE list" exists to prevent,
	 * applied to behaviour rather than to a table.
	 */
	explicit FStandPlaceTool(EPlaceableEntity InKind = EPlaceableEntity::Stand) : Kind(InKind) {}

	virtual FText GetDisplayName() const override;

	virtual void OnClick(const FToolContext& Context) override;
	virtual void OnCancel(const FToolContext& Context) override;
	virtual void OnDragBegin(const FToolContext& Context) override;
	virtual void OnDrag(const FToolContext& Context) override;
	virtual void OnDragEnd(const FToolContext& Context) override;
	virtual void OnDeactivate(const FToolContext& Context) override;
	virtual void BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const override;

	/** Nothing is ever part-placed: a stand exists after one gesture or not at all. */
	virtual bool IsIdle() const override { return !bAiming; }

private:
	/** Heading from the press point to the cursor, or LastHeading when they coincide. */
	double AimedHeading(const FToolContext& Context) const;

	/** Draw a stand's anchors as they would fall for a pose, without placing anything. */
	void PreviewPose(const FToolContext& Context, const FVector2D& At, double Heading,
		IToolPreviewSink& Sink) const;

	bool bAiming = false;

	/** Where the press landed - the stop position the stand will take. */
	FVector2D PressedAt = FVector2D::ZeroVector;

	/**
	 * Heading a click with no drag uses, and what a drag falls back to before it has
	 * travelled far enough to have a direction of its own.
	 *
	 * Remembered between placements because a row of stands on one pier all face the same
	 * way, and re-aiming each from scratch would be the most tedious possible way to say so.
	 */
	double LastHeading = 0.0;

	/** Which installation this tool drops. Fixed at construction by the registry entry that
	 *  made it - a tool is picked, never transitioned into, so this never changes. */
	EPlaceableEntity Kind = EPlaceableEntity::Stand;
};
