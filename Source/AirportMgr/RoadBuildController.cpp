#include "RoadBuildController.h"

#include "Blueprint/UserWidget.h"
#include "BuildActions.h"
#include "BuildBarWidget.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/InputComponent.h"
#include "Content/AirsideSettings.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "Model/AirsideCapability.h"
#include "Model/RoadNetwork.h"
#include "Model/SimClock.h"
#include "Present/OpsRuntime.h"
#include "Present/OpsRuntimeSubsystem.h"
#include "Present/RoadAgentActor.h"
#include "Present/RoadNetworkActor.h"
#include "Profiles/RoadProfile.h"
#include "Solve/RoadGeom.h"

DEFINE_LOG_CATEGORY_STATIC(LogRoadBuild, Log, All);

ARoadBuildController::ARoadBuildController()
{
	bShowMouseCursor = true;
	bEnableClickEvents = true;
	bEnableMouseOverEvents = true;
}

void ARoadBuildController::BeginPlay()
{
	Super::BeginPlay();

	for (TActorIterator<ARoadNetworkActor> It(GetWorld()); It; ++It)
	{
		Target = *It;
		break;
	}

	if (Target == nullptr)
	{
		UE_LOG(LogRoadBuild, Warning,
			TEXT("No ARoadNetworkActor in the level - place one, or clicks will do nothing."));
		return;
	}

	// Session is constructed from ToolRegistry() already - see FBuildSession's constructor.
	// There is no second list here to fall out of step with it: the mismatch this project
	// has shipped three times (a tool with no key, or a key with no tool) is now a mismatch
	// the registry would have to disagree with ITSELF to produce.

	if (bStartAbovePlane)
	{
		CreateBuildCamera();
	}

	// Game AND UI: the bar's buttons must take a click before the road tool sees it, and the
	// camera keys must keep working while the bar has focus.
	FInputModeGameAndUI Mode;
	Mode.SetHideCursorDuringCapture(false);
	SetInputMode(Mode);

	// The bar. The configured Blueprint if there is one, else the C++ class itself - which
	// builds every section in code, so a missing asset degrades rather than breaks.
	const TSubclassOf<UBuildBarWidget> BarClass =
		BuildBarClass != nullptr ? BuildBarClass : TSubclassOf<UBuildBarWidget>(UBuildBarWidget::StaticClass());
	BuildBar = CreateWidget<UBuildBarWidget>(this, BarClass);
	if (BuildBar != nullptr)
	{
		BuildBar->AddToViewport();
		UE_LOG(LogRoadBuild, Log, TEXT("Build bar: %s"),
			BuildBarClass != nullptr ? *BuildBarClass->GetName() : TEXT("code-only (no BuildBarClass configured)"));
	}

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
		TEXT("Road building ready on %s. Left click places and connects, right click ends the chain. ")
		TEXT("Keys: %s. WASD pans, Q/E rotate, wheel zooms - while building or watching. ")
		TEXT("Every key is also a button on the bar."),
		*Target->GetName(), *Keys);
}

void ARoadBuildController::ApplyViewLimits(FBuildCameraRig& Rig) const
{
	Rig.MinDistance = MinViewDistance;
	Rig.MaxDistance = MaxViewDistance;
	Rig.MinPitch = MinPitchDegrees;
	Rig.MaxPitch = MaxPitchDegrees;
}

void ARoadBuildController::CreateBuildCamera()
{
	if (Target == nullptr || GetWorld() == nullptr)
	{
		return;
	}

	ApplyViewLimits(TargetView);
	TargetView.Focus = FVector2D::ZeroVector;
	TargetView.Distance = FMath::Clamp(StartViewDistance, MinViewDistance, MaxViewDistance);
	TargetView.Yaw = 0.0;

	// The view starts settled rather than easing in from wherever a default-constructed
	// rig happens to sit, which would swoop the camera across the map on possession.
	CurrentView = TargetView;

	FActorSpawnParameters Params;
	Params.ObjectFlags |= RF_Transient;
	BuildCamera = GetWorld()->SpawnActor<ACameraActor>(
		CurrentView.CameraLocation(Target->SurfaceZ), CurrentView.CameraRotation(), Params);
	if (BuildCamera == nullptr)
	{
		return;
	}

	UCameraComponent* Camera = BuildCamera->GetCameraComponent();
	Camera->SetProjectionMode(ECameraProjectionMode::Perspective);
	Camera->SetFieldOfView(static_cast<float>(FieldOfView));

	// Viewing through a camera actor takes the view away from the pawn, so the pawn's
	// mouse-look stops fighting the cursor for the same input.
	SetViewTarget(BuildCamera);

	UE_LOG(LogRoadBuild, Log,
		TEXT("Build camera: %.0f uu out at %.1f degrees. Pitch follows the zoom, %.0f to %.0f degrees."),
		CurrentView.Distance, CurrentView.PitchDegrees(), MinPitchDegrees, MaxPitchDegrees);
}

void ARoadBuildController::UpdateView(float DeltaTime)
{
	if (BuildCamera == nullptr || Target == nullptr)
	{
		return;
	}

	// Read as held keys rather than bound as actions: pan and rotate are continuous, and a
	// key binding fires once on press. The same reason WASD was never bound.
	const double Right = (IsInputKeyDown(EKeys::D) ? 1.0 : 0.0) - (IsInputKeyDown(EKeys::A) ? 1.0 : 0.0);
	const double Forward = (IsInputKeyDown(EKeys::W) ? 1.0 : 0.0) - (IsInputKeyDown(EKeys::S) ? 1.0 : 0.0);
	const double Turn = (IsInputKeyDown(EKeys::E) ? 1.0 : 0.0) - (IsInputKeyDown(EKeys::Q) ? 1.0 : 0.0);

	// WATCHING AN AIRCRAFT drives the watch rig with the same keys, and hands the camera
	// straight back when there is nothing to watch - a mode that stranded the view on a
	// despawned aircraft would leave the player looking at empty sky with no way to tell why.
	if (bWatchingAgent)
	{
		if (ARoadAgentActor* Agent = Target->GetNewestAgent())
		{
			ApplyWatchLimits(WatchTarget);
			WatchTarget.Pan(Right, Forward, PanRate, DeltaTime);
			WatchTarget.Focus = WatchTarget.Focus.GetClampedToMaxSize(WatchMaxFocusOffset);
			WatchTarget.Rotate(Turn * RotateRate * DeltaTime);

			// Eased in the AIRCRAFT'S frame, then projected: the aircraft's own motion
			// reaches the camera rigidly and only the player's inputs are smoothed. Easing
			// a world-space rig towards a moving aircraft would trail it instead.
			WatchCurrent.EaseToward(WatchTarget, CameraLag, DeltaTime);

			const FVector At = Agent->GetActorLocation();
			const FBuildCameraRig World = WatchCurrent.InFrame(FVector2D(At), Agent->GetActorRotation().Yaw);
			BuildCamera->SetActorLocationAndRotation(
				World.CameraLocation(At.Z + WatchFocusHeight), World.CameraRotation());
			return;
		}

		bWatchingAgent = false;
		UE_LOG(LogRoadBuild, Log, TEXT("Nothing to watch: back to the build view."));
	}

	ApplyViewLimits(TargetView);
	TargetView.Pan(Right, Forward, PanRate, DeltaTime);
	TargetView.Rotate(Turn * RotateRate * DeltaTime);
	CurrentView.EaseToward(TargetView, CameraLag, DeltaTime);

	BuildCamera->SetActorLocationAndRotation(
		CurrentView.CameraLocation(Target->SurfaceZ), CurrentView.CameraRotation());
}

void ARoadBuildController::ToggleWatchAgent()
{
	if (Target == nullptr)
	{
		return;
	}

	if (!bWatchingAgent && Target->GetNewestAgent() == nullptr)
	{
		// Refused out loud. Silently staying on the build camera is indistinguishable from
		// the key not being bound, which is a class of confusion this project has paid for.
		UE_LOG(LogRoadBuild, Warning,
			TEXT("Nothing to watch: dispatch an aircraft first (4, then click a start and a goal)."));
		return;
	}

	bWatchingAgent = !bWatchingAgent;
	if (bWatchingAgent)
	{
		// Reset on every entry rather than resuming: C is "show me the aircraft", and a
		// view left zoomed into a wheel last time would answer with a wheel.
		ApplyWatchLimits(WatchTarget);
		WatchTarget.Focus = FVector2D::ZeroVector;
		WatchTarget.Distance = FMath::Clamp(WatchStartDistance, WatchMinDistance, WatchMaxDistance);
		WatchTarget.Yaw = WatchStartYaw;
		WatchCurrent = WatchTarget;
	}

	UE_LOG(LogRoadBuild, Log, TEXT("Camera: %s"),
		bWatchingAgent ? TEXT("watching the aircraft") : TEXT("build view"));
}

void ARoadBuildController::ApplyWatchLimits(FBuildCameraRig& Rig) const
{
	Rig.MinDistance = WatchMinDistance;
	Rig.MaxDistance = WatchMaxDistance;
	Rig.MinPitch = WatchMinPitchDegrees;
	Rig.MaxPitch = WatchMaxPitchDegrees;
}

void ARoadBuildController::SetupInputComponent()
{
	Super::SetupInputComponent();

	// Bound as raw keys rather than through Enhanced Input: the mappings would need
	// InputAction and InputMappingContext content assets, and this driver is meant to
	// work the moment the module compiles, with nothing to author first.
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

void ARoadBuildController::OnLandAircraft()
{
	// Kept by name for anything that still calls it. The registry lands near the view
	// focus, and so does this now: one action, one behaviour.
	LandAircraftNearViewFocus();
}

void ARoadBuildController::LandAircraftNearViewFocus()
{
	if (Target == nullptr)
	{
		return;
	}

	// The VIEW FOCUS rather than the cursor (which this used to read): the bar's Land button
	// is clicked with the cursor on the bar, where "nearest the cursor" is meaningless, and
	// the focus is where the player is looking either way.
	UE_LOG(LogRoadBuild, Log, TEXT("Land: nearest runway to the view focus (%.0f, %.0f)"),
		TargetView.Focus.X, TargetView.Focus.Y);

	// The SAME resolver FRouteTool falls back to, for the same reason: an aircraft that
	// approached as one airframe and taxied as another would be two different aircraft
	// depending on which phase you were watching - see UAirsideSettings::
	// ResolveDefaultAirframe. One FAirframe argument now, not four: issue #29 gave
	// DispatchArrival the same shape ResolveDefaultAirframe already returns.
	//
	// DispatchArrival has already logged which runway, which exit and which stand it chose,
	// or why it declined.
	Target->DispatchArrival(TargetView.Focus, UAirsideSettings::ResolveDefaultAirframe());
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
	// Measured against the current view distance rather than a fixed number, because the
	// view spans a hundredfold range and no single cap suits both ends of it.
	const double Furthest = MaxPlaceDistanceFactor * CurrentView.Distance;

	RoadGeom::ERayToPlaneRefusal Why = RoadGeom::ERayToPlaneRefusal::None;
	double Distance = 0.0;
	if (RoadGeom::RayToPlaneZ(Origin, Direction, Target->SurfaceZ, Furthest, OutPosition, &Why, &Distance))
	{
		// The one place a good hit is recorded, so every refusal path below - and
		// MakeToolContext, which cannot afford to skip a frame - can fall back to where
		// the cursor last actually was.
		LastPlaneHit = OutPosition;
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
	ZoomBy(-1.0);
}

void ARoadBuildController::ZoomOut()
{
	ZoomBy(1.0);
}

void ARoadBuildController::ZoomBy(double Notches)
{
	// The wheel drives whichever rig owns the camera. Zooming the hidden build view while
	// watching would be a surprise stored up for the moment the watch ends.
	FBuildCameraRig& View = bWatchingAgent ? WatchTarget : TargetView;
	bWatchingAgent ? ApplyWatchLimits(View) : ApplyViewLimits(View);
	View.Zoom(ZoomStep, Notches);

	UE_LOG(LogRoadBuild, Log, TEXT("%s %.0f uu out, %.1f degrees"),
		bWatchingAgent ? TEXT("Watch") : TEXT("View"), View.Distance, View.PitchDegrees());
}

FRoadSnapSettings ARoadBuildController::MakeSnapSettings() const
{
	FRoadSnapSettings Settings;
	Settings.NodeRadius = PickRadius;
	Settings.SegmentRadius = SegmentSnapRadius;
	Settings.bSnapToSegments = bSnapToSegments;
	Settings.MinSplitFromEndpoint = MinSplitFromEndpoint;
	Settings.JunctionSnapFactor = JunctionSnapFactor;
	return Settings;
}

bool ARoadBuildController::ResolveSnap(FRoadSnapResult& Out, bool bLogRefusals) const
{
	FVector2D Cursor;
	if (Target == nullptr || !CursorOnRoadPlane(Cursor, bLogRefusals))
	{
		return false;
	}

	return Session.ResolveSnap(Target->Network, Cursor, MakeSnapSettings(), Out);
}

FRoadPlacementLimits ARoadBuildController::MakePlacementLimits() const
{
	FRoadPlacementLimits Limits;
	Limits.MinSegmentLength = MinSegmentLength;
	Limits.MinTurnDegrees = MinTurnDegrees;
	// The corner-fit rule needs the width of the road about to be drawn, which only the
	// actor's profile resolver knows.
	if (Target != nullptr)
	{
		if (const URoadProfile* Profile = Target->ResolveProfile())
		{
			Limits.NewRoadHalfWidth = FMath::Max(Profile->GetHalfWidthLeft(), Profile->GetHalfWidthRight());
		}
	}
	return Limits;
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

FToolContext ARoadBuildController::MakeToolContext() const
{
	// Runs every PlayerTick, so a frame where the cursor is off the plane (above the
	// horizon, say) cannot simply skip building a context - the ghost and the snap chain
	// still need a position. Fall back to the last good hit rather than an unwritten
	// FVector2D: before this, CursorOnRoadPlane's failure paths left PlaneHit exactly as
	// its default constructor did (uninitialised), so a refused frame fed garbage into
	// Session.MakeContext -> ResolveSnap -> SnapChain.Resolve. Same fallback
	// URoadBuildEditorTool::MakeContext already uses with HoverPosition.
	FVector2D PlaneHit = LastPlaneHit;
	CursorOnRoadPlane(PlaneHit);

	// Read fresh every call rather than cached, so a details-panel edit to PickRadius and
	// friends takes effect on the very next click - see MakePlacementLimits' PlayerTick
	// caller below for the same reasoning applied to the facade's own copy.
	FBuildSessionTunables Tunables;
	Tunables.Snap = MakeSnapSettings();
	Tunables.Limits = MakePlacementLimits();
	Tunables.ToolPickRadius = ToolPickRadius;

	// See FBuildSession::MakeContext for why Cursor is the raw hit and Snap rides beside
	// it rather than being folded into it.
	// The sticky modifier ORs with the held key: the bar's Remove button and a held Ctrl
	// mean the same thing, and either lights the same button.
	return Session.MakeContext(Target, PlaneHit, Tunables,
		ClickModifier == EClickModifier::Remove || IsRemoveHeld(),
		ClickModifier == EClickModifier::Insert
			|| IsInputKeyDown(EKeys::LeftShift) || IsInputKeyDown(EKeys::RightShift));
}

void ARoadBuildController::SelectToolByKey(FKey Key)
{
	// Kept for callers by name; the registry route is OnActionKey -> SelectTool.
	OnActionKey(Key);
}

void ARoadBuildController::OnActionKey(FKey Key)
{
	// The chord is already matched by the binding; Ctrl state is re-read only to pick between
	// two actions on the same key that differ by it (none today, but the table allows it).
	const bool bCtrl = IsInputKeyDown(EKeys::LeftControl) || IsInputKeyDown(EKeys::RightControl);
	for (const FBuildAction& Action : BuildActions())
	{
		if (Action.Key == Key && Action.bRequiresCtrl == bCtrl)
		{
			Action.Execute(*this);
			return;
		}
	}
}

void ARoadBuildController::OnCtrlActionKey()
{
	for (const FBuildAction& Action : BuildActions())
	{
		if (Action.bRequiresCtrl && Action.Key.IsValid() && WasInputKeyJustPressed(Action.Key))
		{
			Action.Execute(*this);
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
	Session.SelectTool(Index, MakeToolContext());
	// A sticky modifier was chosen for the tool it was lit under. Dropping it here is what
	// stops a Remove left on from the road tool deleting the first stand the player clicks.
	ClickModifier = EClickModifier::None;
	if (IBuildTool* Active = Session.GetActiveTool())
	{
		UE_LOG(LogRoadBuild, Log, TEXT("Tool: %s"), *Active->GetDisplayName().ToString());
	}
}

int32 ARoadBuildController::GetActiveToolIndex() const
{
	return Session.GetActiveToolIndex();
}

void ARoadBuildController::ToggleClickModifier(EClickModifier Mode)
{
	ClickModifier = (ClickModifier == Mode) ? EClickModifier::None : Mode;
	UE_LOG(LogRoadBuild, Log, TEXT("Click modifier: %s"), *UEnum::GetValueAsString(ClickModifier));
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
	// One pass over the segments per query. The bar polls this every frame; at this
	// project's segment counts that is nothing, and a cache would need invalidating on
	// every edit - the facade's OnChanged - for a saving nobody would measure.
	return Target != nullptr && Target->Network != nullptr
		&& AirsideCapability::Summarise(*Target->Network).Runways.Num() > 0;
}

bool ARoadBuildController::HasAgent() const
{
	return Target != nullptr && Target->GetAgentCount() > 0;
}

bool ARoadBuildController::HasOpsRuntime() const
{
	return UOpsRuntimeSubsystem::Get(GetWorld()) != nullptr;
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
}

void ARoadBuildController::OnPrimaryPressed()
{
	bPrimaryDown = true;
	bDragging = false;

	float MouseX = 0.0f;
	float MouseY = 0.0f;
	GetMousePosition(MouseX, MouseY);
	PressScreen = FVector2D(MouseX, MouseY);
}

void ARoadBuildController::UpdateDrag()
{
	IBuildTool* Tool = GetActiveTool();
	if (!bPrimaryDown || Tool == nullptr || Target == nullptr)
	{
		return;
	}

	if (!bDragging)
	{
		// The threshold is the controller's business: it is a fact about the mouse, not
		// about what dragging means. Without it every slightly imprecise click would be
		// read as a drag and the click interactions would be impossible to perform.
		float MouseX = 0.0f;
		float MouseY = 0.0f;
		if (!GetMousePosition(MouseX, MouseY)
			|| FVector2D::Distance(FVector2D(MouseX, MouseY), PressScreen) < DragThresholdPixels)
		{
			return;
		}

		bDragging = true;
		Tool->OnDragBegin(MakeToolContext());
	}

	Tool->OnDrag(MakeToolContext());
}

void ARoadBuildController::OnPrimaryReleased()
{
	const bool bWasDragging = bDragging;
	bPrimaryDown = false;
	bDragging = false;

	IBuildTool* Tool = GetActiveTool();
	if (Tool == nullptr || Target == nullptr)
	{
		return;
	}

	// A press that never travelled was a click after all.
	const FToolContext Context = MakeToolContext();
	if (bWasDragging)
	{
		Tool->OnDragEnd(Context);
	}
	else
	{
		Tool->OnClick(Context);
	}
}

bool ARoadBuildController::NodeWorldLocation(int32 NodeIndex, FVector& OutLocation) const
{
	if (Target == nullptr || Target->Network == nullptr)
	{
		return false;
	}

	const TArray<FRoadNode>& Nodes = Target->Network->GetNodes();
	if (!Nodes.IsValidIndex(NodeIndex) || !Nodes[NodeIndex].bAlive)
	{
		return false;
	}

	OutLocation = FVector(Nodes[NodeIndex].Position.X, Nodes[NodeIndex].Position.Y, Target->SurfaceZ);
	return true;
}

void ARoadBuildController::PlayerTick(float DeltaTime)
{
	Super::PlayerTick(DeltaTime);

	UpdateView(DeltaTime);

	if (Target == nullptr)
	{
		return;
	}

	// The deletion planner judges its rejoins by the same rules a click obeys, so the two
	// cannot drift apart. Pushed every frame so a details-panel edit takes effect at once.
	Target->PlacementLimits = MakePlacementLimits();

	UpdateDrag();

	if (IBuildTool* Tool = GetActiveTool())
	{
		Tool->Tick(MakeToolContext());
	}
}

void ARoadBuildController::OnCancelGesture()
{
	Session.CancelActiveGesture(MakeToolContext());
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
