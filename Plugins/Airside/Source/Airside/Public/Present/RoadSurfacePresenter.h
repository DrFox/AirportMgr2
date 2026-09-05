#pragma once

#include "CoreMinimal.h"
#include "Build/RoadMeshSink.h"
#include "Model/RoadHandles.h"
#include "Tool/RoadSnap.h"
#include "RoadSurfacePresenter.generated.h"

class URoadNetwork;
class URoadProfile;
class URoadMaterialSet;
class UDynamicMeshComponent;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class FRoadMeshBuilder;
struct FRoadSolveResult;

/**
 * Everything the road network LOOKS like: the three dynamic-mesh surfaces (road, apron,
 * ghost) built from a URoadNetwork, and nothing about how that network came to be what it
 * is - split out of ARoadNetworkActor by issue #32.
 *
 * Pattern: Presenter (a Humble Object) - the graph solve, the mesh builder and the sink are
 * all straightforward to unit-test without a world (and already are), so the only thing
 * worth quarantining here is the part that is not: UDynamicMeshComponent, materials,
 * DrawDebugLine, all real engine objects a test cannot easily stand in for. Everything this
 * class does still runs the same solve/build/emit pipeline RebuildMesh always has; nothing
 * about the pipeline changed, only which object calls it.
 *
 * A UCLASS(UObject) rather than a plain C++ class because it owns TObjectPtr fields the
 * garbage collector must trace: GhostNetwork (a whole duplicated URoadNetwork) and GhostMID
 * (a UMaterialInstanceDynamic) are both real UObjects with real lifetime, and a raw member
 * pointer to either is exactly the "collected out from under it" bug this project's own
 * agent-view comment already warns about. Created with CreateDefaultSubobject on the actor
 * and held Transient - like the ghost fields it replaces, none of this is level content: the
 * live mesh components are what gets saved, this object is just what rebuilds them.
 *
 * Knows nothing about tools or agents: it cannot say what a click MEANS, and is not expected
 * to - only what the network looks like once something else has decided.
 */
UCLASS()
class AIRSIDE_API URoadSurfacePresenter : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * The knobs a rebuild needs, copied by value into every call rather than read through a
	 * pointer back to the actor.
	 *
	 * A struct copied per call, not a reference held between calls: a presenter that read
	 * ARoadNetworkActor properties on demand would still couple this class to the actor's
	 * layout, which is the exact coupling this split exists to remove. The resolved pointers
	 * (materials, the material set, the profile) are already the OUTPUT of the actor's own
	 * Resolve* functions - those stay on the actor because only it knows about content
	 * defaults (see ARoadNetworkActor::ResolveSurfaceMaterial and friends) - so this struct
	 * carries answers, never questions.
	 */
	struct FSurfaceSettings
	{
		double SurfaceZ = 10.0;
		double TexelsPerUnit = 512.0;
		int32 RibbonSegments = 1;
		double ApronZOffset = 4.0;
		double GhostZOffset = 2.0;

		bool bUseConstantVertexColour = false;
		bool bUseConstantApronColour = false;
		bool bDebugDrawMesh = false;
		bool bDebugDrawAprons = false;
		double DebugDrawSeconds = 30.0;

		/** Already resolved - see ARoadNetworkActor::ResolveSurfaceMaterial and its siblings. */
		UMaterialInterface* SurfaceMaterial = nullptr;
		UMaterialInterface* ApronMaterial = nullptr;
		UMaterialInterface* GhostMaterial = nullptr;
		URoadMaterialSet* MaterialSet = nullptr;

		/** Already resolved - see ARoadNetworkActor::ResolveProfile. */
		URoadProfile* Profile = nullptr;
	};

	/**
	 * Non-owning: all three components are CreateDefaultSubobjects of the actor that also
	 * creates this presenter, and none of their lifetimes are this class's to manage.
	 */
	void Initialize(UDynamicMeshComponent* InMeshComponent, UDynamicMeshComponent* InGhostComponent,
		UDynamicMeshComponent* InApronComponent);

	/** Solve every node, build the road and apron surfaces, and push them to their components. */
	void Rebuild(URoadNetwork& Network, const FSurfaceSettings& Settings);

	/**
	 * Forget what the ghost cache last showed, without touching the ghost component's
	 * visibility.
	 *
	 * Any real edit invalidates whatever the ghost was showing, and the cache key cannot
	 * see it: a click that splits a segment can leave the cursor and the start node
	 * exactly where they were, so every field the cache compares is unchanged while the
	 * graph underneath is not. Called unconditionally at the top of every rebuild - see
	 * Rebuild, which calls this itself, and ARoadNetworkActor::RebuildMesh, which must call
	 * it even on the one path that returns before there is a Network to rebuild from (a
	 * null Network cannot become the URoadNetwork& Rebuild takes, so that early return has
	 * to live on the actor - this is what lets it still invalidate the cache first).
	 */
	void InvalidateGhostCache();

	/**
	 * Height the apron surface is actually built at.
	 *
	 * ApronZOffset is a MAXIMUM, not a fixed drop: the apron never descends more than
	 * halfway from the road to the ground plane. A fixed drop silently assumes the road has
	 * headroom, and with SurfaceZ at 1 a 4 uu drop put the concrete at Z = -3 - rendering
	 * correctly, normals up, material bound, and buried under the ground where nothing
	 * about it looked wrong.
	 *
	 * Halfway rather than clamped at zero because zero is where the ground is: an apron
	 * pinned exactly to it would z-fight with the terrain instead of vanishing under it,
	 * which trades one silent failure for another.
	 *
	 * The single place this is computed - see ARoadNetworkActor::ApronZOffset and
	 * ::GetApronSurfaceZ, which both point here rather than repeating it. Public and shared
	 * so the mesh, the log and the tests cannot each compute it their own way and disagree.
	 */
	double GetApronSurfaceZ(double SurfaceZ, double ApronZOffset) const;

	/**
	 * True if the ghost already shows FromNodeIndex/Snap and UpdateGhost would therefore
	 * skip rebuilding it - the "a drag holds still for most frames" case (see the cache
	 * fields below). OutValidityChanged reports whether bValid differs from what the
	 * ghost's material is currently blending toward. False (never a hit) whenever
	 * FromNodeIndex is not currently a live node - the same guard UpdateGhost itself
	 * opens with, so a caller that gets false here and calls UpdateGhost sees identical
	 * behaviour to before this query existed.
	 *
	 * Exists so the CALLER can skip its own work on a cache hit too: ARoadNetworkActor's
	 * Resolve* functions are a content lookup plus a LoadSynchronous each, not free, and
	 * UpdateGhost used to pay for all of them every frame of a still drag before this
	 * query let it ask first. A const query, so it is safe to call speculatively.
	 */
	bool IsGhostCacheHit(const URoadNetwork* Network, int32 FromNodeIndex, const FRoadSnapResult& Snap,
		bool bValid, bool& bOutValidityChanged) const;

	/**
	 * The cache-hit path: update only the ghost material's ValidityBlend parameter.
	 *
	 * Geometry untouched: this is the whole reason validity is a material parameter rather
	 * than a second mesh. GhostMaterialBase is the one resolver this path still needs -
	 * see IsGhostCacheHit's own comment for why the other four are skipped entirely here.
	 */
	void SetGhostValidity(bool bValid, UMaterialInterface* GhostMaterialBase);

	/**
	 * Show the segment a click would build, as real solved pavement.
	 *
	 * Built on a DUPLICATE of Network, never the live one - FRoadNetworkSolver::SolveAll
	 * writes trim distances and cut vertices INTO whatever it is handed, so solving a
	 * hypothetical segment against the real graph would leave the real road's stored
	 * geometry describing a road nobody built. Call only on an IsGhostCacheHit miss - see
	 * its comment - since this always does the full rebuild.
	 */
	void UpdateGhost(URoadNetwork* Network, int32 FromNodeIndex, const FRoadSnapResult& Snap,
		bool bValid, const FSurfaceSettings& Settings);

	/**
	 * The ghost's triangles, without touching a component, a material or a renderer.
	 *
	 * Public and separated from UpdateGhost so the one property this whole mechanism rests
	 * on can be asserted in a test with no World: building a preview must leave the REAL
	 * network bitwise unchanged.
	 */
	bool BuildGhostBuffers(URoadNetwork* Network, int32 FromNodeIndex, const FRoadSnapResult& Snap,
		const FSurfaceSettings& Settings, FRoadMeshBuffers& OutBuffers);

	/** Hide the preview and forget what it was showing. */
	void HideGhost();

	/** Triangles currently in the road surface, for Airside.Present.MeshIsFreshAfterLoad. */
	int32 SurfaceTriangleCountForTest() const;

private:
	/** Separate from the roads, which share nothing with it - see AddApron's own comment. */
	void RebuildAprons(URoadNetwork& Network, const FSurfaceSettings& Settings);

	/** Append a solved node's fan to Builder, if that node solved at all. */
	void AddGhostJunction(FRoadMeshBuilder& Builder, const FRoadSolveResult& Solved, int32 NodeIndex) const;

	/** The ghost's material instance, made on first use. Null if GhostMaterialBase is unset. */
	UMaterialInstanceDynamic* GhostMaterialInstance(UMaterialInterface* GhostMaterialBase);

	/** Non-owning: the three components this presenter draws into. See Initialize. */
	UPROPERTY() TObjectPtr<UDynamicMeshComponent> MeshComponent;
	UPROPERTY() TObjectPtr<UDynamicMeshComponent> GhostComponent;
	UPROPERTY() TObjectPtr<UDynamicMeshComponent> ApronComponent;

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
};
