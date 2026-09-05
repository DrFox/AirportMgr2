#include "Present/DynamicMeshSink.h"

#include "AirsideLog.h"
#include "Components/DynamicMeshComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMesh/MeshNormals.h"
#include "Materials/Material.h"
#include "Profiles/RoadMaterialSet.h"

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
