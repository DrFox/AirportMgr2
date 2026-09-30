#pragma once

#include "CoreMinimal.h"
#include "Misc/Optional.h"
#include "Model/RoadEntity.h"
#include "Solve/PlotYard.h"

/**
 * What each depot module occupies on the ground.
 *
 * IN Build/ RATHER THAN IN THE PRESENTER, because two callers need it: UPlotPresenter draws
 * the yard, and FPlotPlaceTool's readout counts what would still fit in one. Two copies of
 * this table would be two things to keep in agreement, and they would disagree the first
 * time a footprint changed - the bug CLAUDE.md names most often.
 *
 * Build/ is the layer that can serve both: Tool/ may include it and Present/ may include it,
 * while Solve/ may not see EDepotModule at all, which is why PlotYard takes a footprint
 * rather than a module.
 */
AIRSIDE_API PlotYard::FFootprint DepotFootprint(EDepotModule Module);

class UAirsideContent;

/**
 * What a module occupies, preferring its authored kit and falling back to the grey-box table.
 *
 * CONTENT IS OPTIONAL AND THAT IS LOAD-BEARING. Every solver and presenter change in this
 * slice is testable with no content at all, so the runtime half of the work and the meshes
 * proceed independently rather than blocking each other. A caller passing nullptr is
 * exercising the shipped path, not a stub.
 */
AIRSIDE_API PlotYard::FFootprint DepotFootprint(EDepotModule Module,
	const UAirsideContent* Content);

/**
 * Every module kind a depot can hold, as specs the yard solver understands.
 *
 * IN ENUM ORDER, so a spec's index IS its EDepotModule and the presenter needs no second map
 * to get back. Built by WALKING the enum rather than from a list written here: a list would
 * answer only for the modules somebody remembered to add, which is the failure
 * AircraftLookTest exists for.
 * ENFORCED BY: Airside.Build.DepotKitSpecsCoverEveryModule (count and per-index footprint)
 */
AIRSIDE_API TArray<PlotYard::FKitSpec> DepotKitSpecs(const UAirsideContent* Content);

/**
 * What to call a module on the readout. Plural, because it labels a count.
 *
 * HERE RATHER THAN IN THE TOOL, for the reason DepotFootprint is here: the readout names
 * them and the inspector will too, and two spellings of "Sheds" is the sort of thing nobody
 * notices until a screenshot.
 */
AIRSIDE_API FString DepotKitLabel(EDepotModule Module);

/**
 * The seed that lays out the yard of a depot posed at Where.
 *
 * ONE CALLER SINCE #450, and the agreement is the whole point: DepotKit::SolveYard seeds every yard - the
 * tool's preview, the facade's commit and the built depot's presenter - off the midpoint of the frontage it
 * is given, which is the value URoadEditFacade::PlaceEntityInPlot stores as the entity's Position. Same
 * seed, same yard, so the count previewed is the count built. Two copies of this arithmetic would be a
 * preview quietly describing a different depot.
 * ENFORCED BY: Check-Architecture rule 4 row 'DepotYardSeed' (DepotKit.cpp, SolveYard's own call, is the one production caller)
 *
 * QUANTISED to whole uu: a float that came back from a save one bit different would re-roll
 * that depot and only that depot, which is the kind of bug that takes a day.
 */
AIRSIDE_API int32 DepotYardSeed(FVector2D Where);

class URoadNetwork;
class UEntityDefinition;
enum class EPlotLayout : uint8;

/**
 * DepotKit's own true namespace, holding only ReportIncomplete (#306) - every OTHER symbol
 * above stays a bare free function so this fix does not touch their ~20 existing call sites
 * for no reason of its own; that consolidation, if it is ever wanted, is a separate change.
 */
namespace DepotKit
{
	/**
	 * Warns, once per rebuild, about every depot missing a shed or a pump - MOVED HERE FROM
	 * FAnchorLink::Build (#306), which had nothing to do with a depot's module census; it ran
	 * this walk only because it was the last thing to touch Network on a Topology rebuild.
	 * Both UE_LOG lines are UNCHANGED from FAnchorLink's own - see git blame on this file
	 * for their provenance if either is ever suspected of having drifted in the move.
	 *
	 * A PLOT THAT CANNOT WORK YET is warned rather than refused: a part-built depot is a
	 * legitimate state - the player may be about to add the missing module - so this says
	 * what is missing and the placement still stands.
	 *
	 * CALLED AGAIN AFTER EVERY TOPOLOGY EDIT, not just at placement, because a depot built correctly
	 * and later reduced would otherwise have been warned about once, at a moment the player
	 * was not looking at it - see AirsideDerivation::Derive (#438, #472), whose Full and Links scopes
	 * run it as the links pass's census after every Topology rebuild, for where "again" means. A Facts
	 * rebuild (#446: a module bought, an unseated one repaired away) derives nothing and does not
	 * re-run it; its warnings repeat on the next Topology.
	 *
	 * SEATED, NOT OWNED (#443, ruled 2026-09-30): a shed or a pump counts only when the plot seats it - the same
	 * FDepotCapability the job board and the purchase rules read - so it warns of a depot whose modules the plot could
	 * not hold, which the player cannot see standing. Specs is the kit table the presenter solves the plot with
	 * (ARoadNetworkActor::ResolveDepotKits, through FSurfaceSettings): with none (a test, or a caller with no content)
	 * there is no plot to seat against and the owned list stands. A depot with no modules at all is the legacy plotless
	 * depot and is not censused.
	 * ENFORCED BY: Airside.Build.DepotKitReportIncomplete
	 */
	AIRSIDE_API void ReportIncomplete(const URoadNetwork& Network, TArrayView<const PlotYard::FKitSpec> Specs = {});

	/**
	 * The layout a depot definition asks for; the scatter when there is none. THE FALLBACK IS TYPED HERE, AND IN NO OTHER
	 * PRODUCTION FILE.
	 * ENFORCED BY: Check-Architecture rule 4 row 'EPlotLayout fallback ternary'
	 *
	 * It was a ternary in the tool, another in the facade and a third in ReservationOf, and the tool's
	 * copy drifted once already (DA_FuelDepot was authored before EPlotLayout existed, so it carried the
	 * Scatter default while the tool's own Kind map said FuelYardBands: the player dragged out a banded ghost
	 * and got a scattered depot). The tool reads it too, as the extra key of its memo, which is why it is
	 * public. A definition the caller cannot resolve falls back to the scatter, which is what an unauthored
	 * plot type would have drawn anyway.
	 */
	AIRSIDE_API EPlotLayout LayoutOf(const UEntityDefinition* Definition);

	/**
	 * THE ONE YARD SOLVE (#450): layout from Definition (LayoutOf), gate = the frontage's midpoint, seed =
	 * DepotYardSeed(gate), then PlotLayoutFor(layout)->Solve. It was hand-assembled at three sites that agreed
	 * only by comment - the tool's preview (FPlotPlaceTool::ReservationFor), the facade's commit
	 * (URoadEditFacade::ReserveForPlot) and the built yard (ReservationOf below) - each typing its own FPlotSite.
	 *
	 * FRONTAGE IS GIVEN, NEVER SEARCHED FOR: the ends of the edge on the road, in Outline's winding
	 * (PlotYard::InwardOf reads the interior side from that direction, so they travel with the outline).
	 * Outline is a VIEW and must outlive the call, as FPlotSite::Outline's own comment says.
	 *
	 * THE GATE IS DERIVED, NOT PASSED: it is the pose URoadEditFacade::PlaceEntityInPlot stores, and a caller
	 * that could pass another would be the drift this function exists to stop.
	 * ENFORCED BY: Airside.Tool.PlotPlace.ToolFacadeAndBuiltDepotSolveOneYard; Check-Architecture rule 4 row 'FPlotSite'
	 */
	AIRSIDE_API PlotYard::FReservation SolveYard(TArrayView<const FVector2D> Outline,
		FVector2D FrontageA, FVector2D FrontageB, const UEntityDefinition* Definition,
		TArrayView<const PlotYard::FKitSpec> Specs);

	/**
	 * Everything a PLACED plotted depot's plot has room for - the SAME solve UPlotPresenter::RebuildFrom
	 * draws from: SolveYard over the entity's own outline, its STORED frontage (FEntityInstance::FrontageEdge,
	 * written by the facade from the edge it was given) and its definition. Unset for anything that is not a
	 * live plotted depot, or that stores no frontage.
	 *
	 * ONE SOLVE, TWO READERS: the presenter's lit/ghosted bays and UFacilityPurchases' "free reserved
	 * slot" (R9) are one fact, so a Buy shed that lights nothing cannot happen. Modules play no part -
	 * what a plot HOLDS does not depend on what was bought.
	 * ENFORCED BY: Check-Architecture rule 4 row 'PlotLayoutFor'; Airside.Build.DepotKit.ReservationOfIsThePresentersSolve
	 */
	AIRSIDE_API TOptional<PlotYard::FReservation> ReservationOf(const FEntityInstance& Depot,
		TArrayView<const PlotYard::FKitSpec> Specs);

	/**
	 * What a depot STARTS with - one shed, one tank, one pump, the concept sheet's Tier 1 depot and the smallest that
	 * works. FPlotPlaceTool's default mix, and what the content test seats on the smallest plot the tool accepts (#266).
	 * HERE, ONE LIST, rather than a literal in the tool and a copy in the test: the test would go on passing for a mix
	 * the tool no longer sold.
	 * ENFORCED BY: Airside.Content.SmallestAcceptedPlotSeatsTheStarterMix
	 */
	AIRSIDE_API TArray<EDepotModule> StarterModules();

	/**
	 * Why a plot with this Reservation cannot take Modules - the mix PlaceEntityInPlot would store - or empty when every
	 * one of them seats. THE PLACEMENT'S HALF OF "there must never be unplaced modules" (#266, owner 2026-09-30: refuse too
	 * small a plot; neither shrink the mix nor charge for what would be dropped).
	 *
	 * ONE RULE, TWO CALLERS, over ONE SOLVE: FPlotPlaceTool's readout (the warning and the grey Build) and
	 * URoadEditFacade::PlaceEntityInPlot's commit both call this with the reservation they already hold - the tool's
	 * memoised ReservationFor and the facade's ReserveForPlot, the identical PlotLayoutFor(Layout)->Solve (#182). NOT ON
	 * IRoadEditTarget::WhyPlotRefused: that is the outline alone, and asking the facade from the readout would solve the
	 * plot a second time every hover frame for an answer the tool's memo already has.
	 *
	 * THROUGH FDepotCapability::Seat, the list overload - so what the preview promises to seat and what the presenter
	 * stands are decided by the same function.
	 * ENFORCED BY: Airside.Tool.PlotPlace.RefusesAPlotThatCannotSeatTheStarterMix
	 */
	AIRSIDE_API FString WhyUnseated(const PlotYard::FReservation& Reservation, TConstArrayView<EDepotModule> Modules);
}
