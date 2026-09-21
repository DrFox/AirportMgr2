#pragma once

#include "CoreMinimal.h"
#include "AnimYard.h"

class USkeletalMesh;

/** One drivable mesh and the rig that drives it, with a name to report it by. */
struct FYardRigEntry
{
	TObjectPtr<USkeletalMesh> Mesh;
	FYardRig Rig;

	/** The asset that declared the pairing - a UAircraftType, or the content asset. */
	FString DeclaredBy;

	/**
	 * DeclaredBy NAMED this mesh, rather than resolving to it through a game-wide fallback.
	 *
	 * The tie-break when two types answer for one mesh - see EveryRig, which records the
	 * A320 that claimed the Meridian's model by declaring no model of its own.
	 */
	bool bMeshDeclaredByItsType = false;
};

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

	/**
	 * Every mesh the project can drive, and what drives it.
	 *
	 * THE ENUMERATION FindRigFor IS BUILT ON, rather than a second walk of the same assets.
	 * It exists as its own function because a test wants the WHOLE list - see
	 * AirportMgr.View.AnimYard.EveryRigDrivesItsBones, which ticks each rig and checks the
	 * Animation Blueprint actually moves something. A test that enumerated the content for
	 * itself would be a second catalogue, and the one it disagreed with would be the one
	 * carrying the rig nobody had checked.
	 */
	void EveryRig(TArray<FYardRigEntry>& Out);
}
