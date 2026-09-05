#include "Present/RoadSurfacePresenter.h"

#include "AirsideLog.h"
#include "Build/AnchorLink.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadMeshBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Components/DynamicMeshComponent.h"
#include "Debug/RoadRebuildCensus.h"
#include "DrawDebugHelpers.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadSlotMap.h"
#include "Present/DynamicMeshSink.h"
#include "Present/RoadEditFacade.h"
#include "Profiles/RoadProfile.h"

namespace
{
	/**
	 * Mirrors URoadEditFacade::MakeLiveNodeId - see that function for why IsSet() alone is
	 * not enough. Kept as its own small copy rather than a shared call into the facade: the
	 * presenter must not depend on the facade for anything but the one static graph-surgery
	 * function it already shares (SplitSegmentIn) - reaching into it for this too would make
	 * "how does a presenter validate a node" a second, growing surface between the two.
	 */
	bool MakeLiveNodeIdIn(const URoadNetwork& Network, int32 Index, FRoadNodeId& OutId)
	{
		if (!Network.GetNodes().IsValidIndex(Index))
		{
			return false;
		}

		FRoadNodeId Candidate;
		Candidate.Index = Index;
		Candidate.Generation = Network.GetNodes()[Index].Generation;

		if (!RoadSlot::IsValid<FRoadNodeId, FRoadNode>(Network.GetNodes(), Candidate))
		{
			return false;
		}

		OutId = Candidate;
		return true;
	}
}

void URoadSurfacePresenter::Initialize(UDynamicMeshComponent* InMeshComponent,
	UDynamicMeshComponent* InGhostComponent, UDynamicMeshComponent* InApronComponent)
{
	MeshComponent = InMeshComponent;
	GhostComponent = InGhostComponent;
	ApronComponent = InApronComponent;
}

int32 URoadSurfacePresenter::SurfaceTriangleCountForTest() const
{
	if (MeshComponent == nullptr || MeshComponent->GetDynamicMesh() == nullptr)
	{
		return 0;
	}
	return MeshComponent->GetDynamicMesh()->GetMeshRef().TriangleCount();
}

double URoadSurfacePresenter::GetApronSurfaceZ(double SurfaceZ, double ApronZOffset) const
{
	// Never more than halfway down to the ground plane, whatever ApronZOffset asks for.
	// SurfaceZ is already a height above that plane, so half of it is the most that can be
	// given away while still leaving the apron above ground.
	const double Drop = FMath::Min(ApronZOffset, FMath::Max(SurfaceZ, 0.0) * 0.5);
	return SurfaceZ - Drop;
}

void URoadSurfacePresenter::RebuildAprons(URoadNetwork& Network, const FSurfaceSettings& Settings)
{
	if (ApronComponent == nullptr)
	{
		return;
	}

	// Its own builder instance, so an apron corner that happens to land exactly on a road
	// vertex cannot weld to it. The two surfaces meet; they are not one surface.
	const double ApronZ = GetApronSurfaceZ(Settings.SurfaceZ, Settings.ApronZOffset);
	FRoadMeshBuilder Builder(ApronZ, Settings.TexelsPerUnit);

	int32 Built = 0;
	for (const FApronSurface& Apron : Network.GetAprons())
	{
		if (Apron.bAlive)
		{
			Builder.AddApron(Apron);
			++Built;
		}
	}

	const FRoadMeshBuffers& Buffers = Builder.GetBuffers();

	FDynamicMeshSink Sink(ApronComponent,
		Settings.ApronMaterial != nullptr ? Settings.ApronMaterial : Settings.SurfaceMaterial,
		Settings.bUseConstantApronColour);
	Sink.Accept(Buffers);

	ApronComponent->SetVisibility(Built > 0);

	// Reported rather than inferred. An apron that is built and never seen, and one that
	// is never built, look identical from outside - and every explanation reasoned from
	// engine source about the roads was wrong before the numbers were printed.
	UE_LOG(LogRoadMesh, Log,
		TEXT("Aprons: %d surface(s), %d triangle(s) at Z=%.1f, material %s%s"),
		Built, Buffers.Indices.Num() / 3, ApronZ,
		Settings.ApronMaterial != nullptr ? *Settings.ApronMaterial->GetName() : TEXT("<fallback>"),
		Settings.bUseConstantApronColour ? TEXT(" (CONSTANT COLOUR - material overridden)") : TEXT(""));

	// Worth saying out loud: a road surface this close to the ground leaves nothing to
	// separate the two surfaces with, and both will z-fight at distance whatever is done
	// here. The apron is above ground either way - this is about the ROAD being too low.
	if (Built > 0 && Settings.SurfaceZ < Settings.ApronZOffset * 2.0)
	{
		UE_LOG(LogRoadMesh, Warning,
			TEXT("SurfaceZ is %.1f, less than twice ApronZOffset (%.1f): the apron is squeezed ")
			TEXT("to Z=%.1f. Raise SurfaceZ for a cleaner separation."),
			Settings.SurfaceZ, Settings.ApronZOffset, ApronZ);
	}

	if (Settings.bDebugDrawAprons && GetWorld() != nullptr)
	{
		// A completely separate route to the screen, from the same buffers the component
		// was handed.
		const float Lifetime = static_cast<float>(Settings.DebugDrawSeconds);
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

void URoadSurfacePresenter::Rebuild(URoadNetwork& Network, const FSurfaceSettings& Settings)
{
	// Any real edit invalidates whatever the ghost was showing, and the cache key cannot
	// see it: a click that splits a segment can leave the cursor and the start node
	// exactly where they were, so every field the cache compares is unchanged while the
	// graph underneath is not. Cleared here because this is what every mutation ends with.
	LastGhostFrom = INDEX_NONE;

	if (MeshComponent == nullptr)
	{
		return;
	}

	// BEFORE the solve, because the solver reads it. Segments made in this session already
	// carry this profile; segments reloaded from a saved level carry null, because the
	// profile they were given lived in the transient package and never survived the save.
	// Handing it to the network repairs both cases through one accessor - see
	// URoadNetwork::ProfileFor, and Airside.Build.ProfileFallback for what it is worth.
	// Settings.Profile is ARoadNetworkActor::ResolveProfile()'s OUTPUT, never resolved here:
	// this class must not decide content defaults, only draw what it is told.
	Network.DefaultProfile = Settings.Profile;

	const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(Network);

	// The guideline graph is derived from the same solve, and until this call existed it
	// was derived NOWHERE outside the tests - so every route query at runtime ran against
	// an empty graph and correctly reported that nothing was connected.
	//
	// Anchor lead-ins go second and must: they join stands to guidelines that only exist
	// once the line above has run, and both are swept and rebuilt together.
	FRoadGuidelineBuilder::Build(Network, Solved);
	FAnchorLink::Build(Network);

	// THROUGH THE RESOLVED SETTING, never a raw property: an unset MaterialSet means "single
	// material", and ARoadNetworkActor::ResolveMaterialSet supplies a content default without
	// the actor being altered to say so - see that function for why. This class only ever
	// sees the result.
	FRoadMeshBuilder Builder(Settings.SurfaceZ, Settings.TexelsPerUnit, Settings.MaterialSet);
	Builder.Build(Network, Solved, Settings.RibbonSegments);

	// MaterialSet null keeps SurfaceMaterial and the single-slot path, so a level that has
	// not been given a set renders exactly as it did before per-band materials.
	FDynamicMeshSink Sink(MeshComponent, Settings.SurfaceMaterial, Settings.bUseConstantVertexColour,
		Settings.MaterialSet);
	Builder.Emit(Sink);

	// Aprons share nothing with the roads and are built separately, but they are rebuilt
	// together so one call still means "make the world match the model".
	RebuildAprons(Network, Settings);

	if (Settings.bDebugDrawMesh)
	{
		// Same buffers, a completely different route to the screen. If these lines land
		// where the clicks did and the surface does not, the fault is in the component or
		// the view, not the geometry - and if the lines are wrong too, every conclusion
		// drawn from vertex counts and bounds so far needs revisiting.
		const FRoadMeshBuffers& Drawn = Builder.GetBuffers();
		const float Lifetime = static_cast<float>(Settings.DebugDrawSeconds);
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
	// ids, profile names in use, and the final "Rebuilt:" line. Kept behind RoadRebuildCensus
	// so this function stays about DOING the rebuild, not reporting on it.
	RoadRebuildCensus::Log(Network, Builder.GetBuffers(), *MeshComponent, Solved,
		Settings.SurfaceMaterial, Settings.MaterialSet);
}

UMaterialInstanceDynamic* URoadSurfacePresenter::GhostMaterialInstance(UMaterialInterface* GhostMaterialBase)
{
	if (GhostMID == nullptr && GhostMaterialBase != nullptr)
	{
		GhostMID = UMaterialInstanceDynamic::Create(GhostMaterialBase, this);
	}
	return GhostMID;
}

void URoadSurfacePresenter::AddGhostJunction(
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

void URoadSurfacePresenter::HideGhost()
{
	if (GhostComponent != nullptr)
	{
		GhostComponent->SetVisibility(false);
	}
	bGhostVisible = false;
	LastGhostFrom = INDEX_NONE;
}

bool URoadSurfacePresenter::BuildGhostBuffers(URoadNetwork* Network, int32 FromNodeIndex,
	const FRoadSnapResult& Snap, const FSurfaceSettings& Settings, FRoadMeshBuffers& OutBuffers)
{
	FRoadNodeId From;
	if (Network == nullptr || !MakeLiveNodeIdIn(*Network, FromNodeIndex, From))
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
		// implementation with URoadEditFacade::SplitSegment is the only thing that stops
		// the preview and the edit diverging.
		To = URoadEditFacade::SplitSegmentIn(*GhostNetwork, Snap.Segment, Snap.Position);
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

	const FRoadSegmentId Ghosted = GhostNetwork->AddStraightSegment(From, To, Settings.Profile);
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
	FRoadMeshBuilder Builder(Settings.SurfaceZ + Settings.GhostZOffset, Settings.TexelsPerUnit);
	Builder.AddSegment(*GhostNetwork, Ghosted, Settings.RibbonSegments);
	AddGhostJunction(Builder, Solved, From.Index);
	AddGhostJunction(Builder, Solved, To.Index);

	OutBuffers = Builder.GetBuffers();
	return true;
}

void URoadSurfacePresenter::UpdateGhost(URoadNetwork* Network, int32 FromNodeIndex,
	const FRoadSnapResult& Snap, bool bValid, const FSurfaceSettings& Settings)
{
	FRoadNodeId From;
	if (Network == nullptr || GhostComponent == nullptr || !MakeLiveNodeIdIn(*Network, FromNodeIndex, From))
	{
		HideGhost();
		return;
	}

	// A drag holds still for most of its frames. Rebuilding then means duplicating the
	// network and re-solving it to produce exactly the same triangles, sixty times a
	// second. Cleared by Rebuild, so any real edit invalidates it.
	if (bGhostVisible
		&& FromNodeIndex == LastGhostFrom
		&& Snap.Kind == LastGhostKind
		&& Snap.Position == LastGhostTo)
	{
		if (bValid != bLastGhostValid)
		{
			// Geometry untouched: this is the whole reason validity is a material
			// parameter rather than a second mesh.
			if (UMaterialInstanceDynamic* Instance = GhostMaterialInstance(Settings.GhostMaterial))
			{
				Instance->SetScalarParameterValue(TEXT("ValidityBlend"), bValid ? 0.0f : 1.0f);
			}
			bLastGhostValid = bValid;
		}
		return;
	}

	FRoadMeshBuffers Buffers;
	if (!BuildGhostBuffers(Network, FromNodeIndex, Snap, Settings, Buffers))
	{
		HideGhost();
		return;
	}

	if (UMaterialInstanceDynamic* Instance = GhostMaterialInstance(Settings.GhostMaterial))
	{
		Instance->SetScalarParameterValue(TEXT("ValidityBlend"), bValid ? 0.0f : 1.0f);

		// The material cannot know where this road's edge is; UV1.X is in uu and the
		// profile owns the half-width. Left at its default a narrow road would glow from
		// edge to edge and a wide one not at all.
		if (const URoadProfile* Used = Settings.Profile)
		{
			Instance->SetScalarParameterValue(TEXT("EdgeHalfWidth"),
				static_cast<float>(FMath::Max(Used->GetHalfWidthLeft(), Used->GetHalfWidthRight())));
		}
	}

	// bUseConstantVertexColour false, and it matters: any ColorOverrideMode other than
	// None makes the scene proxy substitute the engine's vertex-colour debug material for
	// ours, so the ghost would render as flat opaque grey with none of its parameters.
	FDynamicMeshSink Sink(GhostComponent, GhostMaterialInstance(Settings.GhostMaterial), /*bUseConstantVertexColour*/ false);
	Sink.Accept(Buffers);

	GhostComponent->SetVisibility(true);

	bGhostVisible = true;
	LastGhostFrom = FromNodeIndex;
	LastGhostTo = Snap.Position;
	LastGhostKind = Snap.Kind;
	bLastGhostValid = bValid;
}
