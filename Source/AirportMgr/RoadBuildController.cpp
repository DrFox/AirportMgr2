#include "RoadBuildController.h"

#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Content/AirsideSettings.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadAgentActor.h"
#include "Present/RoadNetworkActor.h"
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

	// The key list is GENERATED from the same registry SetupInputComponent binds from, so
	// this banner cannot advertise a key that goes nowhere - which the old hand-written one
	// twice did.
	FString ToolKeys;
	const TConstArrayView<FToolRegistration> Registry = ToolRegistry();
	for (int32 Index = 0; Index < Registry.Num(); ++Index)
	{
		ToolKeys += FString::Printf(TEXT("%s%d %s"),
			Index == 0 ? TEXT("") : TEXT(", "),
			Index + 1,
			*Registry[Index].Name.ToString());
	}

	UE_LOG(LogRoadBuild, Log,
		TEXT("Road building ready on %s. Left click places and connects, right click ends the chain, "
			 "Backspace clears. %s, 7 lands an aircraft on the nearest runway. C watches the "
			 "aircraft, G toggles the guideline overlay. WASD pans, Q/E rotate, wheel zooms."),
		*Target->GetName(), *ToolKeys);
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

	ApplyViewLimits(TargetView);

	const double Right = (IsInputKeyDown(EKeys::D) ? 1.0 : 0.0) - (IsInputKeyDown(EKeys::A) ? 1.0 : 0.0);
	const double Forward = (IsInputKeyDown(EKeys::W) ? 1.0 : 0.0) - (IsInputKeyDown(EKeys::S) ? 1.0 : 0.0);
	const double Turn = (IsInputKeyDown(EKeys::E) ? 1.0 : 0.0) - (IsInputKeyDown(EKeys::Q) ? 1.0 : 0.0);

	TargetView.Pan(Right, Forward, PanRate, DeltaTime);
	TargetView.Rotate(Turn * RotateRate * DeltaTime);

	// Read as held keys rather than bound as actions: pan and rotate are continuous, and a
	// key binding fires once on press. The same reason WASD was never bound.
	CurrentView.EaseToward(TargetView, CameraLag, DeltaTime);

	// WATCHING AN AIRCRAFT takes over the camera entirely, and hands it straight back when
	// there is nothing to watch - a mode that stranded the view on a despawned aircraft would
	// leave the player looking at empty sky with no way to tell why.
	if (bWatchingAgent)
	{
		if (ARoadAgentActor* Agent = Target->GetNewestAgent())
		{
			const FVector At = Agent->GetActorLocation();
			const double Yaw = FMath::DegreesToRadians(Agent->GetActorRotation().Yaw);

			// Offsets are in the AIRCRAFT'S frame, so the view stays side-on through the
			// backtrack turn and the climb rather than being side-on only while it happens
			// to be pointing the way it started.
			const FVector Nose(FMath::Cos(Yaw), FMath::Sin(Yaw), 0.0);
			const FVector Wing(-FMath::Sin(Yaw), FMath::Cos(Yaw), 0.0);

			const FVector Eye = At
				+ Wing * WatchSideOffset
				- Nose * WatchBehindOffset
				+ FVector(0.0, 0.0, WatchHeight);

			// Aimed AT the aircraft rather than along a fixed rotation, so a rotation and a
			// climb stay framed instead of climbing out of shot.
			BuildCamera->SetActorLocationAndRotation(Eye, (At - Eye).Rotation());
			return;
		}

		bWatchingAgent = false;
		UE_LOG(LogRoadBuild, Log, TEXT("Nothing to watch: back to the build view."));
	}

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
	UE_LOG(LogRoadBuild, Log, TEXT("Camera: %s"),
		bWatchingAgent ? TEXT("watching the aircraft") : TEXT("build view"));
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
	// ONE BindKey PER REGISTRY ENTRY, all through the SAME handler - see SelectToolByKey.
	// Before issue #33 this was six separate BindKey calls to six separate SelectXTool
	// methods, a second list that had to agree with Tools by hand and once did not: the
	// route tool was appended there and this list was not touched, so the startup log
	// advertised "4 routes" while EKeys::Four went nowhere. Binding straight from the
	// registry makes that specific disagreement impossible to write.
	for (const FToolRegistration& Registration : ToolRegistry())
	{
		InputComponent->BindKey(Registration.Key, IE_Pressed, this, &ARoadBuildController::SelectToolByKey);
	}

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
	// ResolveDefaultAirframe. One FAirframe argument now, not four: issue #29 gave
	// DispatchArrival the same shape ResolveDefaultAirframe already returns.
	//
	// DispatchArrival has already logged which runway, which exit and which stand it chose,
	// or why it declined.
	Target->DispatchArrival(Cursor, UAirsideSettings::ResolveDefaultAirframe());
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
	if (RoadGeom::RayToPlaneZ(Origin, Direction, Target->SurfaceZ, Furthest, OutPosition))
	{
		return true;
	}

	// RayToPlaneZ only says which of its three guards refused, not why the caller cares -
	// so on refusal, and only then, re-derive which one it was to log something useful.
	// Cheap to redo: this path is never taken by a click that succeeded.
	if (bLogRefusals)
	{
		if (FMath::IsNearlyZero(Direction.Z))
		{
			UE_LOG(LogRoadBuild, Warning,
				TEXT("Click ignored: the view is edge-on to the road plane (dir.Z=%.6f)."), Direction.Z);
		}
		else
		{
			const double Distance = (Target->SurfaceZ - Origin.Z) / Direction.Z;
			if (Distance <= 0.0)
			{
				// Behind the camera. Without this a click on the sky lands on the plane's
				// mirror image, dropping a node far off in the opposite direction.
				UE_LOG(LogRoadBuild, Warning,
					TEXT("Click ignored: the road plane is behind the camera there (distance %.0f)."), Distance);
			}
			else
			{
				UE_LOG(LogRoadBuild, Warning,
					TEXT("Click ignored: the road plane is %.0f uu away there, past %.0f (%.1fx the view)."),
					Distance, Furthest, MaxPlaceDistanceFactor);
			}
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
	ApplyViewLimits(TargetView);
	TargetView.Zoom(ZoomStep, Notches);

	UE_LOG(LogRoadBuild, Log, TEXT("View %.0f uu out, %.1f degrees"),
		TargetView.Distance, TargetView.PitchDegrees());
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

	return Session.ResolveSnap(Target->Network, Cursor, Out);
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

FToolContext ARoadBuildController::MakeToolContext()
{
	// Resolved ONCE and carried, rather than each consumer asking again. The tool acts on
	// this and the overlay draws it, so what is highlighted and what happens cannot come
	// from two searches that merely tend to agree.
	//
	// Cursor is the RAW plane hit and the snap is carried BESIDE it - see
	// FToolContext::SetCursor. This used to assign Context.Snap.Position to Cursor, which
	// handed road-node semantics to every tool including the ones that place no road
	// nodes: hovering a guideline node moved the cursor onto the junction it sits beside,
	// and the route tool could never pick anything again.
	FVector2D PlaneHit;
	CursorOnRoadPlane(PlaneHit);

	// Pushed into the session EVERY call rather than cached: PickRadius and friends are
	// EditAnywhere, so a details-panel edit is meant to take effect on the very next click,
	// the same reason PlayerTick pushes Target->PlacementLimits every frame below.
	Session.Snap = MakeSnapSettings();
	Session.Limits = MakePlacementLimits();
	Session.ToolPickRadius = ToolPickRadius;

	return Session.MakeContext(Target, PlaneHit, IsRemoveHeld(),
		IsInputKeyDown(EKeys::LeftShift) || IsInputKeyDown(EKeys::RightShift));
}

void ARoadBuildController::SelectToolByKey(FKey Key)
{
	const TConstArrayView<FToolRegistration> Registry = ToolRegistry();
	for (int32 Index = 0; Index < Registry.Num(); ++Index)
	{
		if (Registry[Index].Key == Key)
		{
			Session.SelectTool(Index, MakeToolContext());
			if (IBuildTool* Active = Session.GetActiveTool())
			{
				UE_LOG(LogRoadBuild, Log, TEXT("Tool: %s"), *Active->GetDisplayName().ToString());
			}
			return;
		}
	}
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
