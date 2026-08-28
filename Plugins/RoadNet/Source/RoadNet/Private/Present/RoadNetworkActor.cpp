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

	// The mesh carries a normal map, so its tangent frame has to come from UV0 rather
	// than from the default ExternallyProvided mode - which, finding no tangent space,
	// falls back to a frame derived from the normal alone. On a flat +Z road that frame
	// is constant and looks plausible, but its handedness relative to UV0 is accidental,
	// and it either cancels or compounds the deliberate green-channel flip on the
	// OpenGL-convention normal map. Derived beats lucky.
	MeshComponent->SetTangentsType(EDynamicMeshComponentTangentsMode::AutoCalculated);

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
		RuntimeProfile = URoadProfile::MakeTransient(FallbackWidth, FallbackFilletRadius);
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

void ARoadNetworkActor::ClearNetwork()
{
	// A fresh network rather than a drain: node removal bumps generations and prunes
	// incident lists, and none of that bookkeeping is worth doing on the way to empty.
	Network = NewObject<URoadNetwork>(this);
	RebuildMesh();
}

void ARoadNetworkActor::RebuildMesh()
{
	if (Network == nullptr || MeshComponent == nullptr)
	{
		return;
	}

	const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Network);

	FRoadMeshBuilder Builder(SurfaceZ);

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
