#include "Present/AirsideBuildingsActor.h"

#include "AirsideLog.h"
#include "Components/DynamicMeshComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Content/AirsidePrimitives.h"
#include "Content/AirsideSettings.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Model/RoadNetwork.h"
#include "Present/PlotPresenter.h"
#include "Present/RoadNetworkActor.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	/**
	 * An instanced component of engine cubes, no collision.
	 *
	 * THE ENGINE'S OWN PRIMITIVE, not an authored asset, for the reason ARoadAgentActor's
	 * placeholder records at its own FObjectFinder - grey-box geometry that shows only until
	 * real meshes arrive has no business owning content of its own. No collision, matching
	 * every surface the road network draws: the world is flat and every pick is exact maths
	 * against the road plane, so a collider here would be something the build tools could
	 * trace against by accident.
	 */
	void DressAsCubes(UInstancedStaticMeshComponent& Component, UStaticMesh* Cube)
	{
		if (Cube != nullptr)
		{
			Component.SetStaticMesh(Cube);
		}
		Component.SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
}

AAirsideBuildingsActor::AAirsideBuildingsActor()
{
	// Nothing here ticks: every rebuild is driven by the road network's delegate.
	PrimaryActorTick.bCanEverTick = false;

	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));

	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(AirsidePrimitives::CubePath());
	UStaticMesh* CubeMesh = Cube.Succeeded() ? Cube.Object : nullptr;

	ModuleBoxes = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("ModuleBoxes"));
	ModuleBoxes->SetupAttachment(RootComponent);
	DressAsCubes(*ModuleBoxes, CubeMesh);

	// THE GHOSTS GET THEIR OWN COMPONENT, sharing the cube and differing only in material -
	// see ModuleGhosts' own comment.
	ModuleGhosts = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("ModuleGhosts"));
	ModuleGhosts->SetupAttachment(RootComponent);
	DressAsCubes(*ModuleGhosts, CubeMesh);

	// THE FENCE. Posts start as the cube for the grey-box fallback; the presenter swaps in the
	// authored meshes each rebuild when the content set has them.
	FencePosts = CreateDefaultSubobject<UHierarchicalInstancedStaticMeshComponent>(TEXT("FencePosts"));
	FencePosts->SetupAttachment(RootComponent);
	DressAsCubes(*FencePosts, CubeMesh);

	FenceHeavyPosts = CreateDefaultSubobject<UHierarchicalInstancedStaticMeshComponent>(TEXT("FenceHeavyPosts"));
	FenceHeavyPosts->SetupAttachment(RootComponent);
	DressAsCubes(*FenceHeavyPosts, CubeMesh);

	// ABSOLUTE, like every surface ARoadNetworkActor draws: the strip is built in world
	// coordinates and must not be transformed a second time.
	FenceFabric = CreateDefaultSubobject<UDynamicMeshComponent>(TEXT("FenceFabric"));
	FenceFabric->SetupAttachment(RootComponent);
	FenceFabric->SetUsingAbsoluteLocation(true);
	FenceFabric->SetUsingAbsoluteRotation(true);
	FenceFabric->SetUsingAbsoluteScale(true);
	FenceFabric->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	FenceFabric->bAffectDistanceFieldLighting = false;

	Plots = CreateDefaultSubobject<UPlotPresenter>(TEXT("Plots"));
	Plots->Initialise(ModuleBoxes, ModuleGhosts,
		FFenceTargets{ FencePosts, FenceHeavyPosts, FenceFabric }, RootComponent);
}

void AAirsideBuildingsActor::PostInitProperties()
{
	Super::PostInitProperties();

	// PIE DUPLICATES THE LEVEL, and a Transient non-instanced pointer comes back naming the
	// CDO's subobject rather than this actor's - the same repair ARoadNetworkActor makes, by
	// NAME so the constructor's Initialise call is kept rather than lost to a fresh object.
	// The components travel the same way and the presenter must be re-pointed AT THEM, or a
	// duplicate's boxes are added to a component no level renders.
	Plots = Cast<UPlotPresenter>(GetDefaultSubobjectByName(TEXT("Plots")));
	ModuleBoxes = Cast<UInstancedStaticMeshComponent>(GetDefaultSubobjectByName(TEXT("ModuleBoxes")));
	ModuleGhosts = Cast<UInstancedStaticMeshComponent>(GetDefaultSubobjectByName(TEXT("ModuleGhosts")));
	FencePosts = Cast<UHierarchicalInstancedStaticMeshComponent>(GetDefaultSubobjectByName(TEXT("FencePosts")));
	FenceHeavyPosts = Cast<UHierarchicalInstancedStaticMeshComponent>(GetDefaultSubobjectByName(TEXT("FenceHeavyPosts")));
	FenceFabric = Cast<UDynamicMeshComponent>(GetDefaultSubobjectByName(TEXT("FenceFabric")));
	if (Plots != nullptr)
	{
		Plots->Initialise(ModuleBoxes, ModuleGhosts,
			FFenceTargets{ FencePosts, FenceHeavyPosts, FenceFabric }, RootComponent);
	}
}

void AAirsideBuildingsActor::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();

	// Templates excluded, for ARoadNetworkActor::PostRegisterAllComponents' reason: a class
	// default object has nothing to draw and no world to search.
	if (HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
	{
		return;
	}

	ARoadNetworkActor* Road = RoadNetwork.Get();
	if (Road == nullptr)
	{
		int32 Found = 0;
		for (TActorIterator<ARoadNetworkActor> It(GetWorld()); It; ++It)
		{
			Road = *It;
			++Found;
		}
		if (Found != 1)
		{
			// NEVER A GUESS BETWEEN TWO - see RoadNetwork's own comment.
			UE_LOG(LogAirside, Warning,
				TEXT("Buildings: %d road network(s) in %s and none named - drawing nothing. ")
				TEXT("Set RoadNetwork on %s."),
				Found, *GetNameSafe(GetWorld()), *GetName());
			BindTo(nullptr);
			return;
		}
	}
	BindTo(Road);
}

void AAirsideBuildingsActor::UnregisterAllComponents(bool bForReregister)
{
	// A reregister (a property edit in the Details panel does one) keeps the binding: the
	// road network has not changed, and PostRegisterAllComponents rebinds anyway.
	if (!bForReregister)
	{
		Unbind();
	}
	Super::UnregisterAllComponents(bForReregister);
}

AAirsideBuildingsActor* AAirsideBuildingsActor::Find(const UWorld* World)
{
	if (World == nullptr)
	{
		return nullptr;
	}
	for (TActorIterator<AAirsideBuildingsActor> It(const_cast<UWorld*>(World)); It; ++It)
	{
		return *It;
	}
	return nullptr;
}

AAirsideBuildingsActor* AAirsideBuildingsActor::FindOrCreate(UWorld* World, ARoadNetworkActor* Road)
{
	if (AAirsideBuildingsActor* Existing = Find(World))
	{
		if (Existing->GetRoadNetwork() != Road)
		{
			Existing->RoadNetwork = Road;
			Existing->BindTo(Road);
		}
		return Existing;
	}
	if (World == nullptr)
	{
		return nullptr;
	}

	// DEFERRED, so RoadNetwork is set before PostRegisterAllComponents runs and the search
	// there is never consulted for an actor whose road network is already known.
	AAirsideBuildingsActor* Spawned = World->SpawnActorDeferred<AAirsideBuildingsActor>(
		AAirsideBuildingsActor::StaticClass(), FTransform::Identity);
	if (Spawned == nullptr)
	{
		return nullptr;
	}
	Spawned->RoadNetwork = Road;
	Spawned->FinishSpawning(FTransform::Identity);
	return Spawned;
}

void AAirsideBuildingsActor::BindTo(ARoadNetworkActor* Road)
{
	Unbind();

	if (Road == nullptr)
	{
		// Nothing to draw FOR, so nothing drawn - a depot left standing from a road network
		// that has gone would be a building the model no longer holds.
		if (Plots != nullptr)
		{
			Plots->Clear();
		}
		return;
	}

	Bound = Road;
	BoundHandle = Road->OnTopologyRebuilt.AddUObject(this, &AAirsideBuildingsActor::Rebuild);
	UE_LOG(LogAirside, Log, TEXT("Buildings: %s drawing plots for %s"),
		*GetName(), *Road->GetName());

	// THE CATCH-UP - see BindTo's own comment.
	if (Road->Network != nullptr)
	{
		Rebuild(*Road->Network);
	}
}

void AAirsideBuildingsActor::Unbind()
{
	if (ARoadNetworkActor* Road = Bound.Get())
	{
		Road->OnTopologyRebuilt.Remove(BoundHandle);
	}
	Bound.Reset();
	BoundHandle.Reset();
}

void AAirsideBuildingsActor::Rebuild(const URoadNetwork& Network)
{
	ARoadNetworkActor* Road = Bound.Get();
	if (Plots == nullptr || Road == nullptr)
	{
		return;
	}

	// THROUGH THE RESOLVER, never the raw property, and here rather than in the constructor:
	// a CDO cannot LoadSynchronous, and the material a level authored is only known once the
	// road network exists. Null leaves the cube's default, which reads as a built bay - wrong,
	// but visible, which is the failure mode to prefer. Moved from
	// ARoadNetworkActor::RebuildMeshForChange with the components it colours.
	if (ModuleGhosts != nullptr)
	{
		ModuleGhosts->SetMaterial(0, Road->ResolveGhostMaterial());
	}

	// THROUGH THE ROAD NETWORK'S ONE RESOLVER (issue #181) - see UPlotPresenter::RebuildFrom.
	// THE FENCE'S CONTENT AND THE MODULES' MESHES through UAirsideSettings' one resolver each,
	// like every content default.
	Plots->RebuildFrom(Network, Road->ResolveDepotKits(), UAirsideSettings::ResolveFenceKit(),
		UAirsideSettings::ResolveDepotLooks());
}
