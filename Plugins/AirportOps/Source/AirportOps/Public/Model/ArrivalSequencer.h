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
 * WORLD-FREE: "could this flight land now" arrives as a predicate, so the policy is testable
 * with no network at all. UFlightBoard::TickQueue passes its cached clearance - the WHOLE plan
 * (runway, exit, stand), not just the runway: a runway-busy-only test returned a flight the
 * dispatch then refused, every frame, and let one stuck flight block the queue (review C1/C2).
 */
UCLASS()
class AIRPORTOPS_API UArrivalSequencer : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * The FIRST flight in queue order that could land now, or null.
	 *
	 * FIRST-THAT-CAN-LAND, not strictly the head: a flight for a free second runway must not
	 * wait behind one for a busy runway, and nobody waits behind a flight the field can no
	 * longer take. With one runway and nothing stuck it is plain first come.
	 */
	UFlight* Next(TArrayView<UFlight* const> Queue, TFunctionRef<bool(const UFlight&)> CanClear) const;
};
