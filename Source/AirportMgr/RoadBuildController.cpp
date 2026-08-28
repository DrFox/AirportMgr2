#include "RoadBuildController.h"

#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "DrawDebugHelpers.h"
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
		MoveViewAbovePlane();
	}

	UE_LOG(LogRoadBuild, Log,
		TEXT("Road building ready on %s. Left click places and connects, right click ends the chain, "
			 "Backspace clears."),
		*Target->GetName());
}

void ARoadBuildController::MoveViewAbovePlane()
{
	if (Target == nullptr || GetWorld() == nullptr)
	{
		return;
	}

	const FVector Above(0.0, 0.0, Target->SurfaceZ + StartHeight);

	FActorSpawnParameters Params;
	Params.ObjectFlags |= RF_Transient;
	BuildCamera = GetWorld()->SpawnActor<ACameraActor>(Above, FRotator(-90.0, 0.0, 0.0), Params);
	if (BuildCamera == nullptr)
	{
		return;
	}

	// Straight down is set on the camera actor's own transform, not through
	// SetControlRotation: control rotation near +/-90 pitch hits gimbal lock and is
	// silently renormalised to something else, which reads as the camera ignoring you.
	UCameraComponent* Camera = BuildCamera->GetCameraComponent();
	Camera->SetProjectionMode(ECameraProjectionMode::Orthographic);
	Camera->SetOrthoWidth(static_cast<float>(ViewWidth));

	// Viewing through a camera actor also takes the view away from the pawn, so the
	// pawn's mouse-look stops fighting the cursor for the same input.
	SetViewTarget(BuildCamera);

	UE_LOG(LogRoadBuild, Log,
		TEXT("Top-down orthographic view: %.0f uu across, %.0f uu up. Screen maps 1:1 to ground."),
		ViewWidth, Target->SurfaceZ + StartHeight);
}

void ARoadBuildController::PanView(float DeltaTime)
{
	if (BuildCamera == nullptr)
	{
		return;
	}

	// Screen up is +Y in world here, because the camera looks straight down its own
	// -Z with no yaw: panning is a plain XY translation, no basis vectors needed.
	const double Right = (IsInputKeyDown(EKeys::D) ? 1.0 : 0.0) - (IsInputKeyDown(EKeys::A) ? 1.0 : 0.0);
	const double Forward = (IsInputKeyDown(EKeys::W) ? 1.0 : 0.0) - (IsInputKeyDown(EKeys::S) ? 1.0 : 0.0);

	if (Right == 0.0 && Forward == 0.0)
	{
		return;
	}

	const FVector Where = BuildCamera->GetActorLocation();
	BuildCamera->SetActorLocation(FVector(
		Where.X + Forward * PanSpeed * DeltaTime,
		Where.Y + Right * PanSpeed * DeltaTime,
		Where.Z));
}

void ARoadBuildController::SetupInputComponent()
{
	Super::SetupInputComponent();

	// Bound as raw keys rather than through Enhanced Input: the mappings would need
	// InputAction and InputMappingContext content assets, and this driver is meant to
	// work the moment the module compiles, with nothing to author first.
	InputComponent->BindKey(EKeys::LeftMouseButton, IE_Pressed, this, &ARoadBuildController::OnBuildClick);
	InputComponent->BindKey(EKeys::RightMouseButton, IE_Pressed, this, &ARoadBuildController::OnCancelChain);
	InputComponent->BindKey(EKeys::BackSpace, IE_Pressed, this, &ARoadBuildController::OnClearNetwork);
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

	// The horizon guards below only make sense for a perspective view, where the ray
	// starts at the eye and a near-horizontal ray runs away to nothing. Under the
	// orthographic build camera the deprojected origin sits on the near plane rather
	// than at the camera, so that plane can be behind or far above the road and the
	// "distance" carries no information about where the click landed - the intersection
	// is exact either way. Applying them there silently threw away good clicks in the
	// middle of the screen, which is the whole reason a road only appeared sometimes.
	const bool bOrthographic = BuildCamera != nullptr
		&& BuildCamera->GetCameraComponent()->ProjectionMode == ECameraProjectionMode::Orthographic;
	if (bOrthographic)
	{
		return true;
	}

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
	// away, so a click a few pixels too high lands kilometres out.
	if (Distance > MaxPlaceDistance)
	{
		if (bLogRefusals)
		{
			UE_LOG(LogRoadBuild, Warning,
				TEXT("Click ignored: the road plane is %.0f uu away there, past MaxPlaceDistance of %.0f."),
				Distance, MaxPlaceDistance);
		}
		return false;
	}

	return true;
}

void ARoadBuildController::OnBuildClick()
{
	FVector2D Where;
	if (Target == nullptr || !CursorOnRoadPlane(Where, /*bLogRefusals*/ true))
	{
		return;
	}

	// Reuse a node already under the cursor so a click can close a junction. Without
	// this every click would start a road disconnected from the last one, and the
	// junction geometry - the whole point of the mesh slice - would be unreachable.
	const int32 Existing = Target->FindNodeNear(Where, PickRadius);
	const int32 Node = (Existing != INDEX_NONE) ? Existing : Target->PlaceNode(Where);

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

	PanView(DeltaTime);

	if (!bDrawBuildPreview || Target == nullptr)
	{
		return;
	}

	// Thickness is in world units, so these are sized against the road rather than the
	// screen: a marker about a quarter of the default road's width, and a band thin
	// enough not to hide the surface under it. Single digits would be sub-pixel here -
	// indistinguishable from nothing being drawn at all.
	constexpr double MarkerRadius = 50.0;
	constexpr float LineThickness = 10.0f;

	FVector2D Cursor;
	const bool bCursorOnPlane = CursorOnRoadPlane(Cursor);
	const FVector CursorWorld(Cursor.X, Cursor.Y, Target->SurfaceZ);

	// Where the next click would land, and whether it would reuse a node rather than
	// add one - the difference between continuing a road and closing a junction.
	if (bCursorOnPlane)
	{
		const bool bWouldSnap = Target->FindNodeNear(Cursor, PickRadius) != INDEX_NONE;
		DrawDebugSphere(GetWorld(), CursorWorld, MarkerRadius, 12,
			bWouldSnap ? FColor::Yellow : FColor::White, false, -1.0f, 0, LineThickness * 0.25f);
	}

	FVector PendingWorld;
	if (NodeWorldLocation(PendingNode, PendingWorld))
	{
		DrawDebugSphere(GetWorld(), PendingWorld, MarkerRadius * 1.5, 12,
			FColor::Green, false, -1.0f, 0, LineThickness * 0.25f);

		// The rubber band: the segment this click would create.
		if (bCursorOnPlane)
		{
			DrawDebugLine(GetWorld(), PendingWorld, CursorWorld,
				FColor::Green, false, -1.0f, 0, LineThickness);
		}
	}
}

void ARoadBuildController::OnCancelChain()
{
	PendingNode = INDEX_NONE;
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
