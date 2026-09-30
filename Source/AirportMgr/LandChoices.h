#pragma once

#include "CoreMinimal.h"
#include "Model/Airport.h"

class UAircraftType;
class URoadNetwork;

/**
 * Everything LandChoices::Build reads, bar the types (read once, fixed for the session) - so the Land panel builds
 * its rows only when this moves (ops batch 3 PR E; it planned every type every frame while open). Checked against
 * Build, 2026-09-30:
 *  - the runway a landing from Near would use: URoadNetwork::NearestRunwayThreshold, asked here - the one input that
 *    moves with the camera - reduced to its SEED, since CheckArrival asks only of the seed. A pan along one runway
 *    changes nothing; a pan onto another does.
 *  - that runway's length (segments and nodes: EditRevision, a drag included) and facts (surface, approach, use:
 *    through the facade, whose Topology notify rebuilds the guideline graph - GuidelineRevision).
 *  - the network object: a clear or a load is a new one, counting from zero - a weak pointer, so a recycled address
 *    is not mistaken for the old network.
 * NOT the occupancy, and not a runway-freed count, which the spec named: Build asks what the runway ADMITS, never whether
 * it is busy.
 *  - THE AIRPORT'S STATUS (whole-stack review M1): a closed airport admits no arrivals (PR B ruling I1), so while it
 *    is not Open every row is a click the game would refuse, and Build greys them all. It used to grey only the bar's
 *    Land button, leaving an open panel offering clicks the game then refused.
 * ENFORCED BY: AirportMgr.UI.LandPanelBuildsOnlyOnChange (one step per revision and the seed, each red when its field
 * is left out of ==); AirportMgr.UI.LandChoicesKeyNamesTheNetwork (two networks, equal revisions);
 * AirportMgr.UI.LandPanelGreysWhileClosed (the status, alone); Check-Architecture
 * rule 35 (facts-through-facade) for "through the facade".
 */
struct FLandChoicesKey
{
	/** Identity only - see FInspectorCardKey::Network for why weak and untyped. */
	FWeakObjectPtr Network;
	uint32 EditRevision = 0;
	uint32 GuidelineRevision = 0;
	bool bHasRunway = false;
	int32 Seed = INDEX_NONE;
	EAirportStatus Status = EAirportStatus::Open;

	bool operator==(const FLandChoicesKey& Other) const
	{
		return Network == Other.Network && EditRevision == Other.EditRevision && GuidelineRevision == Other.GuidelineRevision
			&& bHasRunway == Other.bHasRunway && Seed == Other.Seed && Status == Other.Status;
	}
	bool operator!=(const FLandChoicesKey& Other) const { return !(*this == Other); }
};

/** One row of the Land panel: an aircraft type, and whether the airport can take it now. */
struct FLandChoice
{
	/** Held alive by the panel's own UPROPERTY list, not by this plain struct. */
	UAircraftType* Type = nullptr;

	/** "C · A320-200" - the ICAO letter, then the name. */
	FText Label;

	/** The runway a landing from here would use admits this type. */
	bool bAdmitted = false;

	/** Why not, in one short phrase, or empty when admitted. */
	FString Refusal;
};

/**
 * What the Land panel offers, and which of it the airport can take - world-free, so it is
 * tested with a bare URoadNetwork and no widget.
 *
 * A NAMESPACE OF TWO FUNCTIONS, not a class: there is no state to own. The widget is the
 * presentation; this is the answer it presents.
 */
namespace LandChoices
{
	/**
	 * Every aircraft type with a model, from the asset registry.
	 *
	 * THE SAME RULE AS the test helper EveryAircraftType (Testing/AirsideTestWorld.h), which
	 * cannot be called from here - it is compiled only WITH_DEV_AUTOMATION_TESTS. The paper
	 * types (DA_Aircraft_A320, _B738) carry no mesh and are left out: the panel exists to
	 * WATCH something land, and they would land as the fallback model.
	 */
	AIRPORTMGR_API TArray<UAircraftType*> EveryMeshedType();

	/**
	 * One choice per type, sorted by ICAO letter then name, each judged against the runway
	 * a landing near Near would use.
	 *
	 * THE RUNWAY NEAREST Near, AND ONLY THAT ONE - URoadNetwork::NearestRunwayThreshold then
	 * RunwayAdmission::CheckArrival, the two calls ArrivalPlanner::Plan makes, in its order. The
	 * planner does NOT fall back to another runway when that one refuses (checked 2026-09-27,
	 * ArrivalPlanner.cpp step 1), so a panel that greyed only what NO runway admits would
	 * offer clicks the game then refuses. Near is the view focus, the point the click lands at.
	 *
	 * Null Network, or none with a runway, refuses everything with a reason.
	 */
	AIRPORTMGR_API TArray<FLandChoice> Build(const URoadNetwork* Network, const FVector2D& Near,
		const TArray<UAircraftType*>& Types, EAirportStatus Status = EAirportStatus::Open);

	/** What Build(Network, Near, ...) would read, now - see FLandChoicesKey. One NearestRunwayThreshold. */
	AIRPORTMGR_API FLandChoicesKey KeyFor(const URoadNetwork* Network, const FVector2D& Near,
		EAirportStatus Status = EAirportStatus::Open);
}
