#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

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
	 * stand that would fit is already held, which is the refusal the inbox shows.
	 */
	bool Reserve(UGroundTraffic& Traffic, const URoadNetwork& Network, UFlight& Flight);

	/** Give the hold back. Safe on a flight that never had one. */
	void Release(UGroundTraffic& Traffic, UFlight& Flight);

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
