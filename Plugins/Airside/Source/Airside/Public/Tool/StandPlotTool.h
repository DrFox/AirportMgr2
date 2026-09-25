#pragma once

#include "CoreMinimal.h"
#include "Tool/PlotGesture.h"
#include "Tool/RoadBuildTool.h"
#include "Tool/StagedPlotTool.h"

/**
 * How far through drawing a stand the gesture is.
 *
 * A UENUM FOR THE READER, A PinnedCount() FOR THE MACHINERY - see FStagedPlotTool's own
 * comment: GetStage() below is a one-line cast from the base's single int, kept so every
 * existing caller and test still names the stage the way it always has.
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
 * a stand and a depot drawn off the same grid can sit flush. A FStagedPlotTool (issue #302),
 * NOT a hand copy of FPlotPlaceTool: the anchor search, the Remove branch, the cancel
 * step-back, OnCommit's return-honouring and OnDeactivate are the base's own, shared
 * bit-for-bit with the depot tool rather than pasted here a second time - only what a
 * rectangle from three clicks genuinely needs of its own (Shape, the letter, WhyStandRefused)
 * lives in this class's hooks.
 *
 * THE ENTRANCE EDGE, never "frontage": the aircraft taxis in nose first across it, so its
 * TAIL is at this edge and its nose points inward. Frontage read as "where the nose is".
 *
 * NO FREE-START GUIDES, unlike the tool it replaces: the first click is snapped to a
 * taxiway's step grid, exactly as the depot's is to a service road's, and a guide drawn over
 * a click that then snaps elsewhere is a guide not obeyed - see IBuildTool::WantsFreeStartGuides.
 */
class AIRSIDE_API FStandPlotTool : public FStagedPlotTool
{
public:
	FStandPlotTool() : FStagedPlotTool(3) {}

	virtual FText GetDisplayName() const override;

	/** For tests: the stage FStandPlotTool's own callers and tests have always named, cast
	 *  from FStagedPlotTool::PinnedCount(). */
	EStandStage GetStage() const { return static_cast<EStandStage>(PinnedCount()); }

	/**
	 * The stand as it stands THIS frame: entrance edge 0->1, then inward to 2 and 3 - the
	 * convention StandBox.h documents and a drawn stand is saved in.
	 *
	 * A THIN NAME FOR Shape() (issue #302's hook), kept public because every test in this
	 * module calls Tool.Rect(...) by name. ONE DERIVATION, EVERY CALLER, as
	 * FPlotPlaceTool::Quad is: the preview draws it, the readout measures it and OnCommit
	 * builds from it. Empty while Idle; from Entrance on, always four corners (zero depth
	 * until the depth is being dragged).
	 */
	void Rect(const FToolContext& Context, TArray<FVector2D>& Out) const { Shape(Context, Out); }

	/**
	 * How many times RefusalFor has actually asked the facade, rather than been asked to.
	 *
	 * FOR TESTS ONLY - issue #302. BuildPreview and BuildReadout each asked
	 * IRoadEditTarget::WhyStandRefused for the same Shown outline every hover frame - the exact
	 * "solve done twice a frame" #180 had already named for the depot's own packer, missed here
	 * because this tool was copied before that memo existed. This is what a test counts to
	 * prove the memo below is doing its job instead of merely existing.
	 */
	int32 GetRefusalCountForTest() const { return RefusalCountForTest; }

protected:
	virtual bool Filter(const URoadNetwork& Network, FRoadSegmentId Id) const override
	{
		// IsTaxiway, never IsServiceRoad - a stand opening onto a road trucks use would have
		// its arrivals taxi on one.
		return PlotGesture::IsTaxiway(Network, Id);
	}

	/**
	 * KIND, NOT OUTLINE: every stand and every drawn depot has an outline now, so IsStand() is
	 * the only question that tells them apart - see FEntityInstance::IsStand. A depot under
	 * the cursor is the depot tool's to remove, where the player can see it is one.
	 */
	virtual bool IsMine(const FEntityInstance& Entity) const override { return Entity.IsStand(); }

	virtual FString RemoveLabel() const override { return TEXT("remove stand"); }
	virtual FString RemoveWarning() const override { return TEXT("Click a stand to remove it"); }
	virtual FString RoadNoun() const override { return TEXT("a taxiway"); }
	virtual FString PointsFactLabel() const override { return TEXT("Stand Points"); }

	virtual void Shape(const FToolContext& Context, TArray<FVector2D>& OutShape) const override;
	virtual bool CanCloseShape(TConstArrayView<FVector2D> Shown) const override;
	virtual int32 Place(const FToolContext& Context, const TArray<FVector2D>& Outline) const override;
	virtual void Describe(const FToolContext& Context, TConstArrayView<FVector2D> Shown,
		IToolPreviewSink& Sink) const override;
	virtual void DescribeReadout(const FToolContext& Context, TConstArrayView<FVector2D> Shown,
		IToolReadoutSink& Sink) const override;
	virtual void DescribeRemoveExtra(const FToolContext& Context, int32 Doomed,
		const FEntityInstance& Entity, IToolPreviewSink& Sink) const override;

private:
	/** How the preview draws a rectangle with a letter: keep-out, lead-in and letter label. */
	void DescribeLetter(const FToolContext& Context, TConstArrayView<FVector2D> Shown,
		IToolPreviewSink& Sink) const;

	/**
	 * WhyStandRefused for Shown, asked of the facade at most once per distinct shape.
	 *
	 * THE SAME MEMO SHAPE AS FPlotPlaceTool::ReservationFor, through the base's TOutlineMemo
	 * (issue #302) - see that class's own comment on why the {bValid, Outline[4]} half is
	 * shared rather than copied a second time. No extra key here: unlike the depot's Layout,
	 * nothing else this tool reads can change what a fixed outline is refused for.
	 */
	FString RefusalFor(const FToolContext& Context, TConstArrayView<FVector2D> Shown) const;

	mutable TOutlineMemo<FString> RefusalMemo;

	/** Bumped only on an actual ask - a cache hit must not move it. See GetRefusalCountForTest. */
	mutable int32 RefusalCountForTest = 0;
};
