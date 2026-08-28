#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Build/RoadMeshSink.h"
#include "RoadNetworkActor.generated.h"

class URoadNetwork;
class UDynamicMeshComponent;

/** Pushes finished buffers into a UDynamicMeshComponent. */
class ROADNET_API FDynamicMeshSink : public IRoadMeshSink
{
public:
	explicit FDynamicMeshSink(UDynamicMeshComponent* InComponent) : Component(InComponent) {}
	virtual void Accept(const FRoadMeshBuffers& Buffers) override;

private:
	// A raw, non-owning pointer: the sink does not own or GC-protect the component and
	// must not outlive it. Both current call sites are stack-scoped inside a single
	// function, so this is safe today; Slice 2b's preview sink will not be.
	UDynamicMeshComponent* Component = nullptr;
};

/** Owns a road network and renders it as one batched dynamic mesh. */
UCLASS()
class ROADNET_API ARoadNetworkActor : public AActor
{
	GENERATED_BODY()

public:
	ARoadNetworkActor();

	/** Solve every node, build the mesh, and push it to the component. */
	UFUNCTION(CallInEditor, Category = "RoadNet")
	void RebuildMesh();

	UPROPERTY(VisibleAnywhere, Category = "RoadNet")
	TObjectPtr<UDynamicMeshComponent> MeshComponent;

	UPROPERTY() TObjectPtr<URoadNetwork> Network;

	/** Absolute world-space Z of the road surface, in uu. Not relative to the actor:
	 *  the mesh builder emits world-space XY at this Z, and MeshComponent is set to use
	 *  absolute location/rotation/scale (see the constructor) so those coordinates are
	 *  not transformed again by the actor's own placement. */
	UPROPERTY(EditAnywhere, Category = "RoadNet") double SurfaceZ = 10.0;

	/** Quads along each segment. 1 is right for straight segments. */
	UPROPERTY(EditAnywhere, Category = "RoadNet", meta = (ClampMin = "1")) int32 RibbonSegments = 1;
};
