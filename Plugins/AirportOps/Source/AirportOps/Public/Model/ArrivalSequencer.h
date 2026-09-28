#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "ArrivalSequencer.generated.h"

class UFlight;

/**
 * Who, of the flights holding for the runway, is cleared to land next.
 *
 * POLICY ONLY, and deliberately so (spec 2026-09-28-arrival-queue section 2): the board owns the
 * flights, derives the queue from them and is the one door onto dispatch; this decides the
 * ORDER. It owns no list, so there is nothing to save and nothing to keep in step with the
 * board. It is its own class rather than a function on the board because it is the seam the
 * ATC tower upgrades - capacity, separation, departure priority - without touching dispatch.
 *
 * WORLD-FREE: "is this flight's runway busy" arrives as a predicate, so the policy is testable
 * with no network at all. UFlightBoard::TickQueue passes ArrivalPlanner::IsRunwayBusy.
 */
UCLASS()
class AIRPORTOPS_API UArrivalSequencer : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * The FIRST flight in queue order whose runway is free, or null.
	 *
	 * FIRST-WHOSE-RUNWAY-IS-FREE, not strictly the head: with two runways, a flight for the free
	 * one must not wait behind a flight for the busy one. With one runway it is plain first come.
	 */
	UFlight* Next(TArrayView<UFlight* const> Queue, TFunctionRef<bool(const UFlight&)> IsRunwayBusy) const;
};
