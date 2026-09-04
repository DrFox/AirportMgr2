#include "Debug/RoadJunctionGallery.h"

#include "Build/RoadMeshBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Components/DynamicMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Content/AirsideContent.h"
#include "Content/AirsideSettings.h"
#include "Debug/RoadDebugDraw.h"
#include "DrawDebugHelpers.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Profiles/RoadProfile.h"
#include "Solve/JunctionSolver.h"

DEFINE_LOG_CATEGORY_STATIC(LogRoadGallery, Log, All);

ARoadJunctionGallery::ARoadJunctionGallery()
{
	PrimaryActorTick.bCanEverTick = true;

	// A bare AActor has no transform to speak of: GetActorLocation() returns zero and
	// the actor cannot be moved in the editor. The gallery reads its own Z to lift the
	// debug lines off the ground, so give it a real root.
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));

	MeshComponent = CreateDefaultSubobject<UDynamicMeshComponent>(TEXT("GalleryMesh"));

	// The same material the network actor uses. The gallery is the harness built to
	// inspect junctions, so leaving it on a placeholder colour would hide the very thing
	// it exists to show.
	// Resolved where it is used, not here. A literal /Game/ path in a constructor is a
	// reference the editor cannot fix up when content moves - see UAirsideSettings.
	MeshComponent->SetupAttachment(RootComponent);

	// FRoadMeshBuilder emits absolute world coordinates, so the component must not
	// transform them again. Absolute placement pins it to world space while leaving the
	// actor free to be moved -- GetActorLocation() stays meaningful for the debug
	// overlay drawn in Tick, which is exactly why MeshComponent is a child of the root
	// rather than the root itself: SetWorldTransform(Identity) on a root component would
	// have teleported the actor (and pinned GetActorLocation().Z to 0) instead.
	MeshComponent->SetUsingAbsoluteLocation(true);
	MeshComponent->SetUsingAbsoluteRotation(true);
	MeshComponent->SetUsingAbsoluteScale(true);
}

void ARoadJunctionGallery::BeginPlay()
{
	Super::BeginPlay();
	BuildGallery();
}

#if WITH_EDITOR
void ARoadJunctionGallery::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// Tick rebuilds whenever Network is null, so dropping it is the whole rebuild.
	Network = nullptr;
	Profile = nullptr;
}
#endif

void ARoadJunctionGallery::BuildGallery()
{
	Network = NewObject<URoadNetwork>(this);
	Profile = URoadProfile::MakeTransient(TaxiwayWidth, FilletRadius);

	CellBearings = {
		{ 0.0, UE_DOUBLE_PI * (15.0 / 180.0) },
		{ 0.0, UE_DOUBLE_PI * 0.25 },
		{ 0.0, UE_DOUBLE_PI * 0.5 },
		{ 0.0, UE_DOUBLE_PI * (170.0 / 180.0) },
		{ 0.0, UE_DOUBLE_PI * 0.5, UE_DOUBLE_PI },
		{ 0.0, UE_DOUBLE_PI * 0.6667, UE_DOUBLE_PI * 1.3333 },
		{ 0.0, UE_DOUBLE_PI * 0.5, UE_DOUBLE_PI, UE_DOUBLE_PI * 1.5 },
		{ 0.0, UE_DOUBLE_PI * 0.4, UE_DOUBLE_PI * 0.8, UE_DOUBLE_PI * 1.2, UE_DOUBLE_PI * 1.6 }
	};

	CellCentres.Reset();
	constexpr int32 Columns = 4;
	for (int32 Index = 0; Index < CellBearings.Num(); ++Index)
	{
		const int32 Column = Index % Columns;
		const int32 Row = Index / Columns;
		CellCentres.Add(FVector2D(Column * CellSpacing, Row * CellSpacing));
	}

	// Each cell is a real sub-network: a centre node plus one outer node per arm.
	// The arm must reach past its own cut or the ribbon inverts, so ArmLength is a
	// floor, not the actual length.
	for (int32 CellIndex = 0; CellIndex < CellCentres.Num(); ++CellIndex)
	{
		const FVector2D Centre = CellCentres[CellIndex];
		const FRoadNodeId CentreNode = Network->AddNode(Centre);

		for (const double Bearing : CellBearings[CellIndex])
		{
			const FVector2D Dir(FMath::Cos(Bearing), FMath::Sin(Bearing));
			const FRoadNodeId Outer = Network->AddNode(Centre + Dir * ArmLength);
			Network->AddStraightSegment(CentreNode, Outer, Profile);
		}
	}

	UE_LOG(LogRoadGallery, Log, TEXT("Gallery built: %d cells, %d nodes, %d segments"),
		CellBearings.Num(), Network->GetNodes().Num(), Network->GetSegments().Num());

	RebuildGalleryMesh();
}

void ARoadJunctionGallery::RebuildGalleryMesh()
{
	if (Network == nullptr || MeshComponent == nullptr)
	{
		return;
	}

	const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Network);

	FRoadMeshBuilder Builder(10.0);

	Builder.Build(*Network, Solved, 1);

	if (SurfaceMaterial == nullptr)
	{
		if (const UAirsideContent* Content = UAirsideSettings::GetContent())
		{
			SurfaceMaterial = Content->SurfaceMaterial.LoadSynchronous();
		}
	}

	FDynamicMeshSink Sink(MeshComponent, SurfaceMaterial, true);
	Builder.Emit(Sink);

	UE_LOG(LogRoadGallery, Log, TEXT("Gallery mesh: %d nodes (%d failed), %d vertices, %d triangles"),
		Solved.SolvedNodes, Solved.FailedNodes,
		Builder.GetBuffers().Positions.Num(), Builder.GetBuffers().Indices.Num() / 3);
}

void ARoadJunctionGallery::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// Build lazily rather than only in BeginPlay, so the gallery is visible in the
	// editor viewport without entering play. Also picks up edits to CellSpacing,
	// ArmLength, TaxiwayWidth and FilletRadius after a rebuild is forced by clearing
	// Network (see PostEditChangeProperty).
	if (Network == nullptr)
	{
		BuildGallery();
	}

	// The solid surface is the slice 2a deliverable, so it renders whether or not the
	// debug overlay is on. Only the overlay is gated here.
	if (!bDrawDebugLines || RoadDebug::GetDebugDrawLevel() <= 0)
	{
		return;
	}

	// Ticking is not observable from outside, and "nothing drawn" looks identical
	// whether Tick never ran or the lines were sub-pixel. Say so, once.
	static int32 TickCount = 0;
	if (++TickCount == 5)
	{
		UE_LOG(LogRoadGallery, Log, TEXT("Tick is running: %d cells, thickness %.1f"),
			CellCentres.Num(), DebugLineThickness);
	}

	const double ZHeight = GetActorLocation().Z + 10.0;

	for (int32 CellIndex = 0; CellIndex < CellCentres.Num(); ++CellIndex)
	{
		FJunctionInput Input;
		Input.Position = CellCentres[CellIndex];
		Input.ArcSegments = 12;

		for (const double Bearing : CellBearings[CellIndex])
		{
			FJunctionArm Arm;
			Arm.Tangent = FVector2D(FMath::Cos(Bearing), FMath::Sin(Bearing));
			Arm.HalfWidthLeft = Profile->GetHalfWidthLeft();
			Arm.HalfWidthRight = Profile->GetHalfWidthRight();
			Arm.FilletRadius = Profile->PreferredFilletRadius;
			Input.Arms.Add(Arm);
		}

		FJunctionResult Result = FJunctionSolver::SolveCuts(Input);
		FJunctionSolver::SolveBoundary(Input, Result);

		RoadDebug::DrawJunction(GetWorld(), Input, Result, ZHeight, DebugLineThickness);

		// Draw each arm's ribbon edges from its cut out to ArmLength, in blue.
		for (int32 ArmIndex = 0; ArmIndex < Input.Arms.Num(); ++ArmIndex)
		{
			const FJunctionArm& Arm = Input.Arms[ArmIndex];
			const FVector2D Normal = FVector2D(-Arm.Tangent.Y, Arm.Tangent.X);

			// The arm must extend past its own cut, or it renders inside-out.
			const double DrawLength =
				FMath::Max(ArmLength, Result.Arms[ArmIndex].CutDistance + 3000.0);
			const FVector2D FarCentre = Input.Position + Arm.Tangent * DrawLength;

			const FVector2D FarLeft  = FarCentre + Normal * Arm.HalfWidthLeft;
			const FVector2D FarRight = FarCentre - Normal * Arm.HalfWidthRight;

			auto ToWorld = [ZHeight](const FVector2D& P) { return FVector(P.X, P.Y, ZHeight); };

			DrawDebugLine(GetWorld(), ToWorld(Result.Arms[ArmIndex].LeftCut),  ToWorld(FarLeft),
				FColor::Blue, false, 0.25f, 0, static_cast<float>(DebugLineThickness * 0.8));
			DrawDebugLine(GetWorld(), ToWorld(Result.Arms[ArmIndex].RightCut), ToWorld(FarRight),
				FColor::Blue, false, 0.25f, 0, static_cast<float>(DebugLineThickness * 0.8));
			DrawDebugLine(GetWorld(), ToWorld(FarLeft), ToWorld(FarRight),
				FColor::Blue, false, 0.25f, 0, static_cast<float>(DebugLineThickness * 0.5));
		}
	}
}
