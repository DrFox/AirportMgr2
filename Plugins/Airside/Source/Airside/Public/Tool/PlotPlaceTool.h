#pragma once

#include "CoreMinimal.h"
#include "Entities/EntityDefinition.h"
#include "Model/RoadEntity.h"
#include "Solve/PlotYard.h"
#include "Tool/PlotGesture.h"
#include "Tool/RoadBuildTool.h"
#include "Tool/StagedPlotTool.h"

/**
 * How far through placing a plot the gesture is.
 *
 * A UENUM FOR THE READER, A PinnedCount() FOR THE MACHINERY: FStagedPlotTool tracks progress
 * as a single int (issue #302 - see its own comment on why), and GetStage() below is a one-line
 * cast back to this so every existing caller and test keeps naming the stage it always did.
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
 *
 * A FStagedPlotTool (issue #302): the anchor search, the Remove branch, the cancel step-back,
 * OnCommit's return-honouring and OnDeactivate are the base's, shared bit-for-bit with
 * FStandPlotTool rather than copied into it. What is genuinely this tool's own - the free
 * quad's shape, the module footprints, the kit facts - lives in the hooks below.
 */
class AIRSIDE_API FPlotPlaceTool : public FStagedPlotTool
{
public:
	explicit FPlotPlaceTool(EPlaceableEntity InKind) : FStagedPlotTool(4), Kind(InKind) {}

	virtual FText GetDisplayName() const override;

	/**
	 * THE BACK CORNERS ONLY - see the implementation for why the anchor and the frontage are
	 * left alone. This is what tells the driver's guide chain which point is moving and which
	 * edge it grew from; the tool resolves nothing itself and remembers no frame.
	 *
	 * NOT A FStagedPlotTool HOOK: FStandPlotTool answers no guide at all (its first two clicks
	 * are already grid-constrained the same way this tool's are - see its header), so there is
	 * nothing here for the base to share and this stays IBuildTool's own virtual, overridden
	 * only where a tool actually has an answer.
	 */
	virtual bool DescribeGuideAnchor(const URoadNetwork* Network, IRoadEditTarget* Target,
		FGuideAnchor& Out) const override;

	/** For tests: the stage FPlotPlaceTool's own callers and tests have always named, cast
	 *  from FStagedPlotTool::PinnedCount(). */
	EPlotStage GetStage() const { return static_cast<EPlotStage>(PinnedCount()); }

	/**
	 * What the depot STARTS with. No longer what the plot can hold.
	 *
	 * IT NO LONGER STEERS THE PREVIEW. Under reservation the ghost and the readout come from
	 * the ground the player is dragging out, not from a mix handed in here, so this decides
	 * only which bays are lit the moment the depot is built.
	 */
	void SetModules(const TArray<EDepotModule>& InModules) { Modules = InModules; }

	/**
	 * How many times ReservationFor has actually run the packer, rather than been asked to.
	 *
	 * FOR TESTS ONLY - issue #180. BuildPreview and BuildReadout ran the whole yard packer
	 * once each per hover frame, though both describe the same outline; this is what a test
	 * counts to prove the memo below is doing its job instead of merely existing.
	 */
	int32 GetSolveCountForTest() const { return SolveCountForTest; }

	/**
	 * The kit specs this tool last resolved, empty before any solve.
	 *
	 * FOR TESTS ONLY - issue #181. ReservationFor now resolves them through
	 * Context.Target->ResolveDepotKits() instead of calling DepotKitSpecs itself, and
	 * UPlotPresenter resolves through the SAME actor method (ARoadNetworkActor::ResolveDepotKits) -
	 * this is what a test compares the presenter's own specs against to prove the two really
	 * are one table read twice, not two tables that happen to agree today.
	 */
	TArray<PlotYard::FKitSpec> GetSpecsForTest() const { return Specs; }

	/**
	 * The plot as it stands THIS frame: pinned corners as placed, the moving one taken from
	 * the cursor, in the outline's own winding with the frontage as edge 0->1.
	 *
	 * A THIN NAME FOR Shape() (issue #302's hook), kept public because every test in this
	 * module calls Tool.Quad(...) by name. ONE DERIVATION, EVERY CALLER - the preview draws
	 * it, the readout measures it and OnCommit builds from it, so the ghost, the facts and the
	 * built thing cannot describe three different shapes. Fewer than two pinned corners gives
	 * fewer than four out.
	 */
	void Quad(const FToolContext& Context, TArray<FVector2D>& OutQuad) const { Shape(Context, OutQuad); }

protected:
	virtual bool Filter(const URoadNetwork& Network, FRoadSegmentId Id) const override
	{
		return PlotGesture::IsServiceRoad(Network, Id);
	}

	/**
	 * DEPOTS ONLY: this is the depot tool, and Remove lit on it removes depots. A stand under
	 * the cursor is the stand tool's to remove, where the player can see it is one. A depot is
	 * a plotted entity, OR a fuel-role entity with no outline - the format depots were placed
	 * in before plots, still saved in M_Starter on 2026-09-22 and otherwise unremovable.
	 *
	 * KIND, NOT OUTLINE: IsDepot() alone is enough now that a stand can be plotted too - the
	 * old `IsPlotted() || PoseRole == Fuel` would have picked up a drawn stand here as well.
	 */
	virtual bool IsMine(const FEntityInstance& Entity) const override { return Entity.IsDepot(); }

	virtual FString RemoveLabel() const override { return TEXT("remove fuel depot"); }
	virtual FString RemoveWarning() const override { return TEXT("Click a fuel depot to remove it"); }
	virtual FString RoadNoun() const override { return TEXT("a service road"); }
	virtual FString PointsFactLabel() const override { return TEXT("Plot Points"); }

	virtual void Shape(const FToolContext& Context, TArray<FVector2D>& OutShape) const override;
	virtual bool CanCloseShape(TConstArrayView<FVector2D> Shown) const override;
	virtual int32 Place(const FToolContext& Context, const TArray<FVector2D>& Outline) const override;
	virtual void Describe(const FToolContext& Context, TConstArrayView<FVector2D> Shown,
		IToolPreviewSink& Sink) const override;
	virtual void DescribeReadout(const FToolContext& Context, TConstArrayView<FVector2D> Shown,
		IToolReadoutSink& Sink) const override;

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
	 * KEYED BY THE OUTLINE THROUGH THE BASE'S TOutlineMemo (issue #302), with Layout as this
	 * payload's own extra key: two plots of the same four corners under two different layouts
	 * (an authored definition changing under the player's feet) must not answer from a memo
	 * that only ever looked at geometry.
	 */
	struct FReservationPayload
	{
		EPlotLayout Layout = EPlotLayout::Scatter;
		PlotYard::FReservation Reservation;
	};
	mutable TOutlineMemo<FReservationPayload> Memo;

	/**
	 * The kit specs, resolved once ever rather than once per outline - DepotKitSpecs walks
	 * EDepotModule, not the quad, so it owes nothing to the memo above and stays a separate
	 * cache: BuildReadout lists every kit at zero from the Frontage stage on, before an
	 * outline has ever been complete enough to key a memo entry at all.
	 */
	mutable bool bSpecsResolved = false;
	mutable TArray<PlotYard::FKitSpec> Specs;

	/** Bumped only on an actual solve - a cache hit must not move it. See GetSolveCountForTest. */
	mutable int32 SolveCountForTest = 0;

	EPlaceableEntity Kind = EPlaceableEntity::FuelDepot;

	/** One of each is the concept sheet's depot, and the smallest one that actually works. */
	TArray<EDepotModule> Modules = {
		EDepotModule::Shed, EDepotModule::Tank, EDepotModule::Pump };
};
