#pragma once

#include "CoreMinimal.h"
#include "Model/RoadEntity.h"

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
	/** Litres per GAME minute per pump module a depot refills a returning vehicle at. */
	double RefillLitresPerMinutePerPump = 500.0;

	virtual EServiceRole Role() const override { return EServiceRole::Fuel; }
	virtual bool NeedsPumpAtHome() const override { return true; }

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
