#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"
#include "Templates/Function.h"

class URoadNetwork;

/**
 * The road-snapping half of a staged plot gesture, shared by every tool that draws one.
 *
 * MOVED OUT OF PlotPlaceTool.cpp's anonymous namespace when a second tool needed the same
 * gesture (the stand, drawn off a taxiway - see the 2026-09-23 drawn-stands spec). Two copies
 * of the anchor grid would be two tools whose plots could never sit flush beside each other,
 * which is the whole reason the frontage has a quantum. The WHY comments on each function
 * live beside its definition in PlotGesture.cpp, where they were written.
 */
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

	/** Which segments a gesture may anchor on - IsServiceRoad, IsTaxiway, or a test's own. */
	using FRoadFilter = TFunctionRef<bool(const URoadNetwork&, FRoadSegmentId)>;

	/** Is this segment a service road - something a truck may drive on? See the .cpp. */
	AIRSIDE_API bool IsServiceRoad(const URoadNetwork& Network, FRoadSegmentId Id);

	/** How far off the centreline the plot's frontage sits on this side, uu. See the .cpp. */
	AIRSIDE_API double KerbOffset(const URoadNetwork& Network, FRoadSegmentId Id, bool bLeftOfSegment);

	/** A frontage length quantised to the plot's own steps. See the .cpp. */
	AIRSIDE_API double QuantisedFrontage(double Raw);

	/** Which anchor point a click would take on this road. See the .cpp. */
	AIRSIDE_API int32 AnchorIndexAt(double SegmentT, double Length);

	/** How far along the road anchor N stands, uu. See the .cpp. */
	AIRSIDE_API double AnchorOffset(int32 Index);

	/**
	 * The accepted road nearest the cursor within AnchorReachUu, and where along it the cursor
	 * falls. OutSegment and OutT are written only on a true return. See the .cpp.
	 */
	AIRSIDE_API bool NearestRoad(const URoadNetwork& Network, const FVector2D& Cursor,
		FRoadFilter Accept, FRoadSegmentId& OutSegment, double& OutT);
}
