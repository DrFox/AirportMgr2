#include "Present/RoadNetworkActor.h"

#include "Build/RoadMeshBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Components/DynamicMeshComponent.h"
#include "Components/SceneComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DrawDebugHelpers.h"
#include "DynamicMesh/MeshNormals.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadSlotMap.h"
#include "Profiles/RoadProfile.h"
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

void FDynamicMeshSink::Accept(const FRoadMeshBuffers& Buffers)
{
	if (Component == nullptr)
	{
		return;
	}

	UE::Geometry::FDynamicMesh3 Mesh;
	Mesh.Clear();

	for (const FVector3d& Position : Buffers.Positions)
	{
		Mesh.AppendVertex(Position);
	}

	int32 Rejected = 0;
	for (int32 Slot = 0; Slot + 2 < Buffers.Indices.Num(); Slot += 3)
	{
		const int32 Result = Mesh.AppendTriangle(
			Buffers.Indices[Slot], Buffers.Indices[Slot + 1], Buffers.Indices[Slot + 2]);

		// AppendTriangle REFUSES rather than throws: negative results mean the triangle
		// was non-manifold or a duplicate. Silently ignoring that would leave holes in
		// the surface that look exactly like the cracks this system exists to remove.
		if (Result < 0)
		{
			++Rejected;
		}
	}

	if (Rejected > 0)
	{
		UE_LOG(LogRoadMesh, Warning,
			TEXT("%d triangle(s) rejected as non-manifold or duplicate - the surface will have holes"),
			Rejected);
	}

	PopulateAttributes(Mesh, Buffers);

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
	if (Material != nullptr)
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
	PrimaryActorTick.bCanEverTick = false;

	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));

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
	const FRoadNodeId Node = EnsureNetwork().AddNode(Where);
	if (!Node.IsSet())
	{
		UE_LOG(LogRoadMesh, Warning, TEXT("PlaceNode refused at (%f, %f)"), Where.X, Where.Y);
		return INDEX_NONE;
	}
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

	// Straight only. The model stores a Bezier control point, but AddSegment still
	// interpolates its interior samples in a straight line, so a curve authored here
	// would render as a chord until slice 2b samples the curve properly.
	const FRoadSegmentId Segment = Network->AddStraightSegment(From, To, ResolveProfile());
	if (!Segment.IsSet())
	{
		UE_LOG(LogRoadMesh, Warning, TEXT("ConnectNodes refused: %d -> %d"), FromIndex, ToIndex);
		return false;
	}
	return true;
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

	const FRoadNodeId Middle = SplitSegmentIn(*Network, Doomed, At);
	if (!Middle.IsSet())
	{
		UE_LOG(LogRoadMesh, Warning,
			TEXT("SplitSegment refused: segment %d at (%f, %f)"), SegmentIndex, At.X, At.Y);
		return INDEX_NONE;
	}
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

void ARoadNetworkActor::ClearNetwork()
{
	HideGhost();

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

	FRoadMeshBuilder Builder(SurfaceZ, TexelsPerUnit);

	Builder.Build(*Network, Solved, RibbonSegments);

	FDynamicMeshSink Sink(MeshComponent, SurfaceMaterial, bUseConstantVertexColour);
	Builder.Emit(Sink);

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
