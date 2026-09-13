#include "Present/RoadNetworkActor.h"

#include "AirsideLog.h"
#include "Containers/StaticArray.h"
#include "Components/BillboardComponent.h"
#include "Components/DynamicMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Content/AirsideContent.h"
#include "Content/AirsideSettings.h"
#include "EngineUtils.h"
#include "Model/DeparturePlanner.h"
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

	// FRoadMeshBuilder emits absolute world coordinates, so the component must not
	// transform them again. Absolute placement pins it to world space while leaving the
	// actor free to be moved: SetWorldTransform(Identity) on a root component would have
	// teleported the actor itself to the origin instead, which is why the mesh component
	// is a child of a plain scene root rather than the root itself. See MakeSurfaceComponent.
	MeshComponent = MakeSurfaceComponent(TEXT("RoadMesh"));

	// Tangents are left at the default ExternallyProvided, which finds no tangent space on
	// this mesh and falls back to a frame derived from the normal alone. On a flat +Z road
	// that is a constant, valid basis.
	//
	// AutoCalculated was tried and reverted. It derives the frame from the UV layers, and
	// this mesh's UV2 is (junction blend, ground blend) - identical at every segment
	// vertex, so every triangle is degenerate in that UV space. A degenerate UV triangle
	// divides by zero and yields NaN tangents, and NaN vertices are discarded by the GPU.
	// That is invisible for an unlit material, which never samples the tangent frame, and
	// fatal for any lit one - which is exactly the split observed: the engine's unlit
	// vertex-colour debug material drew, while every lit material, ours and stock alike,
	// rendered nothing.
	//
	// Slice 2b-ii can revisit this once the normal map's handedness matters, but it must
	// then compute tangents from UV0 specifically rather than from whatever the component
	// picks.

	// NOTHING IS RESOLVED HERE ANY MORE. Materials, the material set, the stand definition
	// and the default profile were all ConstructorHelpers::FObjectFinder calls against
	// literal /Game/ paths, which is what let a freshly placed actor render with no setup -
	// and what made eight references the editor could not see when a content folder moved.
	// The Resolve* functions below fill in whatever is still null, at the moment it is
	// first wanted. See UAirsideSettings for why that cannot happen in a constructor.

	// A second component for the preview, sharing the road's absolute-space setup for the
	// same reason: the builder emits world coordinates and must not have them transformed
	// twice. Hidden until there is something to preview.
	GhostComponent = MakeSurfaceComponent(TEXT("RoadGhost"));
	GhostComponent->SetVisibility(false);

	// The preview is a hint, not scenery: it must never occlude, shadow or be traced
	// against the road it is hovering over.
	GhostComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	GhostComponent->SetCastShadow(false);

	// Aprons: their own component, and no collision or shadows for the same reason the
	// roads have none - the world is flat, so picking is exact maths rather than a trace.
	ApronComponent = MakeSurfaceComponent(TEXT("ApronMesh"));
	ApronComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	MarkingComponent = MakeSurfaceComponent(TEXT("HoldingPositionMarkings"));
	MarkingComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	MarkingComponent->SetCastShadow(false);

	RunwayMarkingComponent = MakeSurfaceComponent(TEXT("RunwayMarkings"));
	RunwayMarkingComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	RunwayMarkingComponent->SetCastShadow(false);

	// The three objects issue #32 split this actor into - see each class's own header for
	// its pattern, and each field's comment above for why CreateDefaultSubobject rather
	// than UPROPERTY(Instanced).
	Presenter = CreateDefaultSubobject<URoadSurfacePresenter>(TEXT("Presenter"));

	// Indexed by ESurfaceLayer, not positional - see URoadSurfacePresenter::Initialize's own
	// comment (issue #81). A local TStaticArray: this actor's own components stay five
	// separately named UPROPERTYs (unchanged, since a saved level's Details panel and
	// ApronDrawToolTest.cpp already know them by those names), and this is just how they are
	// handed across the Present-internal boundary in one indexed call instead of five.
	TStaticArray<TObjectPtr<UDynamicMeshComponent>, static_cast<int32>(ESurfaceLayer::Count)> SurfaceComponents;
	SurfaceComponents[static_cast<int32>(ESurfaceLayer::Road)] = MeshComponent;
	SurfaceComponents[static_cast<int32>(ESurfaceLayer::Ghost)] = GhostComponent;
	SurfaceComponents[static_cast<int32>(ESurfaceLayer::Apron)] = ApronComponent;
	SurfaceComponents[static_cast<int32>(ESurfaceLayer::HoldingPaint)] = MarkingComponent;
	SurfaceComponents[static_cast<int32>(ESurfaceLayer::RunwayPaint)] = RunwayMarkingComponent;
	Presenter->Initialize(SurfaceComponents);

	Facade = CreateDefaultSubobject<URoadEditFacade>(TEXT("Facade"));

	// Reconnects what used to be a direct call: every mutator that once called
	// RebuildMesh() on itself now broadcasts OnChanged instead, because the facade has no
	// pointer to the presenter that does the rebuilding. This is the one place that wires
	// the two back together.
	Facade->OnChanged.AddUObject(this, &ARoadNetworkActor::RebuildMesh);

	Traffic = CreateDefaultSubobject<UAirsideTraffic>(TEXT("Traffic"));
}

UDynamicMeshComponent* ARoadNetworkActor::MakeSurfaceComponent(FName Name)
{
	// See the header: only the part that is IDENTICAL across all five components. Collision,
	// shadow and visibility differ per component and stay at each call site.
	UDynamicMeshComponent* Component = CreateDefaultSubobject<UDynamicMeshComponent>(Name);
	Component->SetupAttachment(RootComponent);
	Component->SetUsingAbsoluteLocation(true);
	Component->SetUsingAbsoluteRotation(true);
	Component->SetUsingAbsoluteScale(true);
	return Component;
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
	Settings.ServiceLinkRadius = ServiceLinkRadius;

	// THROUGH THE RESOLVERS, never the raw properties - see ResolveMaterialSet's own
	// comment for why a resolver that FILLED a null property changed a level.
	Settings.SurfaceMaterial = ResolveSurfaceMaterial();
	Settings.ApronMaterial = ResolveApronMaterial();
	Settings.GhostMaterial = ResolveGhostMaterial();
	Settings.MaterialSet = ResolveMaterialSet();
	Settings.RunwayGrassMaterial = ResolveRunwayMaterial(ERunwaySurface::Grass);
	Settings.RunwayTarmacMaterial = ResolveRunwayMaterial(ERunwaySurface::Tarmac);
	Settings.RunwayConcreteMaterial = ResolveRunwayMaterial(ERunwaySurface::Concrete);
	Settings.Profile = ResolveProfile();
	return Settings;
}

URoadSurfacePresenter::FSurfaceSettings ARoadNetworkActor::MakeGhostSurfaceSettings(ERoadKind Kind)
{
	// Only what UpdateGhost/BuildGhostBuffers read - narrower than MakeSurfaceSettings so
	// the ghost path never pays for SurfaceMaterial/ApronMaterial/MaterialSet, each a
	// content lookup plus a LoadSynchronous that only RebuildMesh/RebuildAprons need.
	URoadSurfacePresenter::FSurfaceSettings Settings;
	Settings.SurfaceZ = SurfaceZ;
	Settings.TexelsPerUnit = TexelsPerUnit;
	Settings.RibbonSegments = RibbonSegments;
	Settings.GhostZOffset = GhostZOffset;
	Settings.GhostMaterial = ResolveGhostMaterial();

	// THE KIND THE CLICK WILL ACTUALLY LAY, not always the taxiway. A ghost is a promise
	// about what a click does, and a 23 m preview over a 6 m road is a promise the player
	// then acts on. Null for a road with no profile is correct and needs no guard here: the
	// presenter already draws nothing without half-widths, which is what the refusal in
	// URoadEditFacade::ConnectNodes is about to say out loud.
	Settings.Profile = Kind == ERoadKind::ServiceRoad ? ResolveServiceRoadProfile() : ResolveProfile();
	return Settings;
}

void ARoadNetworkActor::PostInitProperties()
{
	Super::PostInitProperties();

	// See the header. Every construction path runs this, so the pointers are corrected for
	// a duplicate and left alone for a spawn, where they already name these same objects.
	// By NAME, not by re-creating: the constructor's Facade->OnChanged binding and
	// Presenter->Initialize call were made on these objects, and a fresh one would not
	// carry either.
	Presenter = Cast<URoadSurfacePresenter>(GetDefaultSubobjectByName(TEXT("Presenter")));
	Facade = Cast<URoadEditFacade>(GetDefaultSubobjectByName(TEXT("Facade")));
	Traffic = Cast<UAirsideTraffic>(GetDefaultSubobjectByName(TEXT("Traffic")));
}

UObject* ARoadNetworkActor::FacadeOuterForTest() const
{
	return Facade ? Facade->GetOuter() : nullptr;
}

UObject* ARoadNetworkActor::PresenterOuterForTest() const
{
	return Presenter ? Presenter->GetOuter() : nullptr;
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

UMaterialInterface* ARoadNetworkActor::ResolveRunwayMaterial(ERunwaySurface Surface) const
{
	const UAirsideContent* Content = UAirsideSettings::GetContent();
	if (Content == nullptr)
	{
		return nullptr;
	}
	switch (Surface)
	{
	case ERunwaySurface::Grass:  return Content->RunwayGrassMaterial.LoadSynchronous();
	case ERunwaySurface::Tarmac: return Content->RunwayTarmacMaterial.LoadSynchronous();
	// Reinforced shares concrete's material - URoadMaterialSet::RunwaySlotName says why.
	case ERunwaySurface::Concrete:
	case ERunwaySurface::Reinforced:
	default:
		return Content->RunwayConcreteMaterial.LoadSynchronous();
	}
}

UEntityDefinition* ARoadNetworkActor::ResolveStandDefinition() const
{
	if (StandDefinition != nullptr) { return StandDefinition; }
	const UAirsideContent* Content = UAirsideSettings::GetContent();
	return Content != nullptr ? Content->DefaultStand.LoadSynchronous() : nullptr;
}

UEntityDefinition* ARoadNetworkActor::ResolveFuelDepotDefinition() const
{
	if (FuelDepotDefinition != nullptr) { return FuelDepotDefinition; }
	const UAirsideContent* Content = UAirsideSettings::GetContent();
	return Content != nullptr ? Content->DefaultFuelDepot.LoadSynchronous() : nullptr;
}

UEntityDefinition* ARoadNetworkActor::ResolveEntityDefinition(EPlaceableEntity Kind) const
{
	return Kind == EPlaceableEntity::FuelDepot ? ResolveFuelDepotDefinition() : ResolveStandDefinition();
}

URoadProfile* ARoadNetworkActor::ResolveServiceRoadProfile() const
{
	if (ServiceRoadProfile != nullptr)
	{
		return ServiceRoadProfile;
	}

	// NO TRANSIENT FALLBACK, unlike ResolveProfile below. A segment laid with a transient
	// profile comes back from a save with a null pointer, which URoadNetwork::DefaultProfile
	// repairs as a TAXIWAY - so a "helpful" default here would silently admit aircraft onto
	// a service road the next time the level was loaded. Null instead, and the caller refuses.
	const UAirsideContent* Content = UAirsideSettings::GetContent();
	return Content != nullptr ? Content->ServiceRoadProfile.LoadSynchronous() : nullptr;
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
	// Counted before anything else, so RebuildCountForTest sees every call including the
	// early-return below - a rebuild that bailed for lack of a network still ran.
	++RebuildCount;

	// Unconditional, matching the pre-split RebuildMesh exactly: even the path below that
	// returns before there is a Network must still invalidate the ghost cache. Presenter::
	// Rebuild also calls this itself; doing it again there is harmless.
	Presenter->InvalidateGhostCache();

	// A null Network cannot become a URoadNetwork&, so this guard lives here, not on the
	// presenter. Fires once per actor, before the first node is placed.
	if (Network == nullptr)
	{
		return;
	}
	Presenter->Rebuild(*Network, MakeSurfaceSettings());

	// The guideline graph was just regenerated with new handles. Every agent's route must be
	// re-pointed at the nodes that now hold its positions, or the occupancy table would be
	// keyed on slots the builder has already freed - see UGroundTraffic::OnGraphRebuilt.
	if (Traffic != nullptr)
	{
		Traffic->OnGraphRebuilt(*Network);
	}
}

double ARoadNetworkActor::GetApronSurfaceZ() const
{
	return Presenter->GetApronSurfaceZ(SurfaceZ, ApronZOffset);
}

void ARoadNetworkActor::UpdateGhost(int32 FromNodeIndex, const FRoadSnapResult& Snap, bool bValid,
	ERoadKind Kind)
{
	// Asked FIRST, before anything is resolved: a still drag calls this every frame with an
	// unchanged FromNodeIndex/Snap, and the cache already knows that without a Resolve*
	// call. Only a validity flip on an otherwise-unchanged ghost costs one (GhostMaterial).
	bool bValidityChanged = false;
	if (Presenter->IsGhostCacheHit(Network, FromNodeIndex, Snap, bValid, bValidityChanged))
	{
		if (bValidityChanged)
		{
			Presenter->SetGhostValidity(bValid, ResolveGhostMaterial());
		}
		return;
	}

	Presenter->UpdateGhost(Network, FromNodeIndex, Snap, bValid, MakeGhostSurfaceSettings(Kind));
}

bool ARoadNetworkActor::BuildGhostBuffers(
	int32 FromNodeIndex, const FRoadSnapResult& Snap, FRoadMeshBuffers& OutBuffers)
{
	// TAXIWAY, PASSED EXPLICITLY. This is the seam Airside.Present.AuthoredPropertiesUntouched
	// measures - that building a preview leaves the real network bitwise unchanged - and it
	// has no kind of its own to be given. Spelled out rather than defaulted so the choice is
	// visible at the call site.
	return Presenter->BuildGhostBuffers(Network, FromNodeIndex, Snap,
		MakeGhostSurfaceSettings(ERoadKind::Taxiway), OutBuffers);
}

void ARoadNetworkActor::HideGhost()
{
	Presenter->HideGhost();
}

bool ARoadNetworkActor::MakeLiveNodeId(int32 Index, FRoadNodeId& OutId) const
{
	return Facade->MakeLiveNodeId(Index, OutId);
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
	// Scaled HERE, at the one place real frame time becomes agent time, so nothing below
	// this line ever learns there is a speed setting. See SetSimTimeScale.
	// TrafficRules travels with the tick, not with construction: it is a level-authored
	// UPROPERTY on this actor and the model that reads it is Transient, so handing it over
	// every frame is what keeps a figure tuned in the Details panel true of the arbiter -
	// see the property's own comment and UAirsideTraffic::Advance. EvenDelta moved onto
	// Traffic by issue #80 (see FFrameDeltaSmoother); DeltaSmoothingRate/MaxOwedSeconds are
	// this actor's own level-authored UPROPERTYs and travel in by value the same way.
	const double Evened = Traffic->EvenDelta(DeltaSeconds, DeltaSmoothingRate, MaxOwedSeconds);
	Traffic->Advance(static_cast<float>(Evened * SimTimeScale), SurfaceZ, Network, TrafficRules);
}

void ARoadNetworkActor::SetDeltaSmoothingForTest(double Rate)
{
	// Out of the header: the body now needs UAirsideTraffic's complete type, which the
	// header only forward-declares - see Tick for why (same Traffic->EvenDelta pattern).
	DeltaSmoothingRate = FMath::Clamp(Rate, 0.0, 1.0);
	Traffic->ResetFrameDeltaSmoothingForTest();
}

bool ARoadNetworkActor::DispatchArrival(const FVector2D& Near, const FAirframe& Airframe)
{
	if (Network == nullptr)
	{
		return false;
	}
	return Traffic->DispatchArrival(*Network, Near, Airframe, SurfaceZ, ShutdownPauseSeconds);
}

bool ARoadNetworkActor::DispatchAgent(const FRoutePlan& Plan, const FAirframe& Airframe,
	ETraversalClass Class)
{
	return Traffic->DispatchAgent(Network, Plan, Airframe, SurfaceZ, ShutdownPauseSeconds, Class);
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

const UGroundTraffic* ARoadNetworkActor::GetGroundTraffic() const
{
	return Traffic != nullptr ? Traffic->GetModel() : nullptr;
}

EDepartureRefusal ARoadNetworkActor::DepartAgent(int32 AgentId)
{
	return Traffic->DepartAgent(AgentId, Network);
}

ARoadAgentActor* ARoadNetworkActor::GetAgentView(int32 AgentId) const
{
	return Traffic->GetAgentView(AgentId);
}

EAgentPhase ARoadNetworkActor::LastAgentPhaseForTest() const
{
	return Traffic->LastAgentPhaseForTest();
}

double ARoadNetworkActor::LastAgentTaxiSpeedCapForTest() const
{
	return Traffic->LastAgentTaxiSpeedCapForTest();
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

bool ARoadNetworkActor::ConnectNodes(int32 FromIndex, int32 ToIndex, ERoadKind Kind)
{
	return Facade->ConnectNodes(FromIndex, ToIndex, Kind);
}

int32 ARoadNetworkActor::ConnectGuidelines(int32 FromNodeIndex, int32 ToNodeIndex)
{
	return Facade->ConnectGuidelines(FromNodeIndex, ToNodeIndex);
}

bool ARoadNetworkActor::PlaceRunway(FVector2D From, FVector2D To, URoadProfile* RunwayProfile)
{
	return PlaceRunway(From, To, RunwayProfile, FRunwayFacts());
}

bool ARoadNetworkActor::PlaceRunway(FVector2D From, FVector2D To, URoadProfile* RunwayProfile, const FRunwayFacts& Facts)
{
	return Facade->PlaceRunway(From, To, RunwayProfile, Facts);
}

bool ARoadNetworkActor::SetRunwayFacts(int32 SegmentIndex, const FRunwayFacts& Facts)
{
	return Facade->SetRunwayFacts(SegmentIndex, Facts);
}

bool ARoadNetworkActor::SetIntermediateHoldingPosition(int32 NodeIndex, bool bSet)
{
	return Facade->SetIntermediateHoldingPosition(NodeIndex, bSet);
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

int32 ARoadNetworkActor::PlaceEntity(FVector2D Where, double Heading, EPlaceableEntity Kind)
{
	return Facade->PlaceEntity(Where, Heading, Kind);
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
