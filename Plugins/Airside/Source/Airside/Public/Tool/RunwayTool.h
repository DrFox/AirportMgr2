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
 * WIDTH IS CHOSEN, NOT TYPED. The tool holds an index into the target's runway profiles -
 * IRoadEditTarget::GetRunwayProfileCount/ResolveRunwayProfile, the ICAO standard widths (see
 * that interface's own comment for why this tool does not read UAirsideContent itself, issue
 * #78). A runway conforms to one of them by construction rather than by a validator that can
 * be argued with.
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
	 * Three rows, in OnReselect's modifier order so the row a modifier moves is the row at that
	 * index: Width (plain), Surface (Shift), Approach (Ctrl). NO WIDTH ROW when the target has no
	 * runway profiles - surface and approach are this tool's own enums and stay choosable.
	 */
	virtual void GetVariantAxes(const FToolContext& Context, TArray<FToolVariantAxis>& Out) const override;

	/** Sets the field the named row stands for. Axis is by Id, not index - see the .cpp. */
	virtual bool SelectVariant(const FToolContext& Context, int32 Axis, int32 Option) override;

	/**
	 * The first threshold, once one is down - and a FREE START before that. See
	 * IBuildTool::DescribeGuideAnchor.
	 *
	 * NO REFERENCE, DELIBERATELY, in either state. A runway is two clicks and no chaining -
	 * there is no incoming edge to extend - so FExtendingGuideSource and FPointAlignGuideSource
	 * both correctly propose nothing, and every network column answers instead. Leaving
	 * Reference zero is how a tool says that; inventing an axis here would square the strip to
	 * something arbitrary.
	 *
	 * THE WIDTH IS CARRIED IN BOTH STATES, which is what lets the strip's EDGE sit flush along
	 * an apron on the very first click as readily as on the second.
	 */
	virtual bool DescribeGuideAnchor(const URoadNetwork* Network, IRoadEditTarget* Target,
		FGuideAnchor& Out) const override;

	/**
	 * YES - a threshold placed in line with another runway, or a standard separation off one.
	 *
	 * Ruled 2026-09-20. Already consumed: OnClick has taken Context.GuidedCursor() for BOTH
	 * clicks since the guide-and-snap split, and the idle branch of BuildPreview already calls
	 * RunwayDrawGuide, which was a no-op until this. See IBuildTool::WantsFreeStartGuides.
	 */
	virtual bool WantsFreeStartGuides() const override { return true; }

	/**
	 * Which standard width, as an index into the target's runway profiles
	 * (IRoadEditTarget::GetRunwayProfileCount/ResolveRunwayProfile).
	 *
	 * An index rather than a width in uu: the list IS the set of legal widths, so there is no
	 * value this can hold that names an illegal runway. Clamped on use, because the list is
	 * content and may be shorter than this expects.
	 */
	int32 WidthIndex = 0;

	/** Steps to the next standard width, wrapping, through SelectVariant. Called from OnReselect. */
	void NextWidth(const FToolContext& Context);

	/** What the next runway is paved with and what approach it offers - the facts placement writes. */
	ERunwaySurface Surface = ERunwaySurface::Tarmac;
	ERunwayApproach Approach = ERunwayApproach::Visual;

	/** Steps each scale, wrapping. Take the context now (2026-09-26) because the step goes
	 *  through SelectVariant, which numbers rows against the target's width count. */
	void NextSurface(const FToolContext& Context);
	void NextApproach(const FToolContext& Context);

	/** The row named AxisId, stepped one option on from what it lights, wrapping. */
	void StepAxis(const FToolContext& Context, FName AxisId);

	/** The facts the next placement writes, assembled from the two choices above. */
	FRunwayFacts Facts() const;

private:
	/**
	 * The chosen profile, or null when Target has none.
	 *
	 * THROUGH THE TARGET, not UAirsideSettings::GetContent() (issue #78) - this was the only
	 * file in Tool/ that knew UAirsideContent existed, calling it directly instead of going
	 * through IRoadEditTarget like every other tool's content lookup (ARoadNetworkActor's
	 * Resolve* family). A null Target answers null, same as an empty content set.
	 */
	URoadProfile* ProfileForWidth(const FToolContext& Context) const;

	bool bHasThreshold = false;

	/** The first threshold, in road-plane coordinates. */
	FVector2D Threshold = FVector2D::ZeroVector;
};
