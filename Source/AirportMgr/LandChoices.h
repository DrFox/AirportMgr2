#pragma once

#include "CoreMinimal.h"

class UAircraftType;
class URoadNetwork;

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
		const TArray<UAircraftType*>& Types);
}
