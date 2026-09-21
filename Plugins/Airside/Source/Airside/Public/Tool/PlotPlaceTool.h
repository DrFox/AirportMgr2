#pragma once

#include "CoreMinimal.h"
#include "Entities/EntityDefinition.h"
#include "Model/RoadEntity.h"
#include "Solve/PlotYard.h"
#include "Tool/RoadBuildTool.h"

/**
 * How far through placing a plot the gesture is.
 *
 * AN ENUM, NOT A STATE OBJECT PER STAGE - and that is a deliberate departure from
 * FOutlineDrawTool, which this tool otherwise resembles. There, the data genuinely DIFFERS
 * by state: outlining carries a growing list of corners and idle carries nothing, so a
 * shared struct would leave that list present and readable in a state that never touched it.
 *
 * Here the data ACCUMULATES monotonically. The anchor is meaningful from Width onward, the
 * width from Depth onward, the depth from Confirm onward - nothing is ever meaningless-but-
 * present, so the argument for objects does not apply, and CLAUDE.md's own rule ("a phase is
 * an enum, never a set of bools") is the default this falls back to.
 */
UENUM()
enum class EPlotStage : uint8
{
	/** Nothing pinned. The cursor hunts for a service road. */
	Idle,

	/** The anchor is pinned. The cursor runs along the road setting the frontage. */
	Frontage,

	/** The frontage is pinned. The cursor places the back corner at its far end. */
	CornerA,

	/** Three corners pinned. The cursor places the last one. */
	CornerB,

	/** Four corners. Nothing moves until Build, or until Cancel steps back. */
	Confirm
};

namespace PlotGesture
{
	/** 15 m. A yard narrower than this is not a yard - see the 2026-09-16 gesture spec. */
	inline constexpr double MinFrontageUu = 1500.0;

	/**
	 * 5 m. The step above the minimum.
	 *
	 * PUBLIC because the tests assert against it and a copy of the number in a test is a
	 * second source for it - which is exactly how the anchor came to stride 4 m under a
	 * frontage growing in 5 m steps.
	 */
	inline constexpr double FrontageStepUu = 500.0;

	/**
	 * How far from a service road a plot can be started, uu. 20 m.
	 *
	 * THE PLAYER STANDS WHERE THE PLOT GOES, not on the road it fronts. Requiring the cursor
	 * to be over the carriageway made the anchors vanish the moment you moved off it, which
	 * reads as placing the depot ON the road (PIE, 2026-09-17). Far enough to stand inside
	 * the plot's own footprint; near enough not to grab a road across the field.
	 */
	inline constexpr double AnchorReachUu = 2000.0;
}

/**
 * Placing a fuel depot: snap to a service road, drag a width, drag a depth, press Build.
 *
 * REPLACES the freeform polygon tool, which was the apron's gesture reused because it was
 * already built. PIE on 2026-09-15 showed what that cost: three modules huddled at one end
 * of a large plot, nothing to communicate the minimum depth, and five clicks to draw a
 * rectangle. A building is a rectangle; freeform belongs to the apron it came from.
 *
 * THE FOURTH CLICK LOCKS, IT DOES NOT BUILD. The review beat between locking and committing
 * is where the readout's cost and warnings actually get read - see IToolReadoutSink. Build
 * is a widget, so OnCommit is reachable at any moment and every stage but Confirm ignores it.
 */
class AIRSIDE_API FPlotPlaceTool : public IBuildTool
{
public:
	explicit FPlotPlaceTool(EPlaceableEntity InKind) : Kind(InKind) {}

	virtual FText GetDisplayName() const override;

	virtual void OnClick(const FToolContext& Context) override;
	virtual void OnCancel(const FToolContext& Context) override;
	virtual void OnCommit(const FToolContext& Context) override;
	virtual void OnDeactivate(const FToolContext& Context) override;
	virtual void BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const override;
	virtual void BuildReadout(const FToolContext& Context, IToolReadoutSink& Sink) const override;
	virtual bool IsIdle() const override { return Stage == EPlotStage::Idle; }

	/**
	 * THE BACK CORNERS ONLY - see the implementation for why the anchor and the frontage are
	 * left alone. This is what tells the driver's guide chain which point is moving and which
	 * edge it grew from; the tool resolves nothing itself and remembers no frame.
	 */
	virtual bool DescribeGuideAnchor(const URoadNetwork* Network, IRoadEditTarget* Target,
		FGuideAnchor& Out) const override;

	/** For tests. */
	EPlotStage GetStage() const { return Stage; }

	/**
	 * What the depot STARTS with. No longer what the plot can hold.
	 *
	 * IT NO LONGER STEERS THE PREVIEW. Under reservation the ghost and the readout come from
	 * the ground the player is dragging out, not from a mix handed in here, so this decides
	 * only which bays are lit the moment the depot is built.
	 */
	void SetModules(const TArray<EDepotModule>& InModules) { Modules = InModules; }

	/** How many corners the player has placed, 0 to 4. What the readout reports as "N/4". */
	int32 PinnedCount() const;

	/**
	 * How many times ReservationFor has actually run the packer, rather than been asked to.
	 *
	 * FOR TESTS ONLY - issue #180. BuildPreview and BuildReadout ran the whole yard packer
	 * once each per hover frame, though both describe the same outline; this is what a test
	 * counts to prove the memo below is doing its job instead of merely existing.
	 */
	int32 GetSolveCountForTest() const { return SolveCountForTest; }

	/**
	 * The plot as it stands THIS frame: pinned corners as placed, the moving one taken from
	 * the cursor, in the outline's own winding with the frontage as edge 0->1.
	 *
	 * ONE DERIVATION, EVERY CALLER. The preview draws it, the readout measures it and
	 * OnCommit builds from it, so the ghost, the facts and the built thing cannot describe
	 * three different shapes. Fewer than two pinned corners gives fewer than four out.
	 */
	void Quad(const FToolContext& Context, TArray<FVector2D>& OutQuad) const;

private:
	/**
	 * What this plot would hold, solved by the same code the presenter runs.
	 *
	 * SHARED BY THE GHOST AND THE READOUT. The ghost draws these stands and the readout
	 * counts them, so the boxes on screen and the numbers beside them are ONE computation -
	 * and because DepotYardSeed keys off the pose the facade will store, they are also the
	 * boxes Build actually puts down rather than an impression of them.
	 */
	PlotYard::FReservation ReservationFor(const FToolContext& Context,
		TArrayView<const FVector2D> Outline) const;

	/**
	 * THE SOLVE, DONE ONCE PER SHOWN OUTLINE - issue #180.
	 *
	 * BuildPreview and BuildReadout are each asked for the SAME reservation - see
	 * ReservationFor's own comment on why that is one computation, not two that agree - but
	 * both are const, both used to call Solve directly, and both rebuilt the kit specs on top
	 * of it: a hover frame paid for the packer twice and the spec table four times, and a
	 * frame in Confirm stage paid for both though nothing had moved.
	 *
	 * MUTABLE, AND KEYED RATHER THAN CLEARED PER FRAME, because nothing here is told when a
	 * frame starts: PR #199 gives BuildPreview and BuildReadout the same FToolContext in the
	 * game driver, but the editor mode's Tick and BuildPreview calls are NOT guaranteed the
	 * same cursor (URoadBuildEditorTool::OnUpdateHover and DrawPersistentState build theirs
	 * separately), and CollectToolReadout runs BEFORE Tick in ARoadBuildController::PlayerTick
	 * so a memo written only in Tick would hand BuildReadout last frame's shape. Comparing the
	 * outline itself sidesteps both: whichever caller asks first with a given shape pays for
	 * the solve, and every other caller - that frame, the next, or from the other driver -
	 * reads the same answer until the shape actually changes.
	 *
	 * THE KIT SPECS ARE RESOLVED HERE TOO, once, because they never depended on the outline in
	 * the first place - DepotKitSpecs walks EDepotModule, not the quad - so refetching them
	 * alongside the packer on every outline change (or once, lazily, for an outline that never
	 * completes - see ReservationFor) is what "once per solve, not per caller" means for a
	 * value nothing here invalidates.
	 */
	struct FReservationMemo
	{
		bool bValid = false;
		FVector2D Outline[4] = { FVector2D::ZeroVector, FVector2D::ZeroVector,
			FVector2D::ZeroVector, FVector2D::ZeroVector };
		EPlotLayout Layout = EPlotLayout::Scatter;

		/** Resolved once, lazily: see ReservationFor for why this flag exists apart from bValid. */
		bool bSpecsResolved = false;
		TArray<PlotYard::FKitSpec> Specs;

		PlotYard::FReservation Reservation;
	};
	mutable FReservationMemo Memo;

	/** Bumped only on an actual solve - a cache hit must not move it. See GetSolveCountForTest. */
	mutable int32 SolveCountForTest = 0;

	EPlaceableEntity Kind = EPlaceableEntity::FuelDepot;

	/** One of each is the concept sheet's depot, and the smallest one that actually works. */
	TArray<EDepotModule> Modules = {
		EDepotModule::Shed, EDepotModule::Tank, EDepotModule::Pump };

	EPlotStage Stage = EPlotStage::Idle;

	/** Unit vector along the road at the anchor. */
	FVector2D Along = FVector2D(1.0, 0.0);

	/** Unit vector away from the road, on the side the cursor was when it anchored. */
	FVector2D Inward = FVector2D(0.0, 1.0);

	/**
	 * The corners, in the order they are pinned: anchor, far frontage end, far back, near
	 * back.
	 *
	 * Corners[0] IS the anchor - it had a member of its own until the quad landed, and two
	 * names for one point is how the two come to disagree. It is quantised onto the road's
	 * own step and stood off the kerb at the first click; see OnClick.
	 *
	 * ENTRIES PAST PinnedCount() ARE STALE and must not be read. Quad() rebuilds the moving
	 * one from the cursor every frame rather than trusting what is here, because drawing a
	 * stale corner is how a ghost shows the PREVIOUS gesture's geometry - which this tool
	 * has already done once, with a depth that outlived the gesture that set it.
	 */
	FVector2D Corners[4] = { FVector2D::ZeroVector, FVector2D::ZeroVector,
		FVector2D::ZeroVector, FVector2D::ZeroVector };
};
