#pragma once

#include "CoreMinimal.h"
#include "Model/Pavement.h"
#include "Tool/RoadBuildTool.h"

/**
 * The pavement scale as a tool's variant row. In Tool/ because FToolVariantAxis is a tool
 * concept and Model/ must not see it; Pavement::Offered, which the row and
 * URoadNetwork::SetSegmentSurface both read, stays in Model/Pavement.h for that reason.
 */
namespace Pavement
{
	/**
	 * The "Surface" variant row, for every tool that lays ground - runway, road, stand. ONE
	 * BUILDER so the three rows cannot name or order the scale differently. Current is lit
	 * by its index in Offered(Allowed); a Current the list does not offer lights nothing
	 * (INDEX_NONE) rather than a neighbour.
	 *
	 * THE ENUM'S OWN NAMES, from Model/Pavement.h (Pavement::Name) - the same strings the
	 * runway drag readout and the road connect log line print, so the row and the log cannot
	 * name one surface two ways.
	 */
	AIRSIDE_API void AppendAxis(TArray<FToolVariantAxis>& Out, EPavement Current, TConstArrayView<EPavement> Allowed);
}
