#pragma once

#include "CoreMinimal.h"
#include "Model/ArrivalPlanner.h"
#include "Model/RoadEntity.h"
#include "UObject/WeakObjectPtr.h"

class UFlight;
class UFlightBoard;
class UGroundTraffic;
class URoadNetwork;
class USimClock;
struct FAirframe;

/**
 * Whether an offer can be accepted right now, and whether the airport can serve it.
 *
 * CACHED ON FOUR REVISIONS, TWO ANSWERS (issue #169, moved from UOfferViewModel 2026-09-28; split by #443). Why is a
 * full ArrivalPlanner::Plan - a route search over every stand, then every runway exit - so it
 * is recomputed only when something it depends on has moved: the board itself
 * (UFlightBoard::Revision - added/accepted/declined/expired), the guideline graph
 * (URoadNetwork::GetGuidelineRevision - an edit changed the taxiways), or occupancy
 * (UGroundTraffic::OccupancyRevision - a stand claimed or freed, a runway taken or cleared). bFuelServable is
 * CouldServe, which reads the airport's shape, the fleet's COMPOSITION and which of its vehicles are STRANDED (it is
 * handed the traffic model to ask, #443 - a stranded vehicle never counts) and nothing else: it is dated by the
 * guideline graph and by UJobBoard::GetFleetCompositionRevision (a vehicle added, withdrawn, seeded by the "FleetSeed"
 * pass, sold or a load replacing the fleet, and a vehicle's agent stranding or moving again), and by NEITHER the board
 * (the airframe is the flight's own) NOR occupancy (it judges no traffic's whereabouts) NOR any vehicle state but being
 * stranded. Before #443 the whole verdict hung on the counter that moves on every vehicle TRANSITION
 * (dispatched, arrived, serving, refilled - #428 made each one bump it), so a truck arriving or finishing a refill
 * re-planned every pending offer's full arrival plan, which reads no fleet. A few integer compares replace the
 * search on every frame where none of them moved.
 * AND THE NETWORK ITSELF, AND ITS EDIT REVISION (#471), the way FLandChoicesKey is keyed: since #431 the verdict's stand
 * is the one an accept HOLDS, so a stale yes is no longer a wrong row but a wrong hold. A drag moves the road (EditRevision)
 * a whole gesture before the guideline graph catches up at the drop, and a pre-drag yes served from the cache let the inbox
 * accept onto a graph the planner itself refuses mid-edit (GraphBeingEdited). A clear or a load is a NEW network counting
 * its revisions from zero, so equal numbers on another object are not the same graph - weak, so a recycled address is not
 * mistaken for the old network either.
 * ENFORCED BY: AirportOps.Model.FlightBoard.VehicleTransitionsDoNotReplanOffers,
 * AirportOps.Fuel.CouldServe.StrandingMovesTheCompositionAndTheVerdict (the stranded clause),
 * AirportOps.Model.FlightBoard.VerdictIsDatedByTheEdit, AirportOps.Model.FlightBoard.VerdictNamesTheNetwork
 */
struct FOfferVerdict
{
	EArrivalRefusal Why = EArrivalRefusal::None;
	/** The plan's own sentence for Why - ArrivalPlanner::DescribeRefusal(Plan), with its figures and its admission - or
	 *  empty when None. The row shows THIS, not the reason-only wording, which reads "not admitted to that runway" for
	 *  an arrivals-only field whose real reason is that nothing can take the departure (#456 review). */
	FString Sentence;
	/** The stand the plan taxis to when Why is None - what TryAccept holds (#431). Unset otherwise. */
	FEntityInstanceId Stand;

	/** Could a depot fuel this airframe on a stand it would take? True when nothing checks
	 *  (no fuel service wired, as in most board tests). A MISSING service does not block the
	 *  accept (spec ruling 5) - the row says so and C scores it. */
	bool bFuelServable = true;

	/** What Why was judged at. The guideline stamp and the network date bFuelServable too: an edit re-judges both. */
	uint32 BoardAt = 0;
	uint32 GuidelineAt = 0;
	uint32 OccupancyAt = 0;
	/** URoadNetwork::GetEditRevision when Why was judged - the plan's alone: CouldServe reads only the derived graph, which
	 *  a drag does not move until the drop's guideline revision, and its answer holds nothing. */
	uint32 EditAt = 0;
	/** The network both answers were judged on - compared, never dereferenced; weak, so a new network at a recycled address is not this one. */
	FWeakObjectPtr Network;
	/** UJobBoard::GetFleetCompositionRevision when bFuelServable was judged - a vehicle bought, sold, seeded (by the
	 *  "FleetSeed" pass) or withdrawn changes it (facility-upgrades spec), and so does a vehicle's agent stranding or moving
	 *  again (#443: a stranded vehicle does not count), but a vehicle changing any other STATE does not (#443: this was the
	 *  transition counter, which moved on every one and re-planned every offer's Why with it).
	 *  ENFORCED BY: AirportOps.Model.Fleet.OfferVerdictIsDatedByTheFleet,
	 *  AirportOps.Fuel.CouldServe.StrandingMovesTheCompositionAndTheVerdict */
	uint32 FleetAt = 0;
	bool bValid = false;
};

/**
 * THE ANSWER TO "MAY THIS ARRIVAL BE ACCEPTED NOW" (#431, #432): the arrival plan (ArrivalPlanner::Plan, a busy runway
 * queued), then the airport's gate (UAirport::AdmitsArrivals) - one evaluator for the inbox, key 7 and the Land panel.
 * Why, the sentence the refusal is worded with (the plan's own, or the gate's), and the stand an accept would hold.
 * Plain C++: an answer, not state.
 */
struct FArrivalQuote
{
	EArrivalRefusal Why = EArrivalRefusal::NoRunway;
	FString Sentence;
	FEntityInstanceId Stand;
	bool IsAccepted() const { return Why == EArrivalRefusal::None; }
};

/**
 * THE OFFERS' HALF OF THE FLIGHT BOARD (#442 item 4): every open offer's real-seconds COUNTDOWN and its LAPSE (TickOffers), the
 * cached VERDICT an inbox row shows and a lapse is classified by (VerdictFor), and the ANSWER to an offer - the one accept every
 * door comes through (TryAccept; AcceptImmediate for key 7), the decline, and the quotes they and the Land panel ask (QuoteFor,
 * QuoteArrival, WhyNotAcceptable). UFlightBoard carried this beside six other jobs (1803 lines on 2026-10-01).
 *
 * PATTERN: Extract Class, into a value-type COMPONENT that UFlightBoard holds, the board staying the flight registry and the one
 * transition owner (TransitionTo) - FTurnarounds' shape inside UJobBoard (#427, #494). Every public name the board had stays on it,
 * as a forwarder (TickOffers, VerdictFor, TryAccept, Accept, QuoteFor, QuoteArrival, Decline, AcceptImmediate, WhyNotAcceptable,
 * OfferSnapshotCountForTest), so no caller changed.
 * ENFORCED BY: the build (every caller compiles unchanged), AirportOps.Present.FlightBoardOwnersAreWired (each forwarder reaches its
 * owner, through the runtime)
 * NAMED DEVIATIONS, each forced:
 *  - A FRIEND OF UFlightBoard, as FTurnarounds is of UJobBoard: a lapse, an accept and a decline are phase changes, and TransitionTo -
 *    the one writer of a flight's phase and owner of its effects - is private to the board so that nothing else can change a phase.
 *    The inbox also walks the board's Flights and asks its quote (PlanQuote, Gated). A FRIEND SEES EVERYTHING, so what it may reach
 *    is held by lint instead: no write of the board's flight lists, indices or id counter, and no publish or post of its own.
 *    ENFORCED BY: Check-Architecture rule 96
 *  - THE BOARD IS PASSED PER CALL, NEVER HELD: FTurnarounds' reason - this lives inside the board, and a stored back-pointer is
 *    exactly what a duplicated UObject copies wrongly.
 *  - NOT A UObject AND NOT A USTRUCT: it holds no object reference and nothing of it is saved (the verdicts are a session cache, one
 *    recompute from right after a load - see Verdicts), so reflection would buy it nothing; the board's maps it replaces were never
 *    reflected either.
 *  - THE QUOTE ITSELF (PlanQuote, Gated) STAYS ON THE BOARD: the arrival queue's re-hold asks the very plan an accept holds, and an
 *    evaluator owned by one of its two readers would make the other reach through it.
 */
class AIRPORTOPS_API FOfferInbox
{
public:
	/**
	 * Drain every offer's REAL-seconds countdown and lapse the ones that reach zero.
	 *
	 * REAL SECONDS, AND ONLY WHILE UNPAUSED (spec 2026-09-28 ruling 3). It replaced a
	 * Clock.At callback at a GAME-time ExpiresAt, which made the window shrink with the speed
	 * setting: 600 game seconds was eight real seconds at x1. A plain seconds-left field also
	 * saves as itself, so there is no load-time re-arm to get wrong.
	 *
	 * EACH OFFER'S VERDICT IS REFRESHED HERE TOO (see VerdictFor), which is what lets a lapse
	 * say whether the player could ever have taken it - UFlight::LapseReason - with no inbox
	 * open. Called from UOpsRuntime::Tick (through UFlightBoard::TickOffers); tests call it directly.
	 */
	void TickOffers(UFlightBoard& Board, const UGroundTraffic& Traffic, const URoadNetwork& Network,
		const USimClock& Clock, double RealDeltaSeconds);

	/**
	 * Can this offer be accepted right now, and can the airport serve it - CACHED.
	 *
	 * MOVED HERE FROM UOfferViewModel (issue #169's cache): the board is what classifies a
	 * lapse, so it has to know the answer whether or not a row is on screen, and one cache
	 * both the board and the row read is one evaluator rather than two. Recomputed only when
	 * the board, the guideline graph or the occupancy has moved since - see FOfferVerdict.
	 */
	const FOfferVerdict& VerdictFor(const UFlightBoard& Board, const UGroundTraffic& Traffic, const URoadNetwork& Network,
		const UFlight& Flight) const;

	/**
	 * WHERE EVERY ACCEPT IS DECIDED (#431). QuoteFor - the cached plan verdict, then the airport's gate - and on a yes,
	 * the stand THAT PLAN taxis to is held (UStandAllocator::Hold) and the arrival goes on the clock. Returns the
	 * quote: None and the held stand, or the refusal it actually hit, worded.
	 *
	 * BEFORE THIS, Accept was the airport's status plus UStandAllocator::Reserve - the smallest fitting stand, reachable
	 * or not - while the inbox, the lapse classifier and the queue asked ArrivalPlanner::Plan. Two evaluators: key 7
	 * accepted flights the planner refused (no exit, no route) and they held for ever, AcceptImmediate reported a refusal
	 * by asking the OTHER evaluator, and a hold could sit on a stand nothing could taxi to while the flight waited for
	 * the one it could.
	 *
	 * A refusal before the plan - not an offer, no stand allocator, no airframe - is NotAdmitted with a sentence saying
	 * which, and logged: a caller's bug or a fixture's, never a player's. So is the gate's closure. NotAdmitted because
	 * EArrivalRefusal (the planner's enum, in Airside) has no value for either: it is the nearest reason, and the quote's
	 * Sentence carries the real one - a reader of Why alone must not word it.
	 * ENFORCED BY: AirportOps.Model.FlightBoard.AcceptHoldsTheReachableStand, AirportOps.Present.LandWithNoRouteHoldsNothing
	 */
	FArrivalQuote TryAccept(UFlightBoard& Board, UGroundTraffic& Traffic, const URoadNetwork& Network, USimClock& Clock,
		UFlight& Flight);

	/**
	 * VerdictFor's plan answer, then the airport's gate - what TryAccept asks, and the inbox row shows. The gate is NOT in
	 * the cached verdict: the lapse classifier reads the verdict alone, and a flight that lapses while the player has
	 * closed the airport was ignored, not unacceptable.
	 * ENFORCED BY: AirportOps.Model.Offers.Countdown.LapseReadsThePlanNotTheGate
	 */
	FArrivalQuote QuoteFor(const UFlightBoard& Board, const UGroundTraffic& Traffic, const URoadNetwork& Network,
		const UFlight& Flight) const;

	/**
	 * The same quote for an arrival that is not a flight yet - Airframe, aimed at Focus - UNCACHED. What the Land panel
	 * asks per type (UOpsRuntime::QuoteLanding, #432): the SAME plan and the SAME gate AcceptImmediate's accept will
	 * ask, so a row the panel lights is a click the game takes.
	 */
	FArrivalQuote QuoteArrival(const UFlightBoard& Board, const UGroundTraffic& Traffic, const URoadNetwork& Network,
		const FAirframe& Airframe, const FVector2D& Focus) const;

	/** Retires the offer at once; free (spec ruling 8) - only a lapse will cost anything, in C. */
	void Decline(UFlightBoard& Board, USimClock& Clock, UFlight& Flight);

	/**
	 * Make a flight from an airframe, aim it at Focus, and accept it on the spot - the debug
	 * land key's whole job, and previously done by hand at the call site (issue #96).
	 *
	 * Its lead time is zero, so ArrivesAt is Clock.Now(): this exists to put an aeroplane on the field
	 * THIS SECOND, not to queue a normal offer. Focus travels onto the flight itself - see
	 * UFlight::RunwayPreference - so it never has to touch the board's own field, which the
	 * generator also writes and would otherwise fight over.
	 *
	 * Returns EArrivalRefusal::None on success, or the refusal TryAccept HIT - returned by the gate that refused, not
	 * re-derived by asking the plan again (#431: a closure, a null allocator or a span-0 airframe used to come back as
	 * None, "success"). OutSentence, when given, receives its words. The flight is left in the inbox on refusal, exactly
	 * as a generated offer nobody could accept yet is.
	 */
	EArrivalRefusal AcceptImmediate(UFlightBoard& Board, UGroundTraffic& Traffic, const URoadNetwork& Network,
		USimClock& Clock, const FAirframe& Airframe, const FVector2D& Focus, FText Airline, FString* OutSentence);

	/**
	 * Why this offer could not be accepted this instant, or EArrivalRefusal::None.
	 *
	 * The REAL ArrivalPlanner::Plan against the live occupancy, so the inbox's greyed-out
	 * reason is the same sentence the arrival itself would print - ArrivalPlanner::
	 * DescribeRefusal renders it. A second, cheaper guess here would be a second source of
	 * truth about whether an aeroplane can land.
	 */
	EArrivalRefusal WhyNotAcceptable(const UFlightBoard& Board, const UGroundTraffic& Traffic, const URoadNetwork& Network,
		const UFlight& Flight) const;

	/** How many times TickOffers has copied Flights to walk them - its early-out's counter. */
	int32 OfferSnapshotCount() const { return OfferSnapshots; }

	/** A flight left the board's live list (UFlightBoard::MoveToHistory): its verdict is nobody's now. */
	void Forget(int32 FlightId) { Verdicts.Remove(FlightId); }

	/** Every verdict dropped - a load's step 0 (UFlightBoard::DisarmEveryArrival), whose ids a restored flight may reuse. */
	void ForgetAll() { Verdicts.Reset(); }

private:
	/** See VerdictFor. By flight id; MUTABLE because VerdictFor is const and caching an answer
	 *  is bookkeeping about the board, not a change to what it holds. Not saved: a load starts
	 *  every verdict invalid, one recompute away from correct. */
	mutable TMap<int32, FOfferVerdict> Verdicts;

	/** See OfferSnapshotCount. A session counter, not saved. */
	int32 OfferSnapshots = 0;
};
