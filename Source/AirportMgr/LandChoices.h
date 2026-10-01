#pragma once

#include "CoreMinimal.h"
#include "Model/FlightBoard.h"

class UAircraftType;
class UGroundTraffic;
class URoadNetwork;

/**
 * Everything the Land panel's quotes read, bar the types (read once, fixed for the session) - so the panel quotes its
 * rows only when this moves (ops batch 3 PR E; it judged every type every frame while open). Each row is a whole
 * arrival plan since #432 (UOpsRuntime::QuoteLanding), so this is what keeps a still panel from planning at all:
 *  - the runway the plan asks FIRST from Near (ArrivalPlanner::FirstLandingRunway) - the one input that moves with
 *    the camera, and all Near decides in a plan (which runway's refusal is reported when none will do). A pan along one
 *    runway changes nothing; a pan onto another does.
 *  - the graph: EditRevision (segments and nodes, a drag included) and GuidelineRevision (facts, use, the derived
 *    graph - through the facade, whose Topology notify rebuilds it).
 *  - the traffic's OccupancyRevision: a stand held or freed moves NoFreeStand, which the plan reports.
 *  - the network object: a clear or a load is a new one, counting from zero - a weak pointer, so a recycled address
 *    is not mistaken for the old network.
 *  - whether the airport admits arrivals (UAirport::AdmitsArrivals) - the gate every quote ends with - and whether
 *    there is a runtime to quote from at all.
 * ENFORCED BY: AirportMgr.UI.LandPanelBuildsOnlyOnChange (one step per revision and the runway, each red when its field
 * is left out of ==); AirportMgr.UI.LandChoicesKeyNamesTheNetwork (two networks, equal revisions);
 * AirportMgr.UI.LandPanelGreysWhileClosed (the gate, alone); Check-Architecture rule 35 (facts-through-facade) for
 * "through the facade".
 */
struct FLandChoicesKey
{
	/** Identity only - see FInspectorCardKey::Network for why weak and untyped. */
	FWeakObjectPtr Network;
	uint32 EditRevision = 0;
	uint32 GuidelineRevision = 0;
	uint32 OccupancyRevision = 0;
	/** ArrivalPlanner::FirstLandingRunway(Near)'s index, INDEX_NONE with no landing runway. */
	int32 FirstRunway = INDEX_NONE;
	bool bAdmits = true;
	bool bQuotes = false;

	bool operator==(const FLandChoicesKey& Other) const
	{
		return Network == Other.Network && EditRevision == Other.EditRevision && GuidelineRevision == Other.GuidelineRevision
			&& OccupancyRevision == Other.OccupancyRevision && FirstRunway == Other.FirstRunway && bAdmits == Other.bAdmits
			&& bQuotes == Other.bQuotes;
	}
	bool operator!=(const FLandChoicesKey& Other) const { return !(*this == Other); }

	/** Equal in everything but OccupancyRevision - the one change LandChoices::RequoteForOccupancy answers (#471). */
	bool SameButOccupancy(const FLandChoicesKey& Other) const
	{
		FLandChoicesKey Aligned = Other;
		Aligned.OccupancyRevision = OccupancyRevision;
		return *this == Aligned;
	}
};

/** One row of the Land panel: an aircraft type, and whether the game would land it now. */
struct FLandChoice
{
	/** Held alive by the panel's own UPROPERTY list, not by this plain struct. */
	UAircraftType* Type = nullptr;

	/** "C · A320-200" - the ICAO letter, then the name. */
	FText Label;

	/** The quote accepts it: a click on this row is a landing the game takes. */
	bool bAdmitted = false;

	/** Why not - the quote's own sentence (ArrivalPlanner::DescribeRefusal's, or the airport's gate) - or empty. */
	FString Refusal;

	/** The quote's reason, None when admitted - kept so RequoteForOccupancy can tell a row occupancy could change. */
	EArrivalRefusal Why = EArrivalRefusal::None;
};

/**
 * What the Land panel offers, and which of it the game would take - world-free, so it is tested with any quote.
 *
 * A NAMESPACE OF THREE FUNCTIONS, not a class: there is no state to own. The widget is the presentation; the verdict
 * is the MODEL's (UOpsRuntime::QuoteLanding, #432) - this keeps the labels and the order, and renders the quote. It
 * judged each type itself until #432, by the nearest runway alone, which had been stale since #412 made the planner
 * land on whichever runway takes the arrival.
 */
namespace LandChoices
{
	/** Every aircraft type with a model - UAirsideSettings::EveryAircraftType(true), the one registry scan. The panel
	 *  exists to WATCH something land; the paper types would land as the fallback model. */
	AIRPORTMGR_API TArray<UAircraftType*> EveryMeshedType();

	/**
	 * One choice per type, sorted by ICAO letter then name, each the quote Quote gives its airframe - the widget's is
	 * UOpsRuntime::QuoteLanding at the view focus, the point the click lands at.
	 */
	AIRPORTMGR_API TArray<FLandChoice> Build(const TArray<UAircraftType*>& Types,
		TFunctionRef<FArrivalQuote(const FAirframe&)> Quote);

	/**
	 * THE OCCUPANCY-ONLY RE-QUOTE (#471 item 5): re-quote, in place, only the rows a change of occupancy CAN change - the
	 * admitted, and those refused for a reason that clears on its own (ArrivalPlanner::IsPermanentRefusal false: a held
	 * stand, a busy runway). A PERMANENT refusal - no exit, no route, a stand in a strip, a strip too short - is the
	 * player's to fix by an edit, and every edit moves another field of FLandChoicesKey, which re-builds every row. So
	 * this is EXACT, not a throttle: a row it skips cannot have changed. Returns how many rows it quoted.
	 *
	 * WHY IT EXISTS - MEASURED 2026-10-01 (AirportMgr.UI.LandPanelCostOnAScaleField, Development editor): on #256's scale
	 * field (2 runways, 8x20 taxiway grid, 30 stands) a whole re-quote of the 18 meshed types cost 130 ms (field sized for
	 * the smallest type) to 357 ms (for the largest) - ~13-21 ms for each type whose search fails, against a 16.7 ms
	 * frame - and the open panel paid it on EVERY occupancy move. The failing searches are the permanent refusals; an
	 * admitted type planned in ~0.1 ms on a 30-stand line. What this does not bound: rows refused NoFreeStand on a full
	 * field are failing searches too, and are re-quoted - the planner's reachable-stand search is the cost to cut there.
	 * ENFORCED BY: AirportMgr.UI.LandPanelRequotesOnlyWhatOccupancyCanChange, AirportMgr.UI.LandPanelCostOnAScaleField
	 */
	AIRPORTMGR_API int32 RequoteForOccupancy(TArray<FLandChoice>& Rows, TFunctionRef<FArrivalQuote(const FAirframe&)> Quote);

	/** What the quotes would read, now - see FLandChoicesKey. Traffic may be null (no occupancy to date). */
	AIRPORTMGR_API FLandChoicesKey KeyFor(const URoadNetwork* Network, const UGroundTraffic* Traffic,
		const FVector2D& Near, bool bAdmits, bool bQuotes);
}
