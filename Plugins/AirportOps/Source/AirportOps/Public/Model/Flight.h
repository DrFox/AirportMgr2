#pragma once

#include "CoreMinimal.h"
#include "Model/Airframe.h"
#include "Model/RoadEntity.h"
#include "Model/RoadHandles.h"
#include "UObject/Object.h"

#include "Flight.generated.h"

enum class EAgentPhase : uint8;

/**
 * Where one flight has got to.
 *
 * AN ENUM, NEVER A SET OF BOOLS: "offered" and "inbound" can never both be true, and the
 * states are visited in one order - the same rule EAgentPhase and EFuelDemandState follow.
 *
 * THE DECLARATION ORDER IS LOAD-BEARING. FlightPhaseFromAgent decides which way an aeroplane
 * is taxiing by asking whether the flight has reached Turnaround yet, so Turnaround must stay
 * between TaxiIn and TaxiOut. Reordering these breaks that with no compiler complaint.
 *
 * Manoeuvring is the aeroplane coming off the stand - the PHASE, and it exists because an
 * aeroplane can now be watched doing it. The SERVICE that performs it for one that cannot
 * manage alone is PUSHBACK, which is the job board's and does not exist yet; a Twin Otter
 * reverses under its own power and is not being pushed by anything, so the two are not the
 * same word. Diverted and Cancelled are still deliberately ABSENT: the sequencer owns them
 * and it does not exist either. A phase nothing can enter is a lie.
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
	 * Turnaround or FlightPhaseFromAgent would read the taxi that follows it as a taxi IN.
	 */
	Manoeuvring,
	TaxiOut,
	Departing,
	Departed,
	/** The player said no. */
	Declined,
	/** Nobody said anything and the offer timed out. */
	Expired
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
 * One flight, from the offer to the departure.
 *
 * A UObject AND NOT A STRUCT, because UListView::SetListItems takes UObject* and the inbox
 * binds one viewmodel per row; a struct would need an adapter object per row anyway, and
 * then there would be two things to keep in step.
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
	UPROPERTY() FText TypeName;

	/** What the row prints: "CU 204", or a tail number for the club. See UOfferGenerator::MakeCallsign. */
	UPROPERTY() FString Callsign;

	UPROPERTY() EFlightPhase Phase = EFlightPhase::Offered;

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

	/** From the floor airline (the flying club) - C never penalises its lapses. */
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
	 */
	UPROPERTY() double AirborneAt = 0.0;

	/** The turnaround contract's deadline: AcceptedAt + ContractSeconds. */
	double AirborneBy() const { return AcceptedAt + ContractSeconds; }

	/**
	 * Where THIS flight is aimed. ArrivalPlanner chooses the runway by nearest threshold to
	 * this point.
	 *
	 * PER-FLIGHT, not read off UFlightBoard::ApproachFocus at accept time: that board-level
	 * field is a scratch value the generator and the debug key both write, and whichever
	 * wrote it LAST decided every later offer's WhyNotAcceptable and DispatchNow - one
	 * flight's aim leaking into another's. Set once, at the offer, and carried from there.
	 */
	UPROPERTY() FVector2D ApproachFocus = FVector2D::ZeroVector;

	/**
	 * The stand HELD from the accept, and then the stand actually parked on.
	 *
	 * THE TWO CAN DIFFER, and that is by design: the hold guarantees A stand exists for this
	 * flight, and ArrivalPlanner then picks the nearest free one when it lands, which may be
	 * a different stand if a nearer one freed meanwhile. UFlightBoard overwrites this at
	 * Parked from the agent's own GoalNode, so what is saved is always the stand it is on.
	 */
	UPROPERTY() FEntityInstanceId Stand;

	/** The live agent, or INDEX_NONE before dispatch and once it has gone. */
	UPROPERTY() int32 AgentId = INDEX_NONE;

	/**
	 * USimClock::Now at which it parked, or 0 if it never did.
	 *
	 * THE START OF THE PARKING CLOCK. Zero means "never parked" and is CHECKED rather than
	 * trusted: a flight restored from a save mid-flight, or one put on the field by the debug
	 * land key, can reach TaxiOut without ever having parked, and billing it from the epoch
	 * would hand the player a fee larger than the airport.
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
	 * Whether the landing fee has been banked, so it cannot be banked twice.
	 *
	 * SAVED, not transient: a reload that forgot this would re-bank every live flight's landing
	 * fee the next time its phase changed, and the player would be quietly paid again for
	 * aeroplanes that landed an hour ago.
	 */
	UPROPERTY() bool bLandingFeePaid = false;

	/**
	 * USimClock::Now at which this flight reached a terminal phase (Declined, Expired or
	 * Departed), or 0 before that.
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
};

/**
 * The flight phase an agent phase implies, given where the flight had got to.
 *
 * TAKES THE CURRENT PHASE because EAgentPhase::Taxiing happens twice - once to the stand and
 * once away from it - and the agent cannot tell the two apart. Everything else is a plain
 * map, and the asymmetry is the whole reason this is a function rather than a table.
 */
AIRPORTOPS_API EFlightPhase FlightPhaseFromAgent(EAgentPhase To, EFlightPhase Current);
