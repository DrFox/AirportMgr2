#include "Present/RoadNetworkActor.h"

#include "Build/RoadMeshBuilder.h"
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
#include "UObject/ConstructorHelpers.h"

DEFINE_LOG_CATEGORY_STATIC(LogRoadMesh, Log, All);

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
		Component->SetMaterial(0, Material);
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

	// Resolved by path rather than left for a Blueprint to assign, so a freshly placed
	// actor renders as asphalt with no setup at all. If the asset is missing this stays
	// null and Accept falls back to the engine default plus a colour override - which
	// degrades quietly, so a missing material looks like the old placeholder rather than
	// like an error.
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> RoadMaterial(
		TEXT("/Game/RoadNet/Materials/M_RoadSurface"));
	if (RoadMaterial.Succeeded())
	{
		SurfaceMaterial = RoadMaterial.Object;
	}

	// Defaulted here for the same reason as the materials above, and one more: this actor
	// lives in a level that is deliberately never saved, so a set assigned by hand in the
	// Details panel would be gone at the next editor start. A class default survives.
	//
	// Missing keeps MaterialSet null, which is the supported single-material state - the
	// road renders as it did before per-band materials, rather than as an error.
	static ConstructorHelpers::FObjectFinder<URoadMaterialSet> RoadMaterials(
		TEXT("/Game/RoadNet/DA_RoadMaterials"));
	if (RoadMaterials.Succeeded())
	{
		MaterialSet = RoadMaterials.Object;
	}

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

	static ConstructorHelpers::FObjectFinder<UEntityDefinition> StandAsset(
		TEXT("/Game/RoadNet/Entities/DA_Stand_CodeC"));
	if (StandAsset.Succeeded())
	{
		StandDefinition = StandAsset.Object;
	}

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> ApronAsset(
		TEXT("/Game/RoadNet/Materials/M_ApronConcrete"));
	if (ApronAsset.Succeeded())
	{
		ApronMaterial = ApronAsset.Object;
	}

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> GhostAsset(
		TEXT("/Game/RoadNet/Materials/M_RoadGhost"));
	if (GhostAsset.Succeeded())
	{
		GhostMaterial = GhostAsset.Object;
	}
}

URoadNetwork& ARoadNetworkActor::EnsureNetwork()
{
	if (Network == nullptr)
	{
		Network = NewObject<URoadNetwork>(this);
	}
	return *Network;
}

#if WITH_EDITOR
void ARoadNetworkActor::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();

	// USceneComponent keeps its sprite protected, so the owner cannot reach it by name -
	// but it is a component of this actor, so it can be found by type. Hidden rather than
	// destroyed: the engine re-creates it on the next OnRegister, and a component destroyed
	// out from under the thing that owns the pointer is a worse bargain than a hidden one.
	for (UBillboardComponent* Billboard : TInlineComponentArray<UBillboardComponent*>(this))
	{
		Billboard->SetVisibility(false);
	}
}
#endif

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

URoadProfile* ARoadNetworkActor::ResolveProfile()
{
	if (Profile != nullptr)
	{
		return Profile;
	}

	if (RuntimeProfile == nullptr)
	{
		// A tenth of the width per side. Without a Shoulder band the profile has no outer
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
	if (GhostMID == nullptr && GhostMaterial != nullptr)
	{
		GhostMID = UMaterialInstanceDynamic::Create(GhostMaterial, this);
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
		ApronMaterial != nullptr ? ApronMaterial : SurfaceMaterial,
		bUseConstantApronColour);
	Sink.Accept(Buffers);

	ApronComponent->SetVisibility(Built > 0);

	// Reported rather than inferred. An apron that is built and never seen, and one that
	// is never built, look identical from outside - and every explanation reasoned from
	// engine source about the roads was wrong before the numbers were printed.
	UE_LOG(LogRoadMesh, Log,
		TEXT("Aprons: %d surface(s), %d triangle(s) at Z=%.1f, material %s%s"),
		Built, Buffers.Indices.Num() / 3, GetApronSurfaceZ(),
		ApronMaterial != nullptr ? *ApronMaterial->GetName() : TEXT("<fallback>"),
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
	if (StandDefinition == nullptr)
	{
		UE_LOG(LogRoadMesh, Warning,
			TEXT("PlaceStand refused: no StandDefinition. Author DA_Stand_CodeC with "
				 "Tools/Python/build_stand_asset.py, or set one on the actor."));
		return INDEX_NONE;
	}

	URoadNetwork& Net = EnsureNetwork();
	FRoadEditScope Edit(HistoryForEdit(), &Net, TEXT("place stand"));

	const FEntityInstanceId Placed = Net.PlaceEntity(StandDefinition, Where, Heading);
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

	const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Network);

	// The guideline graph is derived from the same solve, and until this call existed it
	// was derived NOWHERE outside the tests - so every route query at runtime ran against
	// an empty graph and correctly reported that nothing was connected.
	//
	// Anchor lead-ins go second and must: they join stands to guidelines that only exist
	// once the line above has run, and both are swept and rebuilt together.
	FRoadGuidelineBuilder::Build(*Network, Solved);
	FAnchorLink::Build(*Network);

	FRoadMeshBuilder Builder(SurfaceZ, TexelsPerUnit, MaterialSet);

	Builder.Build(*Network, Solved, RibbonSegments);

	// MaterialSet null keeps SurfaceMaterial and the single-slot path, so a level that has
	// not been given a set renders exactly as it did before per-band materials.
	FDynamicMeshSink Sink(MeshComponent, SurfaceMaterial, bUseConstantVertexColour, MaterialSet);
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

	UE_LOG(LogRoadMesh, Log, TEXT("Rebuilt: %d nodes (%d failed), %d vertices, %d triangles"),
		Solved.SolvedNodes, Solved.FailedNodes,
		Builder.GetBuffers().Positions.Num(), Builder.GetBuffers().Indices.Num() / 3);
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

	for (FRoadAgent& Agent : Agents)
	{
		FVector2D At;
		double Heading = 0.0;

		// Advance is the ONLY thing that decides where an agent is. When it declines - no
		// route, or a polyline too short to have a direction - the pose is left exactly as
		// it was. Writing an unset FVector2D through here instead is how this project has
		// twice put things at the world origin.
		if (!Agent.Follower.Advance(DeltaSeconds, At, Heading))
		{
			continue;
		}

		if (Agent.View != nullptr)
		{
			Agent.View->SetPose(At, Heading, SurfaceZ);
		}
	}
}

bool ARoadNetworkActor::DispatchAgent(const FRoutePlan& Plan, double Speed)
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
	Agent.Follower.Start(Plan, Speed);

	FActorSpawnParameters Params;
	Params.Owner = this;
	Params.ObjectFlags |= RF_Transient;

	Agent.View = World->SpawnActor<ARoadAgentActor>(
		FVector::ZeroVector, FRotator::ZeroRotator, Params);
	if (Agent.View == nullptr)
	{
		return false;
	}

	// Posed before the first tick, so it appears at its start rather than at the origin
	// for one frame.
	Agent.View->SetPose(Plan.Polyline[0], 0.0, SurfaceZ);

	FVector2D At;
	double Heading = 0.0;
	if (Agent.Follower.Advance(0.0, At, Heading))
	{
		Agent.View->SetPose(At, Heading, SurfaceZ);
	}

	Agents.Add(Agent);
	return true;
}

void ARoadNetworkActor::ClearAgents()
{
	for (FRoadAgent& Agent : Agents)
	{
		if (Agent.View != nullptr)
		{
			Agent.View->Destroy();
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
