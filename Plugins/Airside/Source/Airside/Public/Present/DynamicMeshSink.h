#pragma once

#include "CoreMinimal.h"
#include "Build/RoadMeshSink.h"

class UDynamicMeshComponent;
class UMaterialInterface;
class URoadMaterialSet;
class FRoadMeshBuilder;

namespace UE::Geometry { class FDynamicMesh3; }

/**
 * Pushes finished buffers into a UDynamicMeshComponent.
 *
 * Split out of RoadNetworkActor.h by issue #32: this is the one place that knows how a
 * FRoadMeshBuffers becomes a rendered FDynamicMesh3, and every one of URoadSurfacePresenter's
 * three surfaces (road, apron, ghost) goes through it, so it belongs next to none of them in
 * particular. Not a UCLASS - it holds only non-owning raw pointers and is stack-scoped at
 * every call site, so it needs neither reflection nor GC.
 */
class AIRSIDE_API FDynamicMeshSink : public IRoadMeshSink
{
public:
	/**
	 * InMaterials null keeps the single-material path: one SetMaterial(0, InMaterial) and
	 * no material-ID attribute, exactly as before per-band materials existed.
	 */
	explicit FDynamicMeshSink(UDynamicMeshComponent* InComponent, UMaterialInterface* InMaterial = nullptr,
		bool bInUseConstantVertexColour = true, const URoadMaterialSet* InMaterials = nullptr)
		: Component(InComponent), Material(InMaterial)
		, bUseConstantVertexColour(bInUseConstantVertexColour), Materials(InMaterials) {}
	virtual void Accept(const FRoadMeshBuffers& Buffers) override;

	/**
	 * Copy the buffers' UV, colour and material-id channels onto an already-populated mesh.
	 *
	 * Static and public so it can be tested without a component, a world or a renderer.
	 * The buffers being correct says nothing about what the component receives, and that
	 * gap is precisely where slice 2a's invisible surface hid.
	 */
	static void PopulateAttributes(UE::Geometry::FDynamicMesh3& Mesh, const FRoadMeshBuffers& Buffers);

	/**
	 * Convert whole buffers into a mesh - vertices, triangles, UVs and material ids.
	 * Returns the number of triangles FDynamicMesh3 refused.
	 *
	 * Static and public for the same reason as PopulateAttributes, and it carries the one
	 * correspondence in this file that is NOT the identity: FDynamicMesh3::AppendTriangle
	 * REFUSES non-manifold and duplicate triangles, so one refusal shifts every later
	 * triangle's id away from its buffer index. Indexing MaterialIDs by mesh triangle id
	 * would then re-skin the entire mesh downstream of the first refusal, silently. That
	 * is the index-parallel defect this codebase has already paid for once, in
	 * FEntityInstance::ResolvedAnchors; here the mapping is recorded as the triangles are
	 * appended, where it is known, rather than assumed afterwards.
	 */
	static int32 BuildMesh(UE::Geometry::FDynamicMesh3& Mesh, const FRoadMeshBuffers& Buffers);

private:
	// Raw, non-owning pointers: the sink owns and GC-protects neither, and must not
	// outlive either. Both current call sites are stack-scoped inside a single function,
	// so this is safe today; a preview sink that lives across frames will not be.
	UDynamicMeshComponent* Component = nullptr;
	UMaterialInterface* Material = nullptr;

	/** Independent of Material by design - see URoadSurfacePresenter::FSurfaceSettings::
	 *  bUseConstantVertexColour, or ARoadNetworkActor::bUseConstantVertexColour for the
	 *  full ForceOverrideMaterial explanation. */
	bool bUseConstantVertexColour = true;

	/** Null means the single-material path. Non-owning, like Component and Material. */
	const URoadMaterialSet* Materials = nullptr;
};
