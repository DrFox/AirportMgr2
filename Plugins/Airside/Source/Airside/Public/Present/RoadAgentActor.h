#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Model/RoadEntity.h"
#include "RoadAgentActor.generated.h"

class UAnimInstance;
class USkeletalMesh;
class USkeletalMeshComponent;
class UStaticMeshComponent;

/**
 * The aircraft that stands where an agent is. A VIEW, and nothing else.
 *
 * It holds no route, no speed and no follower: ARoadNetworkActor advances the model and
 * pushes a pose in. That split is what keeps the whole of "does it go the right way"
 * testable without a world - see FRouteFollower - and it is why this class has one method.
 *
 * It was a placeholder cube, and the airframe replaced the mesh here with nothing else
 * changing, exactly as that comment promised - because nothing else knew it was a cube.
 * The cube survives as the fallback when the airframe asset is missing.
 */
UCLASS()
class AIRSIDE_API ARoadAgentActor : public AActor
{
	GENERATED_BODY()

public:
	ARoadAgentActor();

	/**
	 * Everything about where this agent is and what it is doing, in one call.
	 *
	 * SurfaceZ stays a separate argument because it belongs to the AIRPORT rather than to the
	 * agent - every aircraft on a flat airfield shares it, and folding it into the motion
	 * would have each agent carrying its own copy of one number.
	 */
	void SetMotion(const FAgentMotion& Motion, double SurfaceZ);

	/**
	 * What the model last said this agent was doing. Read by UAirsideAgentAnim.
	 *
	 * The view keeps it and the animation reads it, rather than the animation reaching into
	 * the model: an AnimInstance that knew about followers and departures would be a second
	 * consumer of the simulation, free to disagree with the one that draws the aircraft.
	 */
	const FAgentMotion& GetMotion() const { return LastMotion; }

	void SetAirframe(USkeletalMesh* InAirframe, UClass* AnimClass = nullptr);

private:
	/**
	 * The aircraft. SKELETAL, so the propeller and wheels can turn - see UAirsideAgentAnim.
	 *
	 * The root, so the placeholder can hang off it and be hidden rather than juggled.
	 */
	UPROPERTY() TObjectPtr<USkeletalMeshComponent> Airframe;

	/**
	 * The box that stands in when no airframe was assigned.
	 *
	 * A SEPARATE COMPONENT now, because a static mesh cannot live in a skeletal one. Kept
	 * rather than dropped for the reason it always was: a missing airframe should look like
	 * the box this used to be rather than like an agent that failed to spawn - one of those
	 * reads as a content problem and the other as a routing bug.
	 */
	UPROPERTY() TObjectPtr<UStaticMeshComponent> Placeholder;

	/**
	 * False when the airframe asset was missing and the cube stood in.
	 *
	 * SetPose needs it: the cube's pivot is at its centre and must be lifted, the airframe's
	 * is on the ground and must not be.
	 *
	 * NOT a UPROPERTY. It is decided by the constructor from what loaded, so it is a fact
	 * about construction rather than authored or saved state - and agents are transient and
	 * never serialised anyway.
	 */
	bool bHasAirframe = false;

	/**
	 * What the model last said this agent was doing.
	 *
	 * Not read by anything yet: the mesh is still a static one and cannot animate. It is here
	 * because it is what the AnimInstance will read, and keeping it means the model half of
	 * the animation is complete and can be got right before the rigged asset arrives.
	 */
	FAgentMotion LastMotion;
};
