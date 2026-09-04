#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RoadAgentActor.generated.h"

class UStaticMesh;
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
	 * Position in road-plane XY, heading in radians, Z the road surface height.
	 *
	 * Altitude is ABOVE that surface and pitch is nose-up in degrees, both zero for anything
	 * on the ground - which is every caller until a departure rotates. Defaulted rather than
	 * added to every call site, because "on the wheels" is the overwhelmingly common case and
	 * an explicit zero at each taxi call would be noise.
	 */
	void SetPose(const FVector2D& Position, double Heading, double SurfaceZ,
		double Altitude = 0.0, double PitchDegrees = 0.0);

	/**
	 * Dresses this agent in an airframe. Null leaves the placeholder cube standing.
	 *
	 * PUSHED IN by whoever spawned it, exactly as the pose is. This class held the mesh's
	 * path itself until a content move proved that a path in C++ is a reference the editor
	 * cannot maintain - and a view that fetches its own assets is doing a second job anyway.
	 */
	void SetAirframe(UStaticMesh* Airframe);

private:
	UPROPERTY() TObjectPtr<UStaticMeshComponent> Mesh;

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
};
