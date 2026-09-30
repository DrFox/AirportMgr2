#pragma once

#include "CoreMinimal.h"
#include "Model/Airframe.h"
#include "Model/ExhaustiveSwitch.h"
#include "Model/RoadEntity.h"
#include "Model/RoadHandles.h"
#include "UObject/Object.h"

#include "Flight.generated.h"

enum class EAgentPhase : uint8;
class UGroundTraffic;
class URoadNetwork;
class USimClock;
struct FRoadAgent;
struct FAgentTransition;

/**
 * Where one flight has got to.
 *
 * AN ENUM, NEVER A SET OF BOOLS: "offered" and "inbound" can never both be true, and the
 * states are visited in one order - the same rule EAgentPhase and EFuelDemandState follow.
 *
 * THE DECLARATION ORDER CARRIES NO MEANING ANY MORE (#442). It used to be load-bearing - "unarrived" was Accepted||Inbound
 * at five sites, "on the ground" was Landing..Departing by range at four, FlightPhaseFromTransition asked whether the
 * flight had reached Turnaround by `>=` - and a new phase needed about twelve edits, none of them a compile error. Every
 * grouping is now one of the FlightPhase:: predicates below, each derived from StageOf's ONE exhaustive switch, so a new
 * phase (Diverted) is a BUILD ERROR at StageOf, at UFlightBoard::TransitionTo's row switch and at every other per-phase
 * mapping in the ops and game modules - each inside AIRSIDE_EXHAUSTIVE_SWITCH with no default - and nowhere has to be
 * found by reading. Tests are the exception: they may group phases, and a table of them names every phase by count.
 * ENFORCED BY: Check-Architecture rule 58 (no ordinal, cast or OR-grouped EFlightPhase test, and no switch on one without
 * AIRSIDE_EXHAUSTIVE_SWITCH or with a default, outside this file), C4062 as an error around StageOf
 * (AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN)
 *
 * Manoeuvring is the aeroplane coming off the stand - the PHASE, and it exists because an
 * aeroplane can now be watched doing it. The SERVICE that performs it for one that cannot
 * manage alone is PUSHBACK, which is the job board's and does not exist yet; a Twin Otter
 * reverses under its own power and is not being pushed by anything, so the two are not the
 * same word. Diverted is still deliberately ABSENT: the sequencer owns it and it does not
 * exist either. A phase nothing can enter is a lie. Cancelled is here since 2026-09-29 because
 * three things enter it now: the player despawning the aeroplane (UFlightBoard::CancelByAgent), the
 * airport ceasing to be open before an accepted flight arrived (UFlightBoard::CancelUnarrived,
 * ops batch 3), and the player cancelling one that could never land (UFlightBoard::CancelByPlayer, #442);
 * the sequencer's cancellations will share it when it exists. Withdrawn, appended
 * last, is an OFFER the same closure took back - never accepted, so neither Cancelled nor Expired.
 */
UENUM()
enum class EFlightPhase : uint8
{
	/** In the inbox, undecided. Lapses when OfferSecondsLeft runs out. */
	Offered,
	/** The player said yes. A stand is held and the arrival is on the clock. */
	Accepted,
	/**
	 * HOLDING: the ETA has passed and it is waiting for the runway, off-map, stand kept (spec
	 * 2026-09-28-arrival-queue). Nothing entered this phase before the queue existed.
	 */
	Inbound,
	Landing,
	TaxiIn,
	Turnaround,
	/**
	 * Coming off the stand - see EAgentPhase::Manoeuvring.
	 *
	 * ITS POSITION IN THIS LIST IS LOAD-BEARING, like every other entry's: it must sit AFTER
	 * Turnaround or FlightPhaseFromTransition would read the taxi that follows it as a taxi IN.
	 */
	Manoeuvring,
	TaxiOut,
	Departing,
	Departed,
	/** The player said no. */
	Declined,
	/** Nobody said anything and the offer timed out. */
	Expired,
	/** The player despawned the aeroplane (the inspector's Unstick), or the airport closed before it
	 *  arrived (UFlightBoard::CancelUnarrived). Terminal, like Departed, but it did not leave by the
	 *  runway - see UFlightBoard::CancelByAgent. */
	Cancelled,
	/**
	 * An OFFER the airport took back when it stopped being open - entered by UFlightBoard::CancelUnarrived
	 * (spec 2026-09-29-ops-batch3 §3). Not Expired: nobody let it lapse, so no OfferExpired and no Ignored
	 * penalty; not Cancelled: it was never the airline's flight here.
	 * ENFORCED BY: AirportOps.Model.FlightBoard.CancelUnarrivedCancelsAndWithdraws
	 * APPENDED LAST so the saved values of every phase above keep their numbers - no longer because the order is
	 * load-bearing (see the enum's comment: it is not since #442).
	 */
	Withdrawn
};

/**
 * THE ONE PLACE EFlightPhase IS GROUPED (#442). Where a flight is in its life, coarser than its phase: a grouping every
 * reader used to re-spell by ordinal range or by an OR of names, each copy a place a new phase would be silently left
 * out of. A plain enum: nothing saves or reflects it, it only answers "which of these is it".
 */
enum class EFlightStage : uint8
{
	/** An offer in the inbox, undecided. */
	Offer,
	/** Accepted or holding: a stand is held and the aeroplane is not on the field yet. */
	Unarrived,
	/** On the field and not yet at a stand: landing or taxiing in. */
	OnTheWayIn,
	/** At its stand or on its way off it: the turnaround, the push, the taxi out, the line-up. */
	AtStandOrLeaving,
	/** Nothing will move it again: departed, declined, expired, cancelled or withdrawn. */
	Terminal
};

/**
 * The predicates every reader asks instead of comparing phases. StageOf is the ONE switch: EVERY phase by name, no default,
 * inside AIRSIDE_EXHAUSTIVE_SWITCH (see ExhaustiveSwitch.h - C4062 is off in this toolchain unless a function opts in), so a
 * phase added to EFlightPhase is a build error HERE, at the one place that has to say which stage it belongs to.
 * ENFORCED BY: C4062 as an error around StageOf (checked 2026-09-30 by a stray enumerator: the build failed here),
 * AirportOps.Model.Flight.EveryPhaseHasOneStage (every phase lands in exactly one predicate's set)
 */
namespace FlightPhase
{
	AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN
	inline EFlightStage StageOf(EFlightPhase Phase)
	{
		switch (Phase)
		{
		case EFlightPhase::Offered:
			return EFlightStage::Offer;
		case EFlightPhase::Accepted:
		case EFlightPhase::Inbound:
			return EFlightStage::Unarrived;
		case EFlightPhase::Landing:
		case EFlightPhase::TaxiIn:
			return EFlightStage::OnTheWayIn;
		case EFlightPhase::Turnaround:
		case EFlightPhase::Manoeuvring:
		case EFlightPhase::TaxiOut:
		case EFlightPhase::Departing:
			return EFlightStage::AtStandOrLeaving;
		case EFlightPhase::Departed:
		case EFlightPhase::Declined:
		case EFlightPhase::Expired:
		case EFlightPhase::Cancelled:
		case EFlightPhase::Withdrawn:
			return EFlightStage::Terminal;
		}
		// A BYTE NO ENUMERATOR NAMES (a corrupt save): finished, the one stage nothing acts on.
		return EFlightStage::Terminal;
	}
	AIRSIDE_EXHAUSTIVE_SWITCH_END

	/** Accepted or Inbound: still to arrive - what a closure cancels, what holds a stand without an aeroplane. */
	inline bool IsUnarrived(EFlightPhase Phase) { return StageOf(Phase) == EFlightStage::Unarrived; }

	/** Landing through Departing: an aeroplane on the field - what a closure leaves to finish. */
	inline bool IsOnGround(EFlightPhase Phase)
	{
		const EFlightStage Stage = StageOf(Phase);
		return Stage == EFlightStage::OnTheWayIn || Stage == EFlightStage::AtStandOrLeaving;
	}

	/** Nothing will move it again: it is in History, not in the live list. */
	inline bool IsTerminal(EFlightPhase Phase) { return StageOf(Phase) == EFlightStage::Terminal; }

	/** Accepted through Departing: not an offer and not finished - what UFlightBoard::Live lists. */
	inline bool IsLive(EFlightPhase Phase) { return IsUnarrived(Phase) || IsOnGround(Phase); }

	/**
	 * Has it reached its stand, or gone on from it: the turnaround, the push, the taxi out, the line-up. The test that
	 * says which way a taxi is going (FlightPhaseFromTransition) and which mid-flight flights a load retires rather
	 * than re-queues (UFlightBoard::DemoteRestoredMidFlight). A TERMINAL phase is not "at its stand": it asks nothing of a
	 * finished flight, which no agent transition reaches.
	 */
	inline bool HasReachedStand(EFlightPhase Phase) { return StageOf(Phase) == EFlightStage::AtStandOrLeaving; }

	/** Landing or taxiing in: on the field, not yet at a stand. A load re-queues these (their aeroplane was not saved). */
	inline bool IsArriving(EFlightPhase Phase) { return StageOf(Phase) == EFlightStage::OnTheWayIn; }
}

/**
 * Why a flight was cancelled - FFlightCancelledEvent's reason, and what the airline roster scores it by.
 * AN ENUM: the four are mutually exclusive, and only AirportClosed and PlayerCancelled are the player's choice to pay for.
 */
UENUM()
enum class ECancelReason : uint8
{
	/** The player closed the airport (UAirport::SetClosedByPlayer). Costs ClosureCancelPenalty. */
	AirportClosed,
	/** The last runway went. Free - the user accepted that loophole (spec 2026-09-29-ops-batch3 §0). */
	NoRunway,
	/** The player despawned the aeroplane (UFlightBoard::CancelByAgent). Free. */
	Unstuck,
	/**
	 * The player cancelled a flight that had not arrived (UFlightBoard::CancelByPlayer) - what an unlandable holding flight's
	 * alert offers (#442). Costs ClosureCancelPenalty, the same per-flight penalty the closure charges: the airline lost the
	 * flight to the player's choice either way. APPENDED LAST so the saved values keep their meaning.
	 */
	PlayerCancelled
};

/**
 * Why an offer lapsed - what C needs to decide whether the player is to blame.
 *
 * AN ENUM, not two bools: "ignored" and "never acceptable" cannot both be true, and None is
 * every flight that did not lapse. A (spec 2026-09-28) records it; C rules what it costs.
 */
UENUM()
enum class ELapseReason : uint8
{
	None,
	/** It could have been accepted at some point in its window, and nobody did. */
	Ignored,
	/** No stand was free for it the whole time it stood. */
	NeverAcceptable
};

/**
 * Whether a phase change is live play or a load putting a saved flight where the game left it (#442). The ONE deliberate
 * difference between two writers of the same (from, to): a flight cancelled by a closure is SCORED and announced, and the
 * same flight cancelled by the load that restores the closed airport is not - what closed the airport happened before the
 * save, and its own cancellations were scored then (PR C ruling I2). An enum and not a bool: there will be a third
 * source the day something else moves a flight (the sequencer's diversions), and a bool would then be wrong.
 */
enum class EFlightChangeSource : uint8
{
	/** The game running: every row announces what it announces and scores what it scores. */
	Play,
	/** A load: nothing is published that the roster or the UI would read as news. */
	Load
};

/**
 * WHAT A PHASE CHANGE NEEDS IN HAND BESIDE ITS TWO PHASES (#442) - UFlightBoard::TransitionTo's third argument. Plain
 * C++: an argument, never saved.
 *
 * THE FIELDS ARE WHAT A ROW NEEDS THAT ITS CALLER HAS AND THE BOARD DOES NOT; only Source is a deliberate difference between
 * two doors onto the same row. THE CLOCK AND THE TRAFFIC MODEL, for the rows that disarm an arrival and release a stand: null where the caller
 * has neither (the ETA callback is the clock firing - there is nothing left to cancel; a load with no traffic model has
 * no claims to release), and a row that NEEDED one and was not given it says so in the log rather than leaving the
 * arrival armed in silence. THE REASON, for the Cancelled row's FFlightCancelledEvent. THE AGENT, for the row that puts
 * an aeroplane under a flight. AT, the game time the change is dated - TerminatedAt, and HoldingSince for a flight joining
 * the queue (which is its ETA, not the moment the callback ran).
 *
 * FLUENT SETTERS, each returning *this, so a call site names only what its door supplies:
 * TransitionTo(F, Cancelled, FTransitionCause::Played(Now).WithWorld(Clock, Traffic).Cancelling(Reason)).
 */
struct FTransitionCause
{
	EFlightChangeSource Source = EFlightChangeSource::Play;
	double At = 0.0;
	USimClock* Clock = nullptr;
	UGroundTraffic* Traffic = nullptr;
	/**
	 * WHY, for the Cancelled row - UNSET until Cancelling names it. Not defaulted to a reason: a cancel that forgot to say why
	 * was silently the FREE one (Unstuck, which the roster does not charge), so a door that dropped its reason cost the airline
	 * nothing with no trace. The row logs an Error and publishes nothing for a Play-source cancel with none.
	 * ENFORCED BY: AirportOps.Model.FlightBoard.CancelWithNoReasonIsLoudNotFree
	 */
	TOptional<ECancelReason> CancelReason;
	int32 AgentId = INDEX_NONE;

	static FTransitionCause Played(double InAt)
	{
		FTransitionCause Cause;
		Cause.At = InAt;
		return Cause;
	}
	static FTransitionCause Loaded(double InAt)
	{
		FTransitionCause Cause;
		Cause.Source = EFlightChangeSource::Load;
		Cause.At = InAt;
		return Cause;
	}
	FTransitionCause& WithWorld(USimClock* InClock, UGroundTraffic* InTraffic)
	{
		Clock = InClock;
		Traffic = InTraffic;
		return *this;
	}
	FTransitionCause& Cancelling(ECancelReason InReason)
	{
		CancelReason = InReason;
		return *this;
	}
	FTransitionCause& WithAgent(int32 InAgentId)
	{
		AgentId = InAgentId;
		return *this;
	}
};

/**
 * One flight, from the offer to the departure.
 *
 * A UObject AND NOT A STRUCT, because UListView::SetListItems takes UObject* and the inbox
 * binds one viewmodel per row; a struct would need an adapter object per row anyway, and
 * then there would be two things to keep in step.
 *
 * THAT REASON HAS GONE (#425): the list binds UOfferViewModel rows (OfferInboxWidget, 2026-09-30), not this. What keeps it a
 * UObject is that every reader holds a flight by POINTER - the viewmodels' weak pointers, the inspector's lookup, the
 * board's ById/ByAgent - and a struct in the board's arrays would move whenever they grew. Nor does the save need a
 * struct: UFlightBoard::Serialize writes each flight's own tagged properties BY VALUE, so a UPROPERTY added here is
 * saved with no edit there. NEVER A POINTER TO A RUNTIME OBJECT among them: OpsSave would write it as a path.
 * ENFORCED BY: Check-Architecture rule 37 (persistent-refs-transient, UFlight in $savedByValueClasses)
 */
UCLASS()
class AIRPORTOPS_API UFlight : public UObject
{
	GENERATED_BODY()

public:
	/** Ids start at 1. HolderId() depends on that, so never renumber from 0. */
	UPROPERTY() int32 Id = 0;

	/**
	 * What is flying, FLATTENED OUT OF THE DEFINITION rather than pointed at.
	 *
	 * Model/ may not see Entities/ - Check-Architecture enforces it - and FAirframe exists
	 * for exactly this crossing: its own comment says the agent carries no pointer to its
	 * type by design. Whoever makes the offer reads UAircraftType::Airframe() once, the same
	 * way URoadNetwork::PlaceEntity takes a design wingspan instead of a definition.
	 */
	UPROPERTY() FAirframe Airframe;

	/** For the inbox to print. Captured with the airframe, and for the same reason. */
	UPROPERTY() FText AirlineName;

	/**
	 * WHICH airline, as a key: the UAirlineDefinition's object name, set at the offer. AirlineName is
	 * display text and a display text is not a key - two airlines may share a name, and a rename would
	 * orphan every flight. NAME_None for the debug flight (key 7), which belongs to no airline.
	 */
	UPROPERTY() FName AirlineId;
	UPROPERTY() FText TypeName;

	/** What the row prints: "CU 204", or a tail number for the club. See UOfferGenerator::MakeCallsign. */
	UPROPERTY() FString Callsign;

	/**
	 * REAL seconds this offer has left in the inbox, and the window it started with.
	 *
	 * REAL, NOT GAME, and drained only while unpaused (spec 2026-09-28 ruling 3): 600 GAME
	 * seconds was eight real seconds at x1 and less at speed, too short to read the row. A
	 * countdown saved as a plain number resumes after a load with exactly what was left - an
	 * absolute real timestamp would mean nothing in the next session.
	 */
	UPROPERTY() double OfferSecondsLeft = 0.0;
	UPROPERTY() double OfferWindowSeconds = 0.0;

	/** GAME seconds from the accept to the aircraft on approach. The airline's, captured at offer. */
	UPROPERTY() double LeadTimeSeconds = 0.0;

	/**
	 * The turnaround contract: GAME seconds from the accept to airborne again. Fixed at the
	 * offer (the airline's own UAirlineDefinition::ContractSeconds) so the row can show it
	 * BEFORE the player accepts. C scores AirborneAt against it.
	 */
	UPROPERTY() double ContractSeconds = 0.0;

	/**
	 * The fuel this flight will take on, litres - 50-90% of its tank, drawn at the offer so the
	 * row can show the size of the job (spec 2026-09-28-fuel-litres). 0 = none. Reaches the fuel
	 * demand through UJobBoard::LitresOwedFor.
	 */
	UPROPERTY() double FuelLitres = 0.0;

	/**
	 * From the floor airline (the flying club) - C never penalises its lapses.
	 * ENFORCED BY: AirportOps.Model.Airlines.FloorLapseIsFree
	 */
	UPROPERTY() bool bFloorAirline = false;

	/**
	 * USimClock::Now at which it lands: AcceptedAt + LeadTimeSeconds, set by the accept.
	 *
	 * SAVED, and it is the saved truth about the schedule: USimClock deliberately does not
	 * save its callback queue, so a reload has an ETA and nothing armed until
	 * UFlightBoard::RearmSchedules reads this.
	 */
	UPROPERTY() double ArrivesAt = 0.0;

	/**
	 * Set the first time the board judged this offer acceptable (a stand free for it). What
	 * turns a lapse into Ignored rather than NeverAcceptable - see UFlightBoard::TickOffers.
	 */
	UPROPERTY() bool bWasEverAcceptable = false;

	/** Why it lapsed, or None. Written once, by UFlightBoard::TickOffers. */
	UPROPERTY() ELapseReason LapseReason = ELapseReason::None;

	/** USimClock::Now at which it joined the arrival queue, or 0. See UFlightBoard::Queue. */
	UPROPERTY() double HoldingSince = 0.0;

	/** USimClock::Now of the accept, or 0 if never accepted. The contract runs from here. */
	UPROPERTY() double AcceptedAt = 0.0;

	/**
	 * USimClock::Now at which it reached Departing, or 0 if it has not. C scores this against
	 * AirborneBy(); recorded now so C needs no migration.
	 *
	 * THE LINE-UP, NOT THE CLIMB, despite the name (#436): Departing starts when the aeroplane lines up and rolls.
	 * EAgentEvent::Airborne is a MOMENT - the phase stays Departing - so it is not on OnAgentPhaseChanged; it is on
	 * UGroundTraffic::GetMomentsThisAdvance, which nothing in ops reads. Scoring the real wheels-up would bridge that
	 * list (the per-frame moments), a change to the contract the airlines score, left for its own issue.
	 */
	UPROPERTY() double AirborneAt = 0.0;

	/** The turnaround contract's deadline: AcceptedAt + ContractSeconds. */
	double AirborneBy() const { return AcceptedAt + ContractSeconds; }

	/**
	 * THE POINT RUNWAYS ARE ORDERED FROM, nearest first - not a runway, and not a place the aeroplane goes (#442; this was
	 * ApproachFocus, "where THIS flight is aimed", and the aim stopped choosing the runway in #412).
	 *
	 * ArrivalPlanner::Plan now plans EVERY runway that takes arrivals and keeps the best (free, then dedicated, then the
	 * shortest taxi), so this no longer picks one. What it still decides: which runway's refusal is REPORTED when none will
	 * do, and ties between runways that rank alike - and the debug land key's aim, which sets it to the cursor.
	 *
	 * PER-FLIGHT, not read off UFlightBoard::DefaultRunwayPreference at accept time: that board-level field is a scratch
	 * value the generator and the debug key both write, and whichever wrote it LAST decided every later offer's
	 * WhyNotAcceptable and DispatchNow - one flight's preference leaking into another's (issue #96). Set once, at the offer,
	 * and carried from there. SAVED by the tagged pass like the rest; a save from before the rename restores it as the
	 * origin, which costs only a different refusal being reported (no player saves exist yet).
	 * ENFORCED BY: AirportOps.Model.FlightBoard.AcceptImmediate (the dispatcher is handed the flight's own preference, never the board's)
	 */
	UPROPERTY() FVector2D RunwayPreference = FVector2D::ZeroVector;

	/**
	 * The stand HELD from the accept, and then the stand actually parked on.
	 *
	 * THE TWO CAN DIFFER, and that is by design: the hold guarantees A stand exists for this
	 * flight, and ArrivalPlanner then picks the nearest free one when it lands, which may be
	 * a different stand if a nearer one freed meanwhile. UFlightBoard overwrites this at
	 * Parked AT A STAND (StandAtNode) from the node it parked on, so once it has parked what is
	 * saved is the stand it is on. Before that - landing, taxiing in, or parked on the fallback
	 * junction (#405) - it is still the stand it was accepted onto, which a load's re-queue (#404)
	 * re-holds, or gives up if another flight holds it now (UStandAllocator::Reapply).
	 */
	UPROPERTY() FEntityInstanceId Stand;

	/** The live agent, or INDEX_NONE before dispatch and once it has gone. */
	UPROPERTY() int32 AgentId = INDEX_NONE;

	/**
	 * USimClock::Now at which it parked, or 0 if it never did.
	 *
	 * THE START OF THE PARKING CLOCK, taken when it parks AT A STAND. Zero means "never parked"
	 * and is CHECKED rather than trusted: an aeroplane departed from the fallback junction it
	 * waited on (#405), or one put on the field by the debug land key, can reach TaxiOut without
	 * ever having parked at a stand, and billing it from the epoch would hand the player a fee
	 * larger than the airport. (A flight restored mid-flight no longer can: a load re-queues or
	 * retires it - UFlightBoard::DemoteRestoredMidFlight, #404.)
	 */
	UPROPERTY() double ParkedAt = 0.0;

	/**
	 * What this flight earned.
	 *
	 * LandingFee is fixed at the OFFER (see UOfferGenerator) so the inbox row can show what
	 * accepting it is worth and the player's fee lever moves NEW offers only; ParkingFee is
	 * filled in when it leaves, because nobody knows how long it stayed until it goes.
	 */
	UPROPERTY() double LandingFee = 0.0;
	UPROPERTY() double ParkingFee = 0.0;

	/**
	 * The parking rate per game hour, FIXED AT THE OFFER beside LandingFee (#442). PostParkingFee bills Hours x this; it
	 * used to ask UPricing for the rate AT DEPARTURE, which applies the fee lever as it stands then - the trade
	 * UOfferGenerator::MakeOffer rules out for the landing fee ("a fee computed on landing would let them accept cheaply
	 * and put the price up afterwards"), open for parking. ZERO MEANS NEVER PRICED, like LandingFee's: the debug land key's
	 * flight (AcceptImmediate) is never offered, so it pays neither fee - the landing fee already said so, and parking used
	 * to be the odd one out. SAVED, so a flight accepted before a save is still billed at the rate it was offered at.
	 * ENFORCED BY: AirportOps.Model.FlightFees.ParkingIsBilledAtTheOffersRate
	 */
	UPROPERTY() double ParkingRatePerHour = 0.0;

	/**
	 * Whether the landing fee has been banked, so it cannot be banked twice.
	 *
	 * SAVED, not transient: a reload that forgot this would re-bank every live flight's landing
	 * fee the next time its phase changed, and the player would be quietly paid again for
	 * aeroplanes that landed an hour ago.
	 */
	UPROPERTY() bool bLandingFeePaid = false;

	/**
	 * Whether the parking fee has been banked, so one flight is billed once (#442 review). PostParkingFee runs when the flight
	 * ENTERS TaxiOut; an aeroplane that parks again (Parked -> Turnaround -> TaxiOut) enters it a second time, and without this
	 * was billed the overlapping hours again from the original ParkedAt. SAVED, for bLandingFeePaid's reason.
	 * ENFORCED BY: AirportOps.Model.FlightFees.ParkingIsBilledOncePerFlight
	 */
	UPROPERTY() bool bParkingFeePaid = false;

	/**
	 * USimClock::Now at which this flight reached a terminal phase (Declined, Expired,
	 * Departed, Cancelled or Withdrawn), or 0 before that.
	 *
	 * WHAT UFlightBoard::RollUp AGES AGAINST, the same role FLedgerEntry::At plays for
	 * ULedger::RollUp - see UFlightBoard::History and issue #188. Zero rather than an Optional:
	 * a flight that has not yet terminated is never read against this field, the same way
	 * ParkedAt above is a real time or an unread zero and not a third state to track.
	 */
	UPROPERTY() double TerminatedAt = 0.0;

	/**
	 * The id this flight holds a stand under.
	 *
	 * NEGATIVE, because UGroundTraffic allocates agent ids NextAgentId++ from 1 and the
	 * occupancy table is mechanism, not policy - it never asks what an agent is. The sign is
	 * what keeps the two id spaces disjoint without a registry to keep in step.
	 */
	int32 HolderId() const { return -Id; }

	/**
	 * WHERE THE FLIGHT IS IN ITS LIFE - READ-ONLY EVERYWHERE BUT UFlightBoard::TransitionTo (#442). It was a public field
	 * thirteen places wrote, each choosing its own subset of the phase change's side effects (the stand release, the clock
	 * handle, the agent hooks, the bus publish, the move to History, the pending-offer count, the revision), and the three
	 * writers that cancelled a flight did three different things. Private means the compiler holds that line; TransitionTo
	 * is the one body that changes it, and owns the effects per (from, to) row.
	 * ENFORCED BY: the compiler (private, friend UFlightBoard only), Check-Architecture rule 57 (nothing in the ops or game
	 * modules writes `->Phase = EFlightPhase::` outside TransitionTo, and TransitionTo does)
	 */
	EFlightPhase GetPhase() const { return Phase; }

	/** Accepted or Inbound - still to arrive. See FlightPhase::IsUnarrived. */
	bool IsUnarrived() const { return FlightPhase::IsUnarrived(Phase); }
	/** Landing through Departing - an aeroplane on the field. See FlightPhase::IsOnGround. */
	bool IsOnGround() const { return FlightPhase::IsOnGround(Phase); }
	/** In History: nothing will move it again. See FlightPhase::IsTerminal. */
	bool IsTerminal() const { return FlightPhase::IsTerminal(Phase); }
	/** Accepted through Departing - what UFlightBoard::Live lists. See FlightPhase::IsLive. */
	bool IsLive() const { return FlightPhase::IsLive(Phase); }

	/**
	 * A TEST FIXTURE'S WAY TO STAGE A PHASE: writes it with none of TransitionTo's effects - no count, no hook, no stand,
	 * no publish, no History. That is the point: a test sets a flight where no transition reaches (a flight mid-taxi on a
	 * board that never dispatched it) and then watches what the board does FROM there, the way
	 * FServiceVehicleLifecycle::SeedStateForTest stages a vehicle. A production call is a seventh way to write a phase.
	 * ENFORCED BY: Check-Architecture rule 57 (SetPhaseForTest is called only under a Test file)
	 */
	void SetPhaseForTest(EFlightPhase NewPhase) { Phase = NewPhase; }

private:
	friend class UFlightBoard;

	/** See GetPhase. The default is the flight's BIRTH: every flight is made an offer, and UOfferGenerator::MakeOffer
	 *  writes nothing to say so (it used to, a write of the value this already holds). */
	UPROPERTY() EFlightPhase Phase = EFlightPhase::Offered;
};

/**
 * The flight phase an agent's transition implies, given where the flight had got to.
 *
 * SWITCHED ON THE TRANSITION'S CAUSE (#436), not on its To phase behind a default - it was
 * FlightPhaseFromAgent(To, Current), and UFlightBoard::OnAgentPhase then read the live agent (Phase, GoalNode,
 * bDepartureArmed) a drain late to correct the two cases the pair could not tell apart. Both now come with the
 * event: a Parked is a turnaround only when bParkedAtStand - the transition's GoalAtEvent is a stand's pose (#405) -
 * and a DepartOrdered is the taxi OUT wherever it left from, while a ReOffered stays the taxi in (review M1, M4).
 *
 * STILL TAKES THE CURRENT PHASE for the causes that continue a taxi (a redirect, a rescue, the reverse leg's end):
 * the taxi goes on in whichever direction it was going, and only the flight knows which that was. Asked through
 * FlightPhase::HasReachedStand, not by comparing phases (#442).
 * ENFORCED BY: AirportOps.Model.Flight.PhaseFromTransition, AirportOps.Model.Bus.SameFrameRedirectStaysTaxiIn
 */
AIRPORTOPS_API EFlightPhase FlightPhaseFromTransition(const FAgentTransition& Transition, EFlightPhase Current,
	bool bParkedAtStand);

/**
 * The stand whose pose is this node, or unset: a live IsStand() entity. The ONE "is it at (or bound for) a stand"
 * question both boards ask - UFlightBoard (does a Parked flight enter Turnaround) and UJobBoard (does a Parked
 * aircraft open a turnaround) - so the two cannot disagree about the fallback junction (review M8). Asked of a
 * transition's GoalAtEvent since #436: the node it parked on, not the one it may have been sent to since.
 */
AIRPORTOPS_API FEntityInstanceId StandAtNode(const URoadNetwork& Network, FGuidelineNodeId Node);

