#pragma once

#include "CoreMinimal.h"
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
 * ONE FUNCTION, TWO CALLERS, and the agreement is the whole point: UPlotPresenter seeds off
 * the placed entity's Position, and FPlotPlaceTool's readout seeds off the midpoint of the
 * frontage it is dragging - which is the value URoadEditFacade::PlaceEntityInPlot will store
 * as that Position. Same seed, same yard, so the count previewed is the count built. Two
 * copies of this arithmetic would be a preview quietly describing a different depot.
 *
 * QUANTISED to whole uu: a float that came back from a save one bit different would re-roll
 * that depot and only that depot, which is the kind of bug that takes a day.
 */
AIRSIDE_API int32 DepotYardSeed(FVector2D Where);

class URoadNetwork;

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
	 * CALLED AGAIN AFTER EVERY EDIT, not just at placement, because a depot built correctly
	 * and later reduced would otherwise have been warned about once, at a moment the player
	 * was not looking at it - see URoadSurfacePresenter::RebuildInternal's Topology branch,
	 * the one caller, for where "again" means.
	 */
	AIRSIDE_API void ReportIncomplete(const URoadNetwork& Network);
}
