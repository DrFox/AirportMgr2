#include "Present/AirsideOwnedLandActor.h"

#include "AirsideLog.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Content/AirsideSettings.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "Model/LandGrid.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadEditFacade.h"
#include "Present/RoadNetworkActor.h"

// THE NAMES ARE airside_matnodes.OWNED_LAND_PARAMS'. A name that matches nothing in the collection makes
// SetScalarParameterValue return false, which WriteCollection logs, and the ground stays unclipped.
// ENFORCED BY: Airside.Content.OwnedLandWired (the collection carries all ten, M_Ground reads it)
const FName AAirsideOwnedLandActor::CollectionParams[10] = {
	TEXT("LandValid"), TEXT("LandOriginX"), TEXT("LandOriginY"), TEXT("LandTileSize"), TEXT("LandColumns"),
	TEXT("LandRows"), TEXT("LandMask0"), TEXT("LandMask1"), TEXT("LandMask2"), TEXT("LandMask3") };

namespace
{
	/** The airport's land, or an invalid grid (owns everything) when the level has no airport or no network yet. */
	const FLandGrid& LandOf(const UWorld* World)
	{
		static const FLandGrid None;
		const ARoadNetworkActor* Road = ARoadNetworkActor::Find(World);
		return Road != nullptr && Road->Network != nullptr ? Road->Network->GetOwnedLand() : None;
	}
}

AAirsideOwnedLandActor::AAirsideOwnedLandActor()
{
	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

AAirsideOwnedLandActor* AAirsideOwnedLandActor::Find(const UWorld* World)
{
	if (World == nullptr)
	{
		return nullptr;
	}
	// A linear walk of the level's actors - the tests and the editor ask; nothing per frame (2026-10-02).
	AAirsideOwnedLandActor* Found = nullptr;
	for (TActorIterator<AAirsideOwnedLandActor> It(const_cast<UWorld*>(World)); It; ++It)
	{
		if (Found != nullptr)
		{
			UE_LOG(LogAirside, Warning, TEXT("OwnedLand: more than one edge actor (%s and %s); using the first."),
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
	// BOUND AT CONSTRUCTION, NOT ONLY AT PLAY (final review 2026-10-03): the editor's Road Build mode offers Buy land
	// and undoes land through its transaction, and no BeginPlay ever runs there - an edge bound only in BeginPlay kept
	// its walls and clip on the old outline. Bind draws the current land as its catch-up, so the editor shows the
	// level's authored land as before.
	// ENFORCED BY: Airside.Present.OwnedLand.WallsFollowWithoutPlay
	Bind();
}

void AAirsideOwnedLandActor::BeginPlay()
{
	Super::BeginPlay();
	// AGAIN AT PLAY, through the binding: PIE duplicates the level into a new world with its own collection instance,
	// which starts at the collection's defaults - "everything" - until something writes it.
	Bind();
}

void AAirsideOwnedLandActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	Unbind();
	WriteCollection(FLandGrid());
	Super::EndPlay(EndPlayReason);
}

void AAirsideOwnedLandActor::Destroyed()
{
	// Deleted in the editor: EndPlay never runs there, and the editor world's instance would keep clipping to an
	// edge nothing draws any more.
	Unbind();
	WriteCollection(FLandGrid());
	Super::Destroyed();
}

void AAirsideOwnedLandActor::Bind()
{
	Unbind();
	ARoadNetworkActor* Road = ARoadNetworkActor::Find(GetWorld());
	if (URoadEditFacade* Facade = Road != nullptr ? Road->GetEditFacade() : nullptr)
	{
		BoundFacade = Facade;
		BoundHandle = Facade->OnOwnedLandChanged.AddUObject(this, &AAirsideOwnedLandActor::Apply);
	}
	// THE CATCH-UP: the land may have been set before this bound.
	Apply(LandOf(GetWorld()));
}

void AAirsideOwnedLandActor::Unbind()
{
	if (URoadEditFacade* Facade = BoundFacade.Get())
	{
		Facade->OnOwnedLandChanged.Remove(BoundHandle);
	}
	BoundFacade.Reset();
	BoundHandle.Reset();
}

void AAirsideOwnedLandActor::AuthorStartingLand(FVector2D Origin, double TileSize, int32 Columns, int32 Rows,
	const TArray<FIntPoint>& StartTiles)
{
	ARoadNetworkActor* Road = ARoadNetworkActor::Find(GetWorld());
	URoadEditFacade* Facade = Road != nullptr ? Road->GetEditFacade() : nullptr;
	if (Facade == nullptr)
	{
		UE_LOG(LogAirside, Warning, TEXT("OwnedLand: no airport in this level to author land on"));
		return;
	}
	Facade->AuthorOwnedLand(FLandGrid::Make(Origin, TileSize, Columns, Rows, StartTiles));
	// Bound since construction, so the facade's broadcast has already redrawn; this is the catch-up for an actor whose
	// airport registered after it was constructed.
	Apply(LandOf(GetWorld()));
}

UStaticMeshComponent* AAirsideOwnedLandActor::WallAt(int32 Index)
{
	while (Walls.Num() <= Index)
	{
		UStaticMeshComponent* Wall = NewObject<UStaticMeshComponent>(this, NAME_None, RF_Transient);
		Wall->SetupAttachment(RootComponent);
		// Scenery: nothing collides with the side of the world, and the cursor's road-plane trace must not land on
		// a wall instead of the ground beyond it.
		Wall->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Wall->SetCanEverAffectNavigation(false);
		Wall->RegisterComponent();
		Walls.Add(Wall);
	}
	return Walls[Index];
}

void AAirsideOwnedLandActor::Apply(const FLandGrid& Land)
{
	WriteCollection(Land);

	const FOwnedLandKit Kit = UAirsideSettings::ResolveOwnedLandKit();
	const TArray<FLandEdgeRun> Runs = Land.Outline();   // empty when invalid: no walls
	const double GroundZ = GetActorLocation().Z;
	const double Depth = FMath::Max(PlinthDepth, 100.0);
	const double T = FMath::Max(WallThickness, 1.0);
	// Wall tops 5 uu under the ground: the clipped landscape covers them from above, and a top flush with it would
	// z-fight along the whole rim.
	const double CentreZ = GroundZ - 5.0 - Depth * 0.5;
	for (int32 I = 0; I < Runs.Num(); ++I)
	{
		const FLandEdgeRun& Run = Runs[I];
		const FVector2D Along = Run.B - Run.A;
		// Inside the land, so the outer face IS the edge the clip cuts the ground at.
		const FVector2D Centre = (Run.A + Run.B) * 0.5 - Run.Outward * (T * 0.5);
		const bool bAlongX = FMath::Abs(Along.X) > FMath::Abs(Along.Y);
		const FVector Extent(bAlongX ? FMath::Abs(Along.X) : T, bAlongX ? T : FMath::Abs(Along.Y), Depth);
		UStaticMeshComponent* Wall = WallAt(I);
		Wall->SetStaticMesh(Kit.WallMesh);
		Wall->SetMaterial(0, Kit.WallMaterial);
		// World-space: the actor's placement sets GroundZ only; the runs are road-plane coordinates.
		Wall->SetWorldLocationAndRotation(FVector(Centre.X, Centre.Y, CentreZ), FRotator::ZeroRotator);
		Wall->SetWorldScale3D(Extent / 100.0);   // the engine cube is 100 uu on a side
		Wall->SetVisibility(Kit.WallMesh != nullptr);
	}
	for (int32 I = Runs.Num(); I < Walls.Num(); ++I)
	{
		Walls[I]->SetVisibility(false);
	}
	LiveWalls = Runs.Num();
	if (Runs.Num() > 0 && Kit.WallMesh == nullptr)
	{
		UE_LOG(LogAirside, Warning, TEXT("OwnedLand: no PlinthWallMesh in the content set - the edge has no walls."));
	}
	UE_LOG(LogAirside, Log, TEXT("OwnedLand: %d tile(s), %d wall run(s) in %s"),
		Land.IsValid() ? Land.NumOwned() : 0, LiveWalls, GetWorld() != nullptr ? *GetWorld()->GetName() : TEXT("?"));
}

void AAirsideOwnedLandActor::WriteCollection(const FLandGrid& Land) const
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
	const bool bValid = Land.IsValid();
	const float Values[10] = {
		bValid ? 1.0f : 0.0f, float(Land.Origin.X), float(Land.Origin.Y), float(Land.TileSize),
		float(Land.Columns), float(Land.Rows),
		float(Land.MaskWord(0)), float(Land.MaskWord(1)), float(Land.MaskWord(2)), float(Land.MaskWord(3)) };
	for (int32 I = 0; I < 10; ++I)
	{
		if (!Instance->SetScalarParameterValue(CollectionParams[I], Values[I]))
		{
			UE_LOG(LogAirside, Warning, TEXT("OwnedLand: %s has no scalar '%s'."), *Collection->GetName(), *CollectionParams[I].ToString());
		}
	}
}
