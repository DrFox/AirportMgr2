#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"
#include "UObject/Object.h"

#include "Flight.generated.h"

class UAircraftType;
class UAirlineDefinition;
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
 * Pushback, Diverted and Cancelled are deliberately ABSENT: the job board owns the first and
 * the sequencer the other two, and neither exists yet. A phase nothing can enter is a lie.
 */
UENUM()
enum class EFlightPhase : uint8
{
	/** In the inbox, undecided. Lapses at ExpiresAt. */
	Offered,
	/** The player said yes. A stand is held and the arrival is on the clock. */
	Accepted,
	/** Between the accept and the ETA. Nothing is in the world yet. */
	Inbound,
	Landing,
	TaxiIn,
	Turnaround,
	TaxiOut,
	Departing,
	Departed,
	/** The player said no. */
	Declined,
	/** Nobody said anything and the offer timed out. */
	Expired
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

	UPROPERTY() TObjectPtr<UAirlineDefinition> Airline = nullptr;
	UPROPERTY() TObjectPtr<UAircraftType> Type = nullptr;
	UPROPERTY() EFlightPhase Phase = EFlightPhase::Offered;

	/**
	 * USimClock::Now at which it lands.
	 *
	 * SAVED, and it is the saved truth about the schedule: USimClock deliberately does not
	 * save its callback queue, so a reload has an ETA and nothing armed until
	 * UFlightBoard::RearmSchedules reads this.
	 */
	UPROPERTY() double ArrivesAt = 0.0;

	/** USimClock::Now at which it is due off the stand, from the type's TurnaroundSeconds. */
	UPROPERTY() double OffBlockAt = 0.0;

	/** USimClock::Now at which an unanswered offer lapses. Before ArrivesAt, always. */
	UPROPERTY() double ExpiresAt = 0.0;

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
	 * Banked by the ledger when it exists.
	 *
	 * Carried HERE rather than recomputed later so that adding the ledger is a column in a
	 * widget and a post, not a redesign. Zero until then, and nothing reads them.
	 */
	UPROPERTY() double LandingFee = 0.0;
	UPROPERTY() double ParkingFee = 0.0;

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
