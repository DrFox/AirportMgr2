#pragma once

#include "CoreMinimal.h"
#include "Containers/StaticArray.h"
#include "Templates/Function.h"
#include "Build/AnchorLink.h"
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
 * Which dynamic-mesh component a built surface belongs to. Replaces five near-identical
 * CreateDefaultSubobject blocks on the actor and five near-identical Rebuild* bodies here
 * with one indexed table each - issue #81.
 *
 * ROAD AND GHOST ARE NOT REBUILT BY RebuildLayer below: the road pipeline runs a whole
 * FRoadMeshBuilder plus the effective material set (see EffectiveMaterialSet), and the ghost
 * is solved against a DUPLICATED, hypothetical network rather than the live one (see
 * BuildGhostBuffers) - neither is "fill one FRoadMeshBuffers from the live Network and sink
 * it", which is all Apron/HoldingPaint/RunwayPaint ever were. Both stay enumerated here
 * anyway because Initialize hands over all five components in one indexed call.
 */
enum class ESurfaceLayer : uint8
{
	Road,
	Ghost,
	Apron,
	HoldingPaint,
	RunwayPaint,
	Count
};

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

		/**
		 * How far a SERVICE connection may reach, in any direction, uu.
		 *
		 * The one link figure that comes down from the LEVEL - see
		 * ARoadNetworkActor::ServiceLinkRadius. The aircraft cap does not: 200 m is a fact
		 * about a painted lead-in, not per-airport tuning.
		 */
		double ServiceLinkRadius = FAnchorLink::DefaultServiceLinkRadius;

		/** Already resolved - see ARoadNetworkActor::ResolveSurfaceMaterial and its siblings. */
		UMaterialInterface* SurfaceMaterial = nullptr;
		UMaterialInterface* ApronMaterial = nullptr;
		UMaterialInterface* GhostMaterial = nullptr;
		URoadMaterialSet* MaterialSet = nullptr;

		/**
		 * Already resolved - see ARoadNetworkActor::ResolveRunwayMaterial. What a runway's
		 * bands are skinned with, by surface; null falls back to SurfaceMaterial. Three
		 * pointers rather than a set, because MaterialSet may legitimately be null (the
		 * single-material road) and these must still reach the mesh - see EffectiveMaterialSet.
		 */
		UMaterialInterface* RunwayGrassMaterial = nullptr;
		UMaterialInterface* RunwayTarmacMaterial = nullptr;
		UMaterialInterface* RunwayConcreteMaterial = nullptr;

		/** Already resolved - see ARoadNetworkActor::ResolveProfile. */
		URoadProfile* Profile = nullptr;
	};

	/**
	 * Non-owning: every component is a CreateDefaultSubobject of the actor that also creates
	 * this presenter, and none of their lifetimes are this class's to manage.
	 *
	 * ONE ARGUMENT INDEXED BY ESurfaceLayer, not five positional ones (two defaulted): the
	 * old signature grew a parameter every time a layer was added, with nothing to stop a
	 * caller passing RunwayMarking where HoldingPosition belonged. TStaticArray at this
	 * boundary because the handoff is fixed-size and known at compile time; stored internally
	 * as a TArray (LayerComponents) because UPROPERTY reflection - which is what lets the
	 * garbage collector trace these non-owning pointers - has no TStaticArray support.
	 */
	void Initialize(const TStaticArray<TObjectPtr<UDynamicMeshComponent>, static_cast<int32>(ESurfaceLayer::Count)>& Components);

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

	/** Triangles currently in the runway paint, for Airside.Present.RunwayMarkingsDrawn. */
	int32 RunwayMarkingTriangleCountForTest() const;

	/** The material set the last Rebuild handed the mesh, for tests: see EffectiveMaterialSet. */
	const URoadMaterialSet* EffectiveMaterialSetForTest() const { return EffectiveSet; }

private:
	/** LayerComponents[Layer], or null if Layer has none - see Initialize and
	 *  LayerComponents' own comment for why that is a supported state. */
	UDynamicMeshComponent* GetLayerComponent(ESurfaceLayer Layer) const;

	/**
	 * The shape RebuildAprons/RebuildMarkings/RebuildRunwayMarkings all repeated before issue
	 * #81: null-check Layer's component, fill an FRoadMeshBuffers with BuildFn, sink it with
	 * Material at bUseConstantColour, and show the component only if anything was built.
	 * Returns what BuildFn reported, or INDEX_NONE if Layer has no component - in which case
	 * BuildFn is never even called, so a caller checking for INDEX_NONE can return early
	 * exactly as it did when its own null check opened the function.
	 *
	 * DOES NOT LOG. Each caller's own summary line differs too much to fold in here (an
	 * apron's material name, a runway's whole marking-type census) - see each Rebuild*'s own
	 * UE_LOG for what that layer reports, and OutBuffers exists so it can.
	 */
	int32 RebuildLayer(ESurfaceLayer Layer, TFunctionRef<int32(FRoadMeshBuffers&)> BuildFn,
		UMaterialInterface* Material, bool bUseConstantColour, FRoadMeshBuffers& OutBuffers);

	/** Half a unit above the road, so paint wins the depth test against the pavement it lies
	 *  on - shared by RebuildMarkings and RebuildRunwayMarkings, which used to compute this
	 *  identically and separately (issue #81). */
	double GetMarkingZ(double SurfaceZ) const { return SurfaceZ + 0.5; }

	/**
	 * Every triangle in Buffers, as debug lines - the same ground truth RebuildAprons and
	 * Rebuild both give their surface: these are the buffers the component was actually
	 * handed, reaching the screen by a completely separate route. Was copied at both call
	 * sites (Cyan/12 for aprons, Green/8 for the road) before issue #81.
	 */
	void DebugDrawTriangles(const FRoadMeshBuffers& Buffers, FColor Colour, float Thickness, double Seconds) const;

	/** Separate from the roads, which share nothing with it - see AddApron's own comment. */
	void RebuildAprons(URoadNetwork& Network, const FSurfaceSettings& Settings);

	/** The holding-position paint, from the guideline graph the same Rebuild just derived. */
	void RebuildMarkings(URoadNetwork& Network, const FSurfaceSettings& Settings);

	/**
	 * The runway paint - FRunwayMarkingBuilder's quads on their own component, drawn white.
	 *
	 * A SECOND marking component rather than more quads on the holding-position one,
	 * because the two are painted different colours by the same material trick: UV1 = 0
	 * paints a quad solid MarkingColor, and MarkingColor is a parameter of the material
	 * instance, so two colours need two instances and therefore two components. No new
	 * material asset either way.
	 */
	void RebuildRunwayMarkings(URoadNetwork& Network, const FSurfaceSettings& Settings);

	/**
	 * The material set the mesh is actually built and skinned with: the authored set's
	 * slots (or, with none, one slot carrying SurfaceMaterial) FOLLOWED BY the three
	 * runway surface slots.
	 *
	 * Composed here, per rebuild, into a transient set this presenter owns - never written
	 * to the actor's MaterialSet, whose null is a deliberate state (see
	 * ARoadNetworkActor::ResolveMaterialSet). The authored slots come first and in their
	 * own order, so every material id a band resolved through the authored set is still
	 * the same id in this one: the authored set is a PREFIX, which is what lets the
	 * builder resolve band names against this set and see exactly what it saw before.
	 */
	const URoadMaterialSet* EffectiveMaterialSet(const FSurfaceSettings& Settings);

	/** The runway paint's material instance: SurfaceMaterial with MarkingColor white. Cached like GhostMID. */
	UMaterialInstanceDynamic* RunwayMarkingMaterialInstance(UMaterialInterface* SurfaceMaterialBase);

	/** Append a solved node's fan to Builder, if that node solved at all. */
	void AddGhostJunction(FRoadMeshBuilder& Builder, const FRoadSolveResult& Solved, int32 NodeIndex) const;

	/** The ghost's material instance, made on first use. Null if GhostMaterialBase is unset. */
	UMaterialInstanceDynamic* GhostMaterialInstance(UMaterialInterface* GhostMaterialBase);

	/**
	 * Non-owning: one dynamic mesh per ESurfaceLayer - see Initialize. Sized to
	 * ESurfaceLayer::Count once Initialize has run; empty (every GetLayerComponent null)
	 * before that, same as a default-constructed presenter always was.
	 *
	 * HoldingPaint/RunwayPaint may still be null even after Initialize, on an actor made
	 * before markings existed - RebuildMarkings/RebuildRunwayMarkings then does nothing,
	 * exactly as MarkingComponent/RunwayMarkingComponent being null used to mean.
	 *
	 * A TArray, not the TStaticArray Initialize takes: UPROPERTY reflection - which is what
	 * lets the garbage collector trace these pointers - has no TStaticArray support. Five
	 * separate named UPROPERTYs would GC-trace correctly too; the point of this table is that
	 * RebuildLayer can index it by ESurfaceLayer instead of one of five call sites addressing
	 * one of five identically-shaped fields by name.
	 */
	UPROPERTY() TArray<TObjectPtr<UDynamicMeshComponent>> LayerComponents;

	/** See EffectiveMaterialSet. Transient: composed from resolved settings on every rebuild. */
	UPROPERTY(Transient) TObjectPtr<URoadMaterialSet> EffectiveSet;

	/** See RunwayMarkingMaterialInstance. */
	UPROPERTY(Transient) TObjectPtr<UMaterialInstanceDynamic> RunwayMarkingMID;

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
