#include "Profiles/RoadDesignVehicles.h"

#include "Profiles/RoadProfile.h"

const FChassis& FRoadDesignVehicles::For(const URoadProfile* Profile) const
{
	return VehicleFor(Profile).Chassis;
}

const FVehicle& FRoadDesignVehicles::VehicleFor(const URoadProfile* Profile) const
{
	const FVehicle* Found = Profile != nullptr ? PerProfile.Find(TObjectKey<URoadProfile>(Profile)) : nullptr;
	return Found != nullptr ? *Found : Default;
}
