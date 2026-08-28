#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Build/RoadMeshSink.h"
#include "Model/RoadHandles.h"
#include "RoadNetworkActor.generated.h"

class URoadNetwork;
class URoadProfile;
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

	// --- Runtime graph facade -------------------------------------------------------
	//
	// The player builds the network while the game runs, so these are the entry points
	// a build tool drives. They are deliberately on the actor rather than on
	// URoadNetwork: Model/RoadNetwork.h reserves the mutators for IRoadCommand from
	// Slice 3 onward, and when that lands these bodies start emitting commands while
	// every caller - Blueprint included - stays exactly as it is. Placing them here is
	// what keeps that swap from being a breaking change.
	//
	// Node identity crosses the boundary as a plain int32 slot index rather than
	// FRoadNodeId, because a USTRUCT handle does not round-trip cleanly through
	// Blueprint and the index is unambiguous within one network.

	/** Add a node at a world-space XY position. Returns its index, or INDEX_NONE. */
	UFUNCTION(BlueprintCallable, Category = "RoadNet")
	int32 PlaceNode(FVector2D Where);

	/** Join two placed nodes with a straight segment. Returns false, and logs, if it refused. */
	UFUNCTION(BlueprintCallable, Category = "RoadNet")
	bool ConnectNodes(int32 FromIndex, int32 ToIndex);

	/**
	 * Index of the nearest live node within Radius of Where, or INDEX_NONE.
	 *
	 * A crude stand-in for the snap chain of section 7.4. Something has to let a click
	 * reattach to an existing node, or every road drawn would be disconnected from the
	 * last and no junction could ever be authored.
	 */
	UFUNCTION(BlueprintCallable, Category = "RoadNet")
	int32 FindNodeNear(FVector2D Where, double Radius) const;

	/** Discard the whole graph and the mesh built from it. */
	UFUNCTION(BlueprintCallable, Category = "RoadNet")
	void ClearNetwork();

	/**
	 * Cross-section for segments created through this facade. When unset, a symmetric
	 * one from FallbackWidth and FallbackFilletRadius is made on demand - the solver
	 * cannot produce a boundary without half-widths, so there is no useful null case.
	 */
	UPROPERTY(EditAnywhere, Category = "RoadNet")
	TObjectPtr<URoadProfile> Profile;

	UPROPERTY(EditAnywhere, Category = "RoadNet", meta = (ClampMin = "1.0"))
	double FallbackWidth = 2300.0;

	UPROPERTY(EditAnywhere, Category = "RoadNet", meta = (ClampMin = "0.0"))
	double FallbackFilletRadius = 1500.0;

	UPROPERTY(VisibleAnywhere, Category = "RoadNet")
	TObjectPtr<UDynamicMeshComponent> MeshComponent;

	UPROPERTY() TObjectPtr<URoadNetwork> Network;

private:
	/** Profile made on demand when none is authored. Transient so it is never saved. */
	UPROPERTY(Transient) TObjectPtr<URoadProfile> RuntimeProfile;

	/** The authored profile if there is one, otherwise the on-demand fallback. */
	URoadProfile* ResolveProfile();

	/** Network, creating it on first use. Nothing else in the project makes one yet. */
	URoadNetwork& EnsureNetwork();

	/** A live node's handle from its slot index, or an unset handle if it is not live. */
	bool MakeLiveNodeId(int32 Index, FRoadNodeId& OutId) const;

public:

	/** Absolute world-space Z of the road surface, in uu. Not relative to the actor:
	 *  the mesh builder emits world-space XY at this Z, and MeshComponent is set to use
	 *  absolute location/rotation/scale (see the constructor) so those coordinates are
	 *  not transformed again by the actor's own placement. */
	UPROPERTY(EditAnywhere, Category = "RoadNet") double SurfaceZ = 10.0;

	/** Quads along each segment. 1 is right for straight segments. */
	UPROPERTY(EditAnywhere, Category = "RoadNet", meta = (ClampMin = "1")) int32 RibbonSegments = 1;
};
