#pragma once

#include "CoreMinimal.h"
#include "Model/DeparturePlanner.h"
#include "Model/RoadEntity.h"
#include "Model/RoadHandles.h"
#include "ServiceJob.generated.h"

/**
 * How far one service job has got. Spec 2026-09-28-service-vehicle-lifecycle §2.2.
 *
 * AN ENUM, NEVER A SET OF BOOLS - the rule EAgentPhase and ECrossingPhase follow. It replaced
 * EFuelDemandState, whose TruckEnRoute / Fuelling named the TRUCK's progress on the DEMAND: the
 * truck's own state now lives on the truck (EServiceVehicleState) and this says only what the job
 * is waiting for.
 *
 * Queued AND Underway ARE SEPARATE because the commitment rule turns on the difference (user's
 * ruling 7, 2026-09-28): a queued job may still be re-bid to a better vehicle, one the vehicle has
 * set off toward may not - a truck turned back halfway is visible and reads as broken.
 */
UENUM()
enum class EServiceJobState : uint8
{
	/** Asking for a vehicle. The board bids it on the tick it opens. */
	Open,

	/** On a vehicle's queue, not yet set off toward. Re-biddable (stage 2). */
	Queued,

	/** The vehicle is driving to it. Committed: never re-bid. */
	Underway,

	/** The vehicle is at the stand, serving this trip. */
	Serving,

	/**
	 * Everything owed has been delivered. The job stays in this state until its aircraft leaves,
	 * so the aircraft's card can say so and so a second vehicle is never sent for it.
	 */
	Done,

	/**
	 * Nothing can serve it, and Why says what is missing.
	 *
	 * TERMINAL FOR THIS GRAPH: a re-offer needs the airport itself to have CHANGED, which is what
	 * URoadNetwork::GetGuidelineRevision reports. Without that test a job refused once would be
	 * retried every tick for ever, logging as it went.
	 */
	Unserviceable
};

/**
 * How much of its fuel an aircraft left with - what FTurnaroundEndedEvent tells the airline (spec
 * 2026-09-29-ops-batch3 §2). AN ENUM, not bUnfuelled + bPartFuelled: part-fuelled IS unserviceable with
 * something delivered, and two bools would admit "part-fuelled but fuelled".
 */
UENUM()
enum class EFuelOutcome : uint8
{
	/** Everything it asked for - or it asked for nothing. */
	Fuelled,
	/** Its fuel job went Unserviceable after at least one delivery. */
	PartFuelled,
	/** Its fuel job went Unserviceable before any delivery. */
	Unfuelled
};

/**
 * WHY nothing can serve a job - one cause, named for the thing the player would go and fix.
 *
 * Not a bare "no route", which is the least useful thing a system can say to somebody building an
 * airport - the same argument ERouteResult's own header makes. "No fuel depot" is a building to
 * place and "depot not on a road" is a road to draw, and the two have nothing in common but the
 * symptom.
 *
 * WAS EFuelRefusal, and every member kept its meaning. There is no "busy" member and there never
 * was one to keep: a busy vehicle BIDS now (its queue is priced into its finish time), so a job
 * with any eligible vehicle is never refused for want of a free one.
 */
UENUM()
enum class EServiceRefusal : uint8
{
	None,

	/** No live depot of the job's role anywhere on the airport. */
	NoDepot,

	/** Depots exist; not one of them has a road within its lead-in reach. */
	NoRoad,

	/** The stand's own service anchor joins nothing - no road within reach of it. */
	StandUnjoined,

	/** Everything is joined and the graph still does not connect the two. */
	NoRoute,

	/**
	 * Joined and connected, but only over road the vehicle does not fit - a lane narrower than its
	 * body or a corner its swept path cannot take (route search's TooNarrow, spec 2026-09-23 §6).
	 * ITS OWN REFUSAL AND NOT NoRoute, for NoPump's reason: "no road from depot" about a road that
	 * is there sends the player to fix the wrong thing.
	 */
	TooNarrow,

	/**
	 * A depot is on a road and has vehicles, but no PUMP was built in its plot.
	 *
	 * ITS OWN REFUSAL AND NOT NoRoute: a pumpless depot falling through to the chain's default would
	 * report "no road from depot" and send the player to look at a road that is already there. What
	 * they actually need to do is build a pump in a bay.
	 */
	NoPump,

	/**
	 * A depot is on a road with a pump, but every vehicle it has is LARGER than the stand was built
	 * for (VehicleFit::NoLargerThan against the vehicle its definition's lanes were proven for,
	 * UEntityDefinition::DesignVehicle - spec 2026-09-26 section 2). A stand's lane legs are only
	 * proven drivable by that vehicle and anything no larger, so sending a bigger one would strand it
	 * on a leg it cannot take. ITS OWN REFUSAL AND NOT TooNarrow: nothing about the ROAD is wrong,
	 * and a player widening one would fix nothing.
	 * ENFORCED BY: AirportOps.Fuel.VehicleTooLargeRefused
	 */
	VehicleTooLarge,

	/**
	 * Depots are on a road but not one of them has a vehicle (facility-upgrades spec R3: a new depot
	 * starts empty). ITS OWN REFUSAL AND NOT NoRoute: "no road from depot" about an empty depot on a
	 * road sends the player to fix a road that is fine. APPENDED so no other value moves.
	 * ENFORCED BY: AirportOps.Fuel.EmptyDepotSaysNoVehicles
	 */
	NoVehicles,

	/**
	 * A depot is on a road with a pump and has vehicles, but every one of them is of a kind the catalogue has no row for - a
	 * vehicle restored under a scenario that has since dropped its kind (#430, #478). Judge skips such a vehicle (it has no
	 * chassis to fit a stand with), and before this it skipped it silently, so the chain fell through to NoRoute: "no road
	 * from depot" about a depot on a road that reaches the stand. ITS OWN REFUSAL AND NOT NoVehicles: the card lists the
	 * vehicle, so "buy one" would contradict it - what the player can do is sell the unusable ones and buy a kind that exists.
	 * NOT NoRoute, for NoPump's reason. APPENDED so no other value moves.
	 * ENFORCED BY: AirportOps.Fuel.UnknownKindSaysSo
	 */
	UnknownVehicleKind
};

/**
 * One service a parked aircraft needs: a role, a stand, a quantity. Spec §2.
 *
 * NOT WHO SERVES IT. The vehicle is a queue entry on FServiceVehicle; VehicleId here is only the
 * back-reference that lets the board find that entry without walking every queue, and it is written
 * by exactly the calls that write the queue (UJobBoard::Assign, ::Unassign).
 */
USTRUCT()
struct AIRPORTOPS_API FServiceJob
{
	GENERATED_BODY()

	/** Board-issued, from 1. 0 means "no job". */
	UPROPERTY() int32 Id = 0;

	/** The flight this job is for, or 0 when the aircraft has no flight (a test's bare agent). */
	UPROPERTY() int32 FlightId = 0;

	/** The parked agent. Agent ids start at 1, so 0 means "no aircraft". */
	UPROPERTY() int32 AircraftId = 0;

	UPROPERTY() EServiceRole Role = EServiceRole::Fuel;

	/** Where it parked, so the service anchor can be found again on any later tick. */
	UPROPERTY() FEntityInstanceId Stand;

	UPROPERTY() EServiceJobState State = EServiceJobState::Open;

	/** Still to deliver, and delivered so far - litres, for fuel. Owed comes from the flight. */
	UPROPERTY() double QuantityOwed = 0.0;
	UPROPERTY() double QuantityDelivered = 0.0;

	/** Trips completed. A load bigger than a vehicle's tank takes more than one. */
	UPROPERTY() int32 Trips = 0;

	/** The vehicle whose queue holds it (Queued, Underway, Serving), else 0. */
	UPROPERTY() int32 VehicleId = 0;

	/** The finish time the winning bid promised, USimClock game seconds. What a re-bid must beat. */
	UPROPERTY() double PromisedFinish = 0.0;

	/** The capacity of the vehicle that took the latest trip - what the card counts trips by. */
	UPROPERTY() double TankLitres = 0.0;

	/** Whose depot took the latest trip - kept past the trip, for the log and a test, for
	 *  TankLitres' reason: VehicleId is cleared the moment the vehicle moves on. */
	UPROPERTY() FEntityInstanceId LastDepot;

	/**
	 * This trip, while Serving: how much, and when the pump started and stops (USimClock game time,
	 * spec fuel-litres - a pause stops it). The card counts down from these WHILE the pump runs;
	 * QuantityOwed only moves when the trip ends.
	 */
	UPROPERTY() double TripQuantity = 0.0;
	UPROPERTY() double TripStartedAt = 0.0;
	UPROPERTY() double TripEndsAt = 0.0;

	UPROPERTY() EServiceRefusal Why = EServiceRefusal::None;

	/**
	 * The guideline revision THIS job's refusal was decided against.
	 *
	 * PER JOB, NOT ON THE BOARD (issue #193): a single board-wide field used to stand in for every
	 * demand's own fact, and two refused on different ticks share the one field. A refusing later in
	 * the SAME Tick pass overwrites it with the newer revision before B - already Unserviceable from
	 * an older one - is checked, so B reads "already seen this revision" against a value ITS OWN
	 * refusal never set, and stays stuck with the re-offer log line never firing.
	 */
	UPROPERTY() uint32 RefusedAtRevision = 0;

	/**
	 * Jobs of the same flight that must be Done before this one may START (it may still be queued).
	 * Spec §2 and systems map §3.5: unload before load, no fuelling during boarding, pushback last.
	 * EMPTY FOR EVERY JOB TODAY - fuel is the only role - and the field exists so the ordering rule
	 * has somewhere to live when a second role arrives, rather than being a bool bolted on then.
	 */
	UPROPERTY() TArray<int32> Prerequisites;
};

/**
 * One parked aircraft's time on stand: its deadline, and the jobs it asked for.
 *
 * THE PER-FLIGHT INDEX the user's ruling 8 asked for - "this flight's jobs" is JobIds, kept beside
 * the one job list rather than being a second board. It is also what an aircraft that wants NO
 * service is: a turnaround with no jobs, which still departs at its deadline. The fuel demand used
 * to fake that as a Done demand for zero litres, because DepartTheReady walked the demands.
 */
USTRUCT()
struct AIRPORTOPS_API FTurnaround
{
	GENERATED_BODY()

	UPROPERTY() int32 AircraftId = 0;
	UPROPERTY() int32 FlightId = 0;
	UPROPERTY() FEntityInstanceId Stand;

	/**
	 * USimClock::Now at which this aircraft is free to push back. Set when it parks.
	 *
	 * ALSO THE GRACE PERIOD when nothing can serve the aircraft. One deadline, not two: an aircraft
	 * whose fuel went Unserviceable waits exactly as long as one being fuelled, and then leaves
	 * without it.
	 *
	 * THE SINGLE OFF-BLOCK TRUTH. UFlight used to carry its own OffBlockAt, computed from the ETA at
	 * offer time; this is computed from the actual park time and the two disagreed the moment an
	 * arrival was late. Departure is decided here and nowhere else - see issue #97.
	 */
	UPROPERTY() double TurnaroundEndsAt = 0.0;

	/**
	 * The last reason a departure was refused, so a retry logs only a CHANGE of reason. A departure can be
	 * refused for as long as the player leaves a taxiway busy or a stand without a push arm; it is retried
	 * on an event (UJobBoard::Step's header), and every 30 s by the ops safety net while
	 * UJobBoard::HasRefusedDeparture finds this set.
	 */
	UPROPERTY() EDepartureRefusal LastDepartureRefusal = EDepartureRefusal::None;

	UPROPERTY() TArray<int32> JobIds;
};
