#pragma once

#include "CoreMinimal.h"
#include "Model/RoadEntity.h"
#include "Model/RoadHandles.h"
#include "Model/Vehicle.h"
#include "ServiceVehicle.generated.h"

/**
 * What a service vehicle is doing. Spec 2026-09-28-service-vehicle-lifecycle §2.1.
 *
 * THE VEHICLE'S OWN STATE, and the reason this enum exists (user, 2026-09-28): a fuel truck's state
 * used to be spread over the demand it served (TruckEnRoute / Fuelling), UFuelService's GoingHome
 * and Refilling maps, and Airside's EAgentPhase - and nothing at all while it sat at the depot,
 * because it did not exist there. One enum on the vehicle replaces the first three.
 *
 * NOT EAgentPhase. That is how the agent MOVES (Taxiing, Reversing, Parked) and is Airside's; this
 * is what the vehicle is FOR at the moment, and Airside must never learn it. The two are read side
 * by side: ToJob is Taxiing then Reversing then Parked, and only the arrival (Parked) moves this.
 *
 * NO ToHome. For every role home IS the facility - a fuel vehicle refills there, stairs would park
 * there with a zero-length visit - so a "going home" state would be ToFacility under another name.
 *
 * WHICH STATES HOLD AN AGENT AND A JOB is the vehicle's invariant (issue #428), one table:
 *
 *   state        AgentId   CurrentJob
 *   Idle         0         0
 *   ToJob        set       set
 *   Serving      set       set
 *   ToFacility   set       0
 *   AtFacility   0         0
 *   Deciding     set       0
 *
 * FServiceVehicleLifecycle is the only writer of these three fields and asserts the row on every transition;
 * FFuelFixture asserts it for every vehicle after every Step. THE SPEC (§2.1) SAID ONLY IDLE HAS NO AGENT, and
 * the code has always given AtFacility none too - a refill happens at the depot, not on the road - so the table
 * follows the code and the spec carries a dated note.
 */
UENUM()
enum class EServiceVehicleState : uint8
{
	/** At its home facility with nothing to do. NO AGENT: a vehicle at home is not on the road. */
	Idle,

	/** Driving to its CurrentJob's stand. Has an agent. */
	ToJob,

	/** At the stand serving CurrentJob, until StepEndsAt. Has an agent, parked. */
	Serving,

	/**
	 * Driving to its home facility - to refill before its next job, or to go idle. Has an agent and no job. ALSO
	 * WHAT A STRANDED VEHICLE IS: it heads nowhere until the player unsticks it, but its PURPOSE is still home,
	 * which is what OnVehicleArrived reads to turn it for home wherever it parks.
	 */
	ToFacility,

	/** At the facility being refilled / unloaded until StepEndsAt. No agent, like Idle. */
	AtFacility,

	/**
	 * On the road - parked or driving - with no job and no trip to the facility chosen yet: its serve has just
	 * ended, its job was taken from under it, or it has just been dispatched from home. TRANSIENT BY CONTRACT:
	 * UJobBoard::StartNext ends it the same Step and leaves the vehicle in one of the settled states, so a Step
	 * never finishes with one.
	 * ENFORCED BY: UJobBoard::Step's closing walk (an Error log that also sends a stuck vehicle home -
	 * AirportOps.Fuel.Lifecycle.StepEndSettlesAStrandedDecision); AirportOps.Fuel.Lifecycle.BlockedHeadJobNeverLeavesItServingWithNoJob
	 * (FFuelFixture's per-Step check); its price: AirportOps.Service.Bid.DecidingVehiclePricesWhereItStands
	 *
	 * A STATE, NOT "Serving WITH NO JOB": the re-bid that runs between a serve ending and the decision must price
	 * this vehicle as standing where it is, with the cargo it has, free now - which ToFacility (priced as driving
	 * home and refilling) would get wrong - and the shape that used to be spelled by an illegal Serving was
	 * caught by a per-Step backstop that kept the whole board pass running every frame while it lasted.
	 * Appended last: the enum's values are stable for anything that wrote one down.
	 */
	Deciding
};

/**
 * One row of the fleet catalogue: what a KIND of vehicle is. User's ruling 3 - the utility tow,
 * the bowser and the articulated tanker are three rows under the one fuel policy, differing in
 * these figures and in nothing else.
 *
 * RESOLVED, NOT AUTHORED, here: the chassis comes from UAirsideSettings::ResolveVehicle through UOpsRuntime::Attach
 * (this module's Model/ may not reach Content/), the capacity, rate, name and money from UScenario::FuelVehicles.
 * JOINED ONCE, BY ONE FUNCTION (#430): FServiceFleet::ResolveCatalogue builds every row UJobBoard::TypeFor answers.
 * It used to be re-assembled on every TypeFor call from the scenario map and a scan of the stand-letter table, and a
 * kind no letter was designed for got a zero chassis. The money and the name are HERE, not re-read from the scenario
 * row, so a kind is one struct (CLAUDE.md "one struct per thing"); FServiceFleet's PriceOf, ResaleOf and NameOf are
 * their readers.
 */
USTRUCT()
struct AIRPORTOPS_API FServiceVehicleType
{
	GENERATED_BODY()

	UPROPERTY() FName TypeCode;
	UPROPERTY() EServiceRole Role = EServiceRole::Fuel;

	/** How it moves and how big it is - what the route is searched for and the stand is fitted to. */
	UPROPERTY() FVehicle Vehicle;

	/**
	 * What it carries when full, in the role's unit (litres for fuel). READ THROUGH FFuelRolePolicy::CapacityOf ALONE,
	 * which floors it at 1 (#430: the bid wrote a job's tank from this unfloored while the stand floored it).
	 * ENFORCED BY: Check-Architecture rule 4's 'vehicle row capacity' row, AirportOps.Fuel.ZeroCapacitySpecStillFinishes
	 */
	UPROPERTY() double Capacity = 1.0;

	/** How fast it serves, units per GAME minute (fuel: its pump's flow). Floored at 1 by the policy. */
	UPROPERTY() double RatePerMinute = 1.0;

	/** The scenario's DisplayName for the kind, as authored - EMPTY is allowed, and FServiceFleet::NameOf is the one
	 *  reader that falls back to the code, so the card, the alert and the ledger cannot fall back differently. */
	UPROPERTY() FText DisplayName;

	/** What buying one costs. Read through FServiceFleet::PriceOf alone (Check-Architecture rule 43). */
	UPROPERTY() double Price = 0.0;

	/** What owning one costs a game day - part of the daily "Fleet upkeep" entry (UFacilityPurchases::DailyUpkeep). */
	UPROPERTY() double UpkeepPerDay = 0.0;

	/** What one is worth back, resolved from FFuelVehicleSpec::ResaleValue at the join. Read through
	 *  FServiceFleet::ResaleOf alone (Check-Architecture rule 43). */
	UPROPERTY() double ResaleValue = 0.0;
};

/**
 * How a vehicle came to be in a fleet: the two ways IN of FServiceFleet::Add. A UENUM since #487, when the vehicle began
 * to carry it (FServiceVehicle::Origin) - it was a plain enum beside EFleetChange, which nothing reflected held.
 */
UENUM()
enum class EFleetOrigin : uint8
{
	/** The player paid for it (UFacilityPurchases::BuyVehicle): charged the type's price, and worth its resale on the way out. */
	Bought,
	/** The starter fleet a placed depot begins with (Trucks > 0, SeedStarterFleets): free, and so worth nothing on the way out. */
	Seeded
};

/**
 * One real vehicle. Owns its state, its cargo and its queue - user's ruling 1.
 *
 * EXISTS WHILE IDLE, which is the whole difference from what it replaced: a depot used to be an
 * int32 Trucks and a truck only an agent id while it was out, so nothing could hold its tank, its
 * next job or where it had got to. The Airside agent is now something the vehicle HAS while it is
 * on the road (AgentId), not what it is.
 */
USTRUCT()
struct AIRPORTOPS_API FServiceVehicle
{
	GENERATED_BODY()

	/** Board-issued, from 1. NOT an agent id: a vehicle outlives every agent it drives as. */
	UPROPERTY() int32 Id = 0;

	/** Its row in UJobBoard's type catalogue. */
	UPROPERTY() FName TypeCode;

	UPROPERTY() EServiceRole Role = EServiceRole::Fuel;

	/** The depot it belongs to and returns to - its facility, for fuel. */
	UPROPERTY() FEntityInstanceId Home;

	/**
	 * HOW IT CAME (#487), set once by FServiceFleet::Add and read by FServiceFleet::RefundOf alone: only a vehicle the player
	 * BOUGHT is worth resale. A removed depot used to credit the resale of every vehicle it held whatever its origin, so a
	 * starter depot's free fleet paid out when it was bulldozed - and bulldozing then re-placing a plotless starter depot was a
	 * repeatable money source.
	 *
	 * DEFAULTS TO Seeded, THE SAFE ANSWER: a vehicle nobody made through Add (a saved board from before this field) carries no
	 * claim to a refund, and inventing one is the loop this closes. There are no player saves yet (owner ruling 2026-09-23), so
	 * no save written before this field loses a real purchase; this is a layout break and the PR says so.
	 */
	UPROPERTY() EFleetOrigin Origin = EFleetOrigin::Seeded;

	/**
	 * WRITTEN ONLY BY FServiceVehicleLifecycle - with AgentId and CurrentJob, the three fields whose combination
	 * is the state's invariant (see the table on EServiceVehicleState). Public because a USTRUCT's fields are
	 * how UHT and every reader see it; Check-Architecture rule 38 is what keeps the writers to one file.
	 */
	UPROPERTY() EServiceVehicleState State = EServiceVehicleState::Idle;

	/** The Airside agent while on the road (every state but Idle and AtFacility), else 0. */
	UPROPERTY() int32 AgentId = 0;

	/** What it is carrying now, in the role's unit. Starts full. */
	UPROPERTY() double Cargo = 0.0;

	/** The job it is driving to or serving (ToJob, Serving), else 0. Not in Queue. */
	UPROPERTY() int32 CurrentJob = 0;

	/**
	 * Jobs committed to it and not yet started, in the order it will do them. APPEND ONLY (user's
	 * ruling 6): an insertion would move finish times already promised to the jobs behind it, and
	 * those promises are what other bids and turnaround deadlines were judged against.
	 */
	UPROPERTY() TArray<int32> Queue;

	/**
	 * When the current timed step (Serving, AtFacility) ends, USimClock game seconds; 0 outside one. ITS START IS NOT
	 * KEPT: a StepStartedAt sat beside it from stage 1, was written at three sites and read at none (the job carries
	 * the trip's own TripStartedAt, which is what the card's live litres read); a field with no reader is a second
	 * source of truth waiting to disagree.
	 */
	UPROPERTY() double StepEndsAt = 0.0;
};
