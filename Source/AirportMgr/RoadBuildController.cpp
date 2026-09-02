#include "RoadBuildController.h"

#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Tool/ApronDrawTool.h"
#include "Tool/RoadDrawTool.h"
#include "Tool/RouteTool.h"
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

	if (bStartAbovePlane)
	{
		CreateBuildCamera();
	}

	UE_LOG(LogRoadBuild, Log,
		TEXT("Road building ready on %s. Left click places and connects, right click ends the chain, "
			 "Backspace clears. 1 roads, 2 aprons, 3 stands, 4 routes. WASD pans, Q/E rotate, "
			 "wheel zooms."),
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

	ApplyViewLimits(TargetView);

	const double Right = (IsInputKeyDown(EKeys::D) ? 1.0 : 0.0) - (IsInputKeyDown(EKeys::A) ? 1.0 : 0.0);
	const double Forward = (IsInputKeyDown(EKeys::W) ? 1.0 : 0.0) - (IsInputKeyDown(EKeys::S) ? 1.0 : 0.0);
	const double Turn = (IsInputKeyDown(EKeys::E) ? 1.0 : 0.0) - (IsInputKeyDown(EKeys::Q) ? 1.0 : 0.0);

	TargetView.Pan(Right, Forward, PanRate, DeltaTime);
	TargetView.Rotate(Turn * RotateRate * DeltaTime);

	// Read as held keys rather than bound as actions: pan and rotate are continuous, and a
	// key binding fires once on press. The same reason WASD was never bound.
	CurrentView.EaseToward(TargetView, CameraLag, DeltaTime);

	BuildCamera->SetActorLocationAndRotation(
		CurrentView.CameraLocation(Target->SurfaceZ), CurrentView.CameraRotation());
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
	InputComponent->BindKey(EKeys::One, IE_Pressed, this, &ARoadBuildController::SelectRoadTool);
	InputComponent->BindKey(EKeys::Two, IE_Pressed, this, &ARoadBuildController::SelectApronTool);
	InputComponent->BindKey(EKeys::Three, IE_Pressed, this, &ARoadBuildController::SelectStandTool);
	InputComponent->BindKey(EKeys::BackSpace, IE_Pressed, this, &ARoadBuildController::OnClearNetwork);
	InputComponent->BindKey(EKeys::Z, IE_Pressed, this, &ARoadBuildController::OnUndo);
	InputComponent->BindKey(EKeys::Y, IE_Pressed, this, &ARoadBuildController::OnRedo);

	InputComponent->BindKey(EKeys::MouseScrollUp, IE_Pressed, this, &ARoadBuildController::ZoomIn);
	InputComponent->BindKey(EKeys::MouseScrollDown, IE_Pressed, this, &ARoadBuildController::ZoomOut);
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

bool ARoadBuildController::IsRemoveHeld() const
{
	return IsInputKeyDown(EKeys::LeftControl) || IsInputKeyDown(EKeys::RightControl);
}

FToolContext ARoadBuildController::MakeToolContext() const
{
	FToolContext Context;
	Context.Target = Target;
	Context.Limits = MakePlacementLimits();
	Context.SnapRadius = PickRadius;
	Context.bRemoveModifier = IsRemoveHeld();
	Context.bInsertModifier =
		IsInputKeyDown(EKeys::LeftShift) || IsInputKeyDown(EKeys::RightShift);

	// Resolved ONCE and carried, rather than each consumer asking again. The tool acts on
	// this and the overlay draws it, so what is highlighted and what happens cannot come
	// from two searches that merely tend to agree.
	ResolveSnap(Context.Snap);
	Context.Cursor = Context.Snap.Position;
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
