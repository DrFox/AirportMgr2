#include "RoadBuildController.h"

#include "BuildActions.h"
#include "BuildCameraComponent.h"
#include "BuildHudLayer.h"
#include "LedgerPanelWidget.h"
#include "Components/InputComponent.h"
#include "Content/AirsideSettings.h"
#include "Engine/LocalPlayer.h"
#include "Entities/AircraftType.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "Model/AirsideCapability.h"
#include "Model/DeparturePlanner.h"
#include "Model/GroundTraffic.h"
#include "Model/InspectFacts.h"
#include "Model/RoadNetwork.h"
#include "Model/SimClock.h"
#include "Present/OpsRuntime.h"
#include "Present/AirsideTraffic.h"
#include "Present/OpsRuntimeSubsystem.h"
#include "Present/RoadAgentActor.h"
#include "Present/RoadEditFacade.h"
#include "Present/AirsideBuildingsActor.h"
#include "Present/RoadNetworkActor.h"
#include "SceneView.h"
#include "Solve/RoadGeom.h"
#include "Tool/ScreenPick.h"

// LogRoadBuild is declared AND defined in RoadBuildLog.h/.cpp now - see that header's own
// comment for why a component logging under this category should not have to include this
// file just to reach it.

ARoadBuildController::ARoadBuildController()
{
	bShowMouseCursor = true;
	bEnableClickEvents = true;
	bEnableMouseOverEvents = true;

	// See UBuildCameraComponent's and UBuildHudLayer's own comments for what each owns now -
	// issue #94. CreateDefaultSubobject for both: UBuildHudLayer is a plain UObject, not a
	// component, but the same call works for any UObject subobject that should share the
	// CDO template hierarchy - see ARoadNetworkActor::Facade (URoadEditFacade) for the same
	// pattern already established in this codebase.
	BuildCameraComp = CreateDefaultSubobject<UBuildCameraComponent>(TEXT("BuildCameraComp"));
	Hud = CreateDefaultSubobject<UBuildHudLayer>(TEXT("Hud"));
}

void ARoadBuildController::BeginPlay()
{
	Super::BeginPlay();

	Target = ARoadNetworkActor::Find(GetWorld());

	if (Target == nullptr)
	{
		UE_LOG(LogRoadBuild, Warning,
			TEXT("No ARoadNetworkActor in the level - place one, or clicks will do nothing."));
		return;
	}

	// THE BUILDINGS, found or spawned for this play session. M_Starter has one placed, so this
	// normally finds it; spawning covers a level that predates the split, where the depots
	// would otherwise be invisible in play and present in the editor.
	AAirsideBuildingsActor::FindOrCreate(GetWorld(), Target);

	// See HasRunway's own comment: the cache this subscribes to invalidate is issue #187.
	BindRunwayCacheInvalidation();

	// Session is constructed from ToolRegistry() already - see FBuildSession's constructor.
	// There is no second list here to fall out of step with it: the mismatch this project
	// has shipped three times (a tool with no key, or a key with no tool) is now a mismatch
	// the registry would have to disagree with ITSELF to produce.

	if (bStartAbovePlane)
	{
		BuildCameraComp->CreateBuildCamera(*this, *Target);
	}

	// Game AND UI: the bar's buttons must take a click before the road tool sees it, and the
	// camera keys must keep working while the bar has focus.
	FInputModeGameAndUI Mode;
	Mode.SetHideCursorDuringCapture(false);
	SetInputMode(Mode);

	// The four HUD widgets - see UBuildHudLayer::CreateAll for the recipe and the Z-orders.
	Hud->CreateAll(*this);

	// The key list is GENERATED from the same registry SetupInputComponent binds from and
	// the bar builds from, so this banner cannot advertise a key that goes nowhere - which
	// the old hand-written one twice did.
	FString Keys;
	for (const FBuildAction& Action : BuildActions())
	{
		if (!Action.Key.IsValid())
		{
			continue;
		}
		Keys += FString::Printf(TEXT("%s%s%s %s"), Keys.IsEmpty() ? TEXT("") : TEXT(", "),
			Action.bRequiresCtrl ? TEXT("Ctrl+") : TEXT(""), *Action.Key.GetDisplayName().ToString(),
			*Action.Label.ToString());
	}

	UE_LOG(LogRoadBuild, Log,
		TEXT("Road building ready on %s. Click an aircraft or stand to inspect it; pick a tool to build; right click puts a tool down. ")
		TEXT("Keys: %s. WASD pans, Q/E or middle-mouse drag rotate, wheel zooms - while building or watching. ")
		TEXT("Every key is also a button on the bar."),
		*Target->GetName(), *Keys);
}

void ARoadBuildController::UpdateView(float DeltaTime)
{
	// Read as held keys rather than bound as actions: pan and rotate are continuous, and a
	// key binding fires once on press. The same reason WASD was never bound. Reading them
	// stays here - it is host input, which UBuildCameraComponent has no business owning.
	const double Right = (IsInputKeyDown(EKeys::D) ? 1.0 : 0.0) - (IsInputKeyDown(EKeys::A) ? 1.0 : 0.0);
	const double Forward = (IsInputKeyDown(EKeys::W) ? 1.0 : 0.0) - (IsInputKeyDown(EKeys::S) ? 1.0 : 0.0);
	const double Turn = (IsInputKeyDown(EKeys::E) ? 1.0 : 0.0) - (IsInputKeyDown(EKeys::Q) ? 1.0 : 0.0);
	BuildCameraComp->UpdateView(DeltaTime, Right, Forward, Turn, ReadMouseTurnPixels(), Target);
}

double ARoadBuildController::ReadMouseTurnPixels()
{
	// THE MIDDLE BUTTON, not the right one. Right-click puts a tool down - it is in the
	// startup banner and it is how every gesture is cancelled - so a right-drag that also
	// rotated would cancel whatever was being built every time the player turned the view.
	if (!IsInputKeyDown(EKeys::MiddleMouseButton))
	{
		bRotatingWithMouse = false;
		return 0.0;
	}

	float X = 0.0f;
	float Y = 0.0f;
	if (!GetMousePosition(X, Y))
	{
		// Cursor off the viewport. Drop the drag rather than carrying a stale position
		// across the gap, which would fling the view when it came back.
		bRotatingWithMouse = false;
		return 0.0;
	}

	const FVector2D Now(X, Y);
	const double Pixels = bRotatingWithMouse ? Now.X - LastMousePosition.X : 0.0;
	bRotatingWithMouse = true;
	LastMousePosition = Now;
	return Pixels;
}

bool ARoadBuildController::IsWatchingAgent() const
{
	return BuildCameraComp != nullptr && BuildCameraComp->IsWatchingAgent();
}

void ARoadBuildController::ToggleWatchAgent()
{
	if (Target == nullptr)
	{
		return;
	}

	// The SELECTED aircraft when there is one, else the newest - "follow" means the one you
	// are looking at, and the newest is what you are looking at when nothing is selected.
	// This preference is Session/Selection policy and stays here; UBuildCameraComponent
	// knows only the id it was given.
	const int32 Wanted = HasSelectedAircraft() ? GetSelection().Id : Target->GetTraffic()->GetNewestAgentId();
	if (!BuildCameraComp->ToggleWatchAgent(*Target, Wanted))
	{
		// Refused out loud. Silently staying on the build camera is indistinguishable from
		// the key not being bound, which is a class of confusion this project has paid for.
		UE_LOG(LogRoadBuild, Warning,
			TEXT("Nothing to follow: select an aircraft, or land one (7) first."));
		return;
	}

	UE_LOG(LogRoadBuild, Log, TEXT("Camera: %s"),
		BuildCameraComp->IsWatchingAgent()
			? *FString::Printf(TEXT("following aircraft %d"), BuildCameraComp->GetWatchAgentId())
			: TEXT("build view"));
}

void ARoadBuildController::SetupInputComponent()
{
	Super::SetupInputComponent();

	// Bound as raw keys rather than through Enhanced Input: the mappings would need
	// InputAction and InputMappingContext content assets, and this driver is meant to
	// work the moment the module compiles, with nothing to author first. This was the LAST
	// reason AirportMgr.Build.cs named "EnhancedInput" as a dependency - nothing here ever
	// called into it - so issue #191 dropped the dependency; the engine still configures
	// EnhancedPlayerInput/EnhancedInputComponent as the project-wide default in
	// DefaultInput.ini, which is a plugin-level setting this module's Build.cs never gated.
	InputComponent->BindKey(EKeys::LeftMouseButton, IE_Pressed, this, &ARoadBuildController::OnPrimaryPressed);
	InputComponent->BindKey(EKeys::LeftMouseButton, IE_Released, this, &ARoadBuildController::OnPrimaryReleased);
	InputComponent->BindKey(EKeys::RightMouseButton, IE_Pressed, this, &ARoadBuildController::OnCancelGesture);

	// EVERY key comes from BuildActions(), the same table the bar and the banner read - so a
	// key cannot exist without a button, nor a button without a key. This is the third form
	// of the same rule: before issue #33 six SelectXTool binds had to agree with the tool
	// list by hand and once did not ("4 routes" advertised while EKeys::Four went nowhere);
	// issue #33 bound tools from ToolRegistry(); this binds EVERYTHING from one registry.
	//
	// Ctrl actions bind the chord, which is what makes "Ctrl+Z" one fact rather than a bare
	// Z plus a check inside the handler that a bar button could not share.
	//
	// Numbered tools rather than a third modifier on one button: drawing a polygon is
	// inherently multi-click, so it cannot ride a modifier the way delete and insert do.
	// K/L for save/load rather than F5/F9: PIE already owns the function keys.
	for (const FBuildAction& Action : BuildActions())
	{
		if (!Action.Key.IsValid())
		{
			continue;
		}
		if (Action.bRequiresCtrl)
		{
			// A chord binding hands its handler no key, so every Ctrl action shares one
			// handler that asks which of them was just pressed.
			const FInputChord Chord(Action.Key, /*shift*/ false, /*ctrl*/ true, /*alt*/ false, /*cmd*/ false);
			InputComponent->BindKey(Chord, IE_Pressed, this, &ARoadBuildController::OnCtrlActionKey);
		}
		else
		{
			InputComponent->BindKey(Action.Key, IE_Pressed, this, &ARoadBuildController::OnActionKey);
		}
	}

	InputComponent->BindKey(EKeys::MouseScrollUp, IE_Pressed, this, &ARoadBuildController::ZoomIn);
	InputComponent->BindKey(EKeys::MouseScrollDown, IE_Pressed, this, &ARoadBuildController::ZoomOut);
}

void ARoadBuildController::LandAircraftNearViewFocus()
{
	if (Target == nullptr)
	{
		return;
	}

	// ViewFocus(), not ActiveRig().Focus: while watching an agent, ActiveRig() is the WATCH
	// rig, whose Focus is a leash offset in the AIRCRAFT's frame, not a road-plane position -
	// key 7 must still aim at the build view's own focus regardless of which camera is on
	// screen. This is the one piece of the old ~90-line method that could not move with the
	// rest to UOpsRuntime::LandNear (issue #191): only the controller knows which camera is on
	// screen.
	const FVector2D Focus = BuildCameraComp->ViewFocus();

	// UNLESS A TYPE IS CONFIGURED FOR THE KEY. That override exists so a particular aeroplane
	// can be put on the runway without waiting for the board to offer one, and it is read HERE
	// and nowhere else - offers and their arrivals still resolve their own type, so this
	// cannot become the game's behaviour by being forgotten. Resolved to an FAirframe (the
	// shape every dispatch already takes, issue #29) once, so BOTH paths below - through the
	// board and the no-runtime fallback - honour it the same way the single pre-split method
	// used to.
	const UAircraftType* Configured = LandAircraftType.LoadSynchronous();
	const FAirframe Override = Configured != nullptr ? Configured->Airframe() : FAirframe();

	// THROUGH THE BOARD WHEN THERE IS ONE, so the aeroplane belongs to a flight the rest of
	// the game can track - see LandNear's own header for why a direct dispatch is a second
	// door onto arrival.
	if (UOpsRuntime* Runtime = UOpsRuntimeSubsystem::Get(GetWorld()))
	{
		Runtime->LandNear(Focus, Configured != nullptr ? &Override : nullptr);
		return;
	}

	// No runtime: the editor mode, which has no game instance and so no board - LandNear needs
	// one to reach FlightBoard through, so the direct dispatch this used to fall back to when
	// UOpsRuntimeSubsystem::Get failed stays here, the one path LandNear cannot cover. Still
	// honours the configured override, same as the pre-split method did.
	//
	// DispatchArrival has already logged which runway, which exit and which stand it chose,
	// or why it declined.
	Target->DispatchArrival(Focus, Configured != nullptr ? Override : UAirsideSettings::ResolveDefaultAirframe());
}

bool ARoadBuildController::CursorOnRoadPlane(FVector2D& OutPosition, bool bLogRefusals) const
{
	if (Target == nullptr)
	{
		return false;
	}

	FVector Origin;
	FVector Direction;
	if (!DeprojectMousePositionToWorld(Origin, Direction))
	{
		// Fails whenever there is no mouse position to read at all, so it must never be
		// the quiet path: a click that vanishes here is indistinguishable from a broken
		// tool.
		if (bLogRefusals)
		{
			float MouseX = 0.0f;
			float MouseY = 0.0f;
			const bool bHaveMouse = GetMousePosition(MouseX, MouseY);
			UE_LOG(LogRoadBuild, Warning,
				TEXT("Click ignored: could not deproject the cursor (GetMousePosition=%d at %.0f,%.0f)."),
				bHaveMouse ? 1 : 0, MouseX, MouseY);
		}
		return false;
	}

	// These guards were skipped while the build camera was orthographic, and skipping them
	// was what stopped good clicks vanishing: under an orthographic projection the
	// deprojected origin sits on the near plane rather than at the camera, so the
	// ray/plane distance carries no information about where the click landed. The view is
	// perspective now and they are live and necessary again - if an orthographic mode ever
	// returns, it must exempt itself from both of them.
	//
	// Measured against the ACTIVE rig's distance rather than a fixed number, because the
	// view spans a hundredfold range and no single cap suits both ends of it. BuildCameraComp->
	// ActiveRig(), not the build view unconditionally: before issue #94 this read the build
	// view's distance even while watching an agent, when the watch rig - parked at a very
	// different distance - was the one actually driving the camera. THE FORGOTTEN TERNARY
	// that issue's evidence names; ActiveRig() is the one place that ternary is asked now.
	//
	// THE SAME RULE THE EDITOR APPLIES NOW (issue #191/#92-#93): URoadBuildEditorTool::
	// RayToPlane multiplies its own Render-measured ViewCentreDistance by the identical
	// RoadGeom::DefaultMaxPlaceDistanceFactor this UPROPERTY defaults from, so a near-horizon
	// click refuses in both drivers rather than only here.
	const double Furthest = MaxPlaceDistanceFactor * BuildCameraComp->ActiveRig().Distance;

	RoadGeom::ERayToPlaneRefusal Why = RoadGeom::ERayToPlaneRefusal::None;
	double Distance = 0.0;
	if (RoadGeom::RayToPlaneZ(Origin, Direction, Target->SurfaceZ, Furthest, OutPosition, &Why, &Distance))
	{
		// The one place a good hit is recorded, so every refusal path below - and
		// MakeToolContext, which cannot afford to skip a frame - can fall back to where
		// the cursor last actually was. Recorded on FBuildSession (see RecordPlaneHit) so
		// the editor tool's identical fallback cannot drift from this one - issue #92.
		Session.RecordPlaneHit(OutPosition);
		return true;
	}

	if (bLogRefusals)
	{
		switch (Why)
		{
		case RoadGeom::ERayToPlaneRefusal::Parallel:
			UE_LOG(LogRoadBuild, Warning,
				TEXT("Click ignored: the view is edge-on to the road plane (dir.Z=%.6f)."), Direction.Z);
			break;

		case RoadGeom::ERayToPlaneRefusal::BehindOrigin:
			// Behind the camera. Without this a click on the sky lands on the plane's
			// mirror image, dropping a node far off in the opposite direction.
			UE_LOG(LogRoadBuild, Warning,
				TEXT("Click ignored: the road plane is behind the camera there."));
			break;

		case RoadGeom::ERayToPlaneRefusal::BeyondMaxDistance:
			UE_LOG(LogRoadBuild, Warning,
				TEXT("Click ignored: the road plane is past %.0f uu away there (%.1fx the view, actual %.0f uu)."),
				Furthest, MaxPlaceDistanceFactor, Distance);
			break;

		case RoadGeom::ERayToPlaneRefusal::None:
		default:
			// Unreachable: RayToPlaneZ returned false, so Why is one of the three cases
			// above. Left here rather than omitted so a fourth refusal added there is a
			// compile warning here, not a silent no-op log.
			break;
		}
	}
	return false;
}

void ARoadBuildController::ZoomIn()
{
	BuildCameraComp->ZoomBy(-1.0);
}

void ARoadBuildController::ZoomOut()
{
	BuildCameraComp->ZoomBy(1.0);
}

bool ARoadBuildController::ResolveSnap(FRoadSnapResult& Out, bool bLogRefusals) const
{
	FVector2D Cursor;
	if (Target == nullptr || !CursorOnRoadPlane(Cursor, bLogRefusals))
	{
		return false;
	}

	// Snap is the airport's own now, not this driver's - see ARoadNetworkActor::Snap and
	// issue #93.
	return Session.ResolveSnap(Target->Network, Cursor, Target->Snap, Out);
}

IBuildTool* ARoadBuildController::GetActiveTool() const
{
	return Session.GetActiveTool();
}

void ARoadBuildController::OnToggleGuidelines()
{
	bShowGuidelines = !bShowGuidelines;

	// Logged because an overlay that fails to appear and one that is switched off look
	// identical on screen, and this project has already spent rounds on that distinction.
	UE_LOG(LogRoadBuild, Log, TEXT("Guideline overlay %s"),
		bShowGuidelines ? TEXT("on") : TEXT("off"));
}

bool ARoadBuildController::IsRemoveHeld() const
{
	return IsInputKeyDown(EKeys::LeftControl) || IsInputKeyDown(EKeys::RightControl);
}

void ARoadBuildController::ComputeCurrentPlaneHitAndTunables(
	FVector2D& OutPlaneHit, FBuildSessionTunables& OutTunables) const
{
	// Runs every PlayerTick, so a frame where the cursor is off the plane (above the
	// horizon, say) cannot simply skip building a context - the ghost and the snap chain
	// still need a position. Fall back to the last good hit rather than an unwritten
	// FVector2D: before this, CursorOnRoadPlane's failure paths left PlaneHit exactly as
	// its default constructor did (uninitialised), so a refused frame fed garbage into
	// Session.MakeContext -> ResolveSnap -> SnapChain.Resolve. Same fallback
	// URoadBuildEditorTool::MakeContext already uses with HoverPosition.
	OutPlaneHit = Session.LastPlaneHit();
	CursorOnRoadPlane(OutPlaneHit);

	// Read fresh every call rather than cached, so a details-panel edit to the airport's own
	// Snap/PlacementLimits takes effect on the very next click. Target->MakeTunables is the
	// one place both drivers build this now - see issue #93 - and also refreshes
	// Target->PlacementLimits.NewRoadHalfWidth in place, which is what keeps the deletion
	// planner's own corner-fit check current (see ARoadNetworkActor::PlacementLimits).
	OutTunables = Target != nullptr ? Target->MakeTunables(0.0) : FBuildSessionTunables();

	// ToolPickRadius is this driver's own view fact, not an airport tunable - see its
	// declaration on this class.
	OutTunables.ToolPickRadius = ToolPickRadius;
}

FToolContext ARoadBuildController::MakeToolContext() const
{
	FVector2D PlaneHit;
	FBuildSessionTunables Tunables;
	ComputeCurrentPlaneHitAndTunables(PlaneHit, Tunables);

	// See FBuildSession::MakeContext for why Cursor is the raw hit and Snap rides beside
	// it rather than being folded into it.
	// ONLY THE HELD KEYS from here. The sticky mode ORs with them inside MakeContext now
	// that the session owns it - see EGestureMode.
	// ALT GOES BEFORE THE HOVER AGENT, and the order is load-bearing: bSuspendGuides was
	// inserted ahead of HoverAgent, so leaving this call as it was would have passed an int32
	// agent id into a bool - compiling perfectly and suspending every guide the moment the
	// cursor was over an aeroplane, while the hover pick silently became 0.
	//
	// STRAIGHT TO Session.MakeContext, NOT THROUGH GetFrameContext - deliberately (issue #303).
	// This function exists for callers that want the pipeline run fresh against the mouse
	// position at the exact moment they fire (a click, a drag step - see this method's own
	// header comment); PlayerTick is the one caller that wants the cache, and reaches it
	// directly with the same PlaneHit/Tunables this function just assembled.
	return Session.MakeContext(Target, PlaneHit, Tunables,
		IsRemoveHeld(),
		IsInputKeyDown(EKeys::LeftShift) || IsInputKeyDown(EKeys::RightShift),
		IsInputKeyDown(EKeys::LeftAlt) || IsInputKeyDown(EKeys::RightAlt),
		HoverAgentUnderCursor());
}

void ARoadBuildController::ToggleGuideRelation(SnapGuide::ERelation Relation)
{
	if (ARoadNetworkActor* Actor = GetTarget())
	{
		Actor->GuideSources.ToggleRelation(Relation);
	}
}

bool ARoadBuildController::IsGuideRelationOn(SnapGuide::ERelation Relation) const
{
	const ARoadNetworkActor* Actor = GetTarget();
	return Actor != nullptr && Actor->GuideSources.IsRelationOn(Relation);
}

void ARoadBuildController::ToggleGuideReference(SnapGuide::EReference Reference)
{
	if (ARoadNetworkActor* Actor = GetTarget())
	{
		Actor->GuideSources.ToggleReference(Reference);
	}
}

// THE ROW FLAG ALONE, not IsEnabled: a button is lit when its own axis is on, and a cell that
// happens to be a hole must not make the column look switched off. The AND belongs in the
// chain, where a candidate is judged - not in what the bar draws.
bool ARoadBuildController::IsGuideReferenceOn(SnapGuide::EReference Reference) const
{
	const ARoadNetworkActor* Actor = GetTarget();
	return Actor != nullptr && Actor->GuideSources.IsReferenceOn(Reference);
}

int32 ARoadBuildController::HoverAgentUnderCursor() const
{
	UGroundTraffic* AgentModel = Target != nullptr ? Target->GetGroundTraffic() : nullptr;
	if (AgentModel == nullptr)
	{
		return 0;
	}
	float MouseX = 0.0f, MouseY = 0.0f;
	if (!GetMousePosition(MouseX, MouseY))
	{
		return 0;
	}

	// ONE PROJECTION MATRIX FOR EVERY AGENT, not one per agent - issue #167. What this used
	// to call per agent, ProjectWorldLocationToScreen, forwards to
	// UGameplayStatics::ProjectWorldToScreen, which fetches FSceneViewProjectionData and
	// rebuilds its view-projection matrix from scratch on every single call - the same numbers
	// for every agent this frame, since the view has not moved mid-tick. Fetched once here
	// instead and handed to FSceneView::ProjectWorldToScreen per agent, which is the same
	// underlying maths with the per-call setup taken out.
	const ULocalPlayer* LocalPlayer = GetLocalPlayer();
	if (LocalPlayer == nullptr || LocalPlayer->ViewportClient == nullptr)
	{
		return 0;
	}
	FSceneViewProjectionData ProjectionData;
	if (!LocalPlayer->GetProjectionData(LocalPlayer->ViewportClient->Viewport, ProjectionData))
	{
		return 0;
	}
	const FMatrix ViewProjectionMatrix = ProjectionData.ComputeViewProjectionMatrix();
	const FIntRect ConstrainedViewRect = ProjectionData.GetConstrainedViewRect();

	// Project the VIEW's location, not the model's road-plane position: the view carries
	// altitude, and an aircraft on final is picked where it is drawn.
	//
	// RESERVED, issue #167: these used to grow one push at a time over every live agent.
	const TArray<FRoadAgent>& Agents = AgentModel->GetAgents();
	TArray<FVector2D> Screen;
	TArray<int32> Ids;
	Screen.Reserve(Agents.Num());
	Ids.Reserve(Agents.Num());
	for (const FRoadAgent& Agent : Agents)
	{
		const ARoadAgentActor* View = Target->GetAgentView(Agent.Id);
		FVector2D At;
		if (View != nullptr && FSceneView::ProjectWorldToScreen(
			View->GetActorLocation(), ConstrainedViewRect, ViewProjectionMatrix, At))
		{
			Screen.Add(At);
			Ids.Add(Agent.Id);
		}
	}
	const int32 Pick = ScreenPick::NearestWithin(Screen, FVector2D(MouseX, MouseY), AgentPickPixels);
	return Pick != INDEX_NONE ? Ids[Pick] : 0;
}

bool ARoadBuildController::SelectedAgentFacts(FAgentFacts& Out) const
{
	const FSelection& Sel = GetSelection();
	if (Sel.Kind != ESelectionKind::Aircraft || Target == nullptr || Target->GetGroundTraffic() == nullptr)
	{
		return false;
	}
	return InspectFacts::DescribeAgent(*Target->GetGroundTraffic(), Target->GetNetwork(), Sel.Id, Out);
}

bool ARoadBuildController::SelectedAgentFactsThisFrame(FAgentFacts& Out) const
{
	if (!SelectedAgentFactsCache.IsSet())
	{
		FAgentFacts Facts;
		SelectedAgentFactsCache = SelectedAgentFacts(Facts)
			? TOptional<FAgentFacts>(MoveTemp(Facts))
			: TOptional<FAgentFacts>();
	}
	const TOptional<FAgentFacts>& Cached = SelectedAgentFactsCache.GetValue();
	if (Cached.IsSet())
	{
		Out = Cached.GetValue();
		return true;
	}
	return false;
}

bool ARoadBuildController::SelectedStandFacts(FStandFacts& Out) const
{
	const FSelection& Sel = GetSelection();
	if (Sel.Kind != ESelectionKind::Stand || Target == nullptr || Target->GetNetwork() == nullptr)
	{
		return false;
	}
	return InspectFacts::DescribeStand(Target->GetGroundTraffic(), *Target->GetNetwork(), Sel.Id, Out);
}

bool ARoadBuildController::CanDepartSelected() const
{
	// THROUGH THE PER-FRAME CACHE (issue #187): the bar polls this every tick, and
	// UInspectorWidget::Refresh asks the same question of the same selection the same frame -
	// see SelectedAgentFactsThisFrame's own comment.
	FAgentFacts Facts;
	return SelectedAgentFactsThisFrame(Facts) && Facts.bCanDepart;
}

void ARoadBuildController::DepartSelected()
{
	if (!HasSelectedAircraft() || Target == nullptr)
	{
		UE_LOG(LogRoadBuild, Warning, TEXT("Depart: no aircraft selected."));
		return;
	}
	const EDepartureRefusal Why = Target->DepartAgent(GetSelection().Id);
	UE_LOG(LogRoadBuild, Log, TEXT("Depart aircraft %d: %s"), GetSelection().Id,
		Why == EDepartureRefusal::None ? TEXT("accepted") : *UEnum::GetValueAsString(Why));
}

void ARoadBuildController::OnActionKey(FKey Key)
{
	// The chord is already matched by the binding; Ctrl state is re-read only to pick between
	// two actions on the same key that differ by it (none today, but the table allows it).
	const bool bCtrl = IsInputKeyDown(EKeys::LeftControl) || IsInputKeyDown(EKeys::RightControl);
	RunActionForKey(Key, bCtrl);
}

void ARoadBuildController::RunActionForKey(FKey Key, bool bCtrl)
{
	// TryRun, not Execute: a key used to fire a disabled action (undo with nothing to undo,
	// land with no runway) because this scan never consulted IsEnabled - the bar and the
	// inspector always did. Behaviour change: disabled actions now stop firing from keys too.
	const FBuildAction* Action = FindAction(Key, bCtrl);
	// FALL BACK TO THE PLAIN ACTION, issue #192: FindAction required an EXACT bRequiresCtrl
	// match, so a plain tool key (e.g. '1') registered with bRequiresCtrl=false matched nothing
	// while the player happened to be holding Ctrl for an unrelated reason (remove mode's own
	// modifier) - the key that goes nowhere, CLAUDE.md's own name for this class of bug. Only
	// tried when Ctrl WAS held and the exact match failed: an action that explicitly REQUIRES
	// Ctrl must never fire without it, so this can never invoke one by accident.
	if (Action == nullptr && bCtrl)
	{
		Action = FindAction(Key, false);
	}
	if (Action != nullptr)
	{
		Action->TryRun(*this, TEXT("Key"));
	}
}

void ARoadBuildController::OnCtrlActionKey()
{
	for (const FBuildAction& Action : BuildActions())
	{
		if (Action.bRequiresCtrl && Action.Key.IsValid() && WasInputKeyJustPressed(Action.Key))
		{
			Action.TryRun(*this, TEXT("Key"));
			return;
		}
	}
}

void ARoadBuildController::SelectTool(int32 Index)
{
	if (!ToolRegistry().IsValidIndex(Index))
	{
		return;
	}
	// The session drops a sticky BUILD modifier with the tool it was lit under, and keeps
	// Edit - see FBuildSession::SelectTool for why those two differ.
	Session.SelectTool(Index, MakeToolContext());
	if (IBuildTool* Active = Session.GetActiveTool())
	{
		UE_LOG(LogRoadBuild, Log, TEXT("Tool: %s"), *Active->GetDisplayName().ToString());
	}

	// The active tool pointer usually changes here, which FToolReadoutKey already catches -
	// but SelectTool can also re-select the CURRENT tool's index, dropping its sticky BUILD
	// modifier without changing the pointer. See InvalidateToolReadoutCache's comment.
	InvalidateToolReadoutCache();
}

int32 ARoadBuildController::GetActiveToolIndex() const
{
	return Session.GetActiveToolIndex();
}

int32 ARoadBuildController::MakeContextCallCountForTest() const
{
	return Session.MakeContextCallCountForTest();
}



void ARoadBuildController::ToggleGestureMode(EGestureMode Mode)
{
	// The caller's context, so the outgoing tool can abandon a part-drawn chain against a
	// real target rather than a default-constructed one.
	Session.ToggleGestureMode(Mode, MakeToolContext());

	// Build<->Edit changes EEditHandleKind, already in the key - but also abandons whatever
	// stage the outgoing tool was in. See InvalidateToolReadoutCache's comment.
	InvalidateToolReadoutCache();
}

EGestureMode ARoadBuildController::GetGestureMode() const
{
	return Session.GetGestureMode();
}

bool ARoadBuildController::WantsRoadNodesDrawn() const
{
	return Session.WantsRoadNodesDrawn();
}

bool ARoadBuildController::ActiveToolHasEditHandles() const
{
	const TConstArrayView<FToolRegistration> Registry = ToolRegistry();
	const int32 Index = Session.GetActiveToolIndex();
	return Registry.IsValidIndex(Index) && Registry[Index].EditHandles != EEditHandleKind::None;
}

bool ARoadBuildController::CanUndo() const { return Target != nullptr && Target->CanUndo(); }
bool ARoadBuildController::CanRedo() const { return Target != nullptr && Target->CanRedo(); }

bool ARoadBuildController::HasNetworkContent() const
{
	return Target != nullptr && Target->Network != nullptr
		&& (Target->Network->GetNodes().Num() > 0 || Target->Network->GetAprons().Num() > 0);
}

bool ARoadBuildController::HasRunway() const
{
	// CACHED, invalidated by the facade's OnChanged - issue #187. This comment used to argue
	// AGAINST a cache here ("a saving nobody would measure"), written when this was the only
	// per-tick reader of it; it is now one of roughly thirty rows RefreshState asks every tick
	// (UBuildBarWidget::RefreshState), so the "nobody would measure" premise no longer holds -
	// see CLAUDE.md on comments whose reasoning stops matching the code around them.
	if (!bRunwayCacheValid)
	{
		bRunwayCache = Target != nullptr && Target->Network != nullptr
			&& AirsideCapability::Summarise(*Target->Network).Runways.Num() > 0;
		bRunwayCacheValid = true;
		++RunwayRecomputeCountForTest;
	}
	return bRunwayCache;
}

void ARoadBuildController::BindRunwayCacheInvalidation()
{
	// A fresh Target's runway state has not been read yet, whatever the old one's cache said -
	// invalidated unconditionally, even when Target is null (HasRunway's own null check then
	// answers false, which is correct and needs no cached "true" from a previous target to
	// survive the swap).
	bRunwayCacheValid = false;

	URoadEditFacade* Facade = Target != nullptr ? Target->GetEditFacade() : nullptr;
	if (Facade == BoundRunwayCacheFacade.Get())
	{
		// Already subscribed to this exact facade (or both are null) - AddUObject has no
		// duplicate-add guard of its own, so re-binding here would fire the handler twice
		// per notify.
		return;
	}
	BoundRunwayCacheFacade = Facade;
	if (Facade != nullptr)
	{
		Facade->OnChanged.AddUObject(this, &ARoadBuildController::OnNetworkChangedInvalidateRunwayCache);
	}
}

void ARoadBuildController::OnNetworkChangedInvalidateRunwayCache(EChangeKind Kind)
{
	if (Kind == EChangeKind::Topology)
	{
		bRunwayCacheValid = false;
	}
}

bool ARoadBuildController::HasAgent() const
{
	return Target != nullptr && Target->GetAgentCount() > 0;
}

bool ARoadBuildController::HasOpsRuntime() const
{
	return UOpsRuntimeSubsystem::Get(GetWorld()) != nullptr;
}

void ARoadBuildController::ToggleLedger()
{
	if (Hud != nullptr && Hud->LedgerPanel != nullptr)
	{
		Hud->LedgerPanel->Toggle();
		UE_LOG(LogRoadBuild, Log, TEXT("Ledger panel %s"),
			Hud->LedgerPanel->IsShowing() ? TEXT("opened") : TEXT("closed"));
	}
}

bool ARoadBuildController::IsLedgerShowing() const
{
	return Hud != nullptr && Hud->LedgerPanel != nullptr && Hud->LedgerPanel->IsShowing();
}

bool ARoadBuildController::IsPaused() const
{
	const UOpsRuntime* Runtime = UOpsRuntimeSubsystem::Get(GetWorld());
	return Runtime != nullptr && Runtime->GetClock()->GetSpeed() == ESimSpeed::Paused;
}

void ARoadBuildController::OnUndo()
{
	// Ctrl+Z is bound as a chord from BuildActions(), so the bar's Undo button and the key
	// reach here the same way; the handler no longer re-checks Ctrl.
	if (Target == nullptr)
	{
		return;
	}

	const FString Label = Target->PeekUndoLabel();
	if (!Target->Undo())
	{
		UE_LOG(LogRoadBuild, Log, TEXT("Nothing to undo."));
		return;
	}

	// The tool may be part-way through something built on a graph that no longer exists.
	if (IBuildTool* Tool = GetActiveTool())
	{
		Tool->OnDeactivate(MakeToolContext());
	}

	// The deactivate above abandons the tool's stage - see InvalidateToolReadoutCache.
	InvalidateToolReadoutCache();

	UE_LOG(LogRoadBuild, Log, TEXT("Undid: %s"), *Label);
}

void ARoadBuildController::OnRedo()
{
	// Ctrl+Y is a chord binding now, as Undo's is.
	if (Target == nullptr)
	{
		return;
	}

	if (!Target->Redo())
	{
		UE_LOG(LogRoadBuild, Log, TEXT("Nothing to redo."));
		return;
	}

	if (IBuildTool* Tool = GetActiveTool())
	{
		Tool->OnDeactivate(MakeToolContext());
	}

	// The deactivate above abandons the tool's stage - see InvalidateToolReadoutCache.
	InvalidateToolReadoutCache();
}

void ARoadBuildController::OnPrimaryPressed()
{
	float MouseX = 0.0f;
	float MouseY = 0.0f;
	GetMousePosition(MouseX, MouseY);
	Gesture.Press(FVector2D(MouseX, MouseY));
}

void ARoadBuildController::UpdateDrag()
{
	IBuildTool* Tool = GetActiveTool();
	if (!Gesture.IsPressed() || Tool == nullptr || Target == nullptr)
	{
		return;
	}

	// The threshold is the controller's business: it is a fact about the mouse, not about
	// what dragging means. Without it every slightly imprecise click would be read as a
	// drag and the click interactions would be impossible to perform.
	//
	// GetMousePosition can fail if the mouse has left the viewport. Feeding Move a sentinel
	// position on that frame is wrong, not merely stale: any large sentinel measures as
	// farther than the threshold from the press, so a lost cursor would PROMOTE a click to a
	// drag rather than leave it alone - the opposite of "stale coordinates should not count".
	// A press already dragging still needs feeding (Move(Dragging) does nothing with the
	// position it did not get), so only a fresh, not-yet-dragging press bails here.
	float MouseX = 0.0f;
	float MouseY = 0.0f;
	if (!GetMousePosition(MouseX, MouseY) && !Gesture.IsDragging())
	{
		return;
	}
	const EGestureStep Step = Gesture.Move(FVector2D(MouseX, MouseY), DragThresholdPixels);

	if (Step == EGestureStep::None)
	{
		return;
	}

	// A FRESH CONTEXT HERE, not FrameContext - issue #167. GetMousePosition just read above is
	// this frame's cursor at the MOMENT of the drag, which can differ from where it was when
	// PlayerTick built FrameContext at the top of the frame; a drag legitimately needs that.
	// ONE rebuild covers both calls below, where this used to build one per call (up to two on
	// the frame a drag begins) - the "1-2 more while dragging" issue #167 measured.
	const FToolContext DragContext = MakeToolContext();

	if (Step == EGestureStep::DragBegan)
	{
		Tool->OnDragBegin(DragContext);
	}

	Tool->OnDrag(DragContext);

	// A drag step can advance the tool's own stage with FrameContext's cursor unmoved from
	// this frame's - see InvalidateToolReadoutCache's comment.
	InvalidateToolReadoutCache();
}

void ARoadBuildController::OnPrimaryReleased()
{
	const EGestureEnd End = Gesture.Release();

	IBuildTool* Tool = GetActiveTool();
	if (Tool == nullptr || Target == nullptr || End == EGestureEnd::Nothing)
	{
		return;
	}

	// A press that never travelled was a click after all.
	const FToolContext Context = MakeToolContext();
	if (End == EGestureEnd::DragEnd)
	{
		Tool->OnDragEnd(Context);
	}
	else
	{
		Tool->OnClick(Context);
	}

	// A click or a drag end is exactly the kind of stage advance FToolReadoutKey cannot see -
	// see InvalidateToolReadoutCache's comment.
	InvalidateToolReadoutCache();
}

void ARoadBuildController::PlayerTick(float DeltaTime)
{
	Super::PlayerTick(DeltaTime);

	UpdateView(DeltaTime);

	// ONE CONTEXT FOR THE WHOLE FRAME - issue #167. CollectToolReadout and Tick below, and
	// ARoadBuildHUD::DrawHUD after this frame's PlayerTick returns, all read GetFrameContext()
	// instead of calling MakeToolContext() themselves. MakeToolContext runs the whole snap +
	// guide pipeline (a junction solve per live node outside its cheap-reject radius, then the
	// guide chain over every source) and used to be called by each of those independently -
	// 3-5 times a frame for one cursor position. UpdateDrag is the one exception: a drag reads
	// the mouse position again, after this line, and needs a context that reflects it - see
	// UpdateDrag's own comment.
	//
	// THROUGH Session.GetFrameContext, NOT MakeToolContext, since issue #303 moved the cache
	// itself onto FBuildSession - this driver's own contribution is only assembling the same
	// PlaneHit/Tunables MakeToolContext would, through the one shared helper. The editor mode's
	// OnUpdateHover reaches the SAME session function for the SAME reason: URoadBuildEditorTool
	// has no equivalent "top of frame" hook to build once and pass down the way PlayerTick
	// always has, so the cache is what lets its Render and DrawHUD reuse this frame's answer
	// instead of rebuilding it independently.
	//
	// BUILT BEFORE THE TARGET GUARD BELOW, same as CollectToolReadout always ran before it:
	// ComputeCurrentPlaneHitAndTunables already null-guards Target internally (Target->
	// MakeTunables only runs when Target is set), so building it here costs nothing extra on a
	// frame with no road actor and keeps FrameContext's own null-Target behaviour identical to
	// before.
	{
		FVector2D PlaneHit;
		FBuildSessionTunables Tunables;
		ComputeCurrentPlaneHitAndTunables(PlaneHit, Tunables);
		FrameContext = Session.GetFrameContext(Target, PlaneHit, Tunables,
			IsRemoveHeld(),
			IsInputKeyDown(EKeys::LeftShift) || IsInputKeyDown(EKeys::RightShift),
			IsInputKeyDown(EKeys::LeftAlt) || IsInputKeyDown(EKeys::RightAlt),
			HoverAgentUnderCursor());
	}

	// BEFORE THE TARGET GUARD, so a frame with no road actor clears the bar instead of
	// leaving the last gesture's bay count sitting on it forever.
	CollectToolReadout();

	if (Target == nullptr)
	{
		return;
	}

	// The deletion planner judges its rejoins by the same rules a click obeys, so the two
	// cannot drift apart - Target->MakeTunables, called above while building FrameContext,
	// refreshes Target->PlacementLimits.NewRoadHalfWidth in place for exactly this reason. See
	// ARoadNetworkActor::PlacementLimits and issue #93. STILL ONCE A FRAME after issue #167,
	// not moved to a "profile changed" event: NewRoadHalfWidth comes from
	// ARoadNetworkActor::ResolveProfile (the actor's own authored/fallback profile), which has
	// no connection to a tool's WidthIndex or its OnReselect cycle - MakeTunables refreshes it
	// so a details-panel edit to that profile takes effect on the very next frame, a fact
	// folding this into the once-a-frame build preserves exactly, at a fifth of the cost.

	UpdateDrag();

	if (IBuildTool* Tool = GetActiveTool())
	{
		Tool->Tick(FrameContext);
	}
}

ARoadBuildController::FToolReadoutKey ARoadBuildController::MakeReadoutKey(
	const IBuildTool* Tool, const FToolContext& Context)
{
	FToolReadoutKey Key;
	Key.Tool = Tool;
	Key.Cursor = Context.Cursor;
	Key.SnapKind = Context.Snap.Kind;
	Key.SnapNode = Context.Snap.Node;
	Key.SnapSegment = Context.Snap.Segment;
	Key.bGuideActive = Context.Guide.bActive;
	Key.GuidePoint = Context.Guide.Point;
	Key.EditHandles = Context.EditHandles;
	return Key;
}

void ARoadBuildController::CollectToolReadout()
{
	// The target guard is the same one MakeToolContext's callers already obey: a context
	// built with no actor has no network to snap against, and a tool asked about one would
	// be describing a gesture it could not commit anyway. UNCONDITIONAL - this path never
	// consults FToolReadoutKey, so a road actor removed out from under the controller clears
	// the bar instead of leaving a stale key matching an equally stale readout.
	if (Target == nullptr)
	{
		ToolReadoutCollector.Reset();
		bHasReadoutKey = false;
		return;
	}

	const IBuildTool* Tool = GetActiveTool();
	if (Tool == nullptr)
	{
		ToolReadoutCollector.Reset();
		bHasReadoutKey = false;
		return;
	}

	// FrameContext, not a fresh MakeToolContext() - issue #167. Built once at the top of
	// PlayerTick, above, and shared with Tick and the HUD.
	const FToolReadoutKey NewKey = MakeReadoutKey(Tool, FrameContext);
	if (bHasReadoutKey && NewKey == LastReadoutKey)
	{
		// SAME QUESTION AS LAST FRAME - issue #190. ToolReadoutCollector.Readout still holds
		// last frame's answer; a still cursor now costs one key comparison instead of a
		// TArray<TPair<FString,FString>> and a Printf per fact every tick.
		return;
	}

	ToolReadoutCollector.Reset();
	Tool->BuildReadout(FrameContext, ToolReadoutCollector);
	LastReadoutKey = NewKey;
	bHasReadoutKey = true;
	++ToolReadoutRevision;
}

void ARoadBuildController::OnBuild()
{
	if (IBuildTool* Tool = GetActiveTool())
	{
		// NOT GUARDED ON bCommittable HERE. FBuildAction::TryRun is the one door and has
		// already checked IsEnabled; a tool's OnCommit ignores the call in every stage but
		// its last anyway (see FPlotPlaceTool::OnCommit), so a second copy of the rule here
		// would be a second thing to keep in agreement with the readout.
		Tool->OnCommit(MakeToolContext());
	}

	// A commit resets the tool's stage - see InvalidateToolReadoutCache's comment.
	InvalidateToolReadoutCache();
}

void ARoadBuildController::OnCancelGesture()
{
	Session.CancelActiveGesture(MakeToolContext());

	// A cancel abandons whatever stage the gesture was in - see InvalidateToolReadoutCache.
	InvalidateToolReadoutCache();
}

void ARoadBuildController::OnClearNetwork()
{
	if (Target == nullptr)
	{
		return;
	}

	// The tool may be holding a node from the graph about to be discarded.
	if (IBuildTool* Tool = GetActiveTool())
	{
		Tool->OnDeactivate(MakeToolContext());
	}

	Target->ClearNetwork();
	UE_LOG(LogRoadBuild, Log, TEXT("Network cleared."));

	// The network the last readout described no longer exists - see InvalidateToolReadoutCache.
	InvalidateToolReadoutCache();
}

// --- Sim clock and quick save ---------------------------------------------------------------

namespace
{
	UOpsRuntime* RuntimeFor(const APlayerController& PC)
	{
		UOpsRuntime* Runtime = UOpsRuntimeSubsystem::Get(PC.GetWorld());
		if (Runtime == nullptr)
		{
			// Says so rather than silently doing nothing: "pressing P does nothing" is the
			// exact shape of bug CLAUDE.md warns about, and the reason is worth one line.
			UE_LOG(LogRoadBuild, Warning,
				TEXT("No OpsRuntime: clock and save keys need a game instance (PIE), not the editor mode"));
		}
		return Runtime;
	}
}

void ARoadBuildController::StepSpeed(int32 Delta) { if (UOpsRuntime* R = RuntimeFor(*this)) { R->StepSpeed(Delta); } }
void ARoadBuildController::TogglePause()          { if (UOpsRuntime* R = RuntimeFor(*this)) { R->TogglePause(); } }
void ARoadBuildController::QuickSave()            { if (UOpsRuntime* R = RuntimeFor(*this)) { R->SaveToSlot(TEXT("QuickSave")); } }
void ARoadBuildController::QuickLoad()            { if (UOpsRuntime* R = RuntimeFor(*this)) { R->LoadFromSlot(TEXT("QuickSave")); } }
