#include "Present/RoadSurfacePresenter.h"

#include "AirsideLog.h"
#include "Build/AnchorLink.h"
#include "Build/HoldingPositionMarkingBuilder.h"
#include "Build/RoadLaneMarkingBuilder.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadMeshBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Build/RunwayMarkingBuilder.h"
#include "Build/StandMarkingBuilder.h"
#include "Components/DynamicMeshComponent.h"
#include "Debug/RoadRebuildCensus.h"
#include "DrawDebugHelpers.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadSlotMap.h"
#include "Present/DynamicMeshSink.h"
#include "Profiles/RoadMaterialSet.h"
#include "Profiles/RoadProfile.h"

/**
 * The material parameter names this presenter drives, named once rather than retyped as a
 * TEXT() literal at every SetVectorParameterValue/SetScalarParameterValue call site (#103) -
 * a typo in one copy would silently stop driving the parameter rather than fail to compile.
 */
namespace RoadMaterialParams
{
	constexpr const TCHAR* MarkingColor = TEXT("MarkingColor");
	constexpr const TCHAR* ValidityBlend = TEXT("ValidityBlend");
	constexpr const TCHAR* EdgeHalfWidth = TEXT("EdgeHalfWidth");
}

namespace
{
	/**
	 * Rounds a ghost cache key to the nearest uu (#166). The cache used to compare the raw
	 * cursor position exactly, which a held mouse still fails every tick - sub-uu jitter is
	 * real noise from a real input device, not a decision to move the ghost. One uu is far
	 * below anything a cursor can express on purpose and far above that noise, so this is
	 * the cache KEY only; the geometry BuildGhostBuffers actually builds on a miss still
	 * uses the raw, unquantised Snap.Position.
	 */
	FVector2D QuantiseGhostPosition(const FVector2D& Position)
	{
		return FVector2D(FMath::GridSnap<double>(Position.X, 1.0), FMath::GridSnap<double>(Position.Y, 1.0));
	}
}

void URoadSurfacePresenter::Initialize(
	const TStaticArray<TObjectPtr<UDynamicMeshComponent>, static_cast<int32>(ESurfaceLayer::Count)>& Components)
{
	LayerComponents = TArray<TObjectPtr<UDynamicMeshComponent>>(Components.GetData(), Components.Num());
}

UDynamicMeshComponent* URoadSurfacePresenter::GetLayerComponent(ESurfaceLayer Layer) const
{
	const int32 Index = static_cast<int32>(Layer);
	return LayerComponents.IsValidIndex(Index) ? LayerComponents[Index].Get() : nullptr;
}

int32 URoadSurfacePresenter::RebuildLayer(ESurfaceLayer Layer, TFunctionRef<int32(FRoadMeshBuffers&)> BuildFn,
	UMaterialInterface* Material, bool bUseConstantColour, FRoadMeshBuffers& OutBuffers, bool bQuiet)
{
	UDynamicMeshComponent* Component = GetLayerComponent(Layer);
	if (Component == nullptr)
	{
		return INDEX_NONE;
	}

	const int32 Count = BuildFn(OutBuffers);

	FDynamicMeshSink Sink(Component, Material, bUseConstantColour, nullptr, bQuiet);
	Sink.Accept(OutBuffers);
	Component->SetVisibility(Count > 0);
	return Count;
}

void URoadSurfacePresenter::DebugDrawTriangles(const FRoadMeshBuffers& Buffers, FColor Colour, float Thickness, double Seconds) const
{
	if (GetWorld() == nullptr)
	{
		return;
	}
	const float Lifetime = static_cast<float>(Seconds);
	for (int32 Slot = 0; Slot + 2 < Buffers.Indices.Num(); Slot += 3)
	{
		const FVector A = Buffers.Positions[Buffers.Indices[Slot]];
		const FVector B = Buffers.Positions[Buffers.Indices[Slot + 1]];
		const FVector C = Buffers.Positions[Buffers.Indices[Slot + 2]];

		// Thickness in WORLD units. Single digits are sub-pixel across a scene this large -
		// indistinguishable from nothing being drawn at all.
		DrawDebugLine(GetWorld(), A, B, Colour, false, Lifetime, 0, Thickness);
		DrawDebugLine(GetWorld(), B, C, Colour, false, Lifetime, 0, Thickness);
		DrawDebugLine(GetWorld(), C, A, Colour, false, Lifetime, 0, Thickness);
	}
}

int32 URoadSurfacePresenter::SurfaceTriangleCountForTest() const
{
	UDynamicMeshComponent* Component = GetLayerComponent(ESurfaceLayer::Road);
	if (Component == nullptr || Component->GetDynamicMesh() == nullptr)
	{
		return 0;
	}
	return Component->GetDynamicMesh()->GetMeshRef().TriangleCount();
}

int32 URoadSurfacePresenter::RunwayMarkingTriangleCountForTest() const
{
	UDynamicMeshComponent* Component = GetLayerComponent(ESurfaceLayer::RunwayPaint);
	if (Component == nullptr || Component->GetDynamicMesh() == nullptr)
	{
		return 0;
	}
	return Component->GetDynamicMesh()->GetMeshRef().TriangleCount();
}

int32 URoadSurfacePresenter::RunwayRubberTriangleCountForTest() const
{
	UDynamicMeshComponent* Component = GetLayerComponent(ESurfaceLayer::RunwayRubber);
	if (Component == nullptr || Component->GetDynamicMesh() == nullptr)
	{
		return 0;
	}
	return Component->GetDynamicMesh()->GetMeshRef().TriangleCount();
}

int32 URoadSurfacePresenter::HoldingPaintTriangleCountForTest() const
{
	UDynamicMeshComponent* Component = GetLayerComponent(ESurfaceLayer::HoldingPaint);
	if (Component == nullptr || Component->GetDynamicMesh() == nullptr)
	{
		return 0;
	}
	return Component->GetDynamicMesh()->GetMeshRef().TriangleCount();
}

const URoadMaterialSet* URoadSurfacePresenter::EffectiveMaterialSet(const FSurfaceSettings& Settings)
{
	if (EffectiveSet == nullptr)
	{
		EffectiveSet = NewObject<URoadMaterialSet>(this, NAME_None, RF_Transient);
	}
	EffectiveSet->Slots.Reset();

	if (Settings.MaterialSet != nullptr)
	{
		// The authored slots FIRST and UNCHANGED - see the header: their indices are the
		// ids the bands resolve to, and must survive the append.
		EffectiveSet->Slots.Append(Settings.MaterialSet->Slots);
	}
	else
	{
		// The single-material road: one slot, the surface material, id 0 - which is what
		// every band of a taxiway resolves to without a set, exactly as before.
		FRoadMaterialSlot Surface;
		Surface.Name = TEXT("Surface");
		Surface.Material = Settings.SurfaceMaterial;
		EffectiveSet->Slots.Add(Surface);
	}

	// The runway surfaces, appended, each falling back to the surface material so a
	// project without the runway materials authored still draws its runways as roads. An
	// authored set that already declares one of these names keeps its own binding: the
	// name resolves to the earlier index, and the appended copy is never reached.
	//
	// Grass/Tarmac/Concrete ONLY - Reinforced has no slot of its own; RunwayMaterialSlot is
	// where that alias happens, and Settings.RunwayMaterials is already indexed by it.
	const ERunwaySurface RunwaySurfacesBySlot[] = { ERunwaySurface::Grass, ERunwaySurface::Tarmac, ERunwaySurface::Concrete };
	static_assert(UE_ARRAY_COUNT(RunwaySurfacesBySlot) == RunwayMaterialSlotCount,
		"One entry per runway material slot - see RunwayMaterialSlotCount's own comment");
	for (int32 Slot = 0; Slot < UE_ARRAY_COUNT(RunwaySurfacesBySlot); ++Slot)
	{
		FRoadMaterialSlot MatSlot;
		MatSlot.Name = URoadMaterialSet::RunwaySlotName(RunwaySurfacesBySlot[Slot]);
		MatSlot.Material = Settings.RunwayMaterials[Slot] != nullptr ? Settings.RunwayMaterials[Slot] : Settings.SurfaceMaterial;
		EffectiveSet->Slots.Add(MatSlot);
	}
	return EffectiveSet;
}

UMaterialInstanceDynamic* URoadSurfacePresenter::RunwayMarkingMaterialInstance(UMaterialInterface* SurfaceMaterialBase)
{
	// RE-CREATED WHEN THE BASE MOVES, not just when there is none yet (issue #193). "Created
	// once" used to mean "created once per ACTOR LIFETIME": PR #232's resolved-material cache
	// on the actor already re-resolves SurfaceMaterial the moment SurfaceMaterial itself is
	// edited (bResolvedContentDirty, invalidated by PostEditChangeProperty on every property),
	// so SurfaceMaterialBase here DOES change on the very next rebuild - but this MID kept
	// pointing at whatever it was first created against, so the runway kept its old skin and
	// only the road surface re-skinned. MID->Parent is the parent this instance actually holds
	// (set by Create, below), so comparing against it - not against some separately-tracked
	// base - cannot drift out of step with what was last built.
	if (SurfaceMaterialBase != nullptr && (RunwayMarkingMID == nullptr || RunwayMarkingMID->Parent != SurfaceMaterialBase))
	{
		RunwayMarkingMID = UMaterialInstanceDynamic::Create(SurfaceMaterialBase, this);
		// WHITE, the one thing that differs from the holding-position paint. The road
		// material's MarkingColor parameter is the taxiway yellow by default; a runway's
		// markings are white (ICAO Annex 14, 5.2.1.4) and this is the whole of the change.
		RunwayMarkingMID->SetVectorParameterValue(RoadMaterialParams::MarkingColor, FLinearColor::White);
	}
	return RunwayMarkingMID;
}

void URoadSurfacePresenter::RebuildRunwayMarkings(URoadNetwork& Network, const FSurfaceSettings& Settings)
{
	// Checked BEFORE RunwayMarkingMaterialInstance below, not left to RebuildLayer's own
	// check: that call lazily creates and caches a UMaterialInstanceDynamic, a real
	// allocation this function must not make on an actor with no runway-paint component to
	// use it on (see LayerComponents' own comment for when that is a supported state).
	if (GetLayerComponent(ESurfaceLayer::RunwayPaint) == nullptr)
	{
		return;
	}

	// The same half unit above the road as the holding positions: both are paint on the
	// pavement, and neither overlaps the other by construction (one lies on taxiways at
	// their runway ends, the other on the runway itself). See GetMarkingZ.
	const double MarkingZ = GetMarkingZ(Settings.SurfaceZ);
	FRunwayMarkingCensus Census;
	// The road material through a dynamic instance with MarkingColor white - see
	// RunwayMarkingMaterialInstance. A null base material leaves the sink's own fallback.
	UMaterialInterface* Material = RunwayMarkingMaterialInstance(Settings.SurfaceMaterial);

	FRoadMeshBuffers Buffers;
	// THE WHITE PAINT LAYER CARRIES ROAD LANE LINES TOO (spec 2026-09-23 §7), rather than a
	// component of their own: a lane line is white road paint exactly as a runway's is, and a
	// new default subobject would need every saved level resaved (a removed or added default
	// subobject lingers in the .umap). RunwayCount keeps the census below counting runways.
	int32 RunwayCount = 0;
	int32 LaneDashes = 0;
	int32 LaneSegments = 0;
	const int32 Painted = RebuildLayer(ESurfaceLayer::RunwayPaint,
		[&Network, MarkingZ, &Census, &RunwayCount, &LaneDashes, &LaneSegments](FRoadMeshBuffers& OutBuffers)
		{
			RunwayCount = FRunwayMarkingBuilder::Build(Network, MarkingZ, OutBuffers, &Census);
			LaneDashes = FRoadLaneMarkingBuilder::Build(Network, MarkingZ, OutBuffers, &LaneSegments);
			return RunwayCount + LaneDashes;
		},
		Material != nullptr ? Material : Settings.SurfaceMaterial, Settings.bUseConstantVertexColour, Buffers,
		Settings.bQuiet);
	if (Painted == INDEX_NONE)
	{
		return;
	}

	// The census, reported: which markings were painted says more about a runway's facts
	// than a triangle count, and it is what the probe reads. Skipped on a Geometry rebuild
	// (issue #178) - RunwayFactsFor and the builder above still ran, since the paint itself
	// must track the drag, but this line would otherwise repeat unchanged 60 times a second.
	if (!Settings.bQuiet)
	{
		UE_LOG(LogRoadMesh, Log,
			TEXT("Runway markings: %d runway(s), %d triangle(s) at Z=%.1f - %d threshold stripes, %d designator strokes, ")
			TEXT("%d centreline dashes, %d aiming bars, %d touchdown stripes, %d side stripes, %d grass markers"),
			RunwayCount, Buffers.Indices.Num() / 3, MarkingZ, Census.ThresholdStripes, Census.DesignatorStrokes,
			Census.CentrelineDashes, Census.AimingPointBars, Census.TouchdownStripes, Census.SideStripes, Census.GrassMarkers);
		UE_LOG(LogRoadMesh, Log, TEXT("Lane markings: %d dashes on %d segment(s)"), LaneDashes, LaneSegments);
	}
}

void URoadSurfacePresenter::RebuildRunwayRubber(URoadNetwork& Network, const FSurfaceSettings& Settings)
{
	// Only the COMPONENT is an early return, never the material - and the difference is one
	// the seam test caught.
	//
	// A null component is fixed for the life of the actor. A null material is not: it can be
	// cleared at runtime, and a runway can be deleted, and in both cases this function still
	// has to run so that RebuildLayer sinks an EMPTY buffer and hides the layer. Returning
	// early on a null material instead left the last build's rubber sitting on the pavement
	// after the material that drew it had gone.
	if (GetLayerComponent(ESurfaceLayer::RunwayRubber) == nullptr)
	{
		return;
	}
	const bool bHasMaterial = Settings.RubberMaterial != nullptr;

	const double RubberZ = GetRubberZ(Settings.SurfaceZ);
	FRunwayMarkingCensus Census;

	// The material ASSET, not a dynamic instance. Unlike the two paint layers, nothing about
	// this one varies per actor - there is no equivalent of MarkingColor to override, because
	// rubber is one colour on every runway. A MID here would be an allocation whose only
	// effect is to make the asset harder to find from the component.
	FRoadMeshBuffers Buffers;
	const int32 Runways = RebuildLayer(ESurfaceLayer::RunwayRubber,
		[&Network, RubberZ, &Census, bHasMaterial](FRoadMeshBuffers& OutBuffers)
		{
			// Left empty when there is nothing to draw it with, which clears the component
			// rather than leaving the previous build on screen.
			return bHasMaterial ? FRunwayMarkingBuilder::BuildRubber(Network, RubberZ, OutBuffers, &Census) : 0;
		},
		Settings.RubberMaterial, Settings.bUseConstantVertexColour, Buffers, Settings.bQuiet);
	if (Runways == INDEX_NONE)
	{
		return;
	}

	// Patches, not triangles, because the patch count is what the builder promises and what
	// a wrong one would show: four per paved runway, two per usable end. Skipped on a
	// Geometry rebuild (issue #178) - see RebuildRunwayMarkings' own comment.
	if (!Settings.bQuiet)
	{
		UE_LOG(LogRoadMesh, Log,
			TEXT("Runway rubber: %d runway(s), %d patch(es), %d triangle(s) at Z=%.2f"),
			Runways, Census.RubberPatches, Buffers.Indices.Num() / 3, RubberZ);
	}
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
	// Its own builder instance, so an apron corner that happens to land exactly on a road
	// vertex cannot weld to it. The two surfaces meet; they are not one surface.
	const double ApronZ = GetApronSurfaceZ(Settings.SurfaceZ, Settings.ApronZOffset);

	FRoadMeshBuffers Buffers;
	const int32 Built = RebuildLayer(ESurfaceLayer::Apron,
		[&Network, ApronZ, &Settings](FRoadMeshBuffers& OutBuffers)
		{
			FRoadMeshBuilder Builder(ApronZ, Settings.TexelsPerUnit);
			int32 Count = 0;
			for (const FApronSurface& Apron : Network.GetAprons())
			{
				if (Apron.bAlive)
				{
					Builder.AddApron(Apron);
					++Count;
				}
			}

			// A PLOTTED INSTALLATION'S PAD IS PAVEMENT, and it is the polygon the player
			// drew - not new geometry. Through the SAME builder and the same triangulator
			// the aprons above use, on the same component and the same Z, so a depot's pad
			// and the apron it abuts are one surface rather than two that must agree.
			//
			// The pad's painted lines are deliberately NOT here. The white and yellow lines
			// on the concept sheet mark the truck's path off the pad, and that path is the
			// pose's lead-in, which the guideline graph already computes and draws. Painting
			// them into the mesh would be a second evaluator of where the truck drives -
			// they would agree at one rotation and visibly disagree at every other.
			for (const FEntityInstance& Entity : Network.GetEntities())
			{
				if (Entity.bAlive && Entity.Outline.Num() >= 3)
				{
					Builder.AddApron(Entity.Outline);
					++Count;
				}
			}
			// A COPY, not the zero-copy const& this held before issue #81: Builder is scoped
			// to this lambda, and GetBuffers() returns a const& into IT, which stops existing
			// the moment this lambda returns - MoveTemp cannot turn that into a move either,
			// since a move constructor cannot bind to a const source. FRoadMeshBuilder's own
			// API has no "build into an external FRoadMeshBuffers" - only the two static
			// Build(Network, Z, OutBuffers) builders RebuildLayer was shaped around do - so a
			// copy at this one boundary is the price of routing aprons through RebuildLayer
			// too. One apron rebuild's worth of vertices, not a hot path.
			OutBuffers = Builder.GetBuffers();
			return Count;
		},
		Settings.ApronMaterial != nullptr ? Settings.ApronMaterial : Settings.SurfaceMaterial,
		Settings.bUseConstantApronColour, Buffers, Settings.bQuiet);
	if (Built == INDEX_NONE)
	{
		return;
	}

	// Reported rather than inferred. An apron that is built and never seen, and one that
	// is never built, look identical from outside - and every explanation reasoned from
	// engine source about the roads was wrong before the numbers were printed. Skipped on a
	// Geometry rebuild (issue #178) - see RebuildRunwayMarkings' own comment; the apron is
	// still rebuilt every drag frame (see RebuildInternal), only the report is silenced.
	if (!Settings.bQuiet)
	{
		UE_LOG(LogRoadMesh, Log,
			TEXT("Aprons: %d surface(s), %d triangle(s) at Z=%.1f, material %s%s"),
			Built, Buffers.Indices.Num() / 3, ApronZ,
			Settings.ApronMaterial != nullptr ? *Settings.ApronMaterial->GetName() : TEXT("<fallback>"),
			Settings.bUseConstantApronColour ? TEXT(" (CONSTANT COLOUR - material overridden)") : TEXT(""));
	}

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

	if (Settings.bDebugDrawAprons)
	{
		DebugDrawTriangles(Buffers, FColor::Cyan, 12.0f, Settings.DebugDrawSeconds);
	}
}

void URoadSurfacePresenter::RebuildMarkings(URoadNetwork& Network, const FSurfaceSettings& Settings)
{
	// Half a unit ABOVE the road, so the paint wins the depth test against the pavement it
	// lies on - the road is the highest surface here (the apron sits below it, see
	// GetApronSurfaceZ), so above the road is above everything. See GetMarkingZ.
	const double MarkingZ = GetMarkingZ(Settings.SurfaceZ);

	FRoadMeshBuffers Buffers;
	// THE ROAD'S OWN MATERIAL, on purpose: every vertex carries UV1 = 0, which M_RoadSurface
	// reads as "on the centreline" and paints MarkingColor across the whole quad. See
	// FHoldingPositionMarkingBuilder for why that is the paint wanted and not a defect.
	//
	// STAND PAINT SHARES THIS LAYER (task 8): a drawn stand's lead-in, stop bar and letter are
	// the same solid-MarkingColor quad, so a second component would only be a second material
	// instance drawing the identical thing. HoldingPositionsPainted is captured OUTSIDE the
	// lambda, not read off the returned Count, because Count below has to be the SUM of both
	// builders' finds - RebuildLayer's own SetVisibility(Count > 0) would otherwise hide a
	// level's stand paint whenever it had no holding position to report, which is exactly the
	// wrong answer for an airport with stands and no runway yet.
	int32 HoldingPositionsPainted = 0;
	int32 StandsPainted = 0;
	FStandMarkingCensus StandCensus;
	const int32 Painted = RebuildLayer(ESurfaceLayer::HoldingPaint,
		[&Network, MarkingZ, &HoldingPositionsPainted, &StandsPainted, &StandCensus](FRoadMeshBuffers& OutBuffers)
		{
			HoldingPositionsPainted = FHoldingPositionMarkingBuilder::Build(Network, MarkingZ, OutBuffers);
			StandsPainted = FStandMarkingBuilder::Build(Network, MarkingZ, OutBuffers, &StandCensus);
			return HoldingPositionsPainted + StandsPainted;
		},
		Settings.SurfaceMaterial, Settings.bUseConstantVertexColour, Buffers, Settings.bQuiet);
	if (Painted == INDEX_NONE)
	{
		return;
	}

	// Reported, not inferred - the same reason the aprons say what they built. This function
	// only ever runs on a Topology rebuild (see RebuildInternal), so Settings.bQuiet is
	// always false here today; threaded through anyway so RebuildLayer has one contract for
	// all four callers rather than three that pass it and one that cannot.
	if (!Settings.bQuiet)
	{
		UE_LOG(LogRoadMesh, Log,
			TEXT("Holding positions: %d painted, %d stand(s), %d triangle(s) at Z=%.1f"),
			HoldingPositionsPainted, StandsPainted, Buffers.Indices.Num() / 3, MarkingZ);
	}
}

void URoadSurfacePresenter::InvalidateGhostCache()
{
	LastGhostFrom = INDEX_NONE;
}

void URoadSurfacePresenter::Rebuild(URoadNetwork& Network, const FSurfaceSettings& Settings)
{
	RebuildInternal(Network, Settings, EChangeKind::Topology);
}

void URoadSurfacePresenter::RebuildSurfaceOnly(URoadNetwork& Network, const FSurfaceSettings& Settings)
{
	RebuildInternal(Network, Settings, EChangeKind::Geometry);
}

void URoadSurfacePresenter::RebuildMarkingsOnly(URoadNetwork& Network, const FSurfaceSettings& Settings)
{
	RebuildInternal(Network, Settings, EChangeKind::Markings);
}

void URoadSurfacePresenter::RebuildInternal(URoadNetwork& Network, const FSurfaceSettings& InSettings, EChangeKind Kind)
{
	// A LOCAL, MUTABLE COPY - not a reference to InSettings, which belongs to the caller (see
	// FSurfaceSettings's own header comment: "copied by value into every call"). bQuiet is
	// derived from Kind HERE, the only place issue #178 lets it be set: a Geometry rebuild is
	// the per-frame drag path (see RebuildSurfaceOnly), so it is quiet; a Topology rebuild -
	// including the one a drag ends with - logs exactly as it always has. Every downstream
	// Rebuild*/RebuildLayer/RoadRebuildCensus::Log call below reads Settings.bQuiet rather
	// than Kind directly, so none of them need to know EChangeKind exists.
	FSurfaceSettings Settings = InSettings;
	Settings.bQuiet = (Kind == EChangeKind::Geometry);

	// See InvalidateGhostCache's own comment for why this must happen on every rebuild.
	InvalidateGhostCache();

	if (Kind == EChangeKind::Markings)
	{
		// PAINT ONLY, PAST HERE (issue #179). Nothing below this line - the solve, the road
		// mesh, aprons, the guideline graph, anchor links, runway paint and rubber - can have
		// changed: SetIntermediateHoldingPosition flips a flag on a guideline node that
		// already exists. RebuildMarkings reads Network.GetGuidelineNodes() exactly as they
		// stand, which is safe ONLY because this returns before FRoadGuidelineBuilder::Build
		// (below, under Topology) would reallocate them - see RebuildMarkingsOnly's own
		// comment for why routing this through Topology instead was tried and reverted.
		RebuildMarkings(Network, Settings);
		return;
	}

	UDynamicMeshComponent* MeshComponent = GetLayerComponent(ESurfaceLayer::Road);
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

	// RESOLVED ONCE, HERE, AND PASSED DOWN (issue #190) - see Settings.LargestServiceVehicle's
	// own comment. SolveAll's BuildNodeInput asks a profile's ResolvedFilletRadius per arm of
	// every node it visits; without this, that ran UAirsideSettings::ResolveLargestServiceVehicle
	// fresh each time, on every Geometry rebuild a drag frame produces as well as every Topology
	// one.
	// A TOPOLOGY REBUILD TRACES the bends' widening (EWideningTrace); a drag's Geometry frame
	// reads what the last one traced, so a drag never drives a vehicle per frame.
	const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(Network, 12, &Settings.DesignVehicles,
		Kind == EChangeKind::Topology ? EWideningTrace::Trace : EWideningTrace::ReadCached);

	// TOPOLOGY ONLY, PAST HERE (issue #165). A Geometry change - a MoveNode or
	// MoveApronCorner drag frame - moved positions and nothing else, so the graph's SHAPE is
	// exactly what it was; re-deriving it every frame of a drag is the cost issue #165 is
	// about. See RebuildSurfaceOnly's own comment for what that leaves stale and why that is
	// tolerated for a drag's duration.
	if (Kind == EChangeKind::Topology)
	{
		// The guideline graph is derived from the same solve, and until this call existed it
		// was derived NOWHERE outside the tests - so every route query at runtime ran against
		// an empty graph and correctly reported that nothing was connected.
		//
		// Anchor lead-ins go second and must: they join stands to guidelines that only exist
		// once the line above has run, and both are swept and rebuilt together. Both take the
		// SAME resolved vehicle SolveAll just used, rather than resolving their own (#190).
		FRoadGuidelineBuilder::Build(Network, Solved, Settings.DesignVehicles);
		//
		// THE SERVICE RADIUS COMES DOWN FROM THE LEVEL - see ARoadNetworkActor::ServiceLinkRadius.
		// The aircraft cap keeps FAnchorLink's own default beside it, deliberately: one is
		// per-airport gameplay tuning and the other is a fact about a painted line.
		// THE DEFAULT, NOT PER TIER: a stand or depot link is driven by the rigid trucks that
		// service stands and live in depots. The rig has no stand or depot to go to yet (spec
		// §"Out of this step", step 3); sizing every link's lane radius for it would widen every
		// yard approach for a vehicle that never uses one.
		FAnchorLink::Build(Network, Settings.DesignVehicles.Default.Chassis, FAnchorLink::DefaultMaxLeadIn,
			Settings.ServiceLinkRadius);
	}

	// THROUGH THE RESOLVED SETTING, never a raw property: an unset MaterialSet means "single
	// material", and ARoadNetworkActor::ResolveMaterialSet supplies a content default without
	// the actor being altered to say so - see that function for why. This class only ever
	// sees the result.
	//
	// THE EFFECTIVE SET, not Settings.MaterialSet itself: the authored slots plus the three
	// runway surfaces (see EffectiveMaterialSet). A null authored set used to mean the
	// single-slot sink path; it now means one surface slot at id 0, which skins every
	// non-runway triangle with the same material it always had.
	const URoadMaterialSet* Materials = EffectiveMaterialSet(Settings);
	FRoadMeshBuilder Builder(Settings.SurfaceZ, Settings.TexelsPerUnit, Materials);
	Builder.Build(Network, Solved, Settings.RibbonSegments);

	FDynamicMeshSink Sink(MeshComponent, Settings.SurfaceMaterial, Settings.bUseConstantVertexColour, Materials,
		Settings.bQuiet);
	Builder.Emit(Sink);

	// Aprons share nothing with the roads and are built separately, but they are rebuilt
	// together so one call still means "make the world match the model". Independent of the
	// guideline graph - FRoadMeshBuilder for apron outlines reads Network.GetAprons()/
	// GetEntities() directly - so this runs for a Geometry change too.
	RebuildAprons(Network, Settings);

	if (Kind == EChangeKind::Topology)
	{
		// And the holding-position paint, from the graph derived above. NOT run for a
		// Geometry change: FHoldingPositionMarkingBuilder::Build reads
		// Network.GetGuidelineNodes(), which was deliberately left untouched above - see
		// RebuildSurfaceOnly's own comment.
		RebuildMarkings(Network, Settings);
	}
	// And the runways' own paint, from their facts - Network's segments and RunwayFactsFor,
	// not the guideline graph, so this runs every time same as RebuildAprons.
	RebuildRunwayMarkings(Network, Settings);
	// And the rubber under it, which is not paint at all - see RebuildRunwayRubber.
	RebuildRunwayRubber(Network, Settings);

	if (Settings.bDebugDrawMesh)
	{
		// Same buffers, a completely different route to the screen. If these lines land
		// where the clicks did and the surface does not, the fault is in the component or
		// the view, not the geometry - and if the lines are wrong too, every conclusion
		// drawn from vertex counts and bounds so far needs revisiting.
		DebugDrawTriangles(Builder.GetBuffers(), FColor::Green, 8.0f, Settings.DebugDrawSeconds);

		// The bounding box the renderer culls against, so an off-screen or collapsed box
		// is visible rather than merely reported.
		const float Lifetime = static_cast<float>(Settings.DebugDrawSeconds);
		DrawDebugBox(GetWorld(), MeshComponent->Bounds.Origin,
			MeshComponent->Bounds.BoxExtent + FVector(0.0, 0.0, 50.0),
			FColor::Magenta, false, Lifetime, 0, 8.0f);
	}

	// Diagnostic-only from here: profile counts, the material slot list, distinct material
	// ids, profile names in use, and the final "Rebuilt:" line. Kept behind RoadRebuildCensus
	// so this function stays about DOING the rebuild, not reporting on it. Settings.bQuiet
	// (issue #178) skips the whole thing - strings, sets and all - on a Geometry rebuild; it
	// still runs in full on the Topology rebuild a drag ends with.
	RoadRebuildCensus::Log(Network, Builder.GetBuffers(), *MeshComponent, Solved,
		Settings.SurfaceMaterial, Settings.MaterialSet, Settings.bQuiet);
}

UMaterialInstanceDynamic* URoadSurfacePresenter::GhostMaterialInstance(UMaterialInterface* GhostMaterialBase)
{
	// SEE RunwayMarkingMaterialInstance's comment (issue #193) - the same cache-never-follows
	// defect, the same fix: compare against MID->Parent, the base this instance actually holds,
	// rather than trusting a one-time null check to mean "still current".
	if (GhostMaterialBase != nullptr && (GhostMID == nullptr || GhostMID->Parent != GhostMaterialBase))
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
	if (UDynamicMeshComponent* GhostComponent = GetLayerComponent(ESurfaceLayer::Ghost))
	{
		GhostComponent->SetVisibility(false);
	}
	bGhostVisible = false;
	LastGhostFrom = INDEX_NONE;
}

bool URoadSurfacePresenter::BuildGhostBuffers(URoadNetwork* Network, int32 FromNodeIndex,
	const FRoadSnapResult& Snap, const FSurfaceSettings& Settings, FRoadMeshBuffers& OutBuffers)
{
	const FRoadNodeId From = Network != nullptr ? Network->NodeIdAt(FromNodeIndex) : FRoadNodeId();
	if (!From.IsSet())
	{
		return false;
	}

	// ONE GhostNetwork FOR THE LIFE OF THIS PRESENTER (#166), allocated once and refreshed
	// here every call rather than duplicated per call - a fresh DuplicateObject for every
	// cursor frame was the finding: eighteen UPROPERTY arrays allocated and orphaned to the
	// next GC for every pixel the mouse crossed. CopyFrom is a handful of TArray
	// assignments against an object that already exists, and - just as DuplicateObject did
	// - leaves slot indices and generation counters identical to the live network's, which
	// is what lets Snap's node and segment handles be used directly below without
	// translation.
	if (GhostNetwork == nullptr)
	{
		GhostNetwork = NewObject<URoadNetwork>(this);
		++GhostNetworkAllocCount;
	}
	GhostNetwork->CopyFrom(*Network);

	FRoadNodeId To;
	switch (Snap.Kind)
	{
	case ERoadSnapKind::Node:
		To = Snap.Node;
		break;

	case ERoadSnapKind::Segment:
		// The same surgery the click will perform, run on the copy. Sharing
		// URoadNetwork::SplitSegment with URoadEditFacade::SplitSegment is the only thing
		// that stops the preview and the edit diverging.
		To = GhostNetwork->SplitSegment(Snap.Segment, Snap.Position);
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

	// SOLVE ONLY From AND To (#166), not FRoadNetworkSolver::SolveAll: the ghost draws
	// nothing but this segment and these two junctions (see the loop below), so a full
	// solve was re-deriving geometry for every OTHER junction in the network purely to
	// throw it away - O(N) junction solves for a preview that shows two. SolveNodeInto is
	// SolveAll's own per-node body, so this writes exactly the fields SolveAll would have
	// written for these two nodes (the ghosted segment's cuts AND every other live arm at
	// each of them, which AddGhostJunction's fan needs) and nothing writes them differently
	// depending on which caller asked.
	FRoadSolveResult Solved;
	// THE REBUILD'S DESIGN VEHICLES, passed down (review 2026-09-25): this runs every ghost
	// frame, and the self-resolving path would ask the content set per arm per frame.
	FRoadNetworkSolver::SolveNodeInto(*GhostNetwork, From.Index, 12, Solved, &Settings.DesignVehicles);
	FRoadNetworkSolver::SolveNodeInto(*GhostNetwork, To.Index, 12, Solved, &Settings.DesignVehicles);

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

bool URoadSurfacePresenter::IsGhostCacheHit(const URoadNetwork* Network, int32 FromNodeIndex,
	const FRoadSnapResult& Snap, bool bValid, bool& bOutValidityChanged, ERoadKind Kind, int32 WidthIndex) const
{
	bOutValidityChanged = false;

	// Mirrors UpdateGhost's own opening guard exactly: a node that is not currently live
	// is never a cache hit, so a caller that sees false here and falls through to
	// UpdateGhost gets the same HideGhost() it would have gotten before this query existed.
	const FRoadNodeId From = Network != nullptr ? Network->NodeIdAt(FromNodeIndex) : FRoadNodeId();
	if (GetLayerComponent(ESurfaceLayer::Ghost) == nullptr || !From.IsSet())
	{
		return false;
	}

	// A drag holds still for most of its frames. Rebuilding then means re-solving the two
	// junctions to produce exactly the same triangles, sixty times a second. Cleared by
	// Rebuild, so any real edit invalidates it. QuantiseGhostPosition on Snap.Position, not
	// the raw value - see its own comment for why an exact comparison used to miss almost
	// every frame of a "held still" drag.
	if (bGhostVisible
		&& FromNodeIndex == LastGhostFrom
		&& Snap.Kind == LastGhostKind
		&& QuantiseGhostPosition(Snap.Position) == LastGhostTo
		&& Kind == LastGhostRoadKind
		&& WidthIndex == LastGhostWidthIndex)
	{
		bOutValidityChanged = (bValid != bLastGhostValid);
		return true;
	}

	return false;
}

void URoadSurfacePresenter::SetGhostValidity(bool bValid, UMaterialInterface* GhostMaterialBase)
{
	// Geometry untouched: this is the whole reason validity is a material parameter rather
	// than a second mesh.
	if (UMaterialInstanceDynamic* Instance = GhostMaterialInstance(GhostMaterialBase))
	{
		Instance->SetScalarParameterValue(RoadMaterialParams::ValidityBlend, bValid ? 0.0f : 1.0f);
	}
	bLastGhostValid = bValid;
}

void URoadSurfacePresenter::UpdateGhost(URoadNetwork* Network, int32 FromNodeIndex,
	const FRoadSnapResult& Snap, bool bValid, const FSurfaceSettings& Settings, ERoadKind Kind, int32 WidthIndex)
{
	// The cache-hit short-circuit this used to open with is now the caller's job - see
	// IsGhostCacheHit and SetGhostValidity, which exist so the caller can skip resolving
	// Settings at all on a still drag. This function always does the full rebuild; a
	// caller that already knows it has a cache hit must not reach here.
	UDynamicMeshComponent* GhostComponent = GetLayerComponent(ESurfaceLayer::Ghost);
	const FRoadNodeId From = Network != nullptr ? Network->NodeIdAt(FromNodeIndex) : FRoadNodeId();
	if (GhostComponent == nullptr || !From.IsSet())
	{
		HideGhost();
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
		Instance->SetScalarParameterValue(RoadMaterialParams::ValidityBlend, bValid ? 0.0f : 1.0f);

		// The material cannot know where this road's edge is; UV1.X is in uu and the
		// profile owns the half-width. Left at its default a narrow road would glow from
		// edge to edge and a wide one not at all.
		if (const URoadProfile* Used = Settings.Profile)
		{
			Instance->SetScalarParameterValue(RoadMaterialParams::EdgeHalfWidth,
				static_cast<float>(Used->GetMaxHalfWidth()));
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
	LastGhostTo = QuantiseGhostPosition(Snap.Position);
	LastGhostKind = Snap.Kind;
	LastGhostRoadKind = Kind;
	LastGhostWidthIndex = WidthIndex;
	bLastGhostValid = bValid;
}
