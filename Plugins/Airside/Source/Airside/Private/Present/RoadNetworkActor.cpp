#include "Present/RoadNetworkActor.h"

#include "AirsideLog.h"
#include "Build/RoadMeshBuilder.h"
#include "Content/AirsideContent.h"
#include "Model/ArrivalPlanner.h"
#include "Solve/RunwayDesignator.h"
#include "Content/AirsideSettings.h"
#include "Build/RoadNetworkSolver.h"
#include "Build/AnchorLink.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Components/BillboardComponent.h"
#include "Components/DynamicMeshComponent.h"
#include "Components/SceneComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DrawDebugHelpers.h"
#include "DynamicMesh/MeshNormals.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "EngineUtils.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadSlotMap.h"
#include "Model/RouteSearch.h"
#include "Present/RoadAgentActor.h"
#include "Algo/Reverse.h"
#include "Solve/RoadGeom.h"
#include "Profiles/RoadMaterialSet.h"
#include "Profiles/RoadProfile.h"
#include "Tool/RoadEditHistory.h"
#include "Tool/GuidelineDrawTool.h"
#include "Tool/RoadHeal.h"
#include "Debug/RoadRebuildCensus.h"

void FDynamicMeshSink::PopulateAttributes(UE::Geometry::FDynamicMesh3& Mesh, const FRoadMeshBuffers& Buffers)
{
	using namespace UE::Geometry;

	Mesh.EnableAttributes();
	Mesh.Attributes()->SetNumUVLayers(3);

	// Deliberately NO colour overlay. The junction and ground blends are masks and live
	// in UV2. A UDynamicMeshComponent only ignores its colour overlay while
	// ColorOverrideMode is Constant; assigning any material flips it to None, the
	// converter then reads the overlay, and the surface stops rendering entirely - with
	// any material, ours or a stock one. Enabling primary colours here would reintroduce
	// exactly that.
	FDynamicMeshUVOverlay* UV0Layer = Mesh.Attributes()->GetUVLayer(0);
	FDynamicMeshUVOverlay* UV1Layer = Mesh.Attributes()->GetUVLayer(1);
	FDynamicMeshUVOverlay* UV2Layer = Mesh.Attributes()->GetUVLayer(2);

	// The mesh is fully welded, so there is exactly one UV and one colour per vertex and
	// the overlay element ids can be kept identical to the vertex ids. That is only safe
	// because welding is on exact bits: a tolerance-welded mesh would need split elements
	// wherever two surfaces met at a seam.
	for (int32 Index = 0; Index < Buffers.Positions.Num(); ++Index)
	{
		UV0Layer->AppendElement(Buffers.UV0[Index]);
		UV1Layer->AppendElement(Buffers.UV1[Index]);
		UV2Layer->AppendElement(Buffers.UV2[Index]);
	}

	for (const int32 TriangleId : Mesh.TriangleIndicesItr())
	{
		const FIndex3i Corners = Mesh.GetTriangle(TriangleId);
		UV0Layer->SetTriangle(TriangleId, Corners);
		UV1Layer->SetTriangle(TriangleId, Corners);
		UV2Layer->SetTriangle(TriangleId, Corners);
	}
}

int32 FDynamicMeshSink::BuildMesh(UE::Geometry::FDynamicMesh3& Mesh, const FRoadMeshBuffers& Buffers)
{
	Mesh.Clear();

	for (const FVector3d& Position : Buffers.Positions)
	{
		Mesh.AppendVertex(Position);
	}

	// Mesh triangle id -> index into Buffers.MaterialIDs. Recorded here, where it is known,
	// because it is NOT the identity - see the header.
	TArray<int32> SourceTriangle;
	int32 Rejected = 0;

	for (int32 Slot = 0, Source = 0; Slot + 2 < Buffers.Indices.Num(); Slot += 3, ++Source)
	{
		const int32 Result = Mesh.AppendTriangle(
			Buffers.Indices[Slot], Buffers.Indices[Slot + 1], Buffers.Indices[Slot + 2]);

		// AppendTriangle REFUSES rather than throws: negative results mean the triangle
		// was non-manifold or a duplicate. Silently ignoring that would leave holes in
		// the surface that look exactly like the cracks this system exists to remove.
		if (Result < 0)
		{
			++Rejected;
			continue;
		}

		while (SourceTriangle.Num() <= Result)
		{
			SourceTriangle.Add(INDEX_NONE);
		}
		SourceTriangle[Result] = Source;
	}

	PopulateAttributes(Mesh, Buffers);

	// Material ids, looked up THROUGH the mapping rather than by triangle id. Skipped
	// entirely when the buffers carry none, so a single-material build produces a mesh
	// with no material attribute at all - which is what keeps the scene proxy on its
	// single-buffer path instead of splitting by an attribute of all zeroes.
	if (Buffers.MaterialIDs.Num() > 0)
	{
		Mesh.EnableAttributes();
		Mesh.Attributes()->EnableMaterialID();
		UE::Geometry::FDynamicMeshMaterialAttribute* Attribute = Mesh.Attributes()->GetMaterialID();

		for (const int32 TriangleId : Mesh.TriangleIndicesItr())
		{
			const int32 Source = SourceTriangle.IsValidIndex(TriangleId)
				? SourceTriangle[TriangleId] : INDEX_NONE;

			// 0 for anything unmapped. An id must exist for every triangle the mesh holds:
			// the attribute defaults to 0 anyway, and stating it keeps the invariant here
			// rather than in the engine's initialisation.
			const int32 Id = Buffers.MaterialIDs.IsValidIndex(Source)
				? Buffers.MaterialIDs[Source] : 0;

			Attribute->SetValue(TriangleId, Id);
		}
	}

	return Rejected;
}

void FDynamicMeshSink::Accept(const FRoadMeshBuffers& Buffers)
{
	if (Component == nullptr)
	{
		return;
	}

	UE::Geometry::FDynamicMesh3 Mesh;
	const int32 Rejected = BuildMesh(Mesh, Buffers);

	if (Rejected > 0)
	{
		UE_LOG(LogRoadMesh, Warning,
			TEXT("%d triangle(s) rejected as non-manifold or duplicate - the surface will have holes"),
			Rejected);
	}

	// With an attribute set present the renderer reads the normal overlay rather than the
	// per-vertex normals, so both are filled: the overlay for rendering, and the per-vertex
	// normals because they cost nothing and keep the mesh self-describing. Passing true
	// reuses the per-vertex normals just computed instead of recomputing from scratch.
	UE::Geometry::FMeshNormals::QuickComputeVertexNormals(Mesh);
	UE::Geometry::FMeshNormals::InitializeOverlayToPerVertexNormals(Mesh.Attributes()->PrimaryNormals(), true);

	// Everything from the graph down to this point is covered by automation tests, so
	// when a road is built but not seen, the answer is on this side of the boundary.
	// SetMesh silently does nothing when the component is not editable, and a component
	// that is unregistered, hidden or wrongly bounded renders nothing while reporting
	// success, so state all of it rather than inferring any of it.
	const int32 BuiltTriangles = Mesh.TriangleCount();
	const int32 BuiltVertices = Mesh.VertexCount();

	// A UDynamicMeshComponent has NO surface-material fallback. GetNumMaterials() is
	// just BaseMaterials.Num(), so a component nobody called SetMaterial on reports zero
	// material slots and the renderer has no section to draw - the mesh is present,
	// correctly bounded, registered and visible, and draws nothing at all. The engine's
	// only built-in dynamic-mesh defaults (InitializeDefaultMaterials) are for the
	// wireframe and vertex-colour view modes, not for lit surfaces.
	//
	// Slice 2b replaces this with the real asphalt material; until then the surface has
	// to be given the engine default explicitly rather than assumed.
	// Material and vertex-colour mode are applied INDEPENDENTLY. They used to be decided
	// together in one if/else, so clearing the material also flipped the colour mode and
	// no observation could tell which one mattered.
	//
	// A material SET takes precedence over the single material, and must be applied as one
	// array: FDynamicMeshSceneProxy splits by material id only when HasAttributes() and
	// HasMaterialID() and NumMaterials > 1 all hold. Fail any one of those and it silently
	// falls back to a single buffer drawn entirely with material 0 - a complete, plausible
	// road in one surface, which looks exactly like a set that resolved to one material.
	if (Materials != nullptr && Materials->Slots.Num() > 1 && Buffers.MaterialIDs.Num() > 0)
	{
		TArray<UMaterialInterface*> Resolved;
		Materials->ResolveMaterials(Resolved);
		Component->ConfigureMaterialSet(Resolved);
	}
	else if (Material != nullptr)
	{
		// ONE SLOT, NOT SLOT ZERO. SetMaterial(0, ...) writes the first slot and leaves every
		// other one exactly where it was - so a component that had once been given a material
		// SET kept its extra slots for ever, they were SAVED INTO THE LEVEL, and every editor
		// restart loaded them back:
		//
		//   AS LOADED RoadMesh 3 slots [0]=M_RoadSurface [1]=M_ApronConcrete [2]=M_RoadKerb
		//
		// With material IDs still on the mesh the proxy splits across them and the lane draws
		// as apron concrete, which is a road changing colour on restart with nothing in the
		// model to explain it. Configuring the whole set replaces the slots rather than
		// overwriting the first of them.
		Component->ConfigureMaterialSet({ Material });
	}
	else if (Component->GetNumMaterials() == 0)
	{
		// A UDynamicMeshComponent has NO surface-material fallback: GetNumMaterials() is
		// just BaseMaterials.Num(), so a component nobody called SetMaterial on reports
		// zero material slots and the renderer has no section to draw.
		Component->SetMaterial(0, UMaterial::GetDefaultMaterial(MD_Surface));
	}

	if (bUseConstantVertexColour)
	{
		// Said out loud, because the two together look exactly like a material set that
		// failed to resolve: the colour override wins, every band renders the same flat
		// constant, and nothing else reports anything wrong.
		if (Materials != nullptr && Materials->Slots.Num() > 1)
		{
			UE_LOG(LogRoadMesh, Warning,
				TEXT("bUseConstantVertexColour is on, so the vertex-colour debug material ")
				TEXT("replaces all %d material slots - the road will render as one flat colour"),
				Materials->Slots.Num());
		}

		// This does NOT merely tint. Any mode other than None makes the scene proxy set
		// ForceOverrideMaterial to the engine's vertex-colour debug material and use it
		// in place of ours, so the surface shows a flat constant with no texture no
		// matter what SurfaceMaterial holds. Diagnostic only - see the header.
		Component->SetColorOverrideMode(EDynamicMeshComponentColorOverrideMode::Constant);
		Component->SetConstantOverrideColor(
			Material != nullptr ? FColor::White : FColor(40, 40, 45));
	}
	else
	{
		Component->SetColorOverrideMode(EDynamicMeshComponentColorOverrideMode::None);
	}

	Component->SetMesh(MoveTemp(Mesh));
	Component->NotifyMeshUpdated();

	// Every explanation reasoned from engine source has been wrong, so this reports the
	// runtime state instead of inferring it. Relevance is the one that can make a
	// primitive draw in no pass at all while mesh, bounds and material all look correct.
	{
		using namespace UE::Geometry;
		const FDynamicMesh3& Live = Component->GetDynamicMesh()->GetMeshRef();

		int32 BadNormals = 0;
		int32 CheckedNormals = 0;
		FVector3f FirstNormal(0.0f, 0.0f, 0.0f);
		if (Live.HasAttributes() && Live.Attributes()->PrimaryNormals() != nullptr)
		{
			const FDynamicMeshNormalOverlay* Normals = Live.Attributes()->PrimaryNormals();
			for (const int32 ElementId : Normals->ElementIndicesItr())
			{
				const FVector3f N = Normals->GetElement(ElementId);
				if (CheckedNormals == 0) { FirstNormal = N; }
				++CheckedNormals;
				if (!FMath::IsFinite(N.X) || !FMath::IsFinite(N.Y) || !FMath::IsFinite(N.Z) ||
					N.SizeSquared() < UE_KINDA_SMALL_NUMBER)
				{
					++BadNormals;
				}
			}
		}

		UMaterialInterface* Assigned = Component->GetMaterial(0);

		// Via the component's own scene rather than GMaxRHIShaderPlatform, which lives in
		// the RHI module - not a dependency worth adding for a diagnostic.
		FMaterialRelevance Relevance;
		if (const FSceneInterface* Scene = Component->GetScene())
		{
			Relevance = Component->GetMaterialRelevance(Scene->GetShaderPlatform());
		}

		// Kept, at Log rather than Warning. This one line - specifically FirstNormal -
		// identified a defect that survived two slices, several hand-derivations and a
		// review, all of which agreed with each other while measuring the wrong thing.
		UE_LOG(LogRoadMesh, Log,
			TEXT("DIAG: NumMaterials=%d Mat=%s RenderProxy=%d BlendMode=%d ")
			TEXT("Relevance[Opaque=%d Masked=%d NormalTranslucency=%d SeparateTranslucency=%d] ")
			TEXT("UVLayers=%d NormalElems=%d BadNormals=%d FirstNormal=(%.3f,%.3f,%.3f) ")
			TEXT("TangentsMode=%d ColorMode=%d TwoSided=%d DrawPath=%d"),
			Component->GetNumMaterials(),
			Assigned ? *Assigned->GetName() : TEXT("none"),
			(Assigned && Assigned->GetRenderProxy()) ? 1 : 0,
			Assigned ? static_cast<int32>(Assigned->GetBlendMode()) : -1,
			Relevance.bOpaque ? 1 : 0,
			Relevance.bMasked ? 1 : 0,
			Relevance.bNormalTranslucency ? 1 : 0,
			Relevance.bSeparateTranslucency ? 1 : 0,
			Live.HasAttributes() ? Live.Attributes()->NumUVLayers() : -1,
			CheckedNormals, BadNormals,
			FirstNormal.X, FirstNormal.Y, FirstNormal.Z,
			static_cast<int32>(Component->GetTangentsType()),
			static_cast<int32>(Component->GetColorOverrideMode()),
			Component->GetTwoSided() ? 1 : 0,
			static_cast<int32>(Component->GetMeshDrawPath()));
	}

	const FBoxSphereBounds Bounds = Component->Bounds;
	UE_LOG(LogRoadMesh, Log,
		TEXT("Sink: built %d verts / %d tris -> component holds %d tris. ")
		TEXT("Editable=%d Registered=%d Visible=%d HiddenInGame=%d Mobility=%d "
			 "CompLoc=(%.0f,%.0f,%.0f) BoundsOrigin=(%.0f,%.0f,%.0f) BoundsExtent=(%.0f,%.0f,%.0f) Mat=%s"),
		BuiltVertices, BuiltTriangles, Component->GetDynamicMesh()->GetMeshRef().TriangleCount(),
		Component->IsEditable() ? 1 : 0,
		Component->IsRegistered() ? 1 : 0,
		Component->IsVisible() ? 1 : 0,
		Component->bHiddenInGame ? 1 : 0,
		static_cast<int32>(Component->Mobility),
		Component->GetComponentLocation().X, Component->GetComponentLocation().Y,
		Component->GetComponentLocation().Z,
		Bounds.Origin.X, Bounds.Origin.Y, Bounds.Origin.Z,
		Bounds.BoxExtent.X, Bounds.BoxExtent.Y, Bounds.BoxExtent.Z,
		Component->GetMaterial(0) ? *Component->GetMaterial(0)->GetName() : TEXT("none"));
}

ARoadNetworkActor::ARoadNetworkActor()
{
	// Ticks for the agents and for nothing else. With none dispatched the tick body is a
	// single empty-array test, which is cheaper than the machinery needed to switch
	// ticking on and off as agents come and go.
	PrimaryActorTick.bCanEverTick = true;

	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));

#if WITH_EDITORONLY_DATA
	// No editor sprite. The mesh components are in absolute space, so the actor's own
	// transform never leaves the world origin - and a billboard sitting there is
	// indistinguishable from a node the build tool drew at (0,0).
	RootComponent->bVisualizeComponent = false;
#endif

	MeshComponent = CreateDefaultSubobject<UDynamicMeshComponent>(TEXT("RoadMesh"));
	MeshComponent->SetupAttachment(RootComponent);

	// FRoadMeshBuilder emits absolute world coordinates, so the component must not
	// transform them again. Absolute placement pins it to world space while leaving the
	// actor free to be moved: SetWorldTransform(Identity) on a root component would have
	// teleported the actor itself to the origin instead, which is why the mesh component
	// is a child of a plain scene root rather than the root itself.
	MeshComponent->SetUsingAbsoluteLocation(true);
	MeshComponent->SetUsingAbsoluteRotation(true);
	MeshComponent->SetUsingAbsoluteScale(true);

	// Tangents are left at the default ExternallyProvided, which finds no tangent space on
	// this mesh and falls back to a frame derived from the normal alone. On a flat +Z road
	// that is a constant, valid basis.
	//
	// AutoCalculated was tried and reverted. It derives the frame from the UV layers, and
	// this mesh's UV2 is (junction blend, ground blend) - identical at every segment
	// vertex, so every triangle is degenerate in that UV space. A degenerate UV triangle
	// divides by zero and yields NaN tangents, and NaN vertices are discarded by the GPU.
	// That is invisible for an unlit material, which never samples the tangent frame, and
	// fatal for any lit one - which is exactly the split observed: the engine's unlit
	// vertex-colour debug material drew, while every lit material, ours and stock alike,
	// rendered nothing.
	//
	// Slice 2b-ii can revisit this once the normal map's handedness matters, but it must
	// then compute tangents from UV0 specifically rather than from whatever the component
	// picks.

	// NOTHING IS RESOLVED HERE ANY MORE. Materials, the material set, the stand definition
	// and the default profile were all ConstructorHelpers::FObjectFinder calls against
	// literal /Game/ paths, which is what let a freshly placed actor render with no setup -
	// and what made eight references the editor could not see when a content folder moved.
	// ApplyContentDefaults fills in whatever is still null, at the moment it is first
	// wanted. See UAirsideSettings for why that cannot happen in a constructor.

	// A second component for the preview, sharing the road's absolute-space setup for the
	// same reason: the builder emits world coordinates and must not have them transformed
	// twice. Hidden until there is something to preview.
	GhostComponent = CreateDefaultSubobject<UDynamicMeshComponent>(TEXT("RoadGhost"));
	GhostComponent->SetupAttachment(RootComponent);
	GhostComponent->SetUsingAbsoluteLocation(true);
	GhostComponent->SetUsingAbsoluteRotation(true);
	GhostComponent->SetUsingAbsoluteScale(true);
	GhostComponent->SetVisibility(false);

	// The preview is a hint, not scenery: it must never occlude, shadow or be traced
	// against the road it is hovering over.
	GhostComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	GhostComponent->SetCastShadow(false);

	// Aprons: their own component, and no collision or shadows for the same reason the
	// roads have none - the world is flat, so picking is exact maths rather than a trace.
	ApronComponent = CreateDefaultSubobject<UDynamicMeshComponent>(TEXT("ApronMesh"));
	ApronComponent->SetupAttachment(RootComponent);
	ApronComponent->SetUsingAbsoluteLocation(true);
	ApronComponent->SetUsingAbsoluteRotation(true);
	ApronComponent->SetUsingAbsoluteScale(true);
	ApronComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);

}

URoadNetwork& ARoadNetworkActor::EnsureNetwork()
{
	if (Network == nullptr)
	{
		Network = NewObject<URoadNetwork>(this);
	}
	return *Network;
}

void ARoadNetworkActor::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();

	// See the header. The surface saved in the level is a cache of a derived value, and
	// this is its only invalidation point - without it the picture on screen is whatever
	// was last serialised, and the first rebuild from any cause silently replaces it.
	//
	// Templates excluded: a class default object has no model to build from, and running
	// the solver over one would be work done to produce nothing.
	if (!HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
	{
		// Before RebuildMesh, which is what calls FAnchorLink::Build - the very consumer of
		// GetAnchorWorldHeading this exists to keep correct. Loading a level saved before
		// FResolvedAnchor grew LocalHeading and Role restores those UPROPERTYs at their
		// defaults (0.0 and Aircraft), and nothing else ever repairs that - see
		// UEntityDefinition::RefreshResolvedAnchors for why this is the one moment it can.
		if (Network != nullptr)
		{
			const int32 RefreshedAnchors = UEntityDefinition::RefreshResolvedAnchors(*Network);
			if (RefreshedAnchors > 0)
			{
				UE_LOG(LogRoadMesh, Log,
					TEXT("Refreshed %d resolved anchor(s) against their current definitions."),
					RefreshedAnchors);
			}
		}

		RebuildMesh();
	}

#if WITH_EDITOR
	// USceneComponent keeps its sprite protected, so the owner cannot reach it by name -
	// but it is a component of this actor, so it can be found by type. Hidden rather than
	// destroyed: the engine re-creates it on the next OnRegister, and a component destroyed
	// out from under the thing that owns the pointer is a worse bargain than a hidden one.
	for (UBillboardComponent* Billboard : TInlineComponentArray<UBillboardComponent*>(this))
	{
		Billboard->SetVisibility(false);
	}
#endif
}

int32 ARoadNetworkActor::SurfaceTriangleCountForTest() const
{
	if (MeshComponent == nullptr || MeshComponent->GetDynamicMesh() == nullptr)
	{
		return 0;
	}
	return MeshComponent->GetDynamicMesh()->GetMeshRef().TriangleCount();
}

ARoadNetworkActor* ARoadNetworkActor::FindOrCreate(UWorld* World)
{
	if (World == nullptr)
	{
		return nullptr;
	}

	for (TActorIterator<ARoadNetworkActor> It(World); It; ++It)
	{
		return *It;
	}

	// Not transient, and not RF_Transient: this is the one that will be saved with the
	// level. An actor spawned with the transient flag would vanish on save and take the
	// whole airport with it.
	FActorSpawnParameters Params;
	Params.Name = TEXT("RoadNetwork");
	return World->SpawnActor<ARoadNetworkActor>(FVector::ZeroVector, FRotator::ZeroRotator, Params);
}

URoadEditHistory* ARoadNetworkActor::HistoryForEdit()
{
	// The editor's transaction system does the Memento's job already - see the header. In a
	// game world there is no transaction system, so the history is the only undo there is.
	const UWorld* World = GetWorld();
	if (World != nullptr && !World->IsGameWorld())
	{
		return nullptr;
	}

	return &EnsureHistory();
}

URoadEditHistory& ARoadNetworkActor::EnsureHistory()
{
	if (History == nullptr)
	{
		History = NewObject<URoadEditHistory>(this);
	}

	// Re-applied on every edit rather than only at creation, so lowering the depth in the
	// details panel mid-session takes effect instead of waiting for a restart.
	History->MaxDepth = FMath::Max(MaxUndoDepth, 1);
	return *History;
}

UMaterialInterface* ARoadNetworkActor::ResolveSurfaceMaterial() const
{
	if (SurfaceMaterial != nullptr) { return SurfaceMaterial; }
	const UAirsideContent* Content = UAirsideSettings::GetContent();
	return Content != nullptr ? Content->SurfaceMaterial.LoadSynchronous() : nullptr;
}

UMaterialInterface* ARoadNetworkActor::ResolveApronMaterial() const
{
	if (ApronMaterial != nullptr) { return ApronMaterial; }
	const UAirsideContent* Content = UAirsideSettings::GetContent();
	return Content != nullptr ? Content->ApronMaterial.LoadSynchronous() : nullptr;
}

UMaterialInterface* ARoadNetworkActor::ResolveGhostMaterial() const
{
	if (GhostMaterial != nullptr) { return GhostMaterial; }
	const UAirsideContent* Content = UAirsideSettings::GetContent();
	return Content != nullptr ? Content->GhostMaterial.LoadSynchronous() : nullptr;
}

URoadMaterialSet* ARoadNetworkActor::ResolveMaterialSet() const
{
	// NO DEFAULT, and this one is different from the others on purpose.
	//
	// A null material set is not "unset", it is a STATE: the road is drawn with one material
	// throughout. So a resolver cannot supply a default here without changing what an airport
	// looks like, and it cannot tell "never chosen" from "deliberately cleared" - which is
	// exactly what happened. Removing the write that filled this property was not enough,
	// because handing back the same value from the content set produced the identical road.
	//
	// Per-band materials are still available: assign one on the actor and it is used. What is
	// gone is the plugin deciding you wanted them.
	return MaterialSet;
}

UEntityDefinition* ARoadNetworkActor::ResolveStandDefinition() const
{
	if (StandDefinition != nullptr) { return StandDefinition; }
	const UAirsideContent* Content = UAirsideSettings::GetContent();
	return Content != nullptr ? Content->DefaultStand.LoadSynchronous() : nullptr;
}

URoadProfile* ARoadNetworkActor::ResolveProfile()
{
	// AUTHORED INPUT, READ AND NEVER WRITTEN. This briefly assigned Profile when it found it
	// null - a resolver that writes to the property it resolves has turned a setting into a
	// cache, and in an editor world, where this actor ticks, that write lands on the level.
	if (Profile != nullptr)
	{
		return Profile;
	}

	if (RuntimeProfile == nullptr)
	{
		// FROM THIS ACTOR'S OWN FallbackWidth, and deliberately not from the content set.
		//
		// A configured default was briefly consulted here, above these fields. It looked
		// harmless - both are 2300 by default - and it silently overrode every instance
		// whose width had been tuned in the Details panel, because the class default is not
		// the instance value. A road authored narrow came back at the class width and its
		// centreline marking, which scales with the surface, came back as a yellow slab.
		//
		// The content set has no business here at all: this is per-instance tuning, and the
		// serialisation problem it was brought in to solve is not this function's. Segments
		// reloaded with a null profile are repaired through URoadNetwork::DefaultProfile,
		// which RebuildMesh reassigns every time and so never depends on being saved.
		//
		// A tenth of the width per side: without a Shoulder band the profile has no outer
		// band to fade and the road ends in a knife edge against the ground.
		RuntimeProfile = URoadProfile::MakeTransient(
			FallbackWidth, FallbackFilletRadius, FallbackWidth * 0.1);
	}

	return RuntimeProfile;
}

bool ARoadNetworkActor::MakeLiveNodeId(int32 Index, FRoadNodeId& OutId) const
{
	if (Network == nullptr || !Network->GetNodes().IsValidIndex(Index))
	{
		return false;
	}

	// Build the handle from the slot's own generation and then check liveness properly.
	// FRoadNodeId::IsSet() would only report that a handle was assigned, which is the
	// check this codebase renamed precisely to stop people reaching for it here.
	FRoadNodeId Candidate;
	Candidate.Index = Index;
	Candidate.Generation = Network->GetNodes()[Index].Generation;

	if (!RoadSlot::IsValid<FRoadNodeId, FRoadNode>(Network->GetNodes(), Candidate))
	{
		return false;
	}

	OutId = Candidate;
	return true;
}

int32 ARoadNetworkActor::PlaceNode(FVector2D Where)
{
	// The network is made BEFORE the scope, so the snapshot is of an empty graph rather
	// than of nothing at all - otherwise the first node of a session is the one edit that
	// cannot be undone.
	URoadNetwork& Net = EnsureNetwork();
	FRoadEditScope Edit(HistoryForEdit(), &Net, TEXT("place node"));

	const FRoadNodeId Node = Net.AddNode(Where);
	if (!Node.IsSet())
	{
		UE_LOG(LogRoadMesh, Warning, TEXT("PlaceNode refused at (%f, %f)"), Where.X, Where.Y);
		return INDEX_NONE;
	}

	Edit.Commit();
	return Node.Index;
}

bool ARoadNetworkActor::ConnectNodes(int32 FromIndex, int32 ToIndex)
{
	if (FromIndex == ToIndex)
	{
		UE_LOG(LogRoadMesh, Warning, TEXT("ConnectNodes refused: node %d cannot join itself"), FromIndex);
		return false;
	}

	FRoadNodeId From;
	FRoadNodeId To;
	if (!MakeLiveNodeId(FromIndex, From) || !MakeLiveNodeId(ToIndex, To))
	{
		UE_LOG(LogRoadMesh, Warning,
			TEXT("ConnectNodes refused: %d -> %d, one of them is not a live node"), FromIndex, ToIndex);
		return false;
	}

	// Created after the guards above, all of which refuse without mutating anything, so a
	// rejected connection never costs a snapshot.
	FRoadEditScope Edit(HistoryForEdit(), Network, TEXT("connect nodes"));

	// Straight only. The model stores a Bezier control point, but AddSegment still
	// interpolates its interior samples in a straight line, so a curve authored here
	// would render as a chord until slice 2b samples the curve properly.
	const FRoadSegmentId Segment = Network->AddStraightSegment(From, To, ResolveProfile());
	if (!Segment.IsSet())
	{
		UE_LOG(LogRoadMesh, Warning, TEXT("ConnectNodes refused: %d -> %d"), FromIndex, ToIndex);
		return false;
	}

	Edit.Commit();
	return true;
}

bool ARoadNetworkActor::PlaceRunway(FVector2D From, FVector2D To, URoadProfile* RunwayProfile)
{
	if (Network == nullptr)
	{
		return false;
	}

	if (RunwayProfile == nullptr)
	{
		// Refused rather than defaulted. Falling back to ResolveProfile here would lay a
		// taxiway at runway length and call it a runway - the right shape on screen and the
		// wrong behaviour at every exit, with nothing to say so.
		UE_LOG(LogRoadMesh, Warning,
			TEXT("PlaceRunway refused: no runway profile. Check Project Settings > Plugins > "
				 "Airside, the content set's RunwayProfiles."));
		return false;
	}

	const double Length = FVector2D::Distance(From, To);
	if (Length < MinimumRunwayLength)
	{
		UE_LOG(LogRoadMesh, Warning,
			TEXT("PlaceRunway refused: %.0f uu is under the %.0f uu minimum"),
			Length, MinimumRunwayLength);
		return false;
	}

	// After the guards, all of which refuse without mutating, so a rejected runway never
	// costs a snapshot - the same rule ConnectNodes follows.
	FRoadEditScope Edit(HistoryForEdit(), Network, TEXT("place runway"));

	const FRoadNodeId A = Network->AddNode(From);
	const FRoadNodeId B = Network->AddNode(To);
	if (!A.IsSet() || !B.IsSet())
	{
		return false;
	}

	// STRAIGHT, and the model cannot express otherwise here: AddStraightSegment puts the
	// control point on the midpoint, which IsStraight tests for exactly.
	const FRoadSegmentId Segment = Network->AddStraightSegment(A, B, RunwayProfile);
	if (!Segment.IsSet())
	{
		return false;
	}

	Edit.Commit();
	RebuildMesh();

	UE_LOG(LogRoadMesh, Log, TEXT("Runway %s placed, %.0f uu long, %.0f uu wide"),
		*RunwayDesignator::ToPairText(To - From), Length, RunwayProfile->GetTotalWidth());
	return true;
}

int32 ARoadNetworkActor::ConnectGuidelines(int32 FromNodeIndex, int32 ToNodeIndex)
{
	if (Network == nullptr)
	{
		return INDEX_NONE;
	}

	const TArray<FGuidelineNode>& Nodes = Network->GetGuidelineNodes();
	if (!Nodes.IsValidIndex(FromNodeIndex) || !Nodes.IsValidIndex(ToNodeIndex))
	{
		return INDEX_NONE;
	}

	FGuidelineNodeId From;
	From.Index = FromNodeIndex;
	From.Generation = Nodes[FromNodeIndex].Generation;

	FGuidelineNodeId To;
	To.Index = ToNodeIndex;
	To.Generation = Nodes[ToNodeIndex].Generation;

	if (FGuidelineDrawTool::Validate(*Network, From, To) != EGuidelineLink::Valid)
	{
		UE_LOG(LogRoadMesh, Warning, TEXT("ConnectGuidelines refused: %s"),
			FGuidelineDrawTool::Describe(FGuidelineDrawTool::Validate(*Network, From, To)));
		return INDEX_NONE;
	}

	FRoadEditScope Edit(HistoryForEdit(), Network, TEXT("link guidelines"));

	FGuidelineEdge Edge;
	Edge.A = From;
	Edge.B = To;

	// Straight, like a derived guideline on a straight segment: Control at the midpoint.
	// A curve would need a gesture to author it and would render as a chord regardless.
	Edge.Control = (Nodes[FromNodeIndex].Position + Nodes[ToNodeIndex].Position) * 0.5;

	// EVERY class. A hand-drawn link exists because the graph is missing a connection, and
	// guessing a narrower rule would make it silently useless to whoever needed it - with
	// no way to tell, because a refusal to route looks the same as no link at all.
	Edge.AllowedTraffic = FTrafficMask::All();
	Edge.Direction = EGuidelineDir::Bidirectional;

	// The player's, so the builder steps aside for it.
	Edge.bDerived = false;

	// And WHAT its ends are, not merely where they are now. Without this it survives every
	// rebuild attached to nodes the new derivation abandoned - drawn, and routing nothing.
	Edge.EndRefA = Nodes[FromNodeIndex].Origin;
	Edge.EndRefB = Nodes[ToNodeIndex].Origin;

	const FGuidelineEdgeId Added = Network->AddGuidelineEdge(MoveTemp(Edge));
	return Added.IsSet() ? Added.Index : INDEX_NONE;
}

bool ARoadNetworkActor::DisconnectGuideline(int32 EdgeIndex)
{
	if (Network == nullptr)
	{
		return false;
	}

	const TArray<FGuidelineEdge>& Edges = Network->GetGuidelineEdges();
	if (!Edges.IsValidIndex(EdgeIndex) || !Edges[EdgeIndex].bAlive)
	{
		return false;
	}

	// A derived edge belongs to the pavement, and the next rebuild would put it straight
	// back. Obeying here would be indistinguishable from ignoring the click.
	if (Edges[EdgeIndex].bDerived)
	{
		UE_LOG(LogRoadMesh, Warning,
			TEXT("DisconnectGuideline refused: edge %d is derived from a road, not hand-drawn"),
			EdgeIndex);
		return false;
	}

	FRoadEditScope Edit(HistoryForEdit(), Network, TEXT("unlink guidelines"));

	FGuidelineEdgeId Id;
	Id.Index = EdgeIndex;
	Id.Generation = Edges[EdgeIndex].Generation;
	return Network->RemoveGuidelineEdge(Id);
}

int32 ARoadNetworkActor::FindNodeNear(FVector2D Where, double Radius) const
{
	if (Network == nullptr || Radius <= 0.0)
	{
		return INDEX_NONE;
	}

	// Compared squared, so a caller passing a large radius costs no square roots.
	const double RadiusSquared = Radius * Radius;
	double BestSquared = RadiusSquared;
	int32 Best = INDEX_NONE;

	const TArray<FRoadNode>& Nodes = Network->GetNodes();
	for (int32 Index = 0; Index < Nodes.Num(); ++Index)
	{
		if (!Nodes[Index].bAlive)
		{
			continue;
		}

		const double DistanceSquared = FVector2D::DistSquared(Nodes[Index].Position, Where);
		if (DistanceSquared <= BestSquared)
		{
			BestSquared = DistanceSquared;
			Best = Index;
		}
	}

	return Best;
}

bool ARoadNetworkActor::MakeLiveSegmentId(int32 Index, FRoadSegmentId& OutId) const
{
	if (Network == nullptr || !Network->GetSegments().IsValidIndex(Index))
	{
		return false;
	}

	FRoadSegmentId Candidate;
	Candidate.Index = Index;
	Candidate.Generation = Network->GetSegments()[Index].Generation;

	if (!RoadSlot::IsValid<FRoadSegmentId, FRoadSegment>(Network->GetSegments(), Candidate))
	{
		return false;
	}

	OutId = Candidate;
	return true;
}

int32 ARoadNetworkActor::SplitSegment(int32 SegmentIndex, FVector2D At)
{
	FRoadSegmentId Doomed;
	if (!MakeLiveSegmentId(SegmentIndex, Doomed))
	{
		UE_LOG(LogRoadMesh, Warning,
			TEXT("SplitSegment refused: %d is not a live segment"), SegmentIndex);
		return INDEX_NONE;
	}

	FRoadEditScope Edit(HistoryForEdit(), Network, TEXT("split segment"));

	const FRoadNodeId Middle = SplitSegmentIn(*Network, Doomed, At);
	if (!Middle.IsSet())
	{
		UE_LOG(LogRoadMesh, Warning,
			TEXT("SplitSegment refused: segment %d at (%f, %f)"), SegmentIndex, At.X, At.Y);
		return INDEX_NONE;
	}

	Edit.Commit();
	return Middle.Index;
}

FRoadNodeId ARoadNetworkActor::SplitSegmentIn(URoadNetwork& Net, FRoadSegmentId Doomed, const FVector2D& At)
{
	const FRoadSegment* Segment = Net.GetSegment(Doomed);
	const FRoadNode* EndA = Segment != nullptr ? Net.GetNode(Segment->A) : nullptr;
	const FRoadNode* EndB = Segment != nullptr ? Net.GetNode(Segment->B) : nullptr;
	if (EndA == nullptr || EndB == nullptr)
	{
		return FRoadNodeId();
	}

	// Copied out before anything mutates. Every pointer above dangles the moment the
	// segment is removed or the arrays reallocate, and the two replacements need all of
	// this after that point.
	const FRoadNodeId KeepA = Segment->A;
	const FRoadNodeId KeepB = Segment->B;
	URoadProfile* KeepProfile = Segment->Profile;
	const FVector2D PositionA = EndA->Position;
	const FVector2D PositionB = EndB->Position;

	// A degeneracy floor, NOT a placement policy: how far from an end a split should be
	// allowed is the snap chain's MinSplitFromEndpoint, which is tuned and can be turned
	// down. This is the point below which the result is not a road at all, and no setting
	// may cross it - a zero-length segment has no direction, so the solver cannot derive
	// a bearing for it and the junction at either end loses an arm.
	constexpr double MinSplitOffset = 1.0;
	if (FVector2D::DistSquared(At, PositionA) < MinSplitOffset * MinSplitOffset
		|| FVector2D::DistSquared(At, PositionB) < MinSplitOffset * MinSplitOffset)
	{
		return FRoadNodeId();
	}

	const FRoadNodeId Middle = Net.AddNode(At);
	if (!Middle.IsSet())
	{
		return FRoadNodeId();
	}

	// Removed, not reshaped. A segment's endpoints are its identity and both of them
	// change here, so the handle must die rather than quietly come to mean half a road.
	if (!Net.RemoveSegment(Doomed))
	{
		Net.RemoveNode(Middle);
		return FRoadNodeId();
	}

	const FRoadSegmentId First = Net.AddStraightSegment(KeepA, Middle, KeepProfile);
	const FRoadSegmentId Second = Net.AddStraightSegment(Middle, KeepB, KeepProfile);

	// Both endpoints were checked live and the middle node was just created, so the only
	// way here is a model invariant having changed underneath. Loud rather than silent:
	// the graph is now missing a road the player can still see the ends of.
	if (!First.IsSet() || !Second.IsSet())
	{
		UE_LOG(LogRoadMesh, Error, TEXT("SplitSegmentIn left a segment half-replaced: first=%d second=%d"),
			First.IsSet() ? 1 : 0, Second.IsSet() ? 1 : 0);
	}

	return Middle;
}

UMaterialInstanceDynamic* ARoadNetworkActor::GhostMaterialInstance()
{
	UMaterialInterface* Base = ResolveGhostMaterial();
	if (GhostMID == nullptr && Base != nullptr)
	{
		GhostMID = UMaterialInstanceDynamic::Create(Base, this);
	}
	return GhostMID;
}

void ARoadNetworkActor::AddGhostJunction(
	FRoadMeshBuilder& Builder, const FRoadSolveResult& Solved, int32 NodeIndex) const
{
	const FJunctionResult* Junction = Solved.NodeResults.Find(NodeIndex);
	const TArray<FRoadSegmentId>* Arms = Solved.NodeArmSegments.Find(NodeIndex);

	// A one-armed node solves to no fan at all - the solver trims it back and leaves the
	// end cap to the mesh builder - so a missing entry is ordinary, not an error.
	if (Junction == nullptr || Arms == nullptr || GhostNetwork == nullptr)
	{
		return;
	}
	Builder.AddJunction(*GhostNetwork, NodeIndex, *Junction, *Arms);
}

void ARoadNetworkActor::HideGhost()
{
	if (GhostComponent != nullptr)
	{
		GhostComponent->SetVisibility(false);
	}
	bGhostVisible = false;
	LastGhostFrom = INDEX_NONE;
}

bool ARoadNetworkActor::BuildGhostBuffers(
	int32 FromNodeIndex, const FRoadSnapResult& Snap, FRoadMeshBuffers& OutBuffers)
{
	FRoadNodeId From;
	if (Network == nullptr || !MakeLiveNodeId(FromNodeIndex, From))
	{
		return false;
	}

	// The duplicate, and the reason for the whole function. Slot indices and generation
	// counters survive duplication, so a handle resolved against the real network
	// resolves to the same thing here - which is what lets Snap's node and segment
	// handles be used directly below without translation.
	GhostNetwork = DuplicateObject<URoadNetwork>(Network, this);
	if (GhostNetwork == nullptr)
	{
		return false;
	}

	FRoadNodeId To;
	switch (Snap.Kind)
	{
	case ERoadSnapKind::Node:
		To = Snap.Node;
		break;

	case ERoadSnapKind::Segment:
		// The same surgery the click will perform, run on the copy. Sharing the
		// implementation is the only thing that stops the preview and the edit diverging.
		To = SplitSegmentIn(*GhostNetwork, Snap.Segment, Snap.Position);
		break;

	case ERoadSnapKind::Free:
	default:
		To = GhostNetwork->AddNode(Snap.Position);
		break;
	}

	if (!To.IsSet() || To == From)
	{
		return false;
	}

	const FRoadSegmentId Ghosted = GhostNetwork->AddStraightSegment(From, To, ResolveProfile());
	if (!Ghosted.IsSet())
	{
		return false;
	}

	const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*GhostNetwork);

	// Only the NEW segment and the two junctions it reshapes. Drawing the whole ghost
	// network would lay a translucent copy over every road already on screen, and the one
	// thing the preview has to answer is what THIS click changes.
	//
	// Segments before junctions: the builder's ordering contract. A cut vertex is one
	// welded vertex holding one UV1, first writer wins, and a segment measures its
	// distance-along from its A end while the junction standing at that node would write
	// zero. Reversed, the ghost's markings jump at one end and nothing reports it.
	FRoadMeshBuilder Builder(SurfaceZ + GhostZOffset, TexelsPerUnit);
	Builder.AddSegment(*GhostNetwork, Ghosted, RibbonSegments);
	AddGhostJunction(Builder, Solved, From.Index);
	AddGhostJunction(Builder, Solved, To.Index);

	OutBuffers = Builder.GetBuffers();
	return true;
}

void ARoadNetworkActor::UpdateGhost(int32 FromNodeIndex, const FRoadSnapResult& Snap, bool bValid)
{
	FRoadNodeId From;
	if (Network == nullptr || GhostComponent == nullptr || !MakeLiveNodeId(FromNodeIndex, From))
	{
		HideGhost();
		return;
	}

	// A drag holds still for most of its frames. Rebuilding then means duplicating the
	// network and re-solving it to produce exactly the same triangles, sixty times a
	// second. Cleared by RebuildMesh, so any real edit invalidates it.
	if (bGhostVisible
		&& FromNodeIndex == LastGhostFrom
		&& Snap.Kind == LastGhostKind
		&& Snap.Position == LastGhostTo)
	{
		if (bValid != bLastGhostValid)
		{
			// Geometry untouched: this is the whole reason validity is a material
			// parameter rather than a second mesh.
			if (UMaterialInstanceDynamic* Instance = GhostMaterialInstance())
			{
				Instance->SetScalarParameterValue(TEXT("ValidityBlend"), bValid ? 0.0f : 1.0f);
			}
			bLastGhostValid = bValid;
		}
		return;
	}

	FRoadMeshBuffers Buffers;
	if (!BuildGhostBuffers(FromNodeIndex, Snap, Buffers))
	{
		HideGhost();
		return;
	}

	if (UMaterialInstanceDynamic* Instance = GhostMaterialInstance())
	{
		Instance->SetScalarParameterValue(TEXT("ValidityBlend"), bValid ? 0.0f : 1.0f);

		// The material cannot know where this road's edge is; UV1.X is in uu and the
		// profile owns the half-width. Left at its default a narrow road would glow from
		// edge to edge and a wide one not at all.
		if (const URoadProfile* Used = ResolveProfile())
		{
			Instance->SetScalarParameterValue(TEXT("EdgeHalfWidth"),
				static_cast<float>(FMath::Max(Used->GetHalfWidthLeft(), Used->GetHalfWidthRight())));
		}
	}

	// bUseConstantVertexColour false, and it matters: any ColorOverrideMode other than
	// None makes the scene proxy substitute the engine's vertex-colour debug material for
	// ours, so the ghost would render as flat opaque grey with none of its parameters.
	FDynamicMeshSink Sink(GhostComponent, GhostMaterialInstance(), /*bUseConstantVertexColour*/ false);
	Sink.Accept(Buffers);

	GhostComponent->SetVisibility(true);

	bGhostVisible = true;
	LastGhostFrom = FromNodeIndex;
	LastGhostTo = Snap.Position;
	LastGhostKind = Snap.Kind;
	bLastGhostValid = bValid;
}

void ARoadNetworkActor::BeginInteractiveEdit(const FString& Label)
{
	URoadEditHistory* Use = HistoryForEdit();
	if (Network != nullptr && Use != nullptr && !Use->IsEditing())
	{
		Use->BeginEdit(*Network, Label);
	}
}

void ARoadNetworkActor::EndInteractiveEdit(bool bKeep)
{
	if (History == nullptr || !History->IsEditing())
	{
		return;
	}

	if (bKeep)
	{
		History->CommitEdit();
	}
	else
	{
		History->AbandonEdit();
	}
}

bool ARoadNetworkActor::MoveNode(int32 NodeIndex, FVector2D To)
{
	FRoadNodeId Node;
	if (!MakeLiveNodeId(NodeIndex, Node))
	{
		return false;
	}

	const FRoadNode* Live = Network->GetNode(Node);
	if (Live == nullptr)
	{
		return false;
	}

	// Judged before moving. Every road this node holds gets longer or shorter as it goes,
	// and one pulled under the minimum is one the solver cannot trim back from both ends.
	for (const FRoadSegmentId& Incident : Live->Incident)
	{
		const FRoadNodeId Other = Network->GetOtherEnd(Incident, Node);
		const FRoadNode* Far = Network->GetNode(Other);
		if (Far != nullptr && FVector2D::Distance(Far->Position, To) < PlacementLimits.MinSegmentLength)
		{
			return false;
		}
	}

	// Joins a drag already in progress, so the whole drag is one undo step; on its own it
	// is one edit of its own. IsEditing is what tells the two apart.
	URoadEditHistory* Use = HistoryForEdit();
	const bool bOwnsEdit = Use != nullptr && !Use->IsEditing();
	if (bOwnsEdit)
	{
		Use->BeginEdit(*Network, TEXT("move node"));
	}

	const bool bMoved = Network->SetNodePosition(Node, To);

	if (bOwnsEdit)
	{
		if (bMoved)
		{
			Use->CommitEdit();
		}
		else
		{
			Use->AbandonEdit();
		}
	}

	return bMoved;
}

FRoadDeletionPlan ARoadNetworkActor::PlanNodeDeletion(int32 NodeIndex) const
{
	FRoadNodeId Node;
	if (Network == nullptr || !MakeLiveNodeId(NodeIndex, Node))
	{
		return FRoadDeletionPlan();
	}
	return RoadHeal::PlanNodeDeletion(*Network, Node, PlacementLimits);
}

bool ARoadNetworkActor::DeleteNode(int32 NodeIndex)
{
	FRoadNodeId Node;
	if (!MakeLiveNodeId(NodeIndex, Node))
	{
		UE_LOG(LogRoadMesh, Warning, TEXT("DeleteNode refused: %d is not a live node"), NodeIndex);
		return false;
	}

	const FRoadDeletionPlan Plan = RoadHeal::PlanNodeDeletion(*Network, Node, PlacementLimits);
	if (!Plan.bValid)
	{
		// Refused whole. Nothing has been touched yet, which is the point of planning
		// before acting rather than unwinding afterwards.
		UE_LOG(LogRoadMesh, Warning,
			TEXT("DeleteNode refused: node %d cannot rejoin node %d (%s). Delete its roads "
				 "one at a time to strand it, then it will delete."),
			NodeIndex, Plan.RefusedNeighbour.Index, RoadPlacement::Describe(Plan.Refusal));
		return false;
	}

	FRoadEditScope Edit(HistoryForEdit(), Network, TEXT("delete node"));

	// The cascade is the model's: a segment whose endpoint is gone has no geometry.
	if (!Network->RemoveNode(Node))
	{
		UE_LOG(LogRoadMesh, Warning, TEXT("DeleteNode refused: node %d would not remove"), NodeIndex);
		return false;
	}

	// The heal. Every one of these was judged against the post-deletion graph, so it is
	// being applied to exactly the state it was approved for.
	for (const FRoadNodeId& Stranded : Plan.Rejoin)
	{
		if (!Network->AddStraightSegment(Stranded, Plan.Anchor, ResolveProfile()).IsSet())
		{
			UE_LOG(LogRoadMesh, Error,
				TEXT("DeleteNode healed only partly: node %d could not rejoin %d"),
				Stranded.Index, Plan.Anchor.Index);
		}
	}

	for (const FRoadNodeId& Litter : Plan.Swept)
	{
		Network->RemoveNode(Litter);
	}

	Edit.Commit();
	return true;
}

bool ARoadNetworkActor::DeleteSegment(int32 SegmentIndex)
{
	FRoadSegmentId Segment;
	if (!MakeLiveSegmentId(SegmentIndex, Segment))
	{
		UE_LOG(LogRoadMesh, Warning,
			TEXT("DeleteSegment refused: %d is not a live segment"), SegmentIndex);
		return false;
	}

	// Captured before the removal, because afterwards the segment cannot say what it joined.
	const FRoadSegment* Doomed = Network->GetSegment(Segment);
	const FRoadNodeId EndA = Doomed != nullptr ? Doomed->A : FRoadNodeId();
	const FRoadNodeId EndB = Doomed != nullptr ? Doomed->B : FRoadNodeId();

	FRoadEditScope Edit(HistoryForEdit(), Network, TEXT("delete segment"));

	if (!Network->RemoveSegment(Segment))
	{
		UE_LOG(LogRoadMesh, Warning,
			TEXT("DeleteSegment refused: segment %d would not remove"), SegmentIndex);
		return false;
	}

	// Cleanup, not deletion: an endpoint left with no road holds no geometry, so removing
	// it destroys nothing. An endpoint that still has roads is untouched.
	for (const FRoadNodeId& End : { EndA, EndB })
	{
		if (const FRoadNode* Live = Network->GetNode(End))
		{
			if (Live->Incident.Num() == 0)
			{
				Network->RemoveNode(End);
			}
		}
	}

	Edit.Commit();
	return true;
}

TArray<int32> ARoadNetworkActor::SegmentsIncidentTo(int32 NodeIndex) const
{
	TArray<int32> Found;

	FRoadNodeId Node;
	if (!MakeLiveNodeId(NodeIndex, Node))
	{
		return Found;
	}

	const FRoadNode* Live = Network->GetNode(Node);
	if (Live == nullptr)
	{
		return Found;
	}

	Found.Reserve(Live->Incident.Num());
	for (const FRoadSegmentId& Incident : Live->Incident)
	{
		Found.Add(Incident.Index);
	}
	return Found;
}

bool ARoadNetworkActor::GetSegmentEnds(int32 SegmentIndex, FVector2D& OutA, FVector2D& OutB) const
{
	FRoadSegmentId Id;
	if (!MakeLiveSegmentId(SegmentIndex, Id))
	{
		return false;
	}

	const FRoadSegment* Segment = Network->GetSegment(Id);
	const FRoadNode* EndA = Segment != nullptr ? Network->GetNode(Segment->A) : nullptr;
	const FRoadNode* EndB = Segment != nullptr ? Network->GetNode(Segment->B) : nullptr;
	if (EndA == nullptr || EndB == nullptr)
	{
		return false;
	}

	OutA = EndA->Position;
	OutB = EndB->Position;
	return true;
}

bool ARoadNetworkActor::Undo()
{
	if (Network == nullptr || History == nullptr)
	{
		return false;
	}

	URoadNetwork* Restored = History->Undo(*Network);
	if (Restored == nullptr)
	{
		return false;
	}

	// Adopted outright rather than copied: the history has already let go of it.
	Network = Restored;

	// The preview may be describing a node that no longer exists, and its cache compares
	// only the cursor and the start node - neither of which an undo changes.
	HideGhost();
	RebuildMesh();
	return true;
}

bool ARoadNetworkActor::Redo()
{
	if (Network == nullptr || History == nullptr)
	{
		return false;
	}

	URoadNetwork* Restored = History->Redo(*Network);
	if (Restored == nullptr)
	{
		return false;
	}

	Network = Restored;
	HideGhost();
	RebuildMesh();
	return true;
}

bool ARoadNetworkActor::CanUndo() const
{
	return History != nullptr && History->CanUndo();
}

bool ARoadNetworkActor::CanRedo() const
{
	return History != nullptr && History->CanRedo();
}

FString ARoadNetworkActor::PeekUndoLabel() const
{
	return History != nullptr ? History->PeekUndoLabel() : FString();
}

int32 ARoadNetworkActor::AddApron(const TArray<FVector2D>& Outline)
{
	if (Outline.Num() < 3)
	{
		UE_LOG(LogRoadMesh, Warning,
			TEXT("AddApron refused: %d corners, three is the minimum"), Outline.Num());
		return INDEX_NONE;
	}

	// The triangulator's contract is a SIMPLE polygon. Fed a figure-eight it produces
	// overlapping triangles rather than an error, so the refusal has to happen here.
	if (!RoadGeom::IsSimplePolygon(Outline))
	{
		UE_LOG(LogRoadMesh, Warning, TEXT("AddApron refused: the outline crosses itself"));
		return INDEX_NONE;
	}

	FApronSurface Surface;
	Surface.Outline = Outline;

	// Corrected, not refused. FApronSurface asks for counter-clockwise and the shoelace
	// sign says which way round this is; reversing is an answer, refusing is a complaint.
	if (RoadGeom::PolygonArea(Surface.Outline) < 0.0)
	{
		Algo::Reverse(Surface.Outline);
	}

	FRoadEditScope Edit(HistoryForEdit(), &EnsureNetwork(), TEXT("add apron"));

	const FApronId Added = Network->AddApron(MoveTemp(Surface));
	if (!Added.IsSet())
	{
		UE_LOG(LogRoadMesh, Warning, TEXT("AddApron refused by the model"));
		return INDEX_NONE;
	}

	Edit.Commit();
	return Added.Index;
}

bool ARoadNetworkActor::DeleteApron(int32 ApronIndex)
{
	if (Network == nullptr || !Network->GetAprons().IsValidIndex(ApronIndex)
		|| !Network->GetAprons()[ApronIndex].bAlive)
	{
		return false;
	}

	FApronId Doomed;
	Doomed.Index = ApronIndex;
	Doomed.Generation = Network->GetAprons()[ApronIndex].Generation;

	FRoadEditScope Edit(HistoryForEdit(), Network, TEXT("delete apron"));

	if (!Network->RemoveApron(Doomed))
	{
		return false;
	}

	Edit.Commit();
	return true;
}

int32 ARoadNetworkActor::FindApronAt(FVector2D Where) const
{
	if (Network == nullptr)
	{
		return INDEX_NONE;
	}

	// Walked backwards so the most recently added apron wins where two overlap, which is
	// what "the one on top" means to someone who just drew it.
	const TArray<FApronSurface>& Aprons = Network->GetAprons();
	for (int32 Index = Aprons.Num() - 1; Index >= 0; --Index)
	{
		if (Aprons[Index].bAlive && RoadGeom::PointInPolygon(Aprons[Index].Outline, Where))
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

double ARoadNetworkActor::GetApronSurfaceZ() const
{
	// Never more than halfway down to the ground plane, whatever ApronZOffset asks for.
	// SurfaceZ is already a height above that plane, so half of it is the most that can be
	// given away while still leaving the apron above ground.
	const double Drop = FMath::Min(ApronZOffset, FMath::Max(SurfaceZ, 0.0) * 0.5);
	return SurfaceZ - Drop;
}

void ARoadNetworkActor::RebuildAprons()
{
	if (Network == nullptr || ApronComponent == nullptr)
	{
		return;
	}

	// Its own builder instance, so an apron corner that happens to land exactly on a road
	// vertex cannot weld to it. The two surfaces meet; they are not one surface.
	FRoadMeshBuilder Builder(GetApronSurfaceZ(), TexelsPerUnit);

	int32 Built = 0;
	for (const FApronSurface& Apron : Network->GetAprons())
	{
		if (Apron.bAlive)
		{
			Builder.AddApron(Apron);
			++Built;
		}
	}

	const FRoadMeshBuffers& Buffers = Builder.GetBuffers();

	FDynamicMeshSink Sink(ApronComponent,
		ResolveApronMaterial() != nullptr ? ResolveApronMaterial() : ResolveSurfaceMaterial(),
		bUseConstantApronColour);
	Sink.Accept(Buffers);

	ApronComponent->SetVisibility(Built > 0);

	// Reported rather than inferred. An apron that is built and never seen, and one that
	// is never built, look identical from outside - and every explanation reasoned from
	// engine source about the roads was wrong before the numbers were printed.
	UE_LOG(LogRoadMesh, Log,
		TEXT("Aprons: %d surface(s), %d triangle(s) at Z=%.1f, material %s%s"),
		Built, Buffers.Indices.Num() / 3, GetApronSurfaceZ(),
		ResolveApronMaterial() != nullptr ? *ResolveApronMaterial()->GetName() : TEXT("<fallback>"),
		bUseConstantApronColour ? TEXT(" (CONSTANT COLOUR - material overridden)") : TEXT(""));

	// Worth saying out loud: a road surface this close to the ground leaves nothing to
	// separate the two surfaces with, and both will z-fight at distance whatever is done
	// here. The apron is above ground either way - this is about the ROAD being too low.
	if (Built > 0 && SurfaceZ < ApronZOffset * 2.0)
	{
		UE_LOG(LogRoadMesh, Warning,
			TEXT("SurfaceZ is %.1f, less than twice ApronZOffset (%.1f): the apron is squeezed "
				 "to Z=%.1f. Raise SurfaceZ for a cleaner separation."),
			SurfaceZ, ApronZOffset, GetApronSurfaceZ());
	}

	if (bDebugDrawAprons && GetWorld() != nullptr)
	{
		// A completely separate route to the screen, from the same buffers the component
		// was handed.
		const float Lifetime = static_cast<float>(DebugDrawSeconds);
		for (int32 Slot = 0; Slot + 2 < Buffers.Indices.Num(); Slot += 3)
		{
			const FVector A = Buffers.Positions[Buffers.Indices[Slot]];
			const FVector B = Buffers.Positions[Buffers.Indices[Slot + 1]];
			const FVector C = Buffers.Positions[Buffers.Indices[Slot + 2]];

			// Thickness in WORLD units. Single digits are sub-pixel across a scene this
			// large - indistinguishable from nothing being drawn at all.
			DrawDebugLine(GetWorld(), A, B, FColor::Cyan, false, Lifetime, 0, 12.0f);
			DrawDebugLine(GetWorld(), B, C, FColor::Cyan, false, Lifetime, 0, 12.0f);
			DrawDebugLine(GetWorld(), C, A, FColor::Cyan, false, Lifetime, 0, 12.0f);
		}
	}
}

int32 ARoadNetworkActor::PlaceStand(FVector2D Where, double Heading)
{
	UEntityDefinition* Stand = ResolveStandDefinition();
	if (Stand == nullptr)
	{
		UE_LOG(LogRoadMesh, Warning,
			TEXT("PlaceStand refused: no StandDefinition. Author DA_Stand_CodeC with "
				 "Tools/Python/build_stand_asset.py, or set one on the actor."));
		return INDEX_NONE;
	}

	// Moved down from URoadNetwork::PlaceEntity along with Anchors itself: HasUsableAnchorIds
	// is a UEntityDefinition method, and Model/ no longer calls into Entities/ at all - see
	// Tool/RoadEditTarget.h's header comment for the other half of that seam. Complained
	// about, not refused: a half-authored definition should be visible in the log rather
	// than fatal at the call site. But it IS a real fault - lookup is by id, so two anchors
	// sharing one are indistinguishable and a query for either returns the first, which
	// sends the fuel truck to the belt loader and reports success.
	if (!UEntityDefinition::HasUsableAnchorIds(Stand))
	{
		UE_LOG(LogRoadMesh, Error,
			TEXT("PlaceStand: %s has anchors with empty or duplicate ids. Anchor lookups on "
				 "this entity will be ambiguous."),
			*Stand->GetName());
	}

	URoadNetwork& Net = EnsureNetwork();
	FRoadEditScope Edit(HistoryForEdit(), &Net, TEXT("place stand"));

	const FEntityInstanceId Placed = Net.PlaceEntity(Stand, Stand->Anchors, Where, Heading);
	if (!Placed.IsSet())
	{
		return INDEX_NONE;
	}

	Edit.Commit();
	return Placed.Index;
}

bool ARoadNetworkActor::DeleteEntity(int32 EntityIndex)
{
	if (Network == nullptr || !Network->GetEntities().IsValidIndex(EntityIndex)
		|| !Network->GetEntities()[EntityIndex].bAlive)
	{
		return false;
	}

	FEntityInstanceId Doomed;
	Doomed.Index = EntityIndex;
	Doomed.Generation = Network->GetEntities()[EntityIndex].Generation;

	FRoadEditScope Edit(HistoryForEdit(), Network, TEXT("delete stand"));

	if (!Network->RemoveEntity(Doomed))
	{
		return false;
	}

	Edit.Commit();
	return true;
}

int32 ARoadNetworkActor::FindEntityAt(FVector2D Where, double Radius) const
{
	if (Network == nullptr || Radius <= 0.0)
	{
		return INDEX_NONE;
	}

	// Picked by the entity's own position - its stop mark - rather than by any anchor. An
	// anchor is where a vehicle parks; the stand is the thing being pointed at.
	double BestSquared = Radius * Radius;
	int32 Best = INDEX_NONE;

	const TArray<FEntityInstance>& Entities = Network->GetEntities();
	for (int32 Index = 0; Index < Entities.Num(); ++Index)
	{
		if (!Entities[Index].bAlive)
		{
			continue;
		}

		const double DistanceSquared = FVector2D::DistSquared(Entities[Index].Position, Where);
		if (DistanceSquared <= BestSquared)
		{
			BestSquared = DistanceSquared;
			Best = Index;
		}
	}
	return Best;
}

void ARoadNetworkActor::ClearNetwork()
{
	HideGhost();

	// Undoable, because clearing everything by accident is the worst thing the tool can do
	// and the only one with nothing left on screen to hint at what was lost.
	if (Network != nullptr)
	{
		FRoadEditScope Edit(HistoryForEdit(), Network, TEXT("clear network"));
		Edit.Commit();
	}

	// A fresh network rather than a drain: node removal bumps generations and prunes
	// incident lists, and none of that bookkeeping is worth doing on the way to empty.
	Network = NewObject<URoadNetwork>(this);
	RebuildMesh();
}

void ARoadNetworkActor::RebuildMesh()
{
	// Any real edit invalidates whatever the ghost was showing, and the cache key cannot
	// see it: a click that splits a segment can leave the cursor and the start node
	// exactly where they were, so every field the cache compares is unchanged while the
	// graph underneath is not. Cleared here because this is what every mutation ends with.
	LastGhostFrom = INDEX_NONE;

	if (Network == nullptr || MeshComponent == nullptr)
	{
		return;
	}

	// BEFORE the solve, because the solver reads it. Segments made in this session already
	// carry this profile; segments reloaded from a saved level carry null, because the
	// profile they were given lived in the transient package and never survived the save.
	// Handing it to the network repairs both cases through one accessor - see
	// URoadNetwork::ProfileFor, and Airside.Build.ProfileFallback for what it is worth.
	Network->DefaultProfile = ResolveProfile();

	const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Network);

	// The guideline graph is derived from the same solve, and until this call existed it
	// was derived NOWHERE outside the tests - so every route query at runtime ran against
	// an empty graph and correctly reported that nothing was connected.
	//
	// Anchor lead-ins go second and must: they join stands to guidelines that only exist
	// once the line above has run, and both are swept and rebuilt together.
	FRoadGuidelineBuilder::Build(*Network, Solved);
	FAnchorLink::Build(*Network);

	// THROUGH THE RESOLVERS, never the raw properties: an unset MaterialSet means "single
	// material", and the content default supplies one without the actor being altered to say
	// so. Reading MaterialSet directly here was fine; it was FILLING it that changed a level.
	URoadMaterialSet* const UseMaterials = ResolveMaterialSet();
	FRoadMeshBuilder Builder(SurfaceZ, TexelsPerUnit, UseMaterials);

	Builder.Build(*Network, Solved, RibbonSegments);

	// MaterialSet null keeps SurfaceMaterial and the single-slot path, so a level that has
	// not been given a set renders exactly as it did before per-band materials.
	FDynamicMeshSink Sink(MeshComponent, ResolveSurfaceMaterial(), bUseConstantVertexColour, UseMaterials);
	Builder.Emit(Sink);

	// Aprons share nothing with the roads and are built separately, but they are rebuilt
	// together so one call still means "make the world match the model".
	RebuildAprons();

	if (bDebugDrawMesh)
	{
		// Same buffers, a completely different route to the screen. If these lines land
		// where the clicks did and the surface does not, the fault is in the component or
		// the view, not the geometry - and if the lines are wrong too, every conclusion
		// drawn from vertex counts and bounds so far needs revisiting.
		const FRoadMeshBuffers& Drawn = Builder.GetBuffers();
		const float Lifetime = static_cast<float>(DebugDrawSeconds);
		for (int32 Slot = 0; Slot + 2 < Drawn.Indices.Num(); Slot += 3)
		{
			const FVector A = Drawn.Positions[Drawn.Indices[Slot]];
			const FVector B = Drawn.Positions[Drawn.Indices[Slot + 1]];
			const FVector C = Drawn.Positions[Drawn.Indices[Slot + 2]];

			DrawDebugLine(GetWorld(), A, B, FColor::Green, false, Lifetime, 0, 8.0f);
			DrawDebugLine(GetWorld(), B, C, FColor::Green, false, Lifetime, 0, 8.0f);
			DrawDebugLine(GetWorld(), C, A, FColor::Green, false, Lifetime, 0, 8.0f);
		}

		// The bounding box the renderer culls against, so an off-screen or collapsed box
		// is visible rather than merely reported.
		DrawDebugBox(GetWorld(), MeshComponent->Bounds.Origin,
			MeshComponent->Bounds.BoxExtent + FVector(0.0, 0.0, 50.0),
			FColor::Magenta, false, Lifetime, 0, 8.0f);
	}

	// Diagnostic-only from here: profile counts, the material slot list, distinct material
	// ids, profile names in use, and the final "Rebuilt:" line. Extracted to
	// RoadRebuildCensus so this function stays about DOING the rebuild, not reporting on
	// it - see the header there for why it takes exactly these five things instead of the
	// whole actor.
	RoadRebuildCensus::Log(*Network, Builder.GetBuffers(), *MeshComponent, Solved,
		ResolveSurfaceMaterial(), ResolveMaterialSet());
}

bool ARoadNetworkActor::ShouldTickIfViewportsOnly() const
{
	// Ticks in the EDITOR viewport, not only in play. The build tools work at design time,
	// so an agent dispatched at design time has to move at design time; without this the
	// cube spawns correctly and then stands still for ever.
	//
	// Scoped to non-game worlds so this says nothing about play, where ordinary ticking
	// already applies.
	const UWorld* World = GetWorld();
	return World != nullptr && !World->IsGameWorld();
}

void ARoadNetworkActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// Every handover (arrive -> taxi -> depart -> gone, or arrive -> taxi -> park) is owned
	// by FRoadAgent::Advance now - see its own comment. This loop is left with exactly one
	// job: hand the model's answer to the view, and drop an agent once it says Gone.
	for (int32 Index = Agents.Num() - 1; Index >= 0; --Index)
	{
		FAgentSlot& Slot = Agents[Index];

		FAgentMotion Motion;
		if (!Slot.Agent.Advance(DeltaSeconds, Motion))
		{
			// Cleared or otherwise finished - the aircraft has gone, so the actor goes with
			// it. An agent that stayed in the world would accumulate one per departure,
			// hanging above the airport for ever.
			if (Slot.View != nullptr)
			{
				Slot.View->Destroy();
			}
			Agents.RemoveAt(Index);
			continue;
		}

		if (Slot.View != nullptr)
		{
			Slot.View->SetMotion(Motion, SurfaceZ);
		}
	}
}

bool ARoadNetworkActor::DispatchArrival(const FVector2D& Near, const FAirframe& Airframe)
{
	UWorld* World = GetWorld();
	if (World == nullptr || Network == nullptr)
	{
		return false;
	}

	// WHICH RUNWAY, WHICH EXIT, WHICH STAND - none of that needs a world, so issue #29 moved
	// it to Model/ArrivalPlanner. This is left with arming, spawning and logging the plan.
	const FArrivalPlan Plan = ArrivalPlanner::Plan(*Network, Near, Airframe);

	// Reported whether or not this succeeds, because a refusal that does not say which of
	// these was the problem is a feature that "does nothing". Skipped only for NoRunway,
	// which found no runway at all - every other field here is meaningless until one is.
	if (Plan.Why != EArrivalRefusal::NoRunway)
	{
		UE_LOG(LogAirsideTraffic, Log,
			TEXT("Arrival: runway %s, %.0f uu long, %.0f needed to stop, %d usable exit(s), ")
			TEXT("%d stand(s) on the airport."),
			*RunwayDesignator::ToPairText(Plan.Direction), Plan.RunwayLength, Plan.Needed,
			Plan.ExitCount, Network->GetEntities().Num());
	}

	if (!Plan.IsValid())
	{
		UE_LOG(LogAirsideTraffic, Warning, TEXT("%s"), *ArrivalPlanner::DescribeRefusal(Plan));
		return false;
	}

	FRoadAgent Agent;
	if (!Agent.StartArrival(Plan.Threshold, Plan.Direction, Plan.RunwayLength, Airframe, Plan.VacateAt, Plan.TaxiIn))
	{
		// FLandingRun has already logged why. Nothing spawns: an arrival that cannot be
		// flown must leave no aircraft in the world, rather than one frozen on final.
		return false;
	}

	// FRoadAgent is world-free and cannot read this actor's UPROPERTY, so the pause is copied in here.
	Agent.ShutdownPause = ShutdownPauseSeconds;
	FActorSpawnParameters Params;
	Params.Owner = this;
	Params.ObjectFlags |= RF_Transient;
	FAgentSlot Slot;
	Slot.View = World->SpawnActor<ARoadAgentActor>(FVector::ZeroVector, FRotator::ZeroRotator, Params);
	if (Slot.View == nullptr)
	{
		return false;
	}
	// The airframe MESH, not to be confused with the FAirframe performance struct above.
	if (const UAirsideContent* Content = UAirsideSettings::GetContent())
	{
		Slot.View->SetAirframe(Content->AgentMesh.LoadSynchronous(), Content->AgentAnimClass.LoadSynchronous());
	}

	// Posed before its first tick so it never appears at the origin - zero delta asks where it starts.
	FAgentMotion Motion;
	if (Agent.Advance(0.0, Motion))
	{
		Slot.View->SetMotion(Motion, SurfaceZ);
	}

	UE_LOG(LogAirsideTraffic, Log,
		TEXT("Arrival on runway %s: %.0f uu available, %.0f needed, vacating at exit %d of %d, ")
		TEXT("taxiing %.0f uu to a stand."),
		*RunwayDesignator::ToPairText(Plan.Direction), Plan.RunwayLength, Plan.Needed,
		Plan.ExitOrdinal, Plan.ExitCount, Plan.TaxiIn.Length);

	Slot.Agent = MoveTemp(Agent);
	Agents.Add(MoveTemp(Slot));
	return true;
}

bool ARoadNetworkActor::DispatchAgent(const FRoutePlan& Plan, const FAirframe& Airframe)
{
	if (!Plan.IsValid() || Plan.Polyline.Num() < 2)
	{
		return false;
	}

	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return false;
	}

	// Editor worlds included, deliberately. This first refused outside a game world on the
	// grounds that an editor world would SAVE the cubes - but they are spawned RF_Transient
	// below, so they were never going to be saved and the guard was protecting against
	// nothing. What it DID do was make the tool look broken in the one place the build
	// tools are actually used: the ed mode, where pressing 4 and clicking twice drew a
	// route and produced no cube, with nothing on screen to say why.
	//
	// See ShouldTickIfViewportsOnly: an actor does not tick in an editor world unless it
	// asks to, so allowing the spawn without that would have left a cube frozen at its
	// start - which is a worse lie than no cube at all.

	FRoadAgent Agent;
	Agent.StartTaxi(Plan, Airframe);

	// FRoadAgent is world-free and cannot read this actor's UPROPERTY for itself, so the
	// pause is copied in at dispatch - the only time the two ever need to meet.
	Agent.ShutdownPause = ShutdownPauseSeconds;

	// DOES THIS ROUTE END ON A RUNWAY? Asked here rather than by the tool, because the answer
	// is a fact about the network and the last polyline point is the only thing that knows
	// where the route actually finished. A route that ends anywhere else simply taxis, which
	// is what every route did before departures existed.
	if (Network != nullptr && Plan.Polyline.Num() > 0 && Airframe.Climb.IsSet())
	{
		FVector2D Threshold;
		FVector2D Direction;
		double Length = 0.0;
		if (Network->RunwayExtentAt(Plan.Polyline.Last(), Threshold, Direction, Length))
		{
			Agent.ArmDeparture(Threshold, Direction, Length);

			UE_LOG(LogAirsideTraffic, Log,
				TEXT("Route ends on runway %s: %.0f uu available, departure armed"),
				*RunwayDesignator::ToPairText(Direction), Length);
		}
	}

	FActorSpawnParameters Params;
	Params.Owner = this;
	Params.ObjectFlags |= RF_Transient;

	FAgentSlot Slot;
	Slot.View = World->SpawnActor<ARoadAgentActor>(
		FVector::ZeroVector, FRotator::ZeroRotator, Params);
	if (Slot.View == nullptr)
	{
		return false;
	}

	// The airframe is pushed in, like the pose. A view that fetched its own mesh by path was
	// how a content move turned every aircraft into a cube - see ARoadAgentActor::SetAirframe.
	if (const UAirsideContent* Content = UAirsideSettings::GetContent())
	{
		Slot.View->SetAirframe(Content->AgentMesh.LoadSynchronous(),
			Content->AgentAnimClass.LoadSynchronous());
	}

	// Posed before the first tick, so it appears at its start rather than at the origin for
	// one frame. Zero delta asks Advance for where the taxi starts without moving it; the
	// fallback FRoadAgent::StartTaxi left in LastMotion covers a plan too short to advance.
	{
		FAgentMotion Motion;
		if (Agent.Advance(0.0, Motion))
		{
			Slot.View->SetMotion(Motion, SurfaceZ);
		}
	}

	Slot.Agent = MoveTemp(Agent);
	Agents.Add(MoveTemp(Slot));
	return true;
}

void ARoadNetworkActor::ClearAgents()
{
	for (FAgentSlot& Slot : Agents)
	{
		if (Slot.View != nullptr)
		{
			Slot.View->Destroy();
		}
	}

	Agents.Reset();
}

FRoutePlan ARoadNetworkActor::FindRoute(
	FGuidelineNodeId Start, FGuidelineNodeId Goal,
	ETraversalClass Class, double Wingspan) const
{
	if (Network == nullptr)
	{
		return FRoutePlan();
	}

	FRouteQuery Query;
	Query.Start = Start;
	Query.Goal = Goal;
	Query.Class = Class;
	Query.Wingspan = Wingspan;

	return RouteSearch::Find(*Network, Query);
}
