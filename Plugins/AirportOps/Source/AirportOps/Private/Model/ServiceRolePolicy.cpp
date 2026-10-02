#include "Model/ServiceRolePolicy.h"

#include "Model/ServiceVehicle.h"

double FFuelRolePolicy::CapacityOf(const FServiceVehicleType& Type)
{
	return FMath::Max(Type.Capacity, 1.0);
}

EServiceStep FFuelRolePolicy::NextStep(double Cargo, const FServiceVehicleType& Type, double Owed, double Available) const
{
	// THE SLACK THE JOB ITSELF IS JUDGED DONE BY (DoneWithin): a tank 0.2 L short of the job is not worth a trip to the
	// depot, and a tank that reads 999.8 of 1000 is full.
	const bool bCovers = Cargo >= Owed - DoneWithin();
	const bool bFull = Cargo >= CapacityOf(Type) - DoneWithin();
	// NOTHING TO GAIN AT THE DEPOT: a dry airport refills nothing, so a tank with something in it delivers that instead.
	const bool bDepotDry = Available < DoneWithin() && Cargo >= DoneWithin();
	return (bCovers || bFull || bDepotDry) ? EServiceStep::Direct : EServiceStep::ViaFacility;
}

double FFuelRolePolicy::TripQuantity(double Cargo, const FServiceVehicleType& Type, double Owed) const
{
	return FMath::Clamp(Owed, 0.0, FMath::Max(Cargo, 0.0));
}

double FFuelRolePolicy::ServeSeconds(const FServiceVehicleType& Type, double Quantity) const
{
	// LITRES OVER FLOW, IN GAME TIME (spec 2026-09-28-fuel-litres). Floored like the tank: a 0 L/min
	// pump would never finish.
	return FMath::Max(Quantity, 0.0) / FMath::Max(Type.RatePerMinute, 1.0) * 60.0;
}

double FFuelRolePolicy::CargoAfterServe(double Cargo, double Quantity) const
{
	return FMath::Max(Cargo - Quantity, 0.0);
}

double FFuelRolePolicy::FacilitySeconds(double Cargo, const FServiceVehicleType& Type, int32 Pumps, double Available) const
{
	// WHAT IT PUMPED OUT, back in at the depot's pumps - the refill UFuelService used to size from
	// RefillOnReturn - and since 2026-10-02 no more than the airport holds. A full tank or a dry airport
	// takes no time, which is what lets a vehicle that went home without serving (a recall) be free at once.
	const double Pumped = FMath::Min(FMath::Max(CapacityOf(Type) - Cargo, 0.0), FMath::Max(Available, 0.0));
	return Pumped / (FMath::Max(Pumps, 1) * FMath::Max(RefillLitresPerMinutePerPump, 1.0)) * 60.0;
}

double FFuelRolePolicy::CargoAfterFacility(double Cargo, const FServiceVehicleType& Type, double Available) const
{
	// WAS CapacityOf(Type), unconditionally - the line that made fuel free (spec 2026-10-02 §7). Clamped to the tank so a
	// vehicle arriving over it (a catalogue tank shrunk under a saved vehicle) leaves at the tank, as it always did.
	return FMath::Min(CapacityOf(Type), Cargo + FMath::Max(Available, 0.0));
}
