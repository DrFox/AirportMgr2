#include "Profiles/RoadDesignVehicles.h"

#include "Profiles/RoadProfile.h"

const FChassis& FRoadDesignVehicles::For(const URoadProfile* Profile) const
{
	const FChassis* Found = Profile != nullptr ? PerProfile.Find(TObjectKey<URoadProfile>(Profile)) : nullptr;
	return Found != nullptr ? *Found : Default;
}
