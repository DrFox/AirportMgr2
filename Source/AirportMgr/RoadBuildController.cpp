#include "RoadBuildController.h"

#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/InputComponent.h"
#include "Content/AirsideSettings.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Tool/ApronDrawTool.h"
#include "Tool/RoadDrawTool.h"
#include "Tool/GuidelineDrawTool.h"
#include "Tool/RouteTool.h"
#include "Present/RoadAgentActor.h"
#include "Tool/RunwayTool.h"
#include "Tool/StandPlaceTool.h"

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

	// Key order: 1 is the first. Strategy, so adding a tool is appending to a list.
	Tools.Add(MakeUnique<FRoadDrawTool>());
	Tools.Add(MakeUnique<FApronDrawTool>());
	Tools.Add(MakeUnique<FStandPlaceTool>());
	Tools.Add(MakeUnique<FRouteTool>());
	Tools.Add(MakeUnique<FGuidelineDrawTool>());
	Tools.Add(MakeUnique<FRunwayTool>());

	if (Tools.Num() != ToolKeyCount)
	{
		UE_LOG(LogRoadBuild, Error,
			TEXT("%d tools but %d number keys bound. A tool with no key is unreachable and "
				 "a key with no tool does nothing - see ARoadBuildController::ToolKeyCount."),
			Tools.Num(), ToolKeyCount);
	}

	if (bStartAbovePlane)
	{
		CreateBuildCamera();
	}

	UE_LOG(LogRoadBuild, Log,
		TEXT("Road building ready on %s. Left click places and connects, right click ends the chain, "
			 "Backspace clears. 1 roads, 2 aprons, 3 stands, 4 routes, 5 guideline links, 6 runway, "
			 "7 lands an aircraft on the nearest runway. C orbits the aircraft, G toggles the "
			 "guideline overlay. WASD pans, Q/E rotate, wheel zooms - while building or watching."),
		*Target->GetName());
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

	// Numbered tools rather than a third modifier on one button. Drawing a polygon is
	// inherently multi-click, so it cannot ride a modifier the way delete and insert do.
	//
	// ONE BINDING PER ENTRY IN Tools, and they are two lists that must agree. The route
	// tool was appended to Tools and this line was not written, so the startup log
	// advertised "4 routes" while EKeys::Four went nowhere - the log was the only thing
	// claiming the binding existed, and it was wrong.
	InputComponent->BindKey(EKeys::One, IE_Pressed, this, &ARoadBuildController::SelectRoadTool);
	InputComponent->BindKey(EKeys::Two, IE_Pressed, this, &ARoadBuildController::SelectApronTool);
	InputComponent->BindKey(EKeys::Three, IE_Pressed, this, &ARoadBuildController::SelectStandTool);
	InputComponent->BindKey(EKeys::Four, IE_Pressed, this, &ARoadBuildController::SelectRouteTool);
	InputComponent->BindKey(EKeys::Five, IE_Pressed, this, &ARoadBuildController::SelectGuidelineTool);
	InputComponent->BindKey(EKeys::Six, IE_Pressed, this, &ARoadBuildController::SelectRunwayTool);

	// AN ACTION, NOT A TOOL, so it is bound here and appears in no tool list. The banner above
	// is updated in the same breath: this project has twice advertised a key that was never
	// bound, and the log was the only thing claiming the binding existed.
	InputComponent->BindKey(EKeys::Seven, IE_Pressed, this, &ARoadBuildController::OnLandAircraft);
	InputComponent->BindKey(EKeys::G, IE_Pressed, this, &ARoadBuildController::OnToggleGuidelines);
	InputComponent->BindKey(EKeys::C, IE_Pressed, this, &ARoadBuildController::ToggleWatchAgent);
	InputComponent->BindKey(EKeys::BackSpace, IE_Pressed, this, &ARoadBuildController::OnClearNetwork);
	InputComponent->BindKey(EKeys::Z, IE_Pressed, this, &ARoadBuildController::OnUndo);
	InputComponent->BindKey(EKeys::Y, IE_Pressed, this, &ARoadBuildController::OnRedo);

	InputComponent->BindKey(EKeys::MouseScrollUp, IE_Pressed, this, &ARoadBuildController::ZoomIn);
	InputComponent->BindKey(EKeys::MouseScrollDown, IE_Pressed, this, &ARoadBuildController::ZoomOut);
}

void ARoadBuildController::OnLandAircraft()
{
	if (Target == nullptr)
	{
		return;
	}

	FVector2D Cursor;
	if (!CursorOnRoadPlane(Cursor))
	{
		return;
	}

	// The SAME resolver FRouteTool falls back to, for the same reason: an aircraft that
	// approached as one airframe and taxied as another would be two different aircraft
	// depending on which phase you were watching - see UAirsideSettings::
	// ResolveDefaultAirframe. Unpacked into four here rather than one FAirframe argument
	// because DispatchArrival still takes the four separately (issue #29's job to collapse).
	//
	// DispatchArrival has already logged which runway, which exit and which stand it chose,
	// or why it declined.
	const FAirframe Default = UAirsideSettings::ResolveDefaultAirframe();
	Target->DispatchArrival(Cursor, Default.Ground, Default.Climb, Default.Approach, Default.Engine);
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

	// Parallel to the plane: no intersection to find.
	if (FMath::IsNearlyZero(Direction.Z))
	{
		if (bLogRefusals)
		{
			UE_LOG(LogRoadBuild, Warning,
				TEXT("Click ignored: the view is edge-on to the road plane (dir.Z=%.6f)."), Direction.Z);
		}
		return false;
	}

	const double Distance = (Target->SurfaceZ - Origin.Z) / Direction.Z;

	OutPosition = FVector2D(
		Origin.X + Direction.X * Distance,
		Origin.Y + Direction.Y * Distance);

	// These guards were skipped while the build camera was orthographic, and skipping them
	// was what stopped good clicks vanishing: under an orthographic projection the
	// deprojected origin sits on the near plane rather than at the camera, so the
	// ray/plane distance carries no information about where the click landed. The view is
	// perspective now and they are live and necessary again - if an orthographic mode ever
	// returns, it must exempt itself from both of them.

	// Behind the camera. Without this a click on the sky lands on the plane's mirror
	// image, dropping a node far off in the opposite direction.
	if (Distance <= 0.0)
	{
		if (bLogRefusals)
		{
			UE_LOG(LogRoadBuild, Warning,
				TEXT("Click ignored: the road plane is behind the camera there (distance %.0f)."), Distance);
		}
		return false;
	}

	// Near the horizon the ray is almost parallel to the plane and this distance runs
	// away, so a click a few pixels too high lands kilometres out. Measured against the
	// current view distance rather than a fixed number, because the view spans a
	// hundredfold range and no single cap suits both ends of it.
	const double Furthest = MaxPlaceDistanceFactor * CurrentView.Distance;
	if (Distance > Furthest)
	{
		if (bLogRefusals)
		{
			UE_LOG(LogRoadBuild, Warning,
				TEXT("Click ignored: the road plane is %.0f uu away there, past %.0f (%.1fx the view)."),
				Distance, Furthest, MaxPlaceDistanceFactor);
		}
		return false;
	}

	return true;
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

	// Before the first node exists there is no network to search - the facade builds one
	// lazily inside PlaceNode. Free at the cursor is the right answer here, not a
	// refusal: treating a missing network as failure would make the first click of every
	// session do nothing at all.
	Out = FRoadSnapResult();
	Out.Position = Cursor;

	if (Target->Network != nullptr)
	{
		Out = SnapChain.Resolve(*Target->Network, Cursor, MakeSnapSettings());
	}
	return true;
}

FRoadPlacementLimits ARoadBuildController::MakePlacementLimits() const
{
	FRoadPlacementLimits Limits;
	Limits.MinSegmentLength = MinSegmentLength;
	Limits.MinTurnDegrees = MinTurnDegrees;
	return Limits;
}

IBuildTool* ARoadBuildController::GetActiveTool() const
{
	return Tools.IsValidIndex(ActiveTool) ? Tools[ActiveTool].Get() : nullptr;
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
	FToolContext Context;
	Context.Target = Target;
	Context.Limits = MakePlacementLimits();
	Context.SnapRadius = ToolPickRadius;
	Context.bRemoveModifier = IsRemoveHeld();
	Context.bInsertModifier =
		IsInputKeyDown(EKeys::LeftShift) || IsInputKeyDown(EKeys::RightShift);

	// Resolved ONCE and carried, rather than each consumer asking again. The tool acts on
	// this and the overlay draws it, so what is highlighted and what happens cannot come
	// from two searches that merely tend to agree.
	//
	// Cursor is the RAW plane hit and the snap is carried BESIDE it - see
	// FToolContext::SetCursor. This used to assign Context.Snap.Position to Cursor, which
	// handed road-node semantics to every tool including the ones that place no road
	// nodes: hovering a guideline node moved the cursor onto the junction it sits beside,
	// and the route tool could never pick anything again.
	FRoadSnapResult Snapped;
	ResolveSnap(Snapped);

	FVector2D PlaneHit = Snapped.Position;
	CursorOnRoadPlane(PlaneHit);

	Context.SetCursor(PlaneHit, Snapped);
	return Context;
}

void ARoadBuildController::SelectTool(int32 Index)
{
	if (!Tools.IsValidIndex(Index) || Index == ActiveTool)
	{
		return;
	}

	// The outgoing tool abandons whatever it had part-drawn. Left alone it would reappear
	// on the next selection as a chain the player started minutes ago and has forgotten.
	if (IBuildTool* Outgoing = GetActiveTool())
	{
		Outgoing->OnDeactivate(MakeToolContext());
	}

	ActiveTool = Index;
	UE_LOG(LogRoadBuild, Log, TEXT("Tool: %s"), *Tools[ActiveTool]->GetDisplayName().ToString());
}

void ARoadBuildController::OnUndo()
{
	// Ctrl+Z, read as a chord rather than bound as one: BindKey has no modifier form, and
	// a bare Z would take back an edit every time the key was brushed.
	if (Target == nullptr || !IsRemoveHeld())
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
	if (Target == nullptr || !IsRemoveHeld())
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
	if (IBuildTool* Tool = GetActiveTool())
	{
		Tool->OnCancel(MakeToolContext());
	}
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
