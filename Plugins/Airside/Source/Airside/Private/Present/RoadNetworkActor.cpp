#include "Present/RoadNetworkActor.h"

#include "AirsideLog.h"
#include "Components/BillboardComponent.h"
#include "Components/DynamicMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Content/AirsideContent.h"
#include "Content/AirsideSettings.h"
#include "EngineUtils.h"
#include "Model/RoadNetwork.h"
#include "Present/AirsideTraffic.h"
#include "Present/RoadEditFacade.h"
#include "Profiles/RoadProfile.h"

ARoadNetworkActor::ARoadNetworkActor()
{
	// Ticks for the agents and for nothing else. With none dispatched the tick body is a
	// single empty-array test, which is cheaper than the machinery needed to switch
	// ticking on and off as agents come and go.
	PrimaryActorTick.bCanEverTick = true;

	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));

#if WITH_EDITORONLY_DATA
	// No editor sprite. The mesh components are in absolute space, so the actor's own
	// transform never leaves the world origin - and a billboard sitting there is
	// indistinguishable from a node the build tool drew at (0,0).
	RootComponent->bVisualizeComponent = false;
#endif

	MeshComponent = CreateDefaultSubobject<UDynamicMeshComponent>(TEXT("RoadMesh"));
	MeshComponent->SetupAttachment(RootComponent);

	// FRoadMeshBuilder emits absolute world coordinates, so the component must not
	// transform them again. Absolute placement pins it to world space while leaving the
	// actor free to be moved: SetWorldTransform(Identity) on a root component would have
	// teleported the actor itself to the origin instead, which is why the mesh component
	// is a child of a plain scene root rather than the root itself.
	MeshComponent->SetUsingAbsoluteLocation(true);
	MeshComponent->SetUsingAbsoluteRotation(true);
	MeshComponent->SetUsingAbsoluteScale(true);

	// Tangents are left at the default ExternallyProvided rather than AutoCalculated: this
	// mesh's UV2 (junction blend, ground blend) is identical at every segment vertex, so
	// AutoCalculated's UV-derived frame is degenerate and yields NaN tangents there - fatal
	// for a lit material, invisible for the unlit one that exposed it. ExternallyProvided
	// falls back to a frame derived from the normal alone, a valid constant basis on a flat
	// +Z road.

	// NOTHING IS RESOLVED HERE ANY MORE: materials, the material set, the stand definition
	// and the default profile are filled on demand by the Resolve* functions below, not by
	// ConstructorHelpers against literal /Game/ paths - see UAirsideSettings for why.

	// A second component for the preview, sharing the road's absolute-space setup for the
	// same reason: the builder emits world coordinates and must not have them transformed
	// twice. Hidden until there is something to preview.
	GhostComponent = CreateDefaultSubobject<UDynamicMeshComponent>(TEXT("RoadGhost"));
	GhostComponent->SetupAttachment(RootComponent);
	GhostComponent->SetUsingAbsoluteLocation(true);
	GhostComponent->SetUsingAbsoluteRotation(true);
	GhostComponent->SetUsingAbsoluteScale(true);
	GhostComponent->SetVisibility(false);

	// The preview is a hint, not scenery: it must never occlude, shadow or be traced
	// against the road it is hovering over.
	GhostComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	GhostComponent->SetCastShadow(false);

	// Aprons: their own component, and no collision or shadows for the same reason the
	// roads have none - the world is flat, so picking is exact maths rather than a trace.
	ApronComponent = CreateDefaultSubobject<UDynamicMeshComponent>(TEXT("ApronMesh"));
	ApronComponent->SetupAttachment(RootComponent);
	ApronComponent->SetUsingAbsoluteLocation(true);
	ApronComponent->SetUsingAbsoluteRotation(true);
	ApronComponent->SetUsingAbsoluteScale(true);
	ApronComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	// The three objects issue #32 split this actor into - see each class's own header for
	// its pattern, and each field's comment above for why CreateDefaultSubobject rather
	// than UPROPERTY(Instanced).
	Presenter = CreateDefaultSubobject<URoadSurfacePresenter>(TEXT("Presenter"));
	Presenter->Initialize(MeshComponent, GhostComponent, ApronComponent);

	Facade = CreateDefaultSubobject<URoadEditFacade>(TEXT("Facade"));

	// Reconnects what used to be a direct call: every mutator that once called
	// RebuildMesh() on itself now broadcasts OnChanged instead, because the facade has no
	// pointer to the presenter that does the rebuilding. This is the one place that wires
	// the two back together.
	Facade->OnChanged.AddUObject(this, &ARoadNetworkActor::RebuildMesh);

	Traffic = CreateDefaultSubobject<UAirsideTraffic>(TEXT("Traffic"));
}

URoadSurfacePresenter::FSurfaceSettings ARoadNetworkActor::MakeSurfaceSettings()
{
	URoadSurfacePresenter::FSurfaceSettings Settings;
	Settings.SurfaceZ = SurfaceZ;
	Settings.TexelsPerUnit = TexelsPerUnit;
	Settings.RibbonSegments = RibbonSegments;
	Settings.ApronZOffset = ApronZOffset;
	Settings.GhostZOffset = GhostZOffset;
	Settings.bUseConstantVertexColour = bUseConstantVertexColour;
	Settings.bUseConstantApronColour = bUseConstantApronColour;
	Settings.bDebugDrawMesh = bDebugDrawMesh;
	Settings.bDebugDrawAprons = bDebugDrawAprons;
	Settings.DebugDrawSeconds = DebugDrawSeconds;

	// THROUGH THE RESOLVERS, never the raw properties - see ResolveMaterialSet's own
	// comment for why a resolver that FILLED a null property changed a level.
	Settings.SurfaceMaterial = ResolveSurfaceMaterial();
	Settings.ApronMaterial = ResolveApronMaterial();
	Settings.GhostMaterial = ResolveGhostMaterial();
	Settings.MaterialSet = ResolveMaterialSet();
	Settings.Profile = ResolveProfile();
	return Settings;
}

void ARoadNetworkActor::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();

	// See the header. The surface saved in the level is a cache of a derived value, and
	// this is its only invalidation point - without it the picture on screen is whatever
	// was last serialised, and the first rebuild from any cause silently replaces it.
	//
	// Templates excluded: a class default object has no model to build from, and running
	// the solver over one would be work done to produce nothing.
	if (!HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
	{
		// Before RebuildMesh, which is what calls FAnchorLink::Build - the very consumer of
		// GetAnchorWorldHeading this exists to keep correct. Loading a level saved before
		// FResolvedAnchor grew LocalHeading and Role restores those UPROPERTYs at their
		// defaults (0.0 and Aircraft), and nothing else ever repairs that - see
		// UEntityDefinition::RefreshResolvedAnchors for why this is the one moment it can.
		if (Network != nullptr)
		{
			const int32 RefreshedAnchors = UEntityDefinition::RefreshResolvedAnchors(*Network);
			if (RefreshedAnchors > 0)
			{
				UE_LOG(LogRoadMesh, Log,
					TEXT("Refreshed %d resolved anchor(s) against their current definitions."),
					RefreshedAnchors);
			}
		}

		RebuildMesh();
	}

#if WITH_EDITOR
	// USceneComponent keeps its sprite protected, so the owner cannot reach it by name -
	// but it is a component of this actor, so it can be found by type. Hidden rather than
	// destroyed: the engine re-creates it on the next OnRegister, and a component destroyed
	// out from under the thing that owns the pointer is a worse bargain than a hidden one.
	for (UBillboardComponent* Billboard : TInlineComponentArray<UBillboardComponent*>(this))
	{
		Billboard->SetVisibility(false);
	}
#endif
}

ARoadNetworkActor* ARoadNetworkActor::FindOrCreate(UWorld* World)
{
	if (World == nullptr)
	{
		return nullptr;
	}

	for (TActorIterator<ARoadNetworkActor> It(World); It; ++It)
	{
		return *It;
	}

	// Not transient, and not RF_Transient: this is the one that will be saved with the
	// level. An actor spawned with the transient flag would vanish on save and take the
	// whole airport with it.
	FActorSpawnParameters Params;
	Params.Name = TEXT("RoadNetwork");
	return World->SpawnActor<ARoadNetworkActor>(FVector::ZeroVector, FRotator::ZeroRotator, Params);
}

UMaterialInterface* ARoadNetworkActor::ResolveSurfaceMaterial() const
{
	if (SurfaceMaterial != nullptr) { return SurfaceMaterial; }
	const UAirsideContent* Content = UAirsideSettings::GetContent();
	return Content != nullptr ? Content->SurfaceMaterial.LoadSynchronous() : nullptr;
}

UMaterialInterface* ARoadNetworkActor::ResolveApronMaterial() const
{
	if (ApronMaterial != nullptr) { return ApronMaterial; }
	const UAirsideContent* Content = UAirsideSettings::GetContent();
	return Content != nullptr ? Content->ApronMaterial.LoadSynchronous() : nullptr;
}

UMaterialInterface* ARoadNetworkActor::ResolveGhostMaterial() const
{
	if (GhostMaterial != nullptr) { return GhostMaterial; }
	const UAirsideContent* Content = UAirsideSettings::GetContent();
	return Content != nullptr ? Content->GhostMaterial.LoadSynchronous() : nullptr;
}

URoadMaterialSet* ARoadNetworkActor::ResolveMaterialSet() const
{
	// NO DEFAULT, and this one is different from the others on purpose.
	//
	// A null material set is not "unset", it is a STATE: the road is drawn with one material
	// throughout. So a resolver cannot supply a default here without changing what an airport
	// looks like, and it cannot tell "never chosen" from "deliberately cleared" - which is
	// exactly what happened. Removing the write that filled this property was not enough,
	// because handing back the same value from the content set produced the identical road.
	//
	// Per-band materials are still available: assign one on the actor and it is used. What is
	// gone is the plugin deciding you wanted them.
	return MaterialSet;
}

UEntityDefinition* ARoadNetworkActor::ResolveStandDefinition() const
{
	if (StandDefinition != nullptr) { return StandDefinition; }
	const UAirsideContent* Content = UAirsideSettings::GetContent();
	return Content != nullptr ? Content->DefaultStand.LoadSynchronous() : nullptr;
}

URoadProfile* ARoadNetworkActor::ResolveProfile()
{
	// AUTHORED INPUT, READ AND NEVER WRITTEN. This briefly assigned Profile when it found it
	// null - a resolver that writes to the property it resolves has turned a setting into a
	// cache, and in an editor world, where this actor ticks, that write lands on the level.
	if (Profile != nullptr)
	{
		return Profile;
	}

	if (RuntimeProfile == nullptr)
	{
		// FROM THIS ACTOR'S OWN FallbackWidth, and deliberately not from the content set.
		//
		// A configured default was briefly consulted here, above these fields. It looked
		// harmless - both are 2300 by default - and it silently overrode every instance
		// whose width had been tuned in the Details panel, because the class default is not
		// the instance value. A road authored narrow came back at the class width and its
		// centreline marking, which scales with the surface, came back as a yellow slab.
		//
		// The content set has no business here at all: this is per-instance tuning, and the
		// serialisation problem it was brought in to solve is not this function's. Segments
		// reloaded with a null profile are repaired through URoadNetwork::DefaultProfile,
		// which RebuildMesh reassigns every time and so never depends on being saved.
		//
		// A tenth of the width per side: without a Shoulder band the profile has no outer
		// band to fade and the road ends in a knife edge against the ground.
		RuntimeProfile = URoadProfile::MakeTransient(
			FallbackWidth, FallbackFilletRadius, FallbackWidth * 0.1);
	}

	return RuntimeProfile;
}

void ARoadNetworkActor::RebuildMesh()
{
	// A null Network cannot become a URoadNetwork&, so this guard - unlike the presenter's
	// own LastGhostFrom reset - has to live here rather than there. It only ever fires once
	// per actor, before the first node is placed, when no ghost has ever been shown either.
	if (Network == nullptr)
	{
		return;
	}
	Presenter->Rebuild(*Network, MakeSurfaceSettings());
}

double ARoadNetworkActor::GetApronSurfaceZ() const
{
	return Presenter->GetApronSurfaceZ(SurfaceZ, ApronZOffset);
}

void ARoadNetworkActor::UpdateGhost(int32 FromNodeIndex, const FRoadSnapResult& Snap, bool bValid)
{
	Presenter->UpdateGhost(Network, FromNodeIndex, Snap, bValid, MakeSurfaceSettings());
}

bool ARoadNetworkActor::BuildGhostBuffers(
	int32 FromNodeIndex, const FRoadSnapResult& Snap, FRoadMeshBuffers& OutBuffers)
{
	return Presenter->BuildGhostBuffers(Network, FromNodeIndex, Snap, MakeSurfaceSettings(), OutBuffers);
}

void ARoadNetworkActor::HideGhost()
{
	Presenter->HideGhost();
}

bool ARoadNetworkActor::MakeLiveNodeId(int32 Index, FRoadNodeId& OutId) const
{
	return Facade->MakeLiveNodeId(Index, OutId);
}

int32 ARoadNetworkActor::SurfaceTriangleCountForTest() const
{
	return Presenter->SurfaceTriangleCountForTest();
}

bool ARoadNetworkActor::ShouldTickIfViewportsOnly() const
{
	// Ticks in the EDITOR viewport, not only in play. The build tools work at design time,
	// so an agent dispatched at design time has to move at design time; without this the
	// cube spawns correctly and then stands still for ever.
	//
	// Scoped to non-game worlds so this says nothing about play, where ordinary ticking
	// already applies.
	const UWorld* World = GetWorld();
	return World != nullptr && !World->IsGameWorld();
}

void ARoadNetworkActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	Traffic->Advance(DeltaSeconds, SurfaceZ);
}

bool ARoadNetworkActor::DispatchArrival(const FVector2D& Near, const FAirframe& Airframe)
{
	if (Network == nullptr)
	{
		return false;
	}
	return Traffic->DispatchArrival(*Network, Near, Airframe, SurfaceZ, ShutdownPauseSeconds);
}

bool ARoadNetworkActor::DispatchAgent(const FRoutePlan& Plan, const FAirframe& Airframe)
{
	return Traffic->DispatchAgent(Network, Plan, Airframe, SurfaceZ, ShutdownPauseSeconds);
}

void ARoadNetworkActor::ClearAgents()
{
	Traffic->ClearAgents();
}

int32 ARoadNetworkActor::GetAgentCount() const
{
	return Traffic->GetAgentCount();
}

ARoadAgentActor* ARoadNetworkActor::GetNewestAgent() const
{
	return Traffic->GetNewestAgent();
}

int32 ARoadNetworkActor::AgentCountForTest() const
{
	return Traffic->GetAgentCount();
}

FRoutePlan ARoadNetworkActor::FindRoute(
	FGuidelineNodeId Start, FGuidelineNodeId Goal, ETraversalClass Class, double Wingspan) const
{
	return Facade->FindRoute(Start, Goal, Class, Wingspan);
}

// =========================================================================================
// THIN FORWARDERS to URoadEditFacade. See the header's banner comment: every one of these
// existed on this actor before issue #32 and is kept, unchanged in name and signature, so
// Blueprint, the game module and every existing test compile and behave exactly as before.
// =========================================================================================

int32 ARoadNetworkActor::PlaceNode(FVector2D Where)
{
	return Facade->PlaceNode(Where);
}

bool ARoadNetworkActor::ConnectNodes(int32 FromIndex, int32 ToIndex)
{
	return Facade->ConnectNodes(FromIndex, ToIndex);
}

int32 ARoadNetworkActor::ConnectGuidelines(int32 FromNodeIndex, int32 ToNodeIndex)
{
	return Facade->ConnectGuidelines(FromNodeIndex, ToNodeIndex);
}

bool ARoadNetworkActor::PlaceRunway(FVector2D From, FVector2D To, URoadProfile* RunwayProfile)
{
	return Facade->PlaceRunway(From, To, RunwayProfile);
}

bool ARoadNetworkActor::DisconnectGuideline(int32 EdgeIndex)
{
	return Facade->DisconnectGuideline(EdgeIndex);
}

int32 ARoadNetworkActor::FindNodeNear(FVector2D Where, double Radius) const
{
	return Facade->FindNodeNear(Where, Radius);
}

int32 ARoadNetworkActor::SplitSegment(int32 SegmentIndex, FVector2D At)
{
	return Facade->SplitSegment(SegmentIndex, At);
}

bool ARoadNetworkActor::DeleteNode(int32 NodeIndex)
{
	return Facade->DeleteNode(NodeIndex);
}

bool ARoadNetworkActor::DeleteSegment(int32 SegmentIndex)
{
	return Facade->DeleteSegment(SegmentIndex);
}

TArray<int32> ARoadNetworkActor::SegmentsIncidentTo(int32 NodeIndex) const
{
	return Facade->SegmentsIncidentTo(NodeIndex);
}

bool ARoadNetworkActor::GetSegmentEnds(int32 SegmentIndex, FVector2D& OutA, FVector2D& OutB) const
{
	return Facade->GetSegmentEnds(SegmentIndex, OutA, OutB);
}

bool ARoadNetworkActor::MoveNode(int32 NodeIndex, FVector2D To)
{
	return Facade->MoveNode(NodeIndex, To);
}

void ARoadNetworkActor::BeginInteractiveEdit(const FString& Label)
{
	Facade->BeginInteractiveEdit(Label);
}

void ARoadNetworkActor::EndInteractiveEdit(bool bKeep)
{
	Facade->EndInteractiveEdit(bKeep);
}

FRoadDeletionPlan ARoadNetworkActor::PlanNodeDeletion(int32 NodeIndex) const
{
	return Facade->PlanNodeDeletion(NodeIndex);
}

int32 ARoadNetworkActor::AddApron(const TArray<FVector2D>& Outline)
{
	return Facade->AddApron(Outline);
}

bool ARoadNetworkActor::DeleteApron(int32 ApronIndex)
{
	return Facade->DeleteApron(ApronIndex);
}

int32 ARoadNetworkActor::FindApronAt(FVector2D Where) const
{
	return Facade->FindApronAt(Where);
}

int32 ARoadNetworkActor::PlaceStand(FVector2D Where, double Heading)
{
	return Facade->PlaceStand(Where, Heading);
}

bool ARoadNetworkActor::DeleteEntity(int32 EntityIndex)
{
	return Facade->DeleteEntity(EntityIndex);
}

int32 ARoadNetworkActor::FindEntityAt(FVector2D Where, double Radius) const
{
	return Facade->FindEntityAt(Where, Radius);
}

void ARoadNetworkActor::ClearNetwork()
{
	Facade->ClearNetwork();
}

bool ARoadNetworkActor::Undo()
{
	return Facade->Undo();
}

bool ARoadNetworkActor::Redo()
{
	return Facade->Redo();
}

bool ARoadNetworkActor::CanUndo() const
{
	return Facade->CanUndo();
}

bool ARoadNetworkActor::CanRedo() const
{
	return Facade->CanRedo();
}

FString ARoadNetworkActor::PeekUndoLabel() const
{
	return Facade->PeekUndoLabel();
}
