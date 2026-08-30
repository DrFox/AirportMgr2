#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Build/RoadMeshSink.h"
#include "Model/RoadHandles.h"
#include "Tool/RoadSnap.h"
#include "RoadNetworkActor.generated.h"

class URoadNetwork;
class URoadProfile;
class UDynamicMeshComponent;
class UMaterialInstanceDynamic;
class FRoadMeshBuilder;
struct FRoadSolveResult;
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

	/**
	 * Replace a live segment with two, meeting at a new node placed at At.
	 *
	 * Returns the new node's index, or INDEX_NONE if it refused. This is what makes a
	 * T-junction authorable: without it a junction can only ever form where a node was
	 * already placed, so running a taxiway into a road you have already drawn is
	 * impossible.
	 *
	 * The original segment's handle is DEAD afterwards - the segment is removed, not
	 * reshaped, because its endpoints define its identity and both of them change.
	 * Anything holding that handle must re-resolve. Both replacements inherit the
	 * original's profile.
	 *
	 * At is taken as given rather than projected onto the segment: the snap chain has
	 * already found the point, and re-deriving it here would let the two disagree about
	 * where the split is. A caller passing a point off the segment gets a kink, which is
	 * a caller error and not something to silently correct.
	 */
	UFUNCTION(BlueprintCallable, Category = "RoadNet")
	int32 SplitSegment(int32 SegmentIndex, FVector2D At);

	/** Discard the whole graph and the mesh built from it. */
	UFUNCTION(BlueprintCallable, Category = "RoadNet")
	void ClearNetwork();

	// --- Ghost preview --------------------------------------------------------------

	/**
	 * Show the segment a click would build, as real solved pavement.
	 *
	 * Built on a DUPLICATE of the network, never the live one. FRoadNetworkSolver::SolveAll
	 * takes a non-const network and writes trim distances and cut vertices INTO it, so
	 * solving a hypothetical segment against the real graph would leave the real road's
	 * stored geometry describing a road nobody built - and it would only show once
	 * something forced a rebuild.
	 *
	 * A Segment snap is split on the copy for real, because a split turns one road into a
	 * three-arm junction and nothing short of performing it shows that junction's shape.
	 *
	 * bValid drives the material's ValidityBlend only. Validity is a parameter rather than
	 * a mesh variant, so turning a drag red regenerates no geometry at all.
	 */
	void UpdateGhost(int32 FromNodeIndex, const FRoadSnapResult& Snap, bool bValid);

	/**
	 * The ghost's triangles, without touching a component, a material or a renderer.
	 *
	 * Public and separated from UpdateGhost so the one property this whole mechanism
	 * rests on can be asserted in a test with no World: building a preview must leave the
	 * REAL network bitwise unchanged. That failure is otherwise invisible - the ghost
	 * looks right either way, and the damage only surfaces later as a road whose stored
	 * cut vertices describe a segment nobody built.
	 */
	bool BuildGhostBuffers(int32 FromNodeIndex, const FRoadSnapResult& Snap, FRoadMeshBuffers& OutBuffers);

	/** Hide the preview and forget what it was showing. */
	void HideGhost();

	/** A live node's handle from its slot index, or false if it is not live. */
	bool MakeLiveNodeId(int32 Index, FRoadNodeId& OutId) const;

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
	 * DIAGNOSTIC ONLY. Hold vertex colours at a constant - and, as a side effect nobody
	 * would guess, stop the real material rendering at all.
	 *
	 * Any ColorOverrideMode other than None makes FBaseDynamicMeshSceneProxy set
	 * ForceOverrideMaterial to the engine's vertex-colour debug material, which then
	 * replaces SurfaceMaterial for every buffer set. So this does not tint the surface;
	 * it substitutes a different material entirely and shows a flat constant colour with
	 * no texture, whatever SurfaceMaterial says.
	 *
	 * Default false, because true means "do not render the material you asked for". It
	 * stays available because it is a genuine way to prove geometry reaches the screen
	 * when the material is suspect - just never mistake the result for the material
	 * working.
	 */
	UPROPERTY(EditAnywhere, Category = "RoadNet")
	bool bUseConstantVertexColour = false;

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

	/** Second component, carrying only the preview. Separate so showing and hiding the
	 *  ghost never touches the real road's mesh. */
	UPROPERTY(VisibleAnywhere, Category = "RoadNet|Ghost")
	TObjectPtr<UDynamicMeshComponent> GhostComponent;

	/** Translucent unlit preview material. Defaults to M_RoadGhost. */
	UPROPERTY(EditAnywhere, Category = "RoadNet|Ghost")
	TObjectPtr<UMaterialInterface> GhostMaterial;

	/**
	 * How far above the road surface the ghost sits, in uu.
	 *
	 * Enough to clear the pavement's depth, little enough that it still reads as lying on
	 * it. At zero the two surfaces z-fight; the preview then flickers rather than hovers.
	 */
	UPROPERTY(EditAnywhere, Category = "RoadNet|Ghost", meta = (ClampMin = "0.0"))
	double GhostZOffset = 2.0;

	UPROPERTY() TObjectPtr<URoadNetwork> Network;

private:
	/** Profile made on demand when none is authored. Transient so it is never saved. */
	UPROPERTY(Transient) TObjectPtr<URoadProfile> RuntimeProfile;

	/** The hypothetical graph the ghost is solved against. Rebuilt whenever the drag moves. */
	UPROPERTY(Transient) TObjectPtr<URoadNetwork> GhostNetwork;

	UPROPERTY(Transient) TObjectPtr<UMaterialInstanceDynamic> GhostMID;

	// What the ghost currently shows. A drag holds still for most frames, and rebuilding
	// an unchanged preview means duplicating the network and re-solving it every frame for
	// an identical result.
	int32 LastGhostFrom = INDEX_NONE;
	FVector2D LastGhostTo = FVector2D::ZeroVector;
	ERoadSnapKind LastGhostKind = ERoadSnapKind::Free;
	bool bLastGhostValid = true;
	bool bGhostVisible = false;

	/** The authored profile if there is one, otherwise the on-demand fallback. */
	URoadProfile* ResolveProfile();

	/** Network, creating it on first use. Nothing else in the project makes one yet. */
	URoadNetwork& EnsureNetwork();

	/** A live segment's handle from its slot index. See MakeLiveNodeId. */
	bool MakeLiveSegmentId(int32 Index, FRoadSegmentId& OutId) const;

	/**
	 * The split surgery itself, against any network.
	 *
	 * Shared by the real edit and the ghost deliberately. Two implementations of the same
	 * surgery is precisely how a preview comes to show something the click will not do,
	 * and that failure is invisible - the ghost looks plausible either way.
	 */
	static FRoadNodeId SplitSegmentIn(URoadNetwork& Net, FRoadSegmentId Doomed, const FVector2D& At);

	/** Append a solved node's fan to Builder, if that node solved at all. */
	void AddGhostJunction(FRoadMeshBuilder& Builder, const FRoadSolveResult& Solved, int32 NodeIndex) const;

	/** The ghost's material instance, made on first use. Null if GhostMaterial is unset. */
	UMaterialInstanceDynamic* GhostMaterialInstance();

public:

	/** Absolute world-space Z of the road surface, in uu. Not relative to the actor:
	 *  the mesh builder emits world-space XY at this Z, and MeshComponent is set to use
	 *  absolute location/rotation/scale (see the constructor) so those coordinates are
	 *  not transformed again by the actor's own placement. */
	UPROPERTY(EditAnywhere, Category = "RoadNet") double SurfaceZ = 10.0;

	/** Quads along each segment. 1 is right for straight segments. */
	UPROPERTY(EditAnywhere, Category = "RoadNet", meta = (ClampMin = "1")) int32 RibbonSegments = 1;

	/**
	 * World units per texture tile for the asphalt. Lower means the texture repeats more
	 * often, so more visible grain across the road.
	 *
	 * At the default 512 a 200 uu road shows less than half of one tile across its whole
	 * width, which magnifies the texture until it reads as flat colour. Roughly a fifth
	 * of the road's width is a sane starting point.
	 */
	UPROPERTY(EditAnywhere, Category = "RoadNet", meta = (ClampMin = "1.0")) double TexelsPerUnit = 512.0;

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
