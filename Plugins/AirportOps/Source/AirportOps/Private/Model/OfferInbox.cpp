#include "Model/OfferInbox.h"

#include "AirportOpsLog.h"
#include "Model/Flight.h"
#include "Model/FlightBoard.h"
#include "Model/GroundTraffic.h"
#include "Model/JobBoard.h"
#include "Model/RoadNetwork.h"
#include "Model/SimClock.h"
#include "Model/StandAllocator.h"

// THE OFFERS' HALF OF UFlightBoard (#442 item 4) - see FOfferInbox's class comment for the pattern and its deviations. Every body
// below MOVED here unchanged but for reaching the board through the Board it is handed (Board.TransitionTo, Board.LiveFlights(),
// Board.PlanQuote, Board.Allocator...), where it used to be the board's own `this`.

void FOfferInbox::TickOffers(UFlightBoard& Board, const UGroundTraffic& Traffic, const URoadNetwork& Network,
	const USimClock& Clock, double RealDeltaSeconds)
{
	// PAUSE STOPS IT; SPEED DOES NOT. RealDeltaSeconds is the raw frame time - never scaled
	// by Multiplier() or TimeScale() - so x4 drains exactly as fast as x1.
	if (Clock.IsPaused())
	{
		return;
	}

	// NO OFFERS, NOTHING TO DRAIN (ops batch 3 PR E): the walk below copied Flights every frame to find none. The count is
	// the board's own, kept by AddOffer, every lapse and answer, and rebuilt from Flights with the indices on a load.
	// ENFORCED BY: AirportOps.Model.FlightBoard.EmptyBoardCopiesNothing (an offer added still drains and lapses)
	if (Board.PendingOfferCount() == 0)
	{
		return;
	}

	// SNAPSHOT: a lapse calls MoveToHistory, which removes from the array being walked.
	const TArray<TObjectPtr<UFlight>> Snapshot = Board.LiveFlights();
	++OfferSnapshots;   // See OfferSnapshotCountForTest.
	for (const TObjectPtr<UFlight>& Each : Snapshot)
	{
		if (Each == nullptr || Each->GetPhase() != EFlightPhase::Offered)
		{
			continue;
		}
		// JUDGED BEFORE IT DRAINS, so an offer acceptable in its last frame counts as one the
		// player could have taken.
		if (VerdictFor(Board, Traffic, Network, *Each).Why == EArrivalRefusal::None)
		{
			Each->bWasEverAcceptable = true;
		}
		Each->OfferSecondsLeft -= FMath::Max(RealDeltaSeconds, 0.0);
		if (Each->OfferSecondsLeft > 0.0)
		{
			continue;
		}
		Each->OfferSecondsLeft = 0.0;
		// THE REASON BEFORE THE TRANSITION: the Expired row publishes it (FOfferExpiredEvent carries LapseReason), and the
		// row owns the count, the publish, the move to History and the revision that used to follow here by hand.
		Each->LapseReason = Each->bWasEverAcceptable ? ELapseReason::Ignored : ELapseReason::NeverAcceptable;
		UE_LOG(LogAirportOps, Log, TEXT("Offer %d (%s) lapsed unanswered (%s)"), Each->Id, *Each->Callsign,
			Each->LapseReason == ELapseReason::Ignored ? TEXT("ignored") : TEXT("never acceptable"));
		Board.TransitionTo(*Each, EFlightPhase::Expired, FTransitionCause::Played(Clock.Now()));
	}
}

const FOfferVerdict& FOfferInbox::VerdictFor(const UFlightBoard& Board, const UGroundTraffic& Traffic,
	const URoadNetwork& Network, const UFlight& Flight) const
{
	FOfferVerdict& Verdict = Verdicts.FindOrAdd(Flight.Id);
	const uint32 BoardNow = Board.Revision();
	const uint32 GuidelineNow = Network.GetGuidelineRevision();
	const uint32 OccupancyNow = Traffic.OccupancyRevision();
	// THE NETWORK AND ITS EDIT CLOCK (#471) - FLandChoicesKey's two: a pre-drag yes must not be served mid-drag (the plan
	// answers GraphBeingEdited then, and the stand it named is about to be held), and another network's equal numbers are
	// not this graph. See FOfferVerdict.
	const uint32 EditNow = Network.GetEditRevision();
	// AND THE STAND CHURN (#506 review) - see FOfferVerdict::StandChurnAt: a body on or off a stand's pose moves no OccupancyRevision,
	// and this plan reads exactly that. The clearance and the Land panel read it already, so the inbox now agrees with both. THE COST is
	// one plan per pending offer per change of the held-stand set: a full inbox (8, OpsDesignDefaults::MaxPendingOffers) re-planned in
	// 0.84 ms on the 30-stand line, measured 2026-10-01 (AirportOps.Model.FlightBoard.StandChurnReplansEachOfferOnce, "StandChurnCost:").
	// An offer refused NoFreeStand is a failing search, ~13-21 ms on #256's scale field (LandChoices::RequoteForOccupancy's figure), so
	// a full inbox on a full big field could pay ~100-170 ms a change - but OccupancyRevision already re-planned every offer on the
	// claim, hold and phase change that brings a body to a stand or takes it away, so this adds at most one re-plan beside each.
	const uint32 StandChurnNow = Traffic.StandHoldChangeCount();
	const bool bSameNetwork = Verdict.Network.Get() == &Network;
	// THE FLEET'S COMPOSITION, not its transitions (#443): CouldServe reads which vehicles exist, of what kind and where,
	// and never a vehicle's state, so a truck arriving or finishing a refill must not re-plan the offer.
	const uint32 FleetNow = Board.Fuel != nullptr ? Board.Fuel->GetFleetCompositionRevision() : 0;
	// BOTH DECIDED BEFORE EITHER IS REDONE: the guideline stamp is shared, and the first recompute would write it.
	const bool bPlanStale = !Verdict.bValid || !bSameNetwork || Verdict.BoardAt != BoardNow || Verdict.GuidelineAt != GuidelineNow
		|| Verdict.OccupancyAt != OccupancyNow || Verdict.EditAt != EditNow || Verdict.StandChurnAt != StandChurnNow;
	// bFuelServable is CouldServe(Traffic, Network, Flight.Airframe): the airport's shape (the guideline graph - depots,
	// roads, stands, modules) and the fleet's composition, which includes which vehicles are stranded (#443: the traffic model
	// is asked who is, and the composition counter moves when one strands or moves again). Not the board (the airframe is the
	// flight's own, fixed) and not occupancy (it judges no traffic's whereabouts).
	// ENFORCED BY: AirportOps.Fuel.CouldServe.StrandingMovesTheCompositionAndTheVerdict
	const bool bFuelStale = !Verdict.bValid || !bSameNetwork || Verdict.GuidelineAt != GuidelineNow || Verdict.FleetAt != FleetNow;
	if (bPlanStale)
	{
		// THE REAL PLAN, with the live occupancy. The greyed-out reason is the sentence the
		// arrival itself would print, because it is the same refusal - and since #431 the
		// stand the plan taxis to is kept with it, for TryAccept to hold.
		const FArrivalQuote Plan = Board.PlanQuote(Traffic, Network, Flight.Airframe, Flight.RunwayPreference, 0);
		Verdict.Why = Plan.Why;
		Verdict.Sentence = Plan.Sentence;
		Verdict.Stand = Plan.Stand;
		Verdict.BoardAt = BoardNow;
		Verdict.OccupancyAt = OccupancyNow;
		Verdict.EditAt = EditNow;
		Verdict.StandChurnAt = StandChurnNow;
	}
	if (bFuelStale)
	{
		Verdict.bFuelServable = Board.Fuel == nullptr || Board.Fuel->CouldServe(Traffic, Network, Flight.Airframe);
		Verdict.FleetAt = FleetNow;
	}
	Verdict.GuidelineAt = GuidelineNow;
	Verdict.Network = &Network;
	Verdict.bValid = true;
	return Verdict;
}

FArrivalQuote FOfferInbox::QuoteFor(const UFlightBoard& Board, const UGroundTraffic& Traffic, const URoadNetwork& Network,
	const UFlight& Flight) const
{
	const FOfferVerdict& Verdict = VerdictFor(Board, Traffic, Network, Flight);
	FArrivalQuote Quote;
	Quote.Why = Verdict.Why;
	Quote.Sentence = Verdict.Sentence;
	Quote.Stand = Verdict.Stand;
	return Board.Gated(MoveTemp(Quote));
}

FArrivalQuote FOfferInbox::QuoteArrival(const UFlightBoard& Board, const UGroundTraffic& Traffic, const URoadNetwork& Network,
	const FAirframe& Airframe, const FVector2D& Focus) const
{
	return Board.Gated(Board.PlanQuote(Traffic, Network, Airframe, Focus, 0));
}

FArrivalQuote FOfferInbox::TryAccept(UFlightBoard& Board, UGroundTraffic& Traffic, const URoadNetwork& Network, USimClock& Clock,
	UFlight& Flight)
{
	// REFUSED BEFORE THE PLAN, each worded and logged - a caller's bug or a fixture's, never a player's (see the header).
	// NotAdmitted for Gated's reason: EArrivalRefusal has no value for these, so the Sentence carries the real one.
	const auto Refuse = [&Flight](const TCHAR* Why)
	{
		UE_LOG(LogAirportOps, Log, TEXT("Flight %d not accepted: %s"), Flight.Id, Why);
		FArrivalQuote Out;
		Out.Why = EArrivalRefusal::NotAdmitted;
		Out.Sentence = FString::Printf(TEXT("Arrival refused: %s."), Why);
		return Out;
	};
	if (Flight.GetPhase() != EFlightPhase::Offered)
	{
		return Refuse(TEXT("it is not an open offer"));
	}
	if (Board.Allocator == nullptr)
	{
		return Refuse(TEXT("the board has no stand allocator to hold a stand with"));
	}
	if (Flight.Airframe.Wingspan <= 0.0)
	{
		return Refuse(TEXT("it has no airframe"));
	}

	// THE ONE EVALUATOR (#431): the cached plan verdict, then the airport's gate - the answer the inbox row shows and the
	// Land panel quotes. NOT OPEN, NOTHING ACCEPTED (ruling I1): the gate refuses before the stand is held.
	FArrivalQuote Quote = QuoteFor(Board, Traffic, Network, Flight);
	if (!Quote.IsAccepted())
	{
		UE_LOG(LogAirportOps, Log, TEXT("Flight %d not accepted: %s"), Flight.Id, *Quote.Sentence);
		return Quote;
	}

	// THE STAND THE PLAN TAXIS TO, and no other: the plan chose it from the stands it can REACH (ChooseStand), where
	// Reserve's smallest fit took any admitted stand, reachable or not, and could leave this flight waiting NoFreeStand
	// for the stand a later accept took. The verdict is dated by occupancy and by this board, so the stand it names is
	// free now; a refusal here is a same-call race, and says so.
	if (!Board.Allocator->Hold(Traffic, Network, Flight, Quote.Stand))
	{
		Quote.Why = EArrivalRefusal::NoFreeStand;
		Quote.Sentence = ArrivalPlanner::DescribeRefusal(Quote.Why, Flight.Airframe.Wingspan);
		Quote.Stand = FEntityInstanceId();
		UE_LOG(LogAirportOps, Warning, TEXT("Flight %d not accepted: the stand its plan chose could not be held"), Flight.Id);
		return Quote;
	}

	// THE LEAD TIME RUNS FROM THE ACCEPT, not from the offer: a player who took most of the
	// window to decide still gets the whole lead, and the contract is measured from here. BEFORE the transition: the
	// Accepted row arms the arrival at ArrivesAt.
	Flight.AcceptedAt = Clock.Now();
	Flight.ArrivesAt = Flight.AcceptedAt + Flight.LeadTimeSeconds;

	// THE ACCEPTED ROW OF TransitionTo: the pending-offer count, the arrival on the clock (Schedule), the revision, and
	// FFlightAccepted's publish - PUBLISHED IN THE BOARD, not by the inbox that called it: Accept is a player command reached
	// straight from the game module, and the key-7 path (AcceptImmediate) comes through here too - one
	// publisher for every accept. After the hold, so a refusal above publishes nothing.
	// ENFORCED BY: AirportOps.Model.FlightBoard.AcceptPublishesOfferAccepted
	Board.TransitionTo(Flight, EFlightPhase::Accepted, FTransitionCause::Played(Clock.Now()).WithWorld(&Clock, &Traffic));

	UE_LOG(LogAirportOps, Log, TEXT("Flight %d accepted: stand %d held, landing at %.0f"),
		Flight.Id, Flight.Stand.Index, Flight.ArrivesAt);
	return Quote;
}

void FOfferInbox::Decline(UFlightBoard& Board, USimClock& Clock, UFlight& Flight)
{
	if (Flight.GetPhase() != EFlightPhase::Offered)
	{
		return;
	}
	UE_LOG(LogAirportOps, Log, TEXT("Flight %d declined"), Flight.Id);
	// THE DECLINED ROW: the pending-offer count, FOfferDeclinedEvent, History, the revision.
	Board.TransitionTo(Flight, EFlightPhase::Declined, FTransitionCause::Played(Clock.Now()));
}

EArrivalRefusal FOfferInbox::WhyNotAcceptable(const UFlightBoard& Board, const UGroundTraffic& Traffic,
	const URoadNetwork& Network, const UFlight& Flight) const
{
	// THE PLAN HALF OF THE ONE QUOTE - PlanQuote, where the count and the Queue rule now live, so this and the cached
	// verdict cannot plan two different ways.
	return Board.PlanQuote(Traffic, Network, Flight.Airframe, Flight.RunwayPreference, 0).Why;
}

EArrivalRefusal FOfferInbox::AcceptImmediate(UFlightBoard& Board, UGroundTraffic& Traffic, const URoadNetwork& Network,
	USimClock& Clock, const FAirframe& Airframe, const FVector2D& Focus, FText Airline, FString* OutSentence)
{
	// A FIELD THAT TAKES NO ARRIVALS HOLDS NO OFFERS (ruling I1, 2026-09-30): the closure withdrew them and the generator
	// makes none, so a refused debug flight left in the inbox would be the only offer on a closed field. Not made at all,
	// then - and the refusal is still THE ONE QUOTE (QuoteArrival: the plan, then the gate), asked before the flight
	// exists rather than re-derived after, so a runway-less field says NoRunway and a closed one says it is closed.
	// ENFORCED BY: AirportOps.Present.Airport.LandRefusedWhileClosed
	if (Board.AdmitsArrivals && !Board.AdmitsArrivals())
	{
		const FArrivalQuote Refused = QuoteArrival(Board, Traffic, Network, Airframe, Focus);
		UE_LOG(LogAirportOps, Log, TEXT("AcceptImmediate: no flight made - %s"), *Refused.Sentence);
		if (OutSentence != nullptr)
		{
			*OutSentence = Refused.Sentence;
		}
		return Refused.Why;
	}

	UFlight* Flight = NewObject<UFlight>(&Board);
	Flight->Airframe = Airframe;
	Flight->AirlineName = Airline;
	Flight->TypeName = FText::FromName(Airframe.TypeCode);

	// NO LEAD TIME: AcceptImmediate exists to put an aeroplane on the field this second - see
	// its own header. The one-second window is never drained: the accept below is this call.
	Flight->LeadTimeSeconds = 0.0;
	// THE FUEL SERVICE'S OWN FALLBACK, not a second figure: key 7 was never offered.
	Flight->FuelLitres = UJobBoard::DefaultLitres(Airframe);
	Flight->OfferWindowSeconds = 1.0;
	Flight->OfferSecondsLeft = 1.0;
	Flight->RunwayPreference = Focus;

	Board.AddOffer(Clock, Flight);

	// THE REFUSAL TryAccept HIT, returned by the gate that refused - not re-derived by asking the plan again, which is
	// how a closure, a null allocator or a span-0 airframe used to come back as None, "success" (#431). Says WHICH
	// refusal, the same sentence the inbox would show for it. The flight is left in the inbox rather than removed:
	// an offer nobody could accept yet is exactly what the board already does for one the
	// generator makes, and a player watching the inbox sees the same row either way.
	const FArrivalQuote Quote = TryAccept(Board, Traffic, Network, Clock, *Flight);
	if (OutSentence != nullptr)
	{
		*OutSentence = Quote.Sentence;
	}
	return Quote.Why;
}
