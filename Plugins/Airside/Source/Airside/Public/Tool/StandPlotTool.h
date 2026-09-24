#pragma once

#include "CoreMinimal.h"
#include "Tool/PlotGesture.h"
#include "Tool/RoadBuildTool.h"

/**
 * How far through drawing a stand the gesture is.
 *
 * AN ENUM, for the reason EPlotStage gives: the data ACCUMULATES - the anchor from Entrance
 * on, the entrance's far end from Depth on, the depth from Confirm on - so nothing is ever
 * meaningless-but-present, and CLAUDE.md's "a phase is an enum" is the default that applies.
 *
 * THREE CLICKS, NOT THE DEPOT'S FOUR, because a stand is a RECTANGLE: the template it carries
 * is one, and a skewed quad would need its letter measured from an inscribed rectangle the
 * player cannot see (drawn-stands spec, 2026-09-23).
 */
UENUM()
enum class EStandStage : uint8
{
	/** Nothing pinned. The cursor hunts for a taxiway. */
	Idle,

	/** The anchor is pinned. The cursor runs along the taxiway setting the entrance width. */
	Entrance,

	/** The entrance edge is pinned. The cursor drags inward, away from it, for the depth. */
	Depth,

	/** The rectangle is locked. Nothing moves until Build, or until Cancel steps back. */
	Confirm
};

/**
 * Drawing an aircraft stand: click a taxiway, drag along it for width, drag away for depth,
 * press Build. The rectangle's size decides the ICAO letter - nobody picks one.
 *
 * REPLACES FStandPlaceTool's press-drag-release, which dropped a stand of one fixed size
 * wherever the cursor was. Now that the size of the ground IS the mechanic
 * (IcaoCode::LetterForStandSize), a gesture that could not express a size could not place
 * anything but a Code C.
 *
 * THE DEPOT'S GESTURE POINTED AT A TAXIWAY, sharing its first click through PlotGesture, so
 * a stand and a depot drawn off the same grid can sit flush. Not a subclass of FPlotPlaceTool:
 * everything past the anchor differs - three clicks not four, a rectangle not a free quad, a
 * letter not a yard reservation - and a base that is half-overridden is two tools in one.
 *
 * THE ENTRANCE EDGE, never "frontage": the aircraft taxis in nose first across it, so its
 * TAIL is at this edge and its nose points inward. Frontage read as "where the nose is".
 *
 * NO FREE-START GUIDES, unlike the tool it replaces: the first click is snapped to a
 * taxiway's step grid, exactly as the depot's is to a service road's, and a guide drawn over
 * a click that then snaps elsewhere is a guide not obeyed - see IBuildTool::WantsFreeStartGuides.
 */
class AIRSIDE_API FStandPlotTool : public IBuildTool
{
public:
	virtual FText GetDisplayName() const override;

	virtual void OnClick(const FToolContext& Context) override;
	virtual void OnCancel(const FToolContext& Context) override;
	virtual void OnCommit(const FToolContext& Context) override;
	virtual void OnDeactivate(const FToolContext& Context) override;
	virtual void BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const override;
	virtual void BuildReadout(const FToolContext& Context, IToolReadoutSink& Sink) const override;
	virtual bool IsIdle() const override { return Stage == EStandStage::Idle; }

	/** For tests. */
	EStandStage GetStage() const { return Stage; }

	/** How many of the three points the player has pinned. What the readout reports as "N/3". */
	int32 PinnedCount() const;

	/**
	 * The stand as it stands THIS frame: entrance edge 0->1, then inward to 2 and 3 - the
	 * convention StandBox.h documents and a drawn stand is saved in.
	 *
	 * ONE DERIVATION, EVERY CALLER, as FPlotPlaceTool::Quad is: the preview draws it, the
	 * readout measures it and OnCommit builds from it. Empty while Idle; from Entrance on,
	 * always four corners (zero depth until the depth is being dragged).
	 */
	void Rect(const FToolContext& Context, TArray<FVector2D>& Out) const;

private:
	/** The stand under the cursor for a Remove gesture, or INDEX_NONE. STANDS ONLY - a depot
	 *  under the cursor is the depot tool's to remove, where the player can see it is one. */
	static int32 StandUnder(const FToolContext& Context);

	/** How the preview draws a rectangle with a letter: keep-out, lead-in and letter label. */
	void DescribeLetter(const FToolContext& Context, TConstArrayView<FVector2D> Shown,
		IToolPreviewSink& Sink) const;

	EStandStage Stage = EStandStage::Idle;

	/**
	 * OnCommit's PlaceStandInPlot was refused, for a shape Committable should already have
	 * greyed out - the same safety net FPlotPlaceTool keeps (issue #182), for the same
	 * reason: honour a mutator's return rather than trust another check made it unreachable.
	 * Cleared on every gesture boundary so one stand's refusal cannot bleed onto the next.
	 */
	bool bLastCommitRefused = false;

	/** Unit vector along the taxiway at the anchor. */
	FVector2D Along = FVector2D(1.0, 0.0);

	/** Unit vector away from the taxiway, on the side the cursor was when it anchored. */
	FVector2D Inward = FVector2D(0.0, 1.0);

	/**
	 * The pinned points, in click order: the anchor, the entrance edge's far end, the depth
	 * point (the far end carried inward by the pinned depth).
	 *
	 * ENTRIES PAST PinnedCount() ARE STALE and must not be read - Rect rebuilds the moving one
	 * from the cursor every frame, the lesson FPlotPlaceTool::Corners records.
	 */
	FVector2D Corners[3] = { FVector2D::ZeroVector, FVector2D::ZeroVector, FVector2D::ZeroVector };
};
