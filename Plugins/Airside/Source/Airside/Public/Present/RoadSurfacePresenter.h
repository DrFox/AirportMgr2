#pragma once

#include "CoreMinimal.h"
#include "Containers/StaticArray.h"
#include "Templates/Function.h"
#include "Build/AnchorLink.h"
#include "Build/RoadMeshSink.h"
#include "Model/RoadHandles.h"
#include "Model/RunwayFacts.h"
#include "Tool/RoadEditTarget.h"
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
 * Which dynamic-mesh component a built surface belongs to. Replaces this presenter's five
 * separately named UPROPERTY component fields with one indexed table (LayerComponents), and
 * the three near-identical Rebuild* bodies that read them with one RebuildLayer - issue #81.
 * The actor's own five CreateDefaultSubobject blocks are unchanged; only how it hands those
 * components to Initialize changed, from five positional arguments to one array by this
 * index.
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
	RunwayRubber,
	Count
};

/**
 * Everything the road network LOOKS like: the five dynamic-mesh surfaces (road, ghost,
 * apron, holding-position paint, runway paint - see ESurfaceLayer) built from a
 * URoadNetwork, and nothing about how that network came to be what it is - split out of
 * ARoadNetworkActor by issue #32, back when there were three.
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
		/** Null is supported and means no rubber - see UAirsideContent::RubberMaterial. */
		UMaterialInterface* RubberMaterial = nullptr;
		URoadMaterialSet* MaterialSet = nullptr;

		/**
		 * Already resolved - see ARoadNetworkActor::ResolveRunwayMaterial. What a runway's
		 * bands are skinned with, indexed by RunwayMaterialSlot(Surface); null falls back to
		 * SurfaceMaterial. Raw pointers in an array rather than a set, because MaterialSet
		 * may legitimately be null (the single-material road) and these must still reach the
		 * mesh - see EffectiveMaterialSet. A plain C array, not TStaticArray: this struct is
		 * not UPROPERTY-reflected, so either works, and a fixed array needs no include.
		 *
		 * Sized off RunwayMaterialSlotCount, not a literal 3 - see the static_assert right
		 * below (PR #137 review): a change to that constant with nothing checking this array
		 * against it is exactly the second-truths bug this project keeps a rule against.
		 */
		UMaterialInterface* RunwayMaterials[RunwayMaterialSlotCount] = {};

		/** Already resolved - see ARoadNetworkActor::ResolveProfile. */
		URoadProfile* Profile = nullptr;

		/**
		 * True on a Geometry (drag-frame) rebuild - issue #178. RebuildSurfaceOnly runs this at
		 * up to 60fps while a node or apron corner is held, and every one of RebuildInternal's
		 * own census lines (RebuildAprons, RebuildRunwayMarkings, RebuildRunwayRubber,
		 * RoadRebuildCensus::Log) and FDynamicMeshSink::Accept's per-component diagnostics -
		 * including its O(V) BadNormals scan - cost the same whether or not LogRoadMesh's
		 * verbosity would have printed them. RebuildInternal is the ONLY place that ever sets
		 * this (from Kind, never from a caller): a caller that fills this struct without
		 * knowing the field exists gets false, and logs exactly as it always has. See
		 * RebuildSurfaceOnly's own comment for what a Geometry rebuild already skips upstream
		 * of this - the guideline graph, the anchors, the markings - and CLAUDE.md's
		 * "Diagnosing" section for why the census itself is never removed, only silenced here:
		 * a drag still ends in one Topology rebuild that logs it in full.
		 */
		bool bQuiet = false;
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
	 * Rebuild's SURFACE-ONLY half (issue #165): the same solve and the same road mesh, plus
	 * every layer that reads the live Network directly - aprons, runway paint, runway rubber
	 * - but NEITHER FRoadGuidelineBuilder::Build NOR FAnchorLink::Build, and consequently not
	 * RebuildMarkings either.
	 *
	 * WHY MARKINGS TOO: FHoldingPositionMarkingBuilder::Build reads
	 * Network.GetGuidelineNodes(), which is FRoadGuidelineBuilder::Build's OWN OUTPUT - so
	 * calling it here would paint holding bars at the guideline graph's last-derived (now
	 * stale) positions, not a cheaper answer, a WRONG one that happens to look plausible.
	 * Skipping it leaves the previous frame's bars on screen, unmoved, until Rebuild derives
	 * a fresh graph - the same staleness this presenter already accepts for the guideline
	 * graph itself (see CLAUDE.md's "guideline graph samples once"), extended to its one
	 * consumer, rather than a second, cheaper evaluator that could disagree with the first.
	 *
	 * FOR A CALLER THAT KNOWS NOTHING WAS ADDED, REMOVED, SPLIT, OR RECLASSIFIED - a MoveNode
	 * or MoveApronCorner drag frame - see ARoadNetworkActor::RebuildMeshForChange, the only
	 * caller. A drag ends by calling Rebuild (through a Topology notify), which re-derives
	 * the guideline graph and repaints the markings this skipped, exactly once.
	 */
	void RebuildSurfaceOnly(URoadNetwork& Network, const FSurfaceSettings& Settings);

	/**
	 * RebuildMarkings alone (issue #179, EChangeKind::Markings) - no solve, no road mesh, no
	 * aprons, no runway paint or rubber, and critically NEITHER FRoadGuidelineBuilder::Build
	 * NOR FAnchorLink::Build: SetIntermediateHoldingPosition flips a flag on a guideline node
	 * that already exists, so the graph RebuildMarkings reads is exactly as fresh as it was
	 * before the toggle. THE OPPOSITE PROBLEM FROM RebuildSurfaceOnly's own comment: that one
	 * skips the markings because the graph they would read is stale; this one is safe to
	 * paint from precisely because nothing here made it stale. Routing this flag through
	 * Rebuild (Topology) instead would re-derive the guideline graph unconditionally and
	 * reallocate every node in it - including the one whose flag had just been set - for an
	 * edit that changed no shape at all.
	 */
	void RebuildMarkingsOnly(URoadNetwork& Network, const FSurfaceSettings& Settings);

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
	 * Built on a COPY of Network (GhostNetwork), never the live one -
	 * FRoadNetworkSolver::SolveNodeInto writes trim distances and cut vertices INTO
	 * whatever it is handed, exactly as SolveAll does, so solving a hypothetical segment
	 * against the real graph would leave the real road's stored geometry describing a road
	 * nobody built. Call only on an IsGhostCacheHit miss - see its comment - since this
	 * always does the full rebuild.
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

	/** Triangles currently in the tyre rubber, for Airside.Present.RunwayRubberDrawn. */
	int32 RunwayRubberTriangleCountForTest() const;

	/** Triangles currently in the holding-position paint, for
	 *  Airside.Present.HoldingPositionMeshFollowsToggle (issue #179) - the same shape as
	 *  the two triangle counts above, added for the same reason: a test that reads the
	 *  layer itself rather than trusting the builder was asked to run. */
	int32 HoldingPaintTriangleCountForTest() const;

	/** The material set the last Rebuild handed the mesh, for tests: see EffectiveMaterialSet. */
	const URoadMaterialSet* EffectiveMaterialSetForTest() const { return EffectiveSet; }

	/**
	 * How many times BuildGhostBuffers has allocated a NEW GhostNetwork, for
	 * Airside.Present.NetworkActor: at most once for the life of this presenter, however
	 * many frames a drag crosses, since #166 replaced a DuplicateObject per call with
	 * GhostNetwork->CopyFrom into the same object.
	 */
	int32 GhostNetworkAllocCountForTest() const { return GhostNetworkAllocCount; }

	/**
	 * LayerComponents[Layer], for Airside.Present.NetworkActor.
	 *
	 * GetLayerComponent itself is private, and every OTHER test reads a layer's component
	 * back out through the very table Rebuild* wrote it into - which cannot catch a wiring
	 * bug in Initialize's argument order (Road<->Ghost, HoldingPaint<->RunwayPaint): the
	 * rebuild would still find and paint SOME component at that slot, correctly, and the
	 * test would still pass. This is the one seam that has to compare against something
	 * outside the table - the actor's own named UPROPERTY fields, in
	 * ARoadNetworkActor::LayerComponentForTest.
	 */
	UDynamicMeshComponent* GetLayerComponentForTest(ESurfaceLayer Layer) const { return GetLayerComponent(Layer); }

private:
	/**
	 * Rebuild and RebuildSurfaceOnly are one body (issue #165): both solve and build the road
	 * mesh identically, and differ only in whether the derived-graph passes run, which is a
	 * single `if (Kind == EChangeKind::Topology)` rather than two near-duplicate functions
	 * that could drift the way RebuildAprons/RebuildMarkings/RebuildRunwayMarkings did before
	 * RebuildLayer folded THEM into one shape (issue #81). RebuildMarkingsOnly is NOT a third
	 * near-duplicate for the same reason: EChangeKind::Markings (issue #179) shares nothing
	 * with the solve/mesh path above - it returns before the solve even runs - so it is an
	 * early exit at the top of this function instead of a body that would otherwise fall
	 * through the same steps Geometry and Topology share.
	 */
	void RebuildInternal(URoadNetwork& Network, const FSurfaceSettings& Settings, EChangeKind Kind);

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
	 * DOES NOT LOG ITS OWN CENSUS. Each caller's own summary line differs too much to fold in
	 * here (an apron's material name, a runway's whole marking-type census) - see each
	 * Rebuild*'s own UE_LOG for what that layer reports, and OutBuffers exists so it can.
	 * bQuiet (issue #178) IS handled here, though: it reaches the FDynamicMeshSink this
	 * function constructs, since every caller already has it on hand in its own Settings and
	 * passing it through here is one parameter rather than a sink built at each of the four
	 * call sites instead of inside this shared one.
	 */
	int32 RebuildLayer(ESurfaceLayer Layer, TFunctionRef<int32(FRoadMeshBuffers&)> BuildFn,
		UMaterialInterface* Material, bool bUseConstantColour, FRoadMeshBuffers& OutBuffers, bool bQuiet);

	/** Half a unit above the road, so paint wins the depth test against the pavement it lies
	 *  on - shared by RebuildMarkings and RebuildRunwayMarkings, which used to compute this
	 *  identically and separately (issue #81). */
	static double GetMarkingZ(double SurfaceZ) { return SurfaceZ + 0.5; }

	/**
	 * A QUARTER unit above the road: above the pavement, BELOW the paint at half a unit.
	 *
	 * Backwards physically - rubber is deposited on top of the paint, which is exactly why
	 * touchdown-zone markings are the ones that get repainted - and right for the game. The
	 * designator and the threshold stripes are how a player reads a runway at a glance, and
	 * burying them under a stain to be correct about tyre chemistry is a poor trade.
	 */
	static double GetRubberZ(double SurfaceZ) { return SurfaceZ + 0.25; }

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
	 * The tyre rubber - FRunwayMarkingBuilder::BuildRubber's bands on their own component.
	 *
	 * A THIRD paint-like component, and the reason is not the colour this time. Rubber is
	 * translucent where both marking layers are opaque, so it cannot share a component with
	 * either whatever colour it is drawn: blend mode is a property of the material, and a
	 * component has one material per slot.
	 *
	 * Does nothing when Settings.RubberMaterial is null, which is supported.
	 */
	void RebuildRunwayRubber(URoadNetwork& Network, const FSurfaceSettings& Settings);

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

	/**
	 * The hypothetical graph the ghost is solved against.
	 *
	 * ONE OBJECT FOR THE LIFE OF THIS PRESENTER (#166), not a fresh DuplicateObject per
	 * call: BuildGhostBuffers refreshes it with URoadNetwork::CopyFrom instead of replacing
	 * it, so a drag that crosses hundreds of frames allocates this exactly once (see
	 * GhostNetworkAllocCount) rather than orphaning a whole duplicated network - eighteen
	 * UPROPERTY arrays - to the garbage collector every cursor pixel.
	 */
	UPROPERTY(Transient) TObjectPtr<URoadNetwork> GhostNetwork;

	/** See GhostNetworkAllocCountForTest. Not a UPROPERTY - a session counter, not state. */
	int32 GhostNetworkAllocCount = 0;

	UPROPERTY(Transient) TObjectPtr<UMaterialInstanceDynamic> GhostMID;

	// What the ghost currently shows. A drag holds still for most frames, and rebuilding
	// an unchanged preview means re-solving it every frame for an identical result.
	//
	// LastGhostTo is QUANTISED to the nearest uu (#166), not the raw cursor position: a
	// held mouse still reports sub-uu noise every tick, which used to miss this exact
	// comparison on every one of those frames and rebuild anyway. One uu is far below
	// anything a cursor can express on purpose - see IsGhostCacheHit's own call for the
	// quantiser - and far above the jitter a real input device produces while "held still".
	int32 LastGhostFrom = INDEX_NONE;
	FVector2D LastGhostTo = FVector2D::ZeroVector;
	ERoadSnapKind LastGhostKind = ERoadSnapKind::Free;
	bool bLastGhostValid = true;
	bool bGhostVisible = false;
};
