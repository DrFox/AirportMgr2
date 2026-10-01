// FTrafficRules's three lookups - moved off GroundTraffic.cpp with the struct itself
// (issue #175). See Model/TrafficRules.h for why the struct's own header forward-declares
// ETraversalClass and EPushbackNeed rather than including RoadTraffic.h/RoadEntity.h for
// them: these three bodies are the one place that needs the actual enum values, so they -
// and only they - pay for the include.

#include "Model/TrafficRules.h"

#include "Model/ExhaustiveSwitch.h"
#include "Model/Airframe.h"   // EPushbackNeed itself: reached only transitively before, which a unity blob
                               // hid until 2026-09-26, when an adaptive build compiled this file alone
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
	const double Gap = Class == ETraversalClass::Aircraft ? AircraftGap : VehicleGap;

	// NEVER LESS THAN HALF THE FOOTPRINT, whatever the two knobs say (#455). A vehicle refused a node stops this far
	// short of it, measured from its CENTRE (FClaimPass::StopWithinFor), and the claim it then makes on the node turns
	// OCCUPIED once the centre is within half a footprint of it, plus the node's reach (FClaimPass, the end-node
	// claim: `|End - T| < F * 0.5 + ExcessTo` - and StopWithinFor stops the centre ExcessTo further back by the same
	// reach, so the reach cancels and a gap of half the footprint is exactly on the threshold).
	// A gap under that stops the refused vehicle INSIDE the zone where its own claim is an occupancy, and the table -
	// where a body standing on a node is never moved - hands it the node another vehicle reserved first. VehicleGap
	// was 300 against half a VehicleFootprint of 334.75, and the footprint is authored from the mesh (it has been 500,
	// 620, 850 and 669.5), so a bigger default would only have waited for the next re-measure to be wrong again: the
	// floor holds for any figure either knob takes, and is the one place that says so.
	//
	// THE ONE UNIT MORE is the strict `<` in that test: a gap of exactly half the footprint stops the centre ON the
	// threshold, where floating point may fall either side of it.
	// ENFORCED BY: Airside.Model.Traffic.ReservedNodeKeepsItsReserver (the refused van waits outside the zone),
	// Airside.Model.Traffic.RulesGapClearsHalfTheFootprint (the floor, for any tuning)
	return FMath::Max(Gap, FootprintFor(Class) * 0.5 + 1.0);
}

double FTrafficRules::PushClearBy(ETraversalClass Class) const
{
	// THROUGH THE TWO LOOKUPS, so GapFor's floor holds here too.
	return FootprintFor(Class) + GapFor(Class);
}

// ENFORCED BY: AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN (checked 2026-09-30 by a stray enumerator: the build failed here)
AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN
double FTrafficRules::PushSpeedFor(EPushbackNeed Need) const
{
	// A SWITCH AND NOT A TERNARY CHAIN, deliberately, and unlike the two functions above -
	// which have two cases and a documented "everything else" rule. This is the one place
	// that must agree with EPushbackNeed, so a value added to that enum has to produce a
	// BUILD ERROR here (C4062, raised around this function - see ExhaustiveSwitch.h) rather than fall quietly into an
	// else and push an A320 at a hand tug's pace. The codebase's "lists that must agree are ONE list", applied to arithmetic.
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
AIRSIDE_EXHAUSTIVE_SWITCH_END
