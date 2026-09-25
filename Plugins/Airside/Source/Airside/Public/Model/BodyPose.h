#pragma once

// ITS OWN HEADER FOR THE REASON Model/GearPose.h IS ONE: FAgentMotion carries it, and
// AgentMotion.h is kept to CoreMinimal.h plus the poses it needs.

#include "CoreMinimal.h"
#include "BodyPose.generated.h"

/**
 * Where a ground vehicle's working parts have got to, as fractions: the catering box's lift,
 * its platform, a cart's towbar. What FGearPose is for an undercarriage.
 *
 * NAMED FOR THE DEPLOYED END AND ZERO AT REST, WHICH IS THE OPPOSITE OF FGearPose, and the
 * reason is the same reason turned round. FGearPose names each fraction for its rest state so
 * that its DEFAULT is the bind pose - a parked aeroplane, gear down. A vehicle's bind pose is
 * STOWED - box down, platform in, towbar level - so zero is what has to be the default, and
 * naming each fraction for the end it travels TO makes zero mean exactly that. The model repo
 * already names them this way (rigidCab1/README.md: "LiftFraction 0-1", "PlatformFraction
 * 0-1"), so the names also match the contract the rig was built to.
 *
 * FRACTIONS AND NOT DISTANCES, as FGearPose's are: 1.80 m of platform travel and 70 degrees
 * of towbar are facts about one RIG, and UAirsideAgentAnim multiplies by its own measured
 * figures. The model says how far; the view knows how far that is.
 *
 * NO AIRPORT SERVICE SETS IT ON 2026-09-25. The catering service that raises the box at a
 * door is future work; until it lands the model yard's Body channel is how each rig's graph is
 * proved before a service needs it. A default FBodyPose rotates and moves nothing, so an agent
 * the airport dispatches without setting it stands stowed.
 */
USTRUCT()
struct AIRSIDE_API FBodyPose
{
	GENERATED_BODY()

	/** The body's lift: 0 lowered onto the chassis, 1 at full height. catering1's Lift clip. */
	UPROPERTY() double LiftFraction = 0.0;

	/**
	 * The platform: 0 stowed inside the box, 1 run out to the door. catering1's Platform clip.
	 *
	 * ONLY LEGAL WITH THE BOX HIGH - rigidCab1/README.md: "only while floor >= 2.70 m", since
	 * the platform stowed at a lower floor would slide into the cab roof. The writer enforces
	 * it (see FYardMotion::ToAgentMotion); this struct does not, because it is a pose rather
	 * than a sequencer, exactly as FGearPose does not stop a door shutting on a lowered leg.
	 */
	UPROPERTY() double PlatformFraction = 0.0;

	/** A towbar: 0 level (the coupled pose, and the bind pose), 1 raised to stow. */
	UPROPERTY() double TowbarRaisedFraction = 0.0;
};
