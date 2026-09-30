#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Model/RoadHandles.h"

#include "StandAllocator.generated.h"

class UFlight;
class UGroundTraffic;
class URoadNetwork;

/**
 * Which stand a flight gets, and holding it so that nobody else does.
 *
 * FIRST FIT BY SIZE, SMALLEST THAT ADMITS. A Code C stand is wasted on a Piper while a 737
 * waits for it, and smallest-fit is the cheapest statement of "do not spend the big one".
 *
 * IT HOLDS; IT DOES NOT CHOOSE THE ARRIVAL'S STAND. ArrivalPlanner picks the stand the
 * aeroplane actually taxis to, from the nearest free one at the moment it lands. What this
 * guarantees is that A stand exists for the flight when the player accepts it - which is the
 * whole of "the player cannot over-commit". See UFlight::Stand for why the two can differ
 * and who reconciles them.
 *
 * AN ACCEPT HOLDS THE PLAN'S STAND (#431), through Hold: UFlightBoard::TryAccept passes the stand
 * the accept's own plan taxis to, which is reachable. Reserve's smallest fit is REACH-BLIND and is
 * left to UFlightBoard's re-holds (a flight whose hold was lost) - a known gap, #471.
 * ENFORCED BY: Check-Architecture rule 4 ('UStandAllocator::Reserve (reach-blind hold)' - FlightBoard.cpp only)
 */
UCLASS()
class AIRPORTOPS_API UStandAllocator : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Hold the smallest live stand whose design wingspan admits this flight's type.
	 *
	 * Writes UFlight::Stand and returns true; or changes nothing and returns false when every
	 * stand that would fit is already held. Not reachability: see the class comment.
	 */
	bool Reserve(UGroundTraffic& Traffic, const URoadNetwork& Network, UFlight& Flight);

	/**
	 * Hold THIS stand for the flight - the one an arrival plan chose (FArrivalPlan::StandNode), which UFlightBoard::
	 * TryAccept passes (#431) - after the same checks Reserve makes of every stand it considers: live, a stand, admitted
	 * (StandAdmission::Judge), and not held by another. Writes UFlight::Stand and returns true, or changes nothing.
	 * Reserve holds its own choice through here, so the two cannot disagree about what holding is.
	 */
	bool Hold(UGroundTraffic& Traffic, const URoadNetwork& Network, UFlight& Flight, FEntityInstanceId Stand);

	/** Give the hold back. Safe on a flight that never had one. */
	void Release(UGroundTraffic& Traffic, UFlight& Flight);

	/**
	 * True when Flight holds a stand that is no longer there to hold - deleted, or left with no pose node.
	 * ONE TEST, read by Reapply (which warns) and by the ops HeldStandLost alert (which tells the player),
	 * so the two cannot disagree about what "gone" means.
	 */
	static bool HeldStandIsGone(const UFlight& Flight, const URoadNetwork& Network);

	/**
	 * Re-make every hold after a graph rebuild.
	 *
	 * UGroundTraffic::OnGraphRebuilt goes through FTrafficOccupancy::ReleaseGuidelineClaims,
	 * which removes every Edge and Node claim because those resources have ceased to exist.
	 * Holds are Node claims, so they go with them, and NOTHING ANNOUNCES IT to a caller: the
	 * player edits a taxiway and every accepted flight quietly loses its stand.
	 *
	 * The flight's saved truth is the stand ENTITY, whose handle survives a rebuild, so the
	 * pose node can be looked up again on the new graph.
	 */
	void Reapply(UGroundTraffic& Traffic, const URoadNetwork& Network,
		const TArray<UFlight*>& Held);
};
