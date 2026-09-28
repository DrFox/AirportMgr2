#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"
#include "Solve/GridSnap.h"
#include "Templates/Function.h"

class URoadNetwork;
struct IToolPreviewSink;

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

	/**
	 * How far beyond the kerb a gesture's frontage stands on this road, uu. The stand tool's
	 * is the taxiway's clearance strip (TaxiwayStrip::StripWidthOf); the depot's is 0 until
	 * stage 3 of the strip spec asks it. A FUNCTION OF THE ROAD, not a number, because the
	 * anchor search picks the road - the caller cannot know which before it is found.
	 */
	using FRoadSetback = TFunctionRef<double(const URoadNetwork&, FRoadSegmentId)>;

	/** Is this segment a service road - something a truck may drive on? See the .cpp. */
	AIRSIDE_API bool IsServiceRoad(const URoadNetwork& Network, FRoadSegmentId Id);

	/**
	 * Is this segment a taxiway - somewhere an aircraft may taxi and a truck may not?
	 *
	 * NOT `!IsServiceRoad`. A segment with no profile, or a profile declaring no guideline at
	 * all, is neither, and a stand anchored on one would open onto ground nothing can reach.
	 * Asked of the same guidelines IsServiceRoad reads, for the same reason - see the .cpp.
	 */
	AIRSIDE_API bool IsTaxiway(const URoadNetwork& Network, FRoadSegmentId Id);

	/** How far off the centreline the plot's frontage sits on this side, uu. See the .cpp. */
	AIRSIDE_API double KerbOffset(const URoadNetwork& Network, FRoadSegmentId Id, bool bLeftOfSegment);

	/** A frontage length quantised to the plot's own steps. See the .cpp. */
	AIRSIDE_API double QuantisedFrontage(double Raw);

	/**
	 * Where the frontage's far end goes for a cursor at Cursor, running either way along Along
	 * from Anchor: QuantisedFrontage's steps with the grid off, the nearest crossing of the grid
	 * frame along the kerb line with it on. ONE FUNCTION for the stand and the depot, which used to carry
	 * the same three lines each - the grid would have been a fourth and fifth.
	 *
	 * GRID ON KEEPS THE FLOOR: the nearest crossing at least MinFrontageUu from the anchor, so a
	 * zero-length entrance can never be pinned.
	 */
	AIRSIDE_API FVector2D FrontageEnd(const FVector2D& Anchor, const FVector2D& Along,
		const FVector2D& Cursor, const GridSnap::FGridFrame& Grid);

	/** Which anchor point a click would take on this road. See the .cpp. */
	AIRSIDE_API int32 AnchorIndexAt(double SegmentT, double Length);

	/** How far along the road anchor N stands, uu. See the .cpp. */
	AIRSIDE_API double AnchorOffset(int32 Index);

	/**
	 * The accepted road nearest the cursor within AnchorReachUu of its FRONTAGE LINE (the
	 * centreline plus Setback), and where along it the cursor falls. OutSegment and OutT are
	 * written only on a true return. See the .cpp.
	 */
	AIRSIDE_API bool NearestRoad(const URoadNetwork& Network, const FVector2D& Cursor,
		FRoadFilter Accept, FRoadSetback Setback, FRoadSegmentId& OutSegment, double& OutT);

	/** Where a first click would anchor a plot: the pinned corner and the frame it set. */
	struct FAnchor
	{
		/** On the road's own step grid, stood off the kerb on the cursor's side. */
		FVector2D Corner = FVector2D::ZeroVector;

		/** Unit vector along the road at the anchor. */
		FVector2D Along = FVector2D(1.0, 0.0);

		/** Unit vector away from the road, on the side the cursor was when it anchored. */
		FVector2D Inward = FVector2D(0.0, 1.0);

		/**
		 * THE SEARCH'S OWN ANSWER, carried out rather than thrown away - issue #302.
		 * DescribeAnchors used to call AnchorAt for its "is there an anchor here" question and
		 * then NearestRoad a second time for the segment and T it had just found, an O(segments)
		 * search paid for twice on every Idle-stage hover frame. Road/AlongT/RoadA/RoadB are
		 * exactly what NearestRoad and SegmentEnds resolved to reach Corner/Along/Inward above,
		 * so a caller that also wants the road itself - not just where a plot would anchor on
		 * it - reads it back here instead of asking the network again.
		 */
		FRoadSegmentId Road;
		double AlongT = 0.0;
		FVector2D RoadA = FVector2D::ZeroVector;
		FVector2D RoadB = FVector2D::ZeroVector;
	};

	/**
	 * The anchor a first click at Cursor takes on the nearest accepted road, or false with
	 * Out untouched when there is none in reach. ONE RULE, EVERY PLOT TOOL - see the .cpp.
	 *
	 * A Grid that IsOn puts the corner on THAT grid instead of the segment's own bay grid:
	 * the nearest crossing along the kerb line that lies on the segment (world-grid-snap design
	 * section 2). Refused when the segment is shorter than a step and holds none.
	 */
	AIRSIDE_API bool AnchorAt(const URoadNetwork& Network, const FVector2D& Cursor,
		FRoadFilter Accept, FRoadSetback Setback, FAnchor& Out, const GridSnap::FGridFrame& Grid = GridSnap::FGridFrame());

	/**
	 * Draw the anchor grid a first click would snap to - the one AnchorAt would take heavier
	 * than its neighbours. False, having drawn nothing, when no accepted road is in reach:
	 * the caller names the road it wanted, since only it knows which kind that was.
	 */
	AIRSIDE_API bool DescribeAnchors(const URoadNetwork& Network, const FVector2D& Cursor,
		FRoadFilter Accept, FRoadSetback Setback, IToolPreviewSink& Sink, const GridSnap::FGridFrame& Grid = GridSnap::FGridFrame());
}
