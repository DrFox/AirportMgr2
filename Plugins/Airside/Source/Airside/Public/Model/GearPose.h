#pragma once

// SPLIT INTO ITS OWN HEADER RATHER THAN DECLARED IN Model/Airframe.h, for the reason #176
// gave when it split FAgentMotion out of RoadEntity.h: a POSE is what the view needs and a
// PERFORMANCE is how the airframe moves, and the two change for different reasons. Airframe.h
// drags FAirframe, FSpeedProfile and FEnginePerformance behind it, while AgentMotion.h
// deliberately includes CoreMinimal.h and nothing else - "nothing about where an entity sits
// on the apron or how an airframe moves", in its own words. Both ends of this struct's life
// need it: FGearPerformance::FractionsAt returns it and FAgentMotion carries it. Putting it in
// Airframe.h would have made the view's header include the model's heaviest one.

#include "CoreMinimal.h"
#include "GearPose.generated.h"

/**
 * Where an undercarriage has got to, as fractions. The output of one evaluator and the input
 * to one animgraph.
 *
 * ONE STRUCT BECAUSE THEY ARE ALWAYS WRITTEN TOGETHER - CLAUDE.md's rule. These were two
 * out-parameters on FGearPerformance::FractionsAt and two loose doubles on FAgentMotion until
 * truck tilt made a third, at which point the copying was happening at four sites and a
 * caller that filled two of three would have left the third at whatever the last call put
 * there. They are one table with one row.
 *
 * EVERY FRACTION IS NAMED FOR ITS REST STATE AND IS 1.0 THERE. That is not a coincidence, it
 * is the convention: together these three ones ARE a parked aeroplane - gear down, bay hanging
 * open, truck sitting level on the tarmac - and a parked aeroplane is the BIND POSE of every
 * rig in this fleet. So the default-constructed pose asks the animgraph to rotate nothing,
 * which is the only safe answer for an airframe that declared no gear at all, and the angle
 * for each is (1 - fraction) x that rig's measured travel. See
 * UAirsideAgentAnim::AngleFromRestFraction, which is that one line for all three.
 *
 * A FRACTION AND NOT AN ANGLE, in all three cases, because a travel angle is a fact about one
 * RIG - 90 degrees on plane4, measured in its build_export.py - and the model has no business
 * knowing it. UAirsideAgentAnim multiplies by its own measured figures, the same split
 * MainWheelRadius already makes.
 */
USTRUCT()
struct AIRSIDE_API FGearPose
{
	GENERATED_BODY()

	/**
	 * Where the landing gear is: 1 down and locked, 0 stowed. See FGearPerformance.
	 *
	 * ONE DEFAULTS TO DOWN, deliberately: an airframe with no gear data, a vehicle, and every
	 * aircraft on the ground all want the same answer, and it is this one.
	 */
	UPROPERTY() double GearDownFraction = 1.0;

	/**
	 * The gear bay doors: 1 fully open, 0 shut.
	 *
	 * ONE DEFAULTS TO OPEN, which pairs with GearDownFraction's 1: together they are a parked
	 * aeroplane, gear down and bay hanging open, which is what a 737's linked nose doors
	 * actually do and what SK_Plane4's bind pose already is. An airframe with no doors, and
	 * every ground vehicle, also want this value - it is the one that asks the animgraph to
	 * rotate nothing.
	 */
	UPROPERTY() double BayDoorOpenFraction = 1.0;

	/**
	 * The main gear bogie beam: 1 level, 0 fully tilted. Boeing call it TRUCK TILT.
	 *
	 * WHAT IT IS. A wide-body main leg does not carry one axle, it carries a four- or
	 * six-wheel TRUCK on a beam that pivots about the bottom of the oleo. The truck lies level
	 * on the tarmac because the tarmac is flat, and it has to be swung to a particular angle
	 * before it will pass into the wheel well, which is not deep enough to take a four-metre
	 * bogie lying flat. So the tilt is a SECOND BONE on the same leg, between the retract bone
	 * and the rolling ones.
	 *
	 * NOTHING IN THE FLEET DRIVES IT YET, AND THAT IS NOT AN OVERSIGHT. This was built for
	 * plane6's 777 on 2026-09-21 and plane6 then shipped WITHOUT truck bones - its gear reads
	 * well enough at ramp distance without one. The A380 (plane8, concept sheet already in the
	 * models repo) is the aeroplane that will need it: a four-wheel wing bogie and a six-wheel
	 * body bogie, both of which tilt, and neither of which will fit its bay lying flat.
	 *
	 * SO THIS IS DELIBERATELY THE HALF NOBODY CAN SEE, the same bet FGearPerformance's whole
	 * extend cycle is - "the whole argument for building the half nobody can see", in
	 * ExtendBelowHeight's own words. It costs one fraction that never leaves 1.0 and one
	 * angle that is always zero. What keeps it from rotting is that the tests below exercise
	 * it against an airframe that DOES declare a truck, so the arithmetic is measured even
	 * though no rig reaches it.
	 *
	 * ONE DEFAULTS TO LEVEL, which is the third of the three ones described above, and which
	 * is also every aeroplane in this fleet today: a single-axle main gear, or a bogie nobody
	 * rigged a bone for, has no truck to tilt, so the fraction that rotates nothing is the
	 * right answer for it. See FGearPerformance::TruckTiltSeconds, where zero means exactly
	 * that.
	 *
	 * CYCLE-DRIVEN, NOT WEIGHT-ON-WHEELS. A real truck also hangs tilted whenever the leg is
	 * extended and unloaded, and levels itself with an audible thump as the aeroplane settles
	 * on it; that is a fourth thing to model and it is deliberately NOT modelled here. Gear
	 * down and locked means level at any altitude. Ruled 2026-09-21 when the feature was
	 * scoped: the touchdown rock is worth having, it is worth having on its own terms with its
	 * own cue, and bolting it onto a retraction timeline would have meant one fraction
	 * answering to two unrelated questions.
	 */
	UPROPERTY() double TruckLevelFraction = 1.0;
};
