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
