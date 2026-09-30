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
#include "Present/RoadNetworkRegistry.h"
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
	// NO SHADOW from the fabric. The shadow pass samples T_Chainlink at a mip picked by the
	// shadow map's texel size, which steps with camera distance, and from mip 5 down the
	// alpha is uniform (0.213 at every texel, measured from chainlink.png 2026-09-23) - so
	// alpha coverage can only make the whole fabric solid or empty. It cast a 2.4 m wall
	// shadow at one zoom notch and nothing at the next. A real chainlink shadow is barely
	// there; the posts keep theirs. ENFORCED BY: Airside.Present.PlotFenceFabricCastsNoShadow
	FenceFabric->SetCastShadow(false);

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
	// THE ROOT BY NAME TOO, NOT RootComponent. At this point RootComponent still holds the value
	// InitProperties copied from the CDO - the CDO's own root - and pooled mesh components made
	// under it belong to the CDO: no world, never registered, drawn nowhere, while every
	// instance count reads correctly (PIE, 2026-09-22: "registered 0, world None").
	USceneComponent* Root = Cast<USceneComponent>(GetDefaultSubobjectByName(TEXT("Root")));
	if (Plots != nullptr)
	{
		Plots->Initialise(ModuleBoxes, ModuleGhosts,
			FFenceTargets{ FencePosts, FenceHeavyPosts, FenceFabric }, Root);
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

	// FOLLOW THE WORLD'S AIRPORT (#446) while RoadNetwork is unset: the registry says when one arrives or
	// leaves, so a buildings actor registered before its road network - actors register in no promised
	// order - binds the moment the road does, rather than drawing nothing for the whole session. Removed
	// first: a reregister runs this again, and a second binding would bind twice.
	URoadNetworkRegistry::OnAirportChanged().Remove(RegistryHandle);
	RegistryHandle = URoadNetworkRegistry::OnAirportChanged().AddUObject(this, &AAirsideBuildingsActor::OnAirportChanged);

	ARoadNetworkActor* Road = RoadNetwork.Get();
	if (Road == nullptr)
	{
		// THE REGISTRY'S ANSWER, NOT A SCAN (#446). NEVER A GUESS BETWEEN TWO is the registry's rule now: a
		// second network actor is refused there, loudly, so the one it holds is the only candidate there is.
		Road = URoadNetworkRegistry::Find(GetWorld());
		if (Road == nullptr)
		{
			// LOG, NOT WARNING (#446 review): actors register in level order, so on any PIE start or load where this
			// actor precedes the road it lands here and binds moments later, when the road registers. BeginPlay warns
			// if that never happened - the case that is a real mistake.
			UE_LOG(LogAirside, Log,
				TEXT("Buildings: no road network registered in %s yet - drawing nothing until one registers. ")
				TEXT("Set RoadNetwork on %s to draw for a particular one."),
				*GetNameSafe(GetWorld()), *GetName());
			BindTo(nullptr);
			return;
		}
	}
	BindTo(Road);
}

// A MISSING CASE BELOW IS A BUILD ERROR - see ExhaustiveSwitch.h: a third way for an airport to move must say what the plots do.
AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN
void AAirsideBuildingsActor::OnAirportChanged(UWorld& World, ARoadNetworkActor& Airport, EAirportRegistration Change)
{
	// ANOTHER WORLD'S AIRPORT is not ours - the editor world's and a PIE world's share this one list. A NAMED
	// RoadNetwork is followed by nothing else: the level said which one, and the registry does not overrule it.
	if (&World != GetWorld() || RoadNetwork != nullptr)
	{
		return;
	}
	switch (Change)
	{
	case EAirportRegistration::Arrived:
		if (&Airport != Bound.Get())
		{
			BindTo(&Airport);
		}
		return;
	case EAirportRegistration::Left:
		// ONLY THE ONE WE DRAW FOR: the leaver is named, so a refused second actor going cannot clear our plots.
		if (&Airport == Bound.Get())
		{
			BindTo(nullptr);
		}
		return;
	}
}
AIRSIDE_EXHAUSTIVE_SWITCH_END

void AAirsideBuildingsActor::BeginPlay()
{
	Super::BeginPlay();
	// THE WARNING PostRegisterAllComponents no longer gives: by BeginPlay every actor in the level has registered, so an
	// unbound buildings actor now really has no road network to draw for, and no depot will show.
	if (!Bound.IsValid())
	{
		UE_LOG(LogAirside, Warning, TEXT("Buildings: %s has no road network to draw for in %s - no depot will show."),
			*GetName(), *GetNameSafe(GetWorld()));
	}
}

void AAirsideBuildingsActor::UnregisterAllComponents(bool bForReregister)
{
	// A reregister (a property edit in the Details panel does one) keeps the binding: the
	// road network has not changed, and PostRegisterAllComponents rebinds anyway.
	if (!bForReregister)
	{
		URoadNetworkRegistry::OnAirportChanged().Remove(RegistryHandle);
		RegistryHandle.Reset();
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
	BoundHandle = Road->OnNetworkChanged.AddUObject(this, &AAirsideBuildingsActor::OnNetworkChanged);
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
		Road->OnNetworkChanged.Remove(BoundHandle);
	}
	Bound.Reset();
	BoundHandle.Reset();
}

// A MISSING CASE BELOW IS A BUILD ERROR - see ExhaustiveSwitch.h: a fifth EChangeKind must say whether it moves a plot.
AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN
void AAirsideBuildingsActor::OnNetworkChanged(EChangeKind Kind, const URoadNetwork& Network)
{
	switch (Kind)
	{
	case EChangeKind::Topology:
	case EChangeKind::Facts:
		// A PLOT DRAWN OR REMOVED (Topology), OR A MODULE BOUGHT OR REPAIRED AWAY (Facts, #446) - the two kinds
		// whose edits a yard is drawn from. Facts is new here: a shed purchase used to reach this as a Topology.
		Rebuild(Network);
		return;
	case EChangeKind::Geometry:
	case EChangeKind::Markings:
		// NOTHING A PLOT DERIVES FROM MOVED - RoadNetworkActor::RebuildForKind's reason for skipping these, kept
		// here now that this actor hears every kind (it heard Topology alone, on OnTopologyRebuilt).
		return;
	}
}
AIRSIDE_EXHAUSTIVE_SWITCH_END

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

void AAirsideBuildingsActor::ShowPlotGhosts(bool bVisible, FEntityInstanceId Only)
{
	if (Plots == nullptr)
	{
		return;
	}
	Plots->SetGhostsVisible(bVisible);
	if (!Plots->SetGhostScope(Only))
	{
		return;
	}
	const FString Whose = Only.IsSet() ? FString::Printf(TEXT("depot %d only"), Only.Index) : FString(TEXT("every plot"));
	UE_LOG(LogAirside, Log, TEXT("Plots: ghost bays drawn for %s"), *Whose);
	ARoadNetworkActor* Road = Bound.Get();
	if (Road != nullptr && Road->Network != nullptr)
	{
		Rebuild(*Road->Network);
	}
}
