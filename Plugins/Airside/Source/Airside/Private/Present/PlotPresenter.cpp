#include "Present/PlotPresenter.h"

#include "AirsideLog.h"
#include "Build/DepotKit.h"
#include "Build/PlotLayoutStrategy.h"
#include "Build/RoadMeshSink.h"
#include "Components/DynamicMeshComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/StaticMesh.h"
#include "Entities/EntityDefinition.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Present/DynamicMeshSink.h"
#include "Solve/FenceLayout.h"
#include "Solve/PlotYard.h"
#include "Solve/RoadGeom.h"

namespace
{
	/** The engine cube is 100 uu on a side with a CENTRED pivot. */
	constexpr double CubeUu = 100.0;

	/**
	 * Module heights, uu, so the three read apart at a glance while they are still boxes.
	 *
	 * A shed is tall enough to swallow a 6.2 m truck, a tank sits lower and wider, a pump is
	 * knee-high plant. Grey-box figures chosen for LEGIBILITY rather than measured off the
	 * concept sheet - measuring them would be authoring the model in the wrong file, and the
	 * real heights arrive with the meshes.
	 */
	constexpr double ShedHeightUu = 400.0;
	constexpr double TankHeightUu = 250.0;
	constexpr double PumpHeightUu = 150.0;

	/**
	 * The chainlink kit's figures, uu - the asset README's, not chosen here. POST AND FABRIC
	 * HEIGHT ARE ONE DECISION WITH THE TEXTURE: its V range IS 240 uu of fabric, so changing the
	 * fabric height without regenerating chainlink.png makes the diamonds stop being square.
	 */
	constexpr double FencePostHeightUu = 245.0;
	constexpr double FenceFabricHeightUu = 240.0;
	constexpr double FenceLinePostDiameterUu = 6.0;
	constexpr double FenceHeavyPostDiameterUu = 9.0;

	/** The asset contract's layout, gated at the truck corridor's width. */
	FenceLayout::FSpec FenceSpec()
	{
		FenceLayout::FSpec Spec;
		Spec.GateWidthUu = PlotYard::GateCorridorUu;
		return Spec;
	}

	/**
	 * One post's instance transform.
	 *
	 * TWO ORIGINS, because the fallback and the asset disagree about where theirs is: the
	 * authored post is base-centred at 1:1, the engine cube is 100 uu and centred. A cube
	 * placed like a post sinks to its waist, and a post placed like a cube floats.
	 */
	FTransform FencePostAt(const FenceLayout::FPost& Post, const UStaticMesh* Authored)
	{
		const FRotator Rotation(0.0, FMath::RadiansToDegrees(Post.YawRad), 0.0);
		if (Authored != nullptr)
		{
			return FTransform(Rotation, FVector(Post.Position.X, Post.Position.Y, 0.0));
		}
		const double Diameter = Post.Kind == FenceLayout::EPostKind::Line
			? FenceLinePostDiameterUu : FenceHeavyPostDiameterUu;
		return FTransform(Rotation,
			FVector(Post.Position.X, Post.Position.Y, FencePostHeightUu * 0.5),
			FVector(Diameter / CubeUu, Diameter / CubeUu, FencePostHeightUu / CubeUu));
	}

	/**
	 * One bay of fabric: a vertical quad, four vertices of its own, two triangles.
	 *
	 * NOT SHARED WITH THE NEXT BAY. FDynamicMeshSink computes per-vertex normals, and a vertex
	 * shared at a corner would average two edges' normals and shade a smear down the post.
	 *
	 * NOT THROUGH AppendTriangleUp, whose sliver guard measures area in XY - which is zero for
	 * every vertical triangle, so it would drop the whole fence. Wound (A0, B1, B0), (A0, A1, B1)
	 * so the engine's left-handed normal points OUTWARD; Airside.Present.PlotFenceFabricFacesOut
	 * measures it.
	 */
	void AppendFenceBay(FRoadMeshBuffers& Buffers, const FenceLayout::FSpan& Span)
	{
		const int32 A0 = Buffers.Positions.Num();
		Buffers.Positions.Add(FVector3d(Span.A.X, Span.A.Y, 0.0));
		Buffers.Positions.Add(FVector3d(Span.B.X, Span.B.Y, 0.0));
		Buffers.Positions.Add(FVector3d(Span.B.X, Span.B.Y, FenceFabricHeightUu));
		Buffers.Positions.Add(FVector3d(Span.A.X, Span.A.Y, FenceFabricHeightUu));
		const int32 B0 = A0 + 1;
		const int32 B1 = A0 + 2;
		const int32 A1 = A0 + 3;

		// V 1 AT THE GROUND, 0 AT THE TOP - the image's own orientation. The texture does not
		// tile in V, so this is the one direction that is not arbitrary.
		const float U0 = static_cast<float>(Span.U0);
		const float U1 = static_cast<float>(Span.U1);
		Buffers.UV0.Append({ FVector2f(U0, 1.0f), FVector2f(U1, 1.0f),
		                     FVector2f(U1, 0.0f), FVector2f(U0, 0.0f) });

		// The sink reads three UV layers per vertex; the fabric's material samples only UV0.
		for (int32 Corner = 0; Corner < 4; ++Corner)
		{
			Buffers.UV1.Add(FVector2f::ZeroVector);
			Buffers.UV2.Add(FVector2f::ZeroVector);
		}

		Buffers.Indices.Append({ A0, B1, B0, A0, A1, B1 });
		Buffers.MaterialIDs.Append({ 0, 0 });
	}

	double HeightFor(EDepotModule Module)
	{
		switch (Module)
		{
		case EDepotModule::Shed: return ShedHeightUu;
		case EDepotModule::Tank: return TankHeightUu;
		case EDepotModule::Pump: return PumpHeightUu;
		// THE SENTINEL, NOT A MODULE - named explicitly so a genuinely new module still falls
		// through with no case here and keeps warning.
		case EDepotModule::Count: break;
		}
		return ShedHeightUu;
	}

	/**
	 * A box resting ON the ground at Where, facing Heading.
	 *
	 * THE HALF-HEIGHT IN Z IS NOT OPTIONAL. The engine cube's pivot is its centre, so a box
	 * placed at Z=0 sinks to its waist in the apron - which reads as a modelling error in
	 * the mesh that is not there yet.
	 */
	FTransform BoxAt(const FVector2D& Where, double Heading,
		double LengthUu, double WidthUu, double HeightUu)
	{
		const FVector Scale(LengthUu / CubeUu, WidthUu / CubeUu, HeightUu / CubeUu);
		const FVector Location(Where.X, Where.Y, HeightUu * 0.5);
		const FRotator Rotation(0.0, FMath::RadiansToDegrees(Heading), 0.0);
		return FTransform(Rotation, Location, Scale);
	}

	/**
	 * Every slot of a pooled ghost wears the one ghost material. A mesh has as many slots as
	 * Blender gave it looks, and a slot left alone draws that look solid inside a ghost.
	 */
	void ApplyGhostMaterial(UInstancedStaticMeshComponent& Component, UMaterialInterface* Ghost)
	{
		if (Ghost == nullptr)
		{
			return;
		}
		for (int32 Slot = 0; Slot < Component.GetNumMaterials(); ++Slot)
		{
			Component.SetMaterial(Slot, Ghost);
		}
	}

	/**
	 * A mesh's plan bounds in the KIT's frame - +X away from the road, the run along +Y -
	 * after turning it by YawDeg and, for a far cap, mirroring it along the run.
	 *
	 * EXACT FOR A QUARTER TURN, which is all ResolveDepotLooks hands out: the sine and cosine
	 * are rounded so a 90 degree turn maps the box's corners onto each other rather than
	 * 1e-14 off, and the box stays the box.
	 */
	struct FKitBox
	{
		FVector2D Min = FVector2D::ZeroVector;
		FVector2D Max = FVector2D::ZeroVector;
		double MinZ = 0.0;
	};

	FKitBox KitBoxOf(const UStaticMesh& Mesh, double YawDeg, bool bMirrorRun)
	{
		const FBox Bounds = Mesh.GetBoundingBox();
		const double Rad = FMath::DegreesToRadians(YawDeg);
		const double C = FMath::RoundToDouble(FMath::Cos(Rad));
		const double S = FMath::RoundToDouble(FMath::Sin(Rad));

		FKitBox Out;
		Out.Min = FVector2D(TNumericLimits<double>::Max());
		Out.Max = FVector2D(-TNumericLimits<double>::Max());
		Out.MinZ = Bounds.Min.Z;
		for (const double X : { Bounds.Min.X, Bounds.Max.X })
		{
			for (const double Y : { Bounds.Min.Y, Bounds.Max.Y })
			{
				FVector2D Kit(X * C - Y * S, X * S + Y * C);
				if (bMirrorRun)
				{
					Kit.Y = -Kit.Y;
				}
				Out.Min = FVector2D::Min(Out.Min, Kit);
				Out.Max = FVector2D::Max(Out.Max, Kit);
			}
		}
		return Out;
	}

	/**
	 * One mesh piece's instance transform: its origin at (KitX, KitY) in the stand's frame,
	 * turned by the stand's heading plus the mesh's own yaw, standing on the ground.
	 *
	 * THE MIRROR IS A NEGATIVE SCALE ON WHICHEVER MESH AXIS THE YAW LAYS ALONG THE RUN - X
	 * for a quarter turn, Y for none. It is applied first, in the mesh's own frame, which is
	 * where FTransform applies scale. UE 5.8's GPU scene flips culling per instance on a
	 * negative determinant (INSTANCE_SCENE_DATA_FLAG_DETERMINANT_SIGN), so a mirrored cap in
	 * the same component as an unmirrored one is not drawn inside out.
	 */
	FTransform PieceAt(const FVector2D& Origin, double Heading, double KitX, double KitY,
		double YawDeg, bool bMirrorRun, double BaseZ)
	{
		const FVector2D Forward(FMath::Cos(Heading), FMath::Sin(Heading));
		const FVector2D Across = RoadGeom::PerpCCW(Forward);
		const FVector2D Where = Origin + Forward * KitX + Across * KitY;

		FVector Scale = FVector::OneVector;
		if (bMirrorRun)
		{
			const bool bQuarterTurn = FMath::Abs(FMath::Sin(FMath::DegreesToRadians(YawDeg))) > 0.5;
			if (bQuarterTurn)
			{
				Scale.X = -1.0;
			}
			else
			{
				Scale.Y = -1.0;
			}
		}
		return FTransform(FRotator(0.0, FMath::RadiansToDegrees(Heading) + YawDeg, 0.0),
			FVector(Where.X, Where.Y, BaseZ), Scale);
	}

	/**
	 * Which edge of the plot is its frontage, recovered from the entity alone.
	 *
	 * EXACT, NOT A GUESS, and not a second search either. URoadEditFacade::PlaceEntityInPlot
	 * puts the pose at the MIDPOINT of the frontage edge, so the edge whose midpoint equals
	 * Position is that edge by construction - this reads back a value rather than deriving a
	 * new opinion.
	 *
	 * ASKING FAnchorLink AGAIN WAS REJECTED. It would search the live graph, so a road laid
	 * or deleted after the depot was built could move the frontage, and every shed in the
	 * yard would jump to a new edge without the player touching the depot. Where the thing
	 * faces was decided when it was placed, and it stays decided.
	 */
	bool RecoverFrontage(const FEntityInstance& Entity, FVector2D& OutA, FVector2D& OutB)
	{
		double BestDistance = TNumericLimits<double>::Max();
		int32 BestEdge = INDEX_NONE;

		for (int32 I = 0; I < Entity.Outline.Num(); ++I)
		{
			const FVector2D& A = Entity.Outline[I];
			const FVector2D& B = Entity.Outline[(I + 1) % Entity.Outline.Num()];
			const double Distance = FVector2D::Distance((A + B) * 0.5, Entity.Position);
			if (Distance < BestDistance)
			{
				BestDistance = Distance;
				BestEdge = I;
			}
		}

		if (BestEdge == INDEX_NONE)
		{
			return false;
		}

		OutA = Entity.Outline[BestEdge];
		OutB = Entity.Outline[(BestEdge + 1) % Entity.Outline.Num()];
		return true;
	}

}

void UPlotPresenter::Initialise(UInstancedStaticMeshComponent* InBoxes,
	UInstancedStaticMeshComponent* InGhosts, const FFenceTargets& InFence,
	USceneComponent* InMeshParent)
{
	Boxes = InBoxes;
	GhostBoxes = InGhosts;
	MeshParent = InMeshParent;
	FencePostsInto = InFence.Posts;
	FenceHeavyPostsInto = InFence.HeavyPosts;
	FenceFabricInto = InFence.Fabric;
}

int32 UPlotPresenter::GetInstanceCount() const
{
	// FROM Placed, NOT THE COMPONENT, so this and GetInstanceTransformForTest agree by
	// construction rather than by both happening to read the same place today.
	return Placed.Num();
}

int32 UPlotPresenter::GetMeshInstanceCountForTest(const UStaticMesh* Mesh, bool bGhost) const
{
	const TObjectPtr<UInstancedStaticMeshComponent>* Found =
		(bGhost ? GhostMeshPool : MeshPool).Find(Mesh);
	return Found != nullptr && *Found != nullptr ? (*Found)->GetInstanceCount() : 0;
}

const UInstancedStaticMeshComponent* UPlotPresenter::GetMeshComponentForTest(
	const UStaticMesh* Mesh, bool bGhost) const
{
	const TObjectPtr<UInstancedStaticMeshComponent>* Found =
		(bGhost ? GhostMeshPool : MeshPool).Find(Mesh);
	return Found != nullptr ? Found->Get() : nullptr;
}

bool UPlotPresenter::GetMeshInstanceTransformForTest(const UStaticMesh* Mesh, bool bGhost,
	int32 Index, FTransform& OutTransform) const
{
	const TObjectPtr<UInstancedStaticMeshComponent>* Found =
		(bGhost ? GhostMeshPool : MeshPool).Find(Mesh);
	if (Found == nullptr || *Found == nullptr)
	{
		return false;
	}
	return (*Found)->GetInstanceTransform(Index, OutTransform, /*bWorldSpace=*/true);
}

UInstancedStaticMeshComponent* UPlotPresenter::PoolFor(UStaticMesh* Mesh, bool bGhost)
{
	if (Mesh == nullptr || MeshParent == nullptr || MeshParent->GetOwner() == nullptr)
	{
		return nullptr;
	}
	TMap<TObjectPtr<UStaticMesh>, TObjectPtr<UInstancedStaticMeshComponent>>& Pool =
		bGhost ? GhostMeshPool : MeshPool;
	if (const TObjectPtr<UInstancedStaticMeshComponent>* Found = Pool.Find(Mesh))
	{
		if (IsValid(*Found))
		{
			return *Found;
		}
	}

	// TRANSIENT and never added to the actor's instance components, so nothing about it is
	// saved - see MeshPool's comment. No collision, for DressAsCubes' reason on the actor.
	AActor* Owner = MeshParent->GetOwner();
	UInstancedStaticMeshComponent* Made = NewObject<UInstancedStaticMeshComponent>(
		Owner, NAME_None, RF_Transient);
	Made->SetupAttachment(MeshParent);
	Made->SetStaticMesh(Mesh);
	Made->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	if (bGhost && GhostBoxes != nullptr)
	{
		ApplyGhostMaterial(*Made, GhostBoxes->GetMaterial(0));
	}
	if (Owner->GetWorld() != nullptr)
	{
		Made->RegisterComponent();
	}
	Pool.Add(Mesh, Made);
	UE_LOG(LogAirside, Log, TEXT("Plots: made a %s component for %s"),
		bGhost ? TEXT("ghost") : TEXT("module"), *Mesh->GetName());
	return Made;
}

bool UPlotPresenter::DrawMeshes(const FDepotModuleLook& Look, const PlotYard::FKitSpec& Spec,
	const FVector2D& RunCentre, double Heading, int32 Lit, int32 Dark)
{
	if (!Look.HasMeshes() || MeshParent == nullptr)
	{
		return false;
	}
	const double Yaw = Look.MeshYawDeg;
	const int32 Length = Lit + Dark;

	if (Look.Assembly == EKitAssembly::Baked)
	{
		// EACH SPAN IS ONE BAKED MESH, centred on the span by its own bounds - the lit span
		// and the ghosted remainder, exactly where the grey boxes stood.
		const double FullWidth = Spec.RunWidthUu(Length);
		const double LitWidth = Lit > 0 ? Spec.RunWidthUu(Lit) : 0.0;
		const double Start = -FullWidth * 0.5;
		auto Span = [&](int32 Count, double Centre, bool bGhost)
		{
			UStaticMesh* Mesh = Look.Baked[FMath::Min(Count, Look.Baked.Num()) - 1];
			if (UInstancedStaticMeshComponent* Into = PoolFor(Mesh, bGhost))
			{
				const FKitBox Box = KitBoxOf(*Mesh, Yaw, /*bMirrorRun=*/false);
				const FVector2D Mid = (Box.Min + Box.Max) * 0.5;
				Into->AddInstance(PieceAt(RunCentre, Heading, -Mid.X, Centre - Mid.Y, Yaw,
					/*bMirrorRun=*/false, -Box.MinZ), /*bWorldSpace=*/true);
			}
		};
		if (Lit > 0)
		{
			Span(Lit, Start + LitWidth * 0.5, false);
		}
		if (Dark > 0 && GhostBoxes != nullptr)
		{
			Span(Dark, Start + LitWidth + (FullWidth - LitWidth) * 0.5, true);
		}
		return true;
	}

	// PARTS: cap, N bays, mirrored cap, laid along the run from its near end. Every piece is
	// aligned by its OWN bounds' near edge, so a mesh origin is not a contract; the depth is
	// centred on the BAY's bounds for every piece, so a cap whose eaves reach differently
	// still lines up with the wall it closes.
	const FKitBox Bay = KitBoxOf(*Look.Bay, Yaw, false);
	const FKitBox Cap = KitBoxOf(*Look.Cap, Yaw, false);
	const FKitBox FarCap = KitBoxOf(*Look.Cap, Yaw, /*bMirrorRun=*/true);
	const double DepthOffset = -(Bay.Min.X + Bay.Max.X) * 0.5;
	double Cursor = -Spec.RunWidthUu(Length) * 0.5;

	auto Piece = [&](UStaticMesh* Mesh, const FKitBox& Box, bool bMirror, bool bGhost, double Advance)
	{
		if (!bGhost || GhostBoxes != nullptr)
		{
			if (UInstancedStaticMeshComponent* Into = PoolFor(Mesh, bGhost))
			{
				Into->AddInstance(PieceAt(RunCentre, Heading, DepthOffset, Cursor - Box.Min.Y,
					Yaw, bMirror, -Box.MinZ), /*bWorldSpace=*/true);
			}
		}
		Cursor += Advance;
	};

	// BUILT BAYS GET BOTH CAPS - a partly-bought run is a finished building, not a cut one -
	// and the ghosted bays run on from its far cap with none of their own. That keeps the
	// whole run inside RunWidthUu(Length): two caps and Length bays, however it is split.
	// With nothing built the ghost is the whole building, caps included.
	const bool bBuilt = Lit > 0;
	Piece(Look.Cap, Cap, false, !bBuilt, Spec.RunEndUu);
	for (int32 Bays = 0; Bays < (bBuilt ? Lit : Length); ++Bays)
	{
		Piece(Look.Bay, Bay, false, !bBuilt, Spec.Footprint.WidthUu);
	}
	Piece(Look.Cap, FarCap, true, !bBuilt, Spec.RunEndUu);
	for (int32 Bays = 0; bBuilt && Bays < Dark; ++Bays)
	{
		Piece(Look.Bay, Bay, false, true, Spec.Footprint.WidthUu);
	}
	return true;
}

bool UPlotPresenter::GetInstanceTransformForTest(int32 Index, FTransform& OutTransform) const
{
	if (!Placed.IsValidIndex(Index))
	{
		return false;
	}
	OutTransform = Placed[Index];
	return true;
}

void UPlotPresenter::Clear()
{
	if (Boxes != nullptr)
	{
		Boxes->ClearInstances();
	}
	if (GhostBoxes != nullptr)
	{
		GhostBoxes->ClearInstances();
	}
	if (FencePostsInto != nullptr)
	{
		FencePostsInto->ClearInstances();
	}
	if (FenceHeavyPostsInto != nullptr)
	{
		FenceHeavyPostsInto->ClearInstances();
	}
	for (const TMap<TObjectPtr<UStaticMesh>, TObjectPtr<UInstancedStaticMeshComponent>>* Pool
		: { &MeshPool, &GhostMeshPool })
	{
		for (const auto& Entry : *Pool)
		{
			if (IsValid(Entry.Value))
			{
				Entry.Value->ClearInstances();
			}
		}
	}
	if (FenceFabricInto != nullptr)
	{
		// THROUGH THE SINK, EMPTY, rather than resetting the mesh by hand: the sink is the one
		// place that knows how buffers become this component's mesh, empty ones included.
		FDynamicMeshSink(FenceFabricInto, nullptr, /*bInUseConstantVertexColour=*/false,
			nullptr, /*bInQuiet=*/true).Accept(FRoadMeshBuffers());
	}
	Placed.Reset();
	Gates = 0;
	FencePosts = 0;
	FenceSpans = 0;
	RoomForMore = 0;
	ModuleBoxes = 0;
	Dropped = 0;
	Ghosts = 0;
}

void UPlotPresenter::RebuildFrom(const URoadNetwork& Network,
	TArrayView<const PlotYard::FKitSpec> Specs, const FFenceKit& Kit,
	TArrayView<const FDepotModuleLook> Looks)
{
	if (Boxes == nullptr)
	{
		return;
	}

	// CLEARED AND REBUILT WHOLE, like every other derived geometry in this plugin. An
	// incremental update would need to know which instance belonged to which entity, which
	// is a second index that must agree with the model - and the counts here are tens.
	Clear();

	// THE AUTHORED POSTS, when there are any. Set per rebuild rather than once, because the
	// content set can change under an open editor; SetStaticMesh is a no-op when unchanged.
	if (FencePostsInto != nullptr && Kit.LinePost != nullptr)
	{
		FencePostsInto->SetStaticMesh(Kit.LinePost);
	}
	if (FenceHeavyPostsInto != nullptr && Kit.HeavyPost != nullptr)
	{
		FenceHeavyPostsInto->SetStaticMesh(Kit.HeavyPost);
	}
	FRoadMeshBuffers Fabric;

	// THE GHOST MATERIAL IS THE ONE GhostBoxes WEARS, on every pooled ghost - one source, set
	// by the owner each rebuild, rather than a second resolution here. Re-applied per rebuild
	// for the fence posts' reason: the content set can change under an open editor.
	if (GhostBoxes != nullptr)
	{
		for (const auto& Entry : GhostMeshPool)
		{
			if (IsValid(Entry.Value))
			{
				ApplyGhostMaterial(*Entry.Value, GhostBoxes->GetMaterial(0));
			}
		}
	}

	// RESOLVED ONCE BY THE CALLER, not per plot: the specs are the same for every plot, and
	// resolving the same three kits once per depot would do the work once per building on
	// the airport.
	//
	// THROUGH ARoadNetworkActor::ResolveDepotKits, NOT DepotKitSpecs(GetContent()) (issue
	// #181): that was a second resolution of the table FPlotPlaceTool's ghost reads through
	// IRoadEditTarget::ResolveDepotKits, and the two drifted the moment DA_FuelDepot's layout
	// was authored - see ReservationFor's own comment on that failure. NULL CONTENT IS STILL A
	// LEGAL ANSWER and the tests rely on it: ResolveDepotKits falls every kit back to the
	// grey-box table.

	int32 Plots = 0;

	for (const FEntityInstance& Entity : Network.GetEntities())
	{
		if (!Entity.bAlive || Entity.Outline.Num() < 3)
		{
			continue;
		}

		// THE SAME GEOMETRY THE PLACEMENT USED, not a remembered list of bay transforms.
		// Storing them on the instance would be a second copy of something PlotFit already
		// derives from the outline, and the two would drift the moment a bay size changed.
		FVector2D FrontageA = FVector2D::ZeroVector;
		FVector2D FrontageB = FVector2D::ZeroVector;
		if (!RecoverFrontage(Entity, FrontageA, FrontageB))
		{
			continue;
		}

		++Plots;

		// THE YARD, NOT A GRID. Where each module stands is a question about the plot's
		// CONTENTS, with different inputs from the plot's own shape - and a row of identical
		// boxes all facing one way is what made a built depot read as a placeholder. The bay
		// grid that used to answer it is gone entirely; see the 2026-09-16 design docs.
		// THE PLOT'S WHOLE CAPACITY, decided once. The gate is where the fence is left open,
		// which is the entity's own pose - see the fence loop below, which skips the bay
		// nearest exactly this point.
		//
		// RE-DERIVED, NEVER SAVED. Every input is already on the entity and DepotYardSeed
		// keys off the pose, so the same plot solves the same way on every rebuild and the
		// save keeps only what the player bought.
		//
		// THE PLOT TYPE DECIDES ITS OWN ARRANGEMENT. A fuel depot bands; something meant to
		// look unplanned still scatters. A null definition keeps the scatter, which is what
		// an un-migrated save has.
		FPlotSite PlotSite;
		PlotSite.Outline = Entity.Outline;
		PlotSite.FrontageA = FrontageA;
		PlotSite.FrontageB = FrontageB;
		PlotSite.Gate = Entity.Position;
		PlotSite.Seed = DepotYardSeed(Entity.Position);

		const EPlotLayout Layout = Entity.Definition != nullptr
			? Entity.Definition->Layout : EPlotLayout::Scatter;

		const PlotYard::FReservation Reservation =
			PlotLayoutFor(Layout)->Solve(PlotSite, Specs);

		// HOW MANY OF EACH THE PLAYER HAS BOUGHT. Entity.Modules is still the owned list and
		// still this depot's only record in the save.
		TArray<int32> Owned;
		Owned.SetNumZeroed(Specs.Num());
		for (const EDepotModule Module : Entity.Modules)
		{
			const int32 Kit = static_cast<int32>(Module);
			if (Owned.IsValidIndex(Kit))
			{
				++Owned[Kit];
			}
		}

		// MODULES ONLY IN Placed since 2026-09-22 - the fence draws into its own components, so
		// the first ModuleBoxes instances of a plot are its modules with no ordering to keep.
		// The ghosts go in their own component, so they never enter that count at all.
		for (const PlotYard::FReservedStand& Stand : Reservation.Stands)
		{
			if (!Specs.IsValidIndex(Stand.KitIndex))
			{
				continue;
			}
			const PlotYard::FKitSpec& Spec = Specs[Stand.KitIndex];
			const PlotYard::FFootprint& One = Spec.Footprint;
			const double HeightUu = HeightFor(static_cast<EDepotModule>(Stand.KitIndex));

			// A RUN FILLS FROM ONE END. Bays the player owns are drawn solid at that end and
			// the rest ghosted, so a run visibly GROWS along its length rather than appearing
			// whole. Boxes, a baked mesh per span, or parts laid bay by bay - see DrawMeshes.
			const int32 Lit = FMath::Clamp(Owned[Stand.KitIndex], 0, Stand.RunLength);
			Owned[Stand.KitIndex] -= Lit;
			const int32 Dark = Stand.RunLength - Lit;

			// Along the run's own width axis, which is the stand's LEFT - StandCorners builds
			// its corners from Forward and PerpCCW(Forward), so the same perpendicular here
			// keeps the two halves inside the ground the solver actually reserved.
			const FVector2D Forward(FMath::Cos(Stand.Heading), FMath::Sin(Stand.Heading));
			const FVector2D Across = RoadGeom::PerpCCW(Forward);

			// THROUGH RunWidthUu, caps included, so the spans below split exactly the width the
			// solver reserved. The built span is a whole building - both caps - and the ghosts
			// take what is left; with no caps (every grey box) this is the old N x width.
			const double FullWidth = Spec.RunWidthUu(Stand.RunLength);
			const double LitWidth = Lit > 0 ? Spec.RunWidthUu(Lit) : 0.0;
			const double DarkWidth = FullWidth - LitWidth;

			// FLUSH TO THE BACK OF WHAT IT CLAIMED - ONLY WHEN THE STAND CLAIMED AN APRON AT
			// ALL. A stand's centre is the centre of its footprint PLUS its apron only under
			// UFuelYardBandsStrategy (Reservation.bStandsIncludeApron), and the apron reaches
			// towards the gate - so the object sits half an apron further back there, leaving
			// that ground clear in front of its door. PlotYard::Reserve (Scatter) never reads
			// ApronUu when it samples a pose, so its Stand.Centre is already the footprint's
			// own centre - applying this offset there drew the box half an apron outside the
			// ground the sampler actually fenced off (issue #193).
			const FVector2D ToBack = Reservation.bStandsIncludeApron
				? Forward * (Spec.ApronUu.X * 0.5)
				: FVector2D::ZeroVector;
			const FVector2D RunCentre = Stand.Centre + ToBack;

			const FDepotModuleLook* Look =
				Looks.IsValidIndex(Stand.KitIndex) ? &Looks[Stand.KitIndex] : nullptr;
			const bool bMeshes = Look != nullptr
				&& DrawMeshes(*Look, Spec, RunCentre, Stand.Heading, Lit, Dark);

			if (Lit > 0)
			{
				const FVector2D Centre = RunCentre + Across * ((LitWidth - FullWidth) * 0.5);
				const FTransform ModuleAt =
					BoxAt(Centre, Stand.Heading, One.LengthUu, LitWidth, HeightUu);
				// THE ENVELOPE IS RECORDED EITHER WAY: GetInstanceTransformForTest answers
				// where the building stands, whatever it is drawn with.
				if (!bMeshes)
				{
					Boxes->AddInstance(ModuleAt, /*bWorldSpace=*/true);
				}
				Placed.Add(ModuleAt);
				ModuleBoxes += Lit;
			}

			if (Dark > 0 && GhostBoxes != nullptr && !bMeshes)
			{
				const FVector2D Centre = RunCentre + Across * ((FullWidth - DarkWidth) * 0.5);
				GhostBoxes->AddInstance(
					BoxAt(Centre, Stand.Heading, One.LengthUu, DarkWidth, HeightUu),
					/*bWorldSpace=*/true);
			}
			Ghosts += Dark;
		}

		// AN OWNED MODULE THAT RESERVED NO STAND IS A DROP. Every stand of a kit already took
		// what it could hold (the Owned[Kit] -= Lit above), so whatever is left over is a
		// module the player bought that the reservation never offered any ground - the case
		// GetDroppedCount's own comment calls a bug rather than a refusal. This is the ++Dropped
		// the run-length rewrite (61f92fc) deleted along with the old one-module-per-stand loop;
		// see Airside.Present.PlotPresenterCountsDrops.
		for (const int32 Leftover : Owned)
		{
			Dropped += Leftover;
		}

		// --- The fence -----------------------------------------------------------------
		//
		// THE GATE IS A GAP OF EXACTLY THE TRUCK CORRIDOR, centred on the pose, so the hole in
		// the fence is where the truck actually leaves and as wide as the lane PlotYard keeps
		// clear behind it - one number, not two decisions that could disagree.
		const FenceLayout::FLayout Fence = FenceLayout::Solve(Entity.Outline, Entity.Position, FenceSpec());
		if (Fence.bHasGate)
		{
			++Gates;
		}
		else
		{
			// A DEPOT NO TRUCK CAN LEAVE, said out loud - see GetGateGapCount.
			UE_LOG(LogAirside, Warning,
				TEXT("Plots: the plot gated at (%.0f, %.0f) has no gate - its frontage is ")
				TEXT("shorter than a %.0f uu gate plus a bay either side"),
				Entity.Position.X, Entity.Position.Y, PlotYard::GateCorridorUu);
		}

		for (const FenceLayout::FPost& Post : Fence.Posts)
		{
			const bool bHeavy = Post.Kind != FenceLayout::EPostKind::Line;
			UHierarchicalInstancedStaticMeshComponent* Into =
				bHeavy ? FenceHeavyPostsInto.Get() : FencePostsInto.Get();
			if (Into == nullptr)
			{
				continue;
			}
			Into->AddInstance(FencePostAt(Post, bHeavy ? Kit.HeavyPost : Kit.LinePost),
				/*bWorldSpace=*/true);
			++FencePosts;
		}
		for (const FenceLayout::FSpan& Span : Fence.Spans)
		{
			AppendFenceBay(Fabric, Span);
			++FenceSpans;
		}
	}

	// ONE STRIP FOR EVERY PLOT'S FABRIC, one draw call - the reason the fabric is a strip and
	// not a mesh per bay (asset README). Quiet: the sink's per-call DIAG lines describe the
	// road surface, and a fence line beside them would read as a second road.
	if (FenceFabricInto != nullptr)
	{
		FDynamicMeshSink(FenceFabricInto, Kit.Fabric, /*bInUseConstantVertexColour=*/false,
			nullptr, /*bInQuiet=*/true).Accept(Fabric);
	}

	// WHAT IS LEFT TO GROW INTO: a count of unlit bays, not a sampled estimate. It used to
	// continue the placement loop with a phantom tank until it failed; the reservation
	// already knows, so asking again would be a second opinion about one question.
	//
	// ASSIGNED ONCE, AFTER THE LOOP, because Ghosts accumulates across every plot - adding it
	// per entity counted the first depot's spare room again for the second.
	RoomForMore = Ghosts;

	// ONE CENSUS LINE PER REBUILD, beside the surface builder's own. Zero plots is the
	// common idle rebuild and stays quiet.
	//
	// IT NAMES THE GATES, which is the one thing that cannot be seen from a box count: a
	// fence with no gap is a depot no truck can leave, and it looks completely correct from
	// every angle on screen.
	if (Plots > 0)
	{
		// A POOLED COMPONENT THAT CANNOT DRAW, said out loud. On 2026-09-22 the shed and tank
		// were built in PIE, every count was right, and nothing showed: the components belonged
		// to the CDO - no world, never registered. Quiet when all is well.
		for (const TMap<TObjectPtr<UStaticMesh>, TObjectPtr<UInstancedStaticMeshComponent>>* Pool
			: { &MeshPool, &GhostMeshPool })
		{
			for (const auto& Entry : *Pool)
			{
				const UInstancedStaticMeshComponent* Comp = Entry.Value;
				if (IsValid(Comp) && Comp->GetInstanceCount() > 0 && !Comp->IsRegistered())
				{
					UE_LOG(LogAirside, Warning,
						TEXT("Plots: the %s component for %s holds %d instance(s) but is not ")
						TEXT("registered (owner %s, world %s) - they draw nowhere"),
						Pool == &GhostMeshPool ? TEXT("ghost") : TEXT("module"),
						*GetNameSafe(Entry.Key), Comp->GetInstanceCount(),
						*GetNameSafe(Comp->GetOwner()), *GetNameSafe(Comp->GetWorld()));
				}
			}
		}

		// IT NAMES THE GHOSTS, because a depot drawn entirely in ghosts is a depot nobody has
		// bought anything for - which looks identical to a broken presenter from a box count
		// alone.
		UE_LOG(LogAirside, Log,
			TEXT("Plots: %d plot(s), %d module bay(s) built, %d ghosted, %d dropped, "
				 "%d fence post(s), %d fabric bay(s), %d gate(s)"),
			Plots, ModuleBoxes, Ghosts, Dropped, FencePosts, FenceSpans, Gates);
	}
}
