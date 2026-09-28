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

	/** Driving to its home facility - to refill before its next job, or to go idle. Has an agent. */
	ToFacility,

	/** At the facility being refilled / unloaded until StepEndsAt. No agent, like Idle. */
	AtFacility
};

/**
 * One row of the fleet catalogue: what a KIND of vehicle is. User's ruling 3 - the utility tow,
 * the bowser and the articulated tanker are three rows under the one fuel policy, differing in
 * these figures and in nothing else.
 *
 * RESOLVED, NOT AUTHORED, here: the chassis comes from UAirsideSettings through UOpsRuntime::Attach
 * (this module's Model/ may not reach Content/), the capacity and rate from UScenario::FuelVehicles.
 */
USTRUCT()
struct AIRPORTOPS_API FServiceVehicleType
{
	GENERATED_BODY()

	UPROPERTY() FName TypeCode;
	UPROPERTY() EServiceRole Role = EServiceRole::Fuel;

	/** How it moves and how big it is - what the route is searched for and the stand is fitted to. */
	UPROPERTY() FVehicle Vehicle;

	/** What it carries when full, in the role's unit (litres for fuel). Floored at 1 by the policy. */
	UPROPERTY() double Capacity = 1.0;

	/** How fast it serves, units per GAME minute (fuel: its pump's flow). Floored at 1 by the policy. */
	UPROPERTY() double RatePerMinute = 1.0;
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

	UPROPERTY() EServiceVehicleState State = EServiceVehicleState::Idle;

	/** The Airside agent while on the road (ToJob, Serving, ToFacility), else 0. */
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

	/** The current timed step (Serving, AtFacility), USimClock game seconds. */
	UPROPERTY() double StepStartedAt = 0.0;
	UPROPERTY() double StepEndsAt = 0.0;
};
