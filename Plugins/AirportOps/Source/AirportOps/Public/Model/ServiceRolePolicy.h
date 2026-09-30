#pragma once

#include "CoreMinimal.h"
#include "Model/RoadEntity.h"
#include "Model/OpsDesignDefaults.h"

struct FServiceVehicleType;

/** What a vehicle does before its next job: go straight there, or visit its facility first. */
enum class EServiceStep : uint8
{
	Direct,
	ViaFacility
};

/**
 * The rules one service role plays by. Spec 2026-09-28-service-vehicle-lifecycle §2.3.
 *
 * STRATEGY, and the one place a role differs (user, 2026-09-28): the vehicle lifecycle is the same
 * state machine for every role, and what varies is what the vehicle CARRIES between jobs - nothing
 * (stairs, GPU, tug), a stock that runs down (fuel, catering), or a load moved between two places
 * (baggage). Every such difference reduces to the question NextStep answers - "given what I carry,
 * can I go straight to the next job?" - plus how long serving and the facility take.
 *
 * PURE FUNCTIONS OF (cargo, type, quantity), NOT OF A VEHICLE, deliberately. The live lifecycle and
 * the bid simulation (ServiceBid::Finish) both call these, and they must be the SAME rule: a bid that
 * priced a refill the vehicle then did not make, or skipped one it did, promises a finish the truck
 * never keeps. FSpeedProfile's history is the warning - four attempts shipped green because tests
 * re-implemented its rule instead of calling it.
 *
 * NOT A UObject: a policy has no state to save or reflect, and a plain interface keeps Model/ free
 * of a class nothing else needs.
 */
class AIRPORTOPS_API IServiceRolePolicy
{
public:
	virtual ~IServiceRolePolicy() = default;

	virtual EServiceRole Role() const = 0;

	/** Whether a home depot with no pump module can serve this role at all (fuel: no). */
	virtual bool NeedsPumpAtHome() const = 0;

	/**
	 * How much a job may be short and still be DONE, in the role's unit (fuel: litres). ONE FIGURE for every reader:
	 * the vehicle's NextStep (a tank this close to covering the job goes straight there), the bid's simulation (a job
	 * this close to done is done, and a trip this small delivered nothing) and the board's FinishServe (the job is
	 * called Done within it). They were three copies of a half-litre typed five times (#443), and a change to one made
	 * the bid price a different number of trips than the truck made. It lives on the POLICY because what "done" means
	 * is the role's: a stock of litres has slack a count of bags does not.
	 * ENFORCED BY: Check-Architecture rule 44 (fuelled-tolerance-once), AirportOps.Service.Policy.OneToleranceForBidAndVehicle
	 */
	virtual double DoneWithin() const = 0;

	/** Straight to a job still owed Owed, carrying Cargo - or via the facility first. */
	virtual EServiceStep NextStep(double Cargo, const FServiceVehicleType& Type, double Owed) const = 0;

	/** How much one trip delivers, arriving with Cargo at a job still owed Owed. */
	virtual double TripQuantity(double Cargo, const FServiceVehicleType& Type, double Owed) const = 0;

	/** GAME seconds serving Quantity takes. */
	virtual double ServeSeconds(const FServiceVehicleType& Type, double Quantity) const = 0;

	/** What the vehicle carries after delivering Quantity. */
	virtual double CargoAfterServe(double Cargo, double Quantity) const = 0;

	/** GAME seconds at the facility, arriving with Cargo, at a facility with Pumps service points. */
	virtual double FacilitySeconds(double Cargo, const FServiceVehicleType& Type, int32 Pumps) const = 0;

	/** What the vehicle carries leaving the facility. */
	virtual double CargoAfterFacility(double Cargo, const FServiceVehicleType& Type) const = 0;
};

/**
 * Fuel: the cargo is litres in the tank, the facility is the home depot, and the depot's pumps
 * refill it.
 */
class AIRPORTOPS_API FFuelRolePolicy final : public IServiceRolePolicy
{
public:
	/** Litres per GAME minute per pump module a depot refills a returning vehicle at. UJobBoard hands its own in every
	 *  time it asks; this default is the one design default, not a third copy of it (#449). */
	double RefillLitresPerMinutePerPump = OpsDesignDefaults::RefillLitresPerMinutePerPump;

	/**
	 * Half a litre: a tank 0.2 L short of the job is not worth a trip to the depot, a tank that reads 999.8 of 1000 is full,
	 * a job 0.4 L short is fuelled. A STATIC so the two things that judge a job with no policy in hand
	 * (UJobBoard::FuelOutcomeOf, a departing aircraft's outcome) read the same number DoneWithin answers.
	 */
	static constexpr double FuelledWithinLitres = 0.5;

	virtual EServiceRole Role() const override { return EServiceRole::Fuel; }
	virtual bool NeedsPumpAtHome() const override { return true; }
	virtual double DoneWithin() const override { return FuelledWithinLitres; }

	/**
	 * Direct when the tank covers what is owed - OR IS FULL. A job bigger than the tank takes more
	 * than one trip whatever happens, and a full tank cannot do better by refilling first, so the
	 * vehicle goes and delivers what it has (user's ruling 4: back to the depot only when it does not
	 * have enough - and "enough" for a job no tank covers is a full one).
	 */
	virtual EServiceStep NextStep(double Cargo, const FServiceVehicleType& Type, double Owed) const override;
	virtual double TripQuantity(double Cargo, const FServiceVehicleType& Type, double Owed) const override;
	virtual double ServeSeconds(const FServiceVehicleType& Type, double Quantity) const override;
	virtual double CargoAfterServe(double Cargo, double Quantity) const override;
	virtual double FacilitySeconds(double Cargo, const FServiceVehicleType& Type, int32 Pumps) const override;
	virtual double CargoAfterFacility(double Cargo, const FServiceVehicleType& Type) const override;

	/**
	 * The tank, FLOORED AT A LITRE: ClampMin guards only the editor, and a 0 L tank would make every
	 * trip a zero-litre trip, for ever. ENFORCED BY: AirportOps.Fuel.ZeroCapacitySpecStillFinishes
	 */
	static double CapacityOf(const FServiceVehicleType& Type);
};
