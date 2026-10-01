#include "Model/FlightBilling.h"

#include "Model/Flight.h"
#include "Model/FlightBoard.h"
#include "Model/Ledger.h"
#include "Model/OpsEventBus.h"

// BILLING, A REACTION TO THE PHASE (#442 item 4) - see FlightBilling.h. PostLandingFee and PostParkingFee MOVED here unchanged from
// UFlightBoard (the Ledger they post to is handed in rather than read off `this`); OnFlightPhaseChanged is OnAgentPhase's "THE
// MONEY" block, asked of the event's phase instead of the flight's after the change.

void FlightBilling::PostLandingFee(ULedger* Ledger, double Now, UFlight& Flight)
{
	// bLandingFeePaid AND NOT "is the phase Landing": OnAgentPhase fires for every agent phase
	// change, and more than one of them can map to EFlightPhase::Landing. A flag on the flight
	// is the only thing that survives a reload as well - see UFlight::bLandingFeePaid. SINCE #442 item 4 the reaction hears an
	// ENTRY into Landing, once a landing; the flag is still what keeps a load's re-queued flight, landing a second time, unpaid.
	if (Ledger == nullptr || Flight.LandingFee <= 0.0 || Flight.bLandingFeePaid)
	{
		return;
	}

	Flight.bLandingFeePaid = true;
	Ledger->Post(Now, ELedgerCategory::LandingFee, Flight.LandingFee,
		FText::Format(NSLOCTEXT("Ledger", "LandingBy", "Landing: {0}"), Flight.AirlineName));
}

void FlightBilling::PostParkingFee(ULedger* Ledger, double Now, UFlight& Flight)
{
	// ParkedAt of zero means it never parked - see UFlight::ParkedAt for the two ways a flight
	// reaches TaxiOut without having done so, and for what billing from the epoch would cost.
	// ONCE PER FLIGHT, like the landing fee: an aeroplane that parks again after its taxi out began enters TaxiOut a second time, and
	// would be billed the overlapping hours again from the original ParkedAt (#442 review).
	// ENFORCED BY: AirportOps.Model.FlightFees.ParkingIsBilledOncePerFlight
	if (Ledger == nullptr || Flight.ParkedAt <= 0.0 || Flight.bParkingFeePaid)
	{
		return;
	}

	// THE RATE THE FLIGHT WAS OFFERED AT (#442), not the lever as it stands at departure: UPricing::ParkingFeePerHour applies the
	// CURRENT landing-fee multiplier, so asking it here let a player accept cheaply and put the price up before the aeroplane
	// left - the trade UOfferGenerator::MakeOffer rules out for the landing fee. A flight never offered (the debug land key's)
	// carries no rate and so pays no parking, as it already paid no landing fee.
	// ENFORCED BY: AirportOps.Model.FlightFees.ParkingIsBilledAtTheOffersRate
	const double Hours = FMath::Max(0.0, (Now - Flight.ParkedAt) / 3600.0);
	const double Fee = Flight.ParkingRatePerHour * Hours;
	if (Fee <= 0.0)
	{
		return;
	}

	Flight.bParkingFeePaid = true;
	Flight.ParkingFee = Fee;
	Ledger->Post(Now, ELedgerCategory::ParkingFee, Fee,
		FText::Format(NSLOCTEXT("Ledger", "ParkingBy", "Parking: {0}"), Flight.AirlineName));
}

void FlightBilling::OnFlightPhaseChanged(UFlightBoard& Board, const FFlightPhaseChangedEvent& Event)
{
	// THE FLIGHT, BY ID: an event never carries the UObject (spec 2026-09-29-ops-event-bus §4). An id that finds no flight - one
	// RollUp has forgotten - pays nothing.
	UFlight* Flight = Board.FlightById(Event.FlightId);
	if (Flight == nullptr)
	{
		return;
	}

	// THE MONEY, at two phases and those two specifically.
	//
	// Landing is where an aeroplane becomes the airport's business. TaxiOut is the one phase
	// EVERY departure reaches - Manoeuvring is NOT, because an aeroplane parked within
	// StraightOutDegrees of its exit heading simply drives out and never enters it (commit
	// 021cc2e). Charging parking at a phase some flights never enter would be a fee that went
	// silently uncollected on exactly the layouts the player built best.
	// THE CHANGE'S OWN TIME, carried by the event - not the clock when it is heard, a round later: the fee is priced and dated
	// as it was when this ran inline (FFlightPhaseChangedEvent::At).
	const double Now = Event.At;
	if (Event.To == EFlightPhase::Landing)
	{
		PostLandingFee(Board.Ledger, Now, *Flight);
	}
	else if (Event.To == EFlightPhase::Turnaround && Flight->ParkedAt <= 0.0)
	{
		// The start of the parking clock, taken once - Turnaround is reached again by anything
		// that re-enters it, and the second visit must not restart the meter in the player's
		// favour.
		Flight->ParkedAt = Now;
	}
	else if (Event.To == EFlightPhase::TaxiOut)
	{
		// ON ENTERING TAXIOUT, NOT ON EVERY EVENT WHILE IN IT (#442): this block ran after every agent event, so a redirect of an
		// aeroplane already taxiing out (an edit re-routed it, a stranding was rescued) found the flight still TaxiOut and posted
		// the parking fee AGAIN, for a longer stay, as a second ledger row. The landing fee is guarded by its own flag and
		// the turnaround stamp by ParkedAt; this one had no guard. A PHASE EVENT IS AN ENTRY (#442 item 4): TransitionTo publishes
		// FFlightPhaseChangedEvent only for a change, so the bChanged that guarded this inline is the event's own premise now.
		// ENFORCED BY: AirportOps.Model.FlightFees.ParkingIsBilledOnceAcrossARedirect
		PostParkingFee(Board.Ledger, Now, *Flight);
	}
}
