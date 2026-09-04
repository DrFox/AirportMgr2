#pragma once

#include "CoreMinimal.h"
#include "Tool/RoadBuildTool.h"

class URoadProfile;

/**
 * Lays a runway: click a threshold, click the other one.
 *
 * TWO CLICKS AND NO CHAINING, which is what "a runway must be straight" comes to in practice.
 * The road tool chains because a taxiway system is a graph you walk; a runway is a single
 * strip between two thresholds, so there is no third click to give a meaning to. Enforcing
 * straightness by refusing a curve would have been the same rule stated as a rejection
 * instead of as a shape.
 *
 * WIDTH IS CHOSEN, NOT TYPED. The tool holds an index into the content set's RunwayProfiles,
 * which are the ICAO standard widths. A runway conforms to one of them by construction rather
 * than by a validator that can be argued with.
 *
 * It reports the DESIGNATOR while you drag, because the number is the first thing that tells
 * you whether the strip is pointing where you meant - see RunwayDesignator, and note the
 * whole pair is shown, since a runway is named from both ends.
 */
class AIRSIDE_API FRunwayTool : public IBuildTool
{
public:
	virtual FText GetDisplayName() const override;

	virtual void OnClick(const FToolContext& Context) override;
	virtual void OnCancel(const FToolContext& Context) override;
	virtual void OnDeactivate(const FToolContext& Context) override;
	virtual void BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const override;

	virtual bool IsIdle() const override { return !bHasThreshold; }

	/**
	 * Which standard width, as an index into the content set's RunwayProfiles.
	 *
	 * An index rather than a width in uu: the list IS the set of legal widths, so there is no
	 * value this can hold that names an illegal runway. Clamped on use, because the list is
	 * content and may be shorter than this expects.
	 */
	int32 WidthIndex = 0;

	/** Steps to the next standard width, wrapping. Bound to a key by the driver. */
	void NextWidth(const FToolContext& Context);

private:
	/** The chosen profile, or null when no content set is configured. */
	URoadProfile* ProfileForWidth() const;

	bool bHasThreshold = false;

	/** The first threshold, in road-plane coordinates. */
	FVector2D Threshold = FVector2D::ZeroVector;
};
