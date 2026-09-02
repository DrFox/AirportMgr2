#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RoadAgentActor.generated.h"

class UStaticMeshComponent;

/**
 * A cube that stands where an agent is. A VIEW, and nothing else.
 *
 * It holds no route, no speed and no follower: ARoadNetworkActor advances the model and
 * pushes a pose in. That split is what keeps the whole of "does it go the right way"
 * testable without a world - see FRouteFollower - and it is why this class has one method.
 *
 * Placeholder geometry on purpose. When a real airframe arrives it replaces the mesh here
 * and nothing else changes, because nothing else knows this is a cube.
 */
UCLASS()
class ROADNET_API ARoadAgentActor : public AActor
{
	GENERATED_BODY()

public:
	ARoadAgentActor();

	/** Position in road-plane XY, heading in radians, Z the road surface height. */
	void SetPose(const FVector2D& Position, double Heading, double SurfaceZ);

private:
	UPROPERTY() TObjectPtr<UStaticMeshComponent> Mesh;
};
