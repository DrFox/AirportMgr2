#pragma once

#include "CoreMinimal.h"
#include "Model/RunwayFacts.h"
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
 *
 * SURFACE AND APPROACH are chosen beside the width and written onto every segment of the
 * strip at placement (FRunwayFacts). All three cycle on the tool's own key: plain for the
 * width, Shift (the insert modifier) for the surface, Ctrl (the remove modifier) for the
 * approach - see OnReselect. The modifiers are borrowed rather than new keys because the
 * bar already has buttons for them and a runway is placed rarely enough that three
 * cycles on one key read better than three keys.
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
	 * The key this tool is lit under, pressed again: cycle the width; with the insert
	 * modifier the surface; with the remove modifier the approach. THE CALLER NextWidth
	 * never had - see IBuildTool::OnReselect.
	 */
	virtual void OnReselect(const FToolContext& Context) override;

	/**
	 * Which standard width, as an index into the content set's RunwayProfiles.
	 *
	 * An index rather than a width in uu: the list IS the set of legal widths, so there is no
	 * value this can hold that names an illegal runway. Clamped on use, because the list is
	 * content and may be shorter than this expects.
	 */
	int32 WidthIndex = 0;

	/** Steps to the next standard width, wrapping. Called from OnReselect. */
	void NextWidth(const FToolContext& Context);

	/** What the next runway is paved with and what approach it offers - the facts placement writes. */
	ERunwaySurface Surface = ERunwaySurface::Tarmac;
	ERunwayApproach Approach = ERunwayApproach::Visual;

	/** Steps each scale, wrapping. */
	void NextSurface();
	void NextApproach();

	/** The facts the next placement writes, assembled from the two choices above. */
	FRunwayFacts Facts() const;

private:
	/** The chosen profile, or null when no content set is configured. */
	URoadProfile* ProfileForWidth() const;

	bool bHasThreshold = false;

	/** The first threshold, in road-plane coordinates. */
	FVector2D Threshold = FVector2D::ZeroVector;
};
