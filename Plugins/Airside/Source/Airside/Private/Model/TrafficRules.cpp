// FTrafficRules's three lookups - moved off GroundTraffic.cpp with the struct itself
// (issue #175). See Model/TrafficRules.h for why the struct's own header forward-declares
// ETraversalClass and EPushbackNeed rather than including RoadTraffic.h/RoadEntity.h for
// them: these three bodies are the one place that needs the actual enum values, so they -
// and only they - pay for the include.

#include "Model/TrafficRules.h"

#include "Model/RoadEntity.h"
#include "Model/RoadTraffic.h"

double FTrafficRules::FootprintFor(ETraversalClass Class) const
{
	// Pedestrians and emergency vehicles take the vehicle figures: nothing authored says
	// otherwise yet, and a fire truck is nearer a van than an aeroplane.
	return Class == ETraversalClass::Aircraft ? AircraftFootprint : VehicleFootprint;
}

double FTrafficRules::GapFor(ETraversalClass Class) const
{
	return Class == ETraversalClass::Aircraft ? AircraftGap : VehicleGap;
}

double FTrafficRules::PushSpeedFor(EPushbackNeed Need) const
{
	// A SWITCH AND NOT A TERNARY CHAIN, deliberately, and unlike the two functions above -
	// which have two cases and a documented "everything else" rule. This is the one place
	// that must agree with EPushbackNeed, so a value added to that enum has to produce a
	// compiler warning here rather than fall quietly into an else and push an A320 at a hand
	// tug's pace. The codebase's "lists that must agree are ONE list", applied to arithmetic.
	switch (Need)
	{
	case EPushbackNeed::SelfManoeuvre: return SelfManoeuvrePushSpeed;
	case EPushbackNeed::HandTug:       return HandTugPushSpeed;
	case EPushbackNeed::VehicleTug:    return VehicleTugPushSpeed;
	}

	// Unreachable while the switch is total. The CONSERVATIVE answer anyway, matching
	// FAirframe::PushbackNeed's own default: slowest is never unsafe.
	return HandTugPushSpeed;
}
