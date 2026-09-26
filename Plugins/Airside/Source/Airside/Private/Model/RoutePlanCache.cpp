#include "Model/RoutePlanCache.h"

#include "Model/RoadNetwork.h"
#include "Model/Vehicle.h"

namespace RoutePlanCache
{
	uint32 VehicleIdentity(const FVehicle& Vehicle)
	{
		uint32 Hash = GetTypeHash(Vehicle.TypeCode);
		auto Mix = [&Hash](double Value) { Hash = HashCombine(Hash, GetTypeHash(Value)); };
		Mix(Vehicle.BodyWidth);
		Mix(Vehicle.BodyFrontX);
		Mix(Vehicle.BodyRearX);
		Mix(Vehicle.Chassis.SteerAxleX);
		Mix(Vehicle.Chassis.FixedAxleX);
		Mix(Vehicle.Chassis.Ground.MaxSteerDegrees);
		// EffectiveSteerLaw, NOT SteerLaw (review of #301): TightestFollowableRadius branches on
		// it, and it folds in the HasAxles() fallback (RollingSteer with no measured wheelbase
		// answers as Pivot) - two vehicles equal in every OTHER mixed field but differing here
		// can be gated differently by VehicleFit::Judge (Pivot fits everywhere; RollingSteer's
		// Wheelbase/sin(lock) can refuse), so a shared key here is a wrong cache hit, not a
		// coincidence to ignore.
		Hash = HashCombine(Hash, GetTypeHash(static_cast<uint8>(Vehicle.Chassis.EffectiveSteerLaw())));
		// The speed figures too: the whole-route tow check drives the plan at them.
		Mix(Vehicle.Chassis.Ground.Taxi.SpeedCap);
		Mix(Vehicle.Chassis.Ground.Taxi.Accel);
		Mix(Vehicle.Chassis.Ground.Taxi.Decel);
		Mix(Vehicle.Chassis.Ground.MaxLateralAccelUu);
		for (const FTowLink& Link : Vehicle.Tow)
		{
			Mix(Link.HitchX);
			Mix(Link.Length);
			Mix(Link.BodyFront);
			Mix(Link.BodyRear);
			Mix(Link.Width);
		}
		return HashCombine(Hash, GetTypeHash(Vehicle.Tow.Num()));
	}
}

void FRoutePlanCache::EnsureFresh(const URoadNetwork& Network)
{
	const uint32 Revision = Network.GetGuidelineRevision();
	if (&Network != CachedNetwork.Get() || Revision != CachedRevision)
	{
		Plans.Reset();
		FitCaches.Reset();
		CachedNetwork = &Network;
		CachedRevision = Revision;
	}
}

const FCachedRoutePlan* FRoutePlanCache::Lookup(FGuidelineNodeId Start, FGuidelineNodeId Goal, const FVehicle& Vehicle) const
{
	const FKey Key{ Start, Goal, RoutePlanCache::VehicleIdentity(Vehicle) };
	return Plans.Find(Key);
}

void FRoutePlanCache::Store(FGuidelineNodeId Start, FGuidelineNodeId Goal, const FVehicle& Vehicle,
	const FRoutePlan& Plan, const FString& Reason)
{
	const FKey Key{ Start, Goal, RoutePlanCache::VehicleIdentity(Vehicle) };
	Plans.Add(Key, FCachedRoutePlan{ Plan, Reason });
}

TMap<FGuidelineEdgeId, bool>& FRoutePlanCache::FitCacheFor(const FVehicle& Vehicle)
{
	return FitCaches.FindOrAdd(RoutePlanCache::VehicleIdentity(Vehicle));
}

void FRoutePlanCache::ForEachFitCacheEntryForTest(TFunctionRef<void(FGuidelineEdgeId, bool)> Visit) const
{
	for (const TPair<uint32, TMap<FGuidelineEdgeId, bool>>& Vehicle : FitCaches)
	{
		for (const TPair<FGuidelineEdgeId, bool>& Entry : Vehicle.Value)
		{
			Visit(Entry.Key, Entry.Value);
		}
	}
}
