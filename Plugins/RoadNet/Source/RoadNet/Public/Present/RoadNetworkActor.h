#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Build/RoadMeshSink.h"
#include "Model/RoadHandles.h"
#include "RoadNetworkActor.generated.h"

class URoadNetwork;
class URoadProfile;
class UDynamicMeshComponent;
class UMaterialInterface;

namespace UE::Geometry { class FDynamicMesh3; }

/** Pushes finished buffers into a UDynamicMeshComponent. */
class ROADNET_API FDynamicMeshSink : public IRoadMeshSink
{
public:
	explicit FDynamicMeshSink(UDynamicMeshComponent* InComponent, UMaterialInterface* InMaterial = nullptr,
		bool bInUseConstantVertexColour = true)
		: Component(InComponent), Material(InMaterial)
		, bUseConstantVertexColour(bInUseConstantVertexColour) {}
	virtual void Accept(const FRoadMeshBuffers& Buffers) override;

	/**
	 * Copy the buffers' UV and colour channels onto an already-populated mesh.
	 *
	 * Static and public so it can be tested without a component, a world or a renderer.
	 * The buffers being correct says nothing about what the component receives, and that
	 * gap is precisely where slice 2a's invisible surface hid.
	 */
	static void PopulateAttributes(UE::Geometry::FDynamicMesh3& Mesh, const FRoadMeshBuffers& Buffers);

private:
	// Raw, non-owning pointers: the sink owns and GC-protects neither, and must not
	// outlive either. Both current call sites are stack-scoped inside a single function,
	// so this is safe today; a preview sink that lives across frames will not be.
	UDynamicMeshComponent* Component = nullptr;
	UMaterialInterface* Material = nullptr;

	/** Independent of Material by design - see ARoadNetworkActor::bUseConstantVertexColour. */
	bool bUseConstantVertexColour = true;
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

	/**
	 * Material for the road surface. Defaults to M_RoadSurface, which reads UV0 for
	 * asphalt and UV1 for markings. Left null, the surface falls back to the engine
	 * default - which is WorldGridMaterial, the same world-aligned checker the template
	 * floor uses, so the road becomes very hard to tell apart from the ground.
	 */
	UPROPERTY(EditAnywhere, Category = "RoadNet")
	TObjectPtr<UMaterialInterface> SurfaceMaterial;

	/**
	 * Hold the component's vertex colours at a constant instead of reading the mesh.
	 *
	 * Deliberately independent of SurfaceMaterial. These two were previously decided
	 * together in one if/else, which meant clearing the material also flipped this - so
	 * "it renders without a material" moved two variables at once and could never say
	 * which of them mattered. Both are now properties, so all four combinations can be
	 * tried in the details panel without a rebuild.
	 */
	UPROPERTY(EditAnywhere, Category = "RoadNet")
	bool bUseConstantVertexColour = true;

	/**
	 * Deliberately far narrower than a real taxiway's 2300 uu. A corner needs roughly
	 * five times the road's width in segment length before its fillet has room to be a
	 * curve rather than a clamped-away stub, so this is sized for roads drawn by hand at
	 * a few thousand uu a click. Airport-realistic widths belong in a URoadProfile asset
	 * assigned to Profile above, where the segment lengths are planned rather than
	 * clicked.
	 */
	UPROPERTY(EditAnywhere, Category = "RoadNet", meta = (ClampMin = "1.0"))
	double FallbackWidth = 200.0;

	UPROPERTY(EditAnywhere, Category = "RoadNet", meta = (ClampMin = "0.0"))
	double FallbackFilletRadius = 100.0;

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

	/**
	 * Draw every triangle the builder produced as debug lines.
	 *
	 * Ground truth for "the mesh is correct but nothing renders": these come from the
	 * same buffers the component is handed, but reach the screen by a completely separate
	 * path, so whatever shows here is the geometry itself - independent of materials,
	 * bounds, clip planes and the scene proxy.
	 */
	UPROPERTY(EditAnywhere, Category = "RoadNet") bool bDebugDrawMesh = false;

	/** How long the debug wireframe survives, in seconds. */
	UPROPERTY(EditAnywhere, Category = "RoadNet", meta = (ClampMin = "0.0")) double DebugDrawSeconds = 30.0;
};
