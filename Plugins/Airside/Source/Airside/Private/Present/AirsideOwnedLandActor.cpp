#include "Present/AirsideOwnedLandActor.h"

#include "AirsideLog.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Content/AirsideSettings.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"

// THE NAMES ARE airside_matnodes.OWNED_RECT_PARAMS'. A name that matches nothing in the collection
// makes SetScalarParameterValue return false, which Apply logs, and the ground stays unclipped.
// ENFORCED BY: Airside.Content.OwnedLandWired (the collection carries all four, M_Ground reads it)
const FName AAirsideOwnedLandActor::CollectionParams[4] = {
	TEXT("OwnedMinX"), TEXT("OwnedMinY"), TEXT("OwnedMaxX"), TEXT("OwnedMaxY") };

namespace
{
	// "Owns everything" - the collection's own defaults, and airside_matnodes.OWNED_RECT_EVERYTHING.
	// Written back when the actor leaves, so a deleted edge does not leave the ground clipped.
	constexpr double Everything = 1.0e9;
}

AAirsideOwnedLandActor::AAirsideOwnedLandActor()
{
	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));

	static const TCHAR* Names[] = { TEXT("WallSouth"), TEXT("WallNorth"), TEXT("WallWest"), TEXT("WallEast") };
	for (const TCHAR* Name : Names)
	{
		UStaticMeshComponent* Wall = CreateDefaultSubobject<UStaticMeshComponent>(Name);
		Wall->SetupAttachment(RootComponent);
		// Scenery: nothing collides with the side of the world, and the cursor's road-plane
		// trace must not land on a wall instead of the ground beyond it.
		Wall->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Wall->SetCanEverAffectNavigation(false);
		Walls.Add(Wall);
	}
}

FBox2D AAirsideOwnedLandActor::GetOwnedLand() const
{
	if (OwnedMin.X < OwnedMax.X && OwnedMin.Y < OwnedMax.Y)
	{
		return FBox2D(OwnedMin, OwnedMax);
	}
	return FBox2D(ForceInit);
}

void AAirsideOwnedLandActor::SetOwnedLand(const FVector2D& InMin, const FVector2D& InMax)
{
	OwnedMin = InMin;
	OwnedMax = InMax;
	Apply();
}

AAirsideOwnedLandActor* AAirsideOwnedLandActor::Find(const UWorld* World)
{
	if (World == nullptr)
	{
		return nullptr;
	}
	// A linear walk of the level's actors, once per caller per session - two callers today
	// (the build camera on creation, the grass on a mask rebuild), 2026-10-02.
	AAirsideOwnedLandActor* Found = nullptr;
	for (TActorIterator<AAirsideOwnedLandActor> It(const_cast<UWorld*>(World)); It; ++It)
	{
		if (Found != nullptr)
		{
			UE_LOG(LogAirside, Warning, TEXT("OwnedLand: more than one owned-land actor (%s and %s); using the first."),
				*Found->GetName(), *It->GetName());
			break;
		}
		Found = *It;
	}
	return Found;
}

void AAirsideOwnedLandActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	Apply();
}

void AAirsideOwnedLandActor::BeginPlay()
{
	Super::BeginPlay();
	// AGAIN AT PLAY: PIE duplicates the level into a new world with its own collection instance,
	// which starts at the collection's defaults - "everything" - until something writes it.
	Apply();
}

void AAirsideOwnedLandActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	WriteCollection(FBox2D(ForceInit));
	Super::EndPlay(EndPlayReason);
}

void AAirsideOwnedLandActor::Destroyed()
{
	// Deleted in the editor: EndPlay never runs there, and the editor world's instance would
	// keep clipping to an edge that no longer exists.
	WriteCollection(FBox2D(ForceInit));
	Super::Destroyed();
}

void AAirsideOwnedLandActor::Apply()
{
	const FBox2D Land = GetOwnedLand();
	if (!Land.bIsValid)
	{
		UE_LOG(LogAirside, Warning, TEXT("OwnedLand: %s has Min (%.0f, %.0f) not below Max (%.0f, %.0f) - owning everything."),
			*GetName(), OwnedMin.X, OwnedMin.Y, OwnedMax.X, OwnedMax.Y);
	}
	WriteCollection(Land);

	const FOwnedLandKit Kit = UAirsideSettings::ResolveOwnedLandKit();
	const double GroundZ = GetActorLocation().Z;
	const double Depth = FMath::Max(PlinthDepth, 100.0);
	const double T = FMath::Max(WallThickness, 1.0);
	// Wall tops 5 uu under the ground: the clipped landscape covers them from above, and a top
	// flush with it would z-fight along the whole rim.
	const double CentreZ = GroundZ - 5.0 - Depth * 0.5;
	const FVector2D Size = Land.bIsValid ? Land.GetSize() : FVector2D::ZeroVector;
	const FVector2D Centre = Land.bIsValid ? Land.GetCenter() : FVector2D::ZeroVector;

	// Inside the rectangle, so the outer face IS the edge the clip cuts the ground at.
	const struct { FVector Location; FVector Extent; } Placement[4] = {
		{ FVector(Centre.X, OwnedMin.Y + T * 0.5, CentreZ), FVector(Size.X, T, Depth) },
		{ FVector(Centre.X, OwnedMax.Y - T * 0.5, CentreZ), FVector(Size.X, T, Depth) },
		{ FVector(OwnedMin.X + T * 0.5, Centre.Y, CentreZ), FVector(T, Size.Y, Depth) },
		{ FVector(OwnedMax.X - T * 0.5, Centre.Y, CentreZ), FVector(T, Size.Y, Depth) },
	};
	for (int32 i = 0; i < Walls.Num(); ++i)
	{
		UStaticMeshComponent* Wall = Walls[i];
		Wall->SetStaticMesh(Kit.WallMesh);
		Wall->SetMaterial(0, Kit.WallMaterial);
		Wall->SetVisibility(Land.bIsValid && Kit.WallMesh != nullptr);
		// World-space, so the actor's own placement moves only GroundZ, never the rectangle - the
		// rectangle is in road-plane coordinates the camera and grass read directly.
		Wall->SetWorldLocationAndRotation(Placement[i].Location, FRotator::ZeroRotator);
		// The engine cube is 100 uu on a side.
		Wall->SetWorldScale3D(Placement[i].Extent / 100.0);
	}
	if (Kit.WallMesh == nullptr)
	{
		UE_LOG(LogAirside, Warning, TEXT("OwnedLand: no PlinthWallMesh in the content set - the edge has no walls."));
	}
}

void AAirsideOwnedLandActor::WriteCollection(const FBox2D& Bounds) const
{
	UWorld* World = GetWorld();
	UMaterialParameterCollection* Collection = UAirsideSettings::ResolveOwnedLandKit().Collection;
	if (World == nullptr || Collection == nullptr)
	{
		if (World != nullptr)
		{
			UE_LOG(LogAirside, Warning, TEXT("OwnedLand: no OwnedLandCollection in the content set - the ground is not clipped."));
		}
		return;
	}
	UMaterialParameterCollectionInstance* Instance = World->GetParameterCollectionInstance(Collection);
	if (Instance == nullptr)
	{
		return;
	}
	const double Values[4] = {
		Bounds.bIsValid ? Bounds.Min.X : -Everything, Bounds.bIsValid ? Bounds.Min.Y : -Everything,
		Bounds.bIsValid ? Bounds.Max.X : Everything, Bounds.bIsValid ? Bounds.Max.Y : Everything };
	for (int32 i = 0; i < 4; ++i)
	{
		if (!Instance->SetScalarParameterValue(CollectionParams[i], static_cast<float>(Values[i])))
		{
			UE_LOG(LogAirside, Warning, TEXT("OwnedLand: %s has no scalar '%s'."),
				*Collection->GetName(), *CollectionParams[i].ToString());
		}
	}
	UE_LOG(LogAirside, Log, TEXT("OwnedLand: ground clipped to (%.0f, %.0f)-(%.0f, %.0f) in %s."),
		Values[0], Values[1], Values[2], Values[3], *World->GetName());
}
