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

	/** What the quotes would read, now - see FLandChoicesKey. Traffic may be null (no occupancy to date). */
	AIRPORTMGR_API FLandChoicesKey KeyFor(const URoadNetwork* Network, const UGroundTraffic* Traffic,
		const FVector2D& Near, bool bAdmits, bool bQuotes);
}
