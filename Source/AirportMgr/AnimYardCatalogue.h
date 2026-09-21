#pragma once

#include "CoreMinimal.h"

struct FYardRig;
class USkeletalMesh;

/**
 * Which Animation Blueprint drives which mesh, answered from the content the GAME uses.
 *
 * NO TABLE OF ITS OWN, deliberately. Every aircraft's mesh and Animation Blueprint are already
 * declared together on its UAircraftType, and the one rigged ground vehicle's on
 * UAirsideContent; a list here pairing them again would be a second source of truth for a fact
 * the content already states, and Content/'s rule - one resolver per default - is the same
 * rule. The consequence is worth having: author DA_Aircraft_Plane8 and the bench shows it,
 * with no code change and no list to remember to edit.
 *
 * A MESH WITH NO ANSWER IS NOT AN ERROR. Three of the four ground vehicles have no Animation
 * Blueprint at all today - SK_GPU1, SK_Tug1 and SK_Utility1 are rigged in the .glb but nothing
 * in UAirsideAgentAnim produces what their beacons and booms would want. They stand still in
 * the yard, labelled, which is the honest report.
 */
namespace AnimYardCatalogue
{
	/**
	 * The rig for this mesh, or false if the project's content names none.
	 *
	 * SCANS THE ASSET REGISTRY for UAircraftType, rather than walking an airline's Fleet: a
	 * type belongs in the yard because it EXISTS, not because someone has scheduled flights
	 * with it, and DA_Aircraft_A320 and DA_Aircraft_B738 are in the project without being in
	 * Cumbria's fleet.
	 */
	bool FindRigFor(USkeletalMesh* Mesh, FYardRig& OutRig);
}
