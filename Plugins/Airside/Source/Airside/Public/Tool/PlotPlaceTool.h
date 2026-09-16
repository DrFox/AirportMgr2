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
	/** Nothing anchored. The cursor hunts for a service road. */
	Idle,

	/** Anchored. The cursor runs along the road, setting the frontage width in bays. */
	Width,

	/** Width locked. The cursor runs away from the road, setting the depth in rows. */
	Depth,

	/** Everything chosen. Nothing moves until Build, or until Cancel steps back. */
	Confirm
};

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

	/** For tests. */
	EPlotStage GetStage() const { return Stage; }

	/** What fills row 1. For tests, and for the mix UI when buying arrives. */
	void SetModules(const TArray<EDepotModule>& InModules) { Modules = InModules; }

private:
	/** The frontage edge as the grid wants it: interior on the LEFT of A->B. */
	void Frontage(FVector2D& OutA, FVector2D& OutB) const;

	/** Bays the cursor is asking for, at least one. */
	int32 WidthAt(const FToolContext& Context) const;

	/** Rows the cursor is asking for, at least one. */
	int32 DepthAt(const FToolContext& Context) const;

	/**
	 * The plot as it stands THIS frame: dragged where the cursor decides it, locked where a
	 * click already has, and one row wherever depth has not been reached yet.
	 *
	 * ONE FUNCTION, TWO CALLERS, because BuildPreview and BuildReadout describing the same
	 * gesture is the entire contract IToolReadoutSink exists to keep. They carried a copy of
	 * this each and had already drifted: during the Width stage the readout said one row
	 * while the preview drew the PREVIOUS gesture's depth, so the second depot a player drew
	 * showed a ghost that disagreed with the bar above it. Two copies of a rule is how that
	 * happens; see CLAUDE.md, "Lists that must agree are ONE list".
	 */
	void ShownSize(const FToolContext& Context, int32& OutWidth, int32& OutDepth) const;

	/**
	 * This frame's plot: its size, and the frontage edge ordered as the grid wants it.
	 *
	 * SHARED BY THE PREVIEW AND THE READOUT for the same reason ShownSize is. The ordering
	 * rule - interior on the LEFT of A->B - was written out twice, once here and once in
	 * Frontage(), and a third copy for the readout is how the ghost and the facts come to
	 * describe different rectangles.
	 */
	void ShownPlot(const FToolContext& Context, FVector2D& OutA, FVector2D& OutB,
		int32& OutWidth, int32& OutDepth) const;

	/**
	 * The yard this plot would get, laid out by the same solver the presenter runs.
	 *
	 * SHARED BY THE GHOST AND THE READOUT. The ghost draws these footprints and the readout
	 * counts them, so the boxes on screen and the numbers on the bar are ONE computation -
	 * and because DepotYardSeed keys off the pose the facade will store, they are also the
	 * boxes Build actually puts down rather than an impression of them.
	 */
	PlotYard::FYard YardFor(const FVector2D& FrontA, const FVector2D& FrontB,
		int32 InWidth, int32 InDepth) const;

	EPlaceableEntity Kind = EPlaceableEntity::FuelDepot;

	/** One of each is the concept sheet's depot, and the smallest one that actually works. */
	TArray<EDepotModule> Modules = {
		EDepotModule::Shed, EDepotModule::Tank, EDepotModule::Pump };

	EPlotStage Stage = EPlotStage::Idle;

	/** Where the frontage starts, quantised onto the road's own bay grid. */
	FVector2D Anchor = FVector2D::ZeroVector;

	/** Unit vector along the road at the anchor. */
	FVector2D Along = FVector2D(1.0, 0.0);

	/** Unit vector away from the road, on the side the cursor was when it anchored. */
	FVector2D Inward = FVector2D(0.0, 1.0);

	/** Locked at the Width click, and at the Depth click. Meaningless before. */
	int32 Width = 1;
	int32 Depth = 1;
};
