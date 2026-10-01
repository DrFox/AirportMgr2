#pragma once

#include "CoreMinimal.h"

class UFlight;
class UFlightBoard;
class ULedger;
struct FFlightPhaseChangedEvent;

/**
 * WHAT A FLIGHT PAYS, AND WHEN (#442 item 4): the landing fee, the parking clock and the parking fee - billing, which
 * UFlightBoard::OnAgentPhase did inline after every agent event it heard, beside the phase mapping it exists for.
 *
 * A Sim-TIER REACTION TO A FLIGHT PHASE EVENT now: UFlightBoard::TransitionTo publishes FFlightPhaseChangedEvent for every
 * change, and UOpsRuntime::WireBus's "Billing" handler hands it to OnFlightPhaseChanged. The board no longer knows money
 * exists beyond the Ledger pointer it is wired with (and two forwarders a test posts through).
 *
 * WHEN MONEY POSTS MOVED, and this is the one behaviour change of #442 item 4: a round later, INSIDE THE SAME DRAIN, for every
 * phase change a drain makes - which is every billed one in play: Landing is entered by the "ArrivalQueue" pass (DispatchNow),
 * Turnaround and TaxiOut by the flight board's FAgentPhaseEvent handler. Both run inside UOpsRuntime::Tick's drain, so the fee is
 * in the ledger when Tick returns, before anything that reads it this frame (the bar's balance, a build quote's CanAfford). A
 * change made OUTSIDE a drain (a test calling TickQueue by hand) bills at the next drain. The FIGURES did not move: the event
 * carries At, the game time the change was dated, and the fee is priced and dated by it - not by the clock when it is heard.
 * ENFORCED BY: AirportOps.Model.FlightFees.BilledOnTheBusARoundLater (when), AirportOps.Present.Bus.BillingIsWired (the
 * subscription, through the runtime, and the same-frame ledger read)
 *
 * PATTERN: Observer, through the ops bus - billing subscribes to the phase rather than being called by the thing that changes
 * it. A NAMESPACE, not an owner object: billing holds no state of its own (what was paid is the FLIGHT's - bLandingFeePaid,
 * bParkingFeePaid, ParkedAt, all saved), so there is nothing to construct or wire but the subscription. The precedent is
 * ServiceText and ArrivalPlanner: rules with no state are functions.
 * ENFORCED BY: Check-Architecture rule 4 (FlightBilling::OnFlightPhaseChanged is called only by the runtime's wiring, and the
 * landing and parking fees are posted from FlightBilling.cpp alone)
 */
namespace FlightBilling
{
	/**
	 * THE REACTION: Event's flight, billed for the phase it entered - the landing fee on Landing, the parking clock started on
	 * Turnaround, the parking fee on TaxiOut - against Board's Ledger. Nothing for any other phase, an unknown flight, or a
	 * board with no ledger (a test that does not care about money). Resolves the flight by id through the board: an event
	 * carries ids, never a UObject that may be gone by the time the queue drains (spec 2026-09-29-ops-event-bus §4).
	 */
	AIRPORTOPS_API void OnFlightPhaseChanged(UFlightBoard& Board, const FFlightPhaseChangedEvent& Event);

	/**
	 * Bank the landing fee this flight was OFFERED at. Idempotent - a flight lands once. Ledger may be null (nothing posted).
	 * UFlightBoard::PostLandingFee forwards here.
	 */
	AIRPORTOPS_API void PostLandingFee(ULedger* Ledger, double Now, UFlight& Flight);

	/** Bank the parking fee for the hours actually occupied, at the rate the flight was OFFERED at (UFlight::
	 *  ParkingRatePerHour), and record it on the flight. UFlightBoard::PostParkingFee forwards here. */
	AIRPORTOPS_API void PostParkingFee(ULedger* Ledger, double Now, UFlight& Flight);
}
