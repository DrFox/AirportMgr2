#include "RoadBuildController.h"

#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"

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

	if (bStartAbovePlane)
	{
		CreateBuildCamera();
	}

	UE_LOG(LogRoadBuild, Log,
		TEXT("Road building ready on %s. Left click places and connects, right click ends the chain, "
			 "Backspace clears. WASD pans, Q/E rotate, wheel zooms."),
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
	InputComponent->BindKey(EKeys::RightMouseButton, IE_Pressed, this, &ARoadBuildController::OnCancelChain);
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

ERoadPlacement ARoadBuildController::JudgePlacement(const FRoadSnapResult& Snap) const
{
	// Nothing to judge until a chain is in progress: the first click of a road places a
	// node and builds no segment, and there is no rule a lone node can break.
	FRoadNodeId From;
	if (PendingNode == INDEX_NONE || Target == nullptr || Target->Network == nullptr
		|| !Target->MakeLiveNodeId(PendingNode, From))
	{
		return ERoadPlacement::Valid;
	}

	return RoadPlacement::Validate(*Target->Network, From, Snap, MakePlacementLimits());
}

bool ARoadBuildController::GetPendingPlacement(ERoadPlacement& Out) const
{
	Out = LastPlacement;
	return bLastPlacementRelevant;
}

bool ARoadBuildController::IsDeleteHeld() const
{
	return IsInputKeyDown(EKeys::LeftControl) || IsInputKeyDown(EKeys::RightControl);
}

void ARoadBuildController::OnUndo()
{
	// Ctrl+Z, read as a chord rather than bound as one: BindKey has no modifier form, and
	// a bare Z would take back an edit every time the key was brushed.
	if (Target == nullptr || !IsDeleteHeld())
	{
		return;
	}

	const FString Label = Target->PeekUndoLabel();
	if (!Target->Undo())
	{
		UE_LOG(LogRoadBuild, Log, TEXT("Nothing to undo."));
		return;
	}

	// The node being chained from may not exist any more, and an index outlives the thing
	// it pointed at. Dropped rather than validated: after an undo the player's next click
	// should start fresh, not silently continue a road from before it.
	PendingNode = INDEX_NONE;
	bPendingNodeCreated = false;
	UE_LOG(LogRoadBuild, Log, TEXT("Undid: %s"), *Label);
}

void ARoadBuildController::OnRedo()
{
	if (Target == nullptr || !IsDeleteHeld())
	{
		return;
	}

	if (!Target->Redo())
	{
		UE_LOG(LogRoadBuild, Log, TEXT("Nothing to redo."));
		return;
	}

	PendingNode = INDEX_NONE;
}

void ARoadBuildController::OnDeleteClick(const FRoadSnapResult& Snap)
{
	switch (Snap.Kind)
	{
	case ERoadSnapKind::Node:
		if (Target->DeleteNode(Snap.Node.Index))
		{
			UE_LOG(LogRoadBuild, Log, TEXT("Deleted node %d and its roads"), Snap.Node.Index);
		}
		break;

	case ERoadSnapKind::Segment:
		if (Target->DeleteSegment(Snap.Segment.Index))
		{
			UE_LOG(LogRoadBuild, Log, TEXT("Deleted segment %d"), Snap.Segment.Index);
		}
		break;

	case ERoadSnapKind::Free:
	default:
		// Nothing under the cursor. Silent: a click on open ground meaning nothing is the
		// correct outcome, not a refusal worth reporting.
		return;
	}

	// A deletion can remove the node the chain was running from, and can invalidate the
	// preview's cached start. Both are dropped rather than checked.
	PendingNode = INDEX_NONE;
	bPendingNodeCreated = false;
	Target->RebuildMesh();
}

void ARoadBuildController::OnPrimaryPressed()
{
	bPrimaryDown = true;
	bDragging = false;
	DragNode = INDEX_NONE;

	float MouseX = 0.0f;
	float MouseY = 0.0f;
	GetMousePosition(MouseX, MouseY);
	PressScreen = FVector2D(MouseX, MouseY);

	// Only a press that lands on a node can become a drag. Ctrl is delete, and dragging
	// something you are about to remove would be nonsense.
	FRoadSnapResult Snap;
	if (!IsDeleteHeld() && ResolveSnap(Snap) && Snap.Kind == ERoadSnapKind::Node)
	{
		DragNode = Snap.Node.Index;
	}
}

void ARoadBuildController::UpdateDrag()
{
	if (!bPrimaryDown || DragNode == INDEX_NONE || Target == nullptr)
	{
		return;
	}

	if (!bDragging)
	{
		float MouseX = 0.0f;
		float MouseY = 0.0f;
		if (!GetMousePosition(MouseX, MouseY)
			|| FVector2D::Distance(FVector2D(MouseX, MouseY), PressScreen) < DragThresholdPixels)
		{
			return;
		}

		// One undo step for the whole drag, not one per frame - otherwise undo crawls back
		// along the path the mouse took.
		Target->BeginInteractiveEdit(TEXT("move node"));
		bDragging = true;
	}

	FVector2D Cursor;
	if (!CursorOnRoadPlane(Cursor))
	{
		return;
	}

	// A refused move simply does not happen, so the node stops following the cursor rather
	// than dragging a road shorter than the solver can trim.
	if (Target->MoveNode(DragNode, Cursor))
	{
		Target->RebuildMesh();
	}
}

void ARoadBuildController::OnPrimaryReleased()
{
	const bool bWasDragging = bDragging;

	bPrimaryDown = false;
	bDragging = false;
	const int32 Moved = DragNode;
	DragNode = INDEX_NONE;

	if (bWasDragging && Target != nullptr)
	{
		Target->EndInteractiveEdit(/*bKeep*/ true);
		Target->RebuildMesh();
		UE_LOG(LogRoadBuild, Log, TEXT("Moved node %d"), Moved);
		return;
	}

	// Never travelled: it was a click after all.
	OnBuildClick();
}

void ARoadBuildController::OnBuildClick()
{
	FRoadSnapResult Snap;
	if (Target == nullptr || !ResolveSnap(Snap, /*bLogRefusals*/ true))
	{
		return;
	}

	// The same click, read through the modifier. The snap chain has already decided WHAT is
	// under the cursor; delete only chooses what to do about it.
	if (IsDeleteHeld())
	{
		OnDeleteClick(Snap);
		return;
	}

	// Shift on a road inserts a node and stops there. A plain click splits too, but also
	// starts a chain from the new node - which is right when you are drawing a road INTO
	// an existing one, and a nuisance when all you wanted was somewhere to drag.
	if ((IsInputKeyDown(EKeys::LeftShift) || IsInputKeyDown(EKeys::RightShift))
		&& Snap.Kind == ERoadSnapKind::Segment)
	{
		const int32 Inserted = Target->SplitSegment(Snap.Segment.Index, Snap.Position);
		if (Inserted != INDEX_NONE)
		{
			Target->RebuildMesh();
			UE_LOG(LogRoadBuild, Log, TEXT("Inserted node %d into segment %d"),
				Inserted, Snap.Segment.Index);
		}
		return;
	}

	const FVector2D Where = Snap.Position;

	// Judged BEFORE anything is created. Validating after placing the node would leave a
	// stray node behind on every refused click - the road would not appear, but the graph
	// would still have grown.
	const ERoadPlacement Judgement = JudgePlacement(Snap);
	if (Judgement != ERoadPlacement::Valid)
	{
		UE_LOG(LogRoadBuild, Log, TEXT("Click refused: %s"), RoadPlacement::Describe(Judgement));
		return;
	}

	// One decision, taken by the chain, acted on here and drawn by the HUD. The three
	// outcomes are the whole difference between continuing a road, closing a junction on
	// an existing node, and cutting a new junction into a road already drawn.
	int32 Node = INDEX_NONE;
	bool bCreated = true;
	switch (Snap.Kind)
	{
	case ERoadSnapKind::Node:
		Node = Snap.Node.Index;

		// Already there. Cancelling must not take it away.
		bCreated = false;
		break;

	case ERoadSnapKind::Segment:
		// PendingNode survives this. The split kills a SEGMENT handle and recycles its
		// slot, and nodes live in a separate slot array that a split only ever appends
		// to - so the index being chained from still means the same node.
		Node = Target->SplitSegment(Snap.Segment.Index, Where);
		if (Node != INDEX_NONE)
		{
			UE_LOG(LogRoadBuild, Log, TEXT("Split segment %d at (%.0f, %.0f) into node %d"),
				Snap.Segment.Index, Where.X, Where.Y, Node);
		}
		break;

	case ERoadSnapKind::Free:
	default:
		Node = Target->PlaceNode(Where);
		break;
	}

	if (Node == INDEX_NONE)
	{
		return;
	}

	if (PendingNode != INDEX_NONE && PendingNode != Node)
	{
		if (!Target->ConnectNodes(PendingNode, Node))
		{
			// The facade already logged why. Drop the chain rather than leaving the
			// player clicking against a connection that will not form.
			PendingNode = INDEX_NONE;
			return;
		}

		// Log both endpoints' world positions, not just the segment's node indices. The
		// indices alone cannot show whether a click landed where it was aimed, which is
		// the one question a wrong-looking road actually raises.
		FVector FromWorld;
		NodeWorldLocation(PendingNode, FromWorld);
		UE_LOG(LogRoadBuild, Log, TEXT("Segment %d (%.0f, %.0f) -> %d (%.0f, %.0f), length %.0f"),
			PendingNode, FromWorld.X, FromWorld.Y, Node, Where.X, Where.Y,
			FVector2D::Distance(FVector2D(FromWorld.X, FromWorld.Y), Where));
	}
	else if (PendingNode == INDEX_NONE)
	{
		// A node on its own draws nothing: SolveAll skips a node with no incident
		// segments, so the first click of a chain would otherwise look like a no-op.
		// The preview draw is what makes it visible; say so in the log too.
		UE_LOG(LogRoadBuild, Log,
			TEXT("Started at node %d (%.0f, %.0f). Click again to run a segment to it."),
			Node, Where.X, Where.Y);
	}

	// Chain from the node just placed, so a road is drawn click by click rather than a
	// pair of clicks per segment.
	PendingNode = Node;
	bPendingNodeCreated = bCreated;
	Target->RebuildMesh();
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
	if (bDragging)
	{
		// The ghost previews a road a click would build. Mid-drag there is no such click,
		// and the road being reshaped is already on screen.
		Target->HideGhost();
		return;
	}

	FRoadSnapResult Snap;
	const bool bHaveSnap = ResolveSnap(Snap);

	// Judged every frame whether or not the ghost is drawn: the overlay reports the reason
	// a click will be refused, and that has to be true even with previews switched off.
	bLastPlacementRelevant = bHaveSnap && PendingNode != INDEX_NONE;
	LastPlacement = bHaveSnap ? JudgePlacement(Snap) : ERoadPlacement::Valid;

	// No ghost while a deletion is being aimed: the preview would be offering to build the
	// very thing the click is about to remove.
	if (!bDrawBuildPreview || !bLastPlacementRelevant || IsDeleteHeld())
	{
		Target->HideGhost();
		return;
	}

	// The ghost shows the segment even when it is illegal, coloured rather than withheld.
	// Hiding it would answer "why can I not build here" with nothing at all.
	Target->UpdateGhost(PendingNode, Snap, LastPlacement == ERoadPlacement::Valid);
}

void ARoadBuildController::OnCancelChain()
{
	// A chain that placed a node and drew nothing from it leaves that node behind with no
	// road on it. Removed here rather than swept later: it is this gesture that created it,
	// and this gesture that is being abandoned.
	//
	// Only if the chain created it, and only if it is still bare - a node that picked up a
	// segment is part of the network now, whoever made it.
	if (Target != nullptr && bPendingNodeCreated && PendingNode != INDEX_NONE
		&& Target->Network != nullptr
		&& Target->Network->GetNodes().IsValidIndex(PendingNode)
		&& Target->Network->GetNodes()[PendingNode].bAlive
		&& Target->Network->GetNodes()[PendingNode].Incident.Num() == 0)
	{
		if (Target->DeleteNode(PendingNode))
		{
			Target->RebuildMesh();
			UE_LOG(LogRoadBuild, Log, TEXT("Chain cancelled; removed the node it had dropped."));
		}
	}

	PendingNode = INDEX_NONE;
	bPendingNodeCreated = false;
	UE_LOG(LogRoadBuild, Log, TEXT("Chain ended; the next click starts a new road."));
}

void ARoadBuildController::OnClearNetwork()
{
	if (Target == nullptr)
	{
		return;
	}

	PendingNode = INDEX_NONE;
	Target->ClearNetwork();
	UE_LOG(LogRoadBuild, Log, TEXT("Network cleared."));
}
