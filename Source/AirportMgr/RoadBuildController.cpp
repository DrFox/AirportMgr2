#include "RoadBuildController.h"

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
	APawn* Possessed = GetPawn();
	if (Possessed == nullptr || Target == nullptr)
	{
		return;
	}

	const FVector Where = Possessed->GetActorLocation();
	Possessed->SetActorLocation(FVector(Where.X, Where.Y, Target->SurfaceZ + StartHeight));

	// Steeply down, but NOT straight down: at +/-90 pitch the rotation hits gimbal lock
	// and gets silently renormalised to something else entirely, which reads as the
	// camera ignoring this call.
	SetControlRotation(FRotator(-70.0, 0.0, 0.0));
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

bool ARoadBuildController::CursorOnRoadPlane(FVector2D& OutPosition) const
{
	if (Target == nullptr)
	{
		return false;
	}

	FVector Origin;
	FVector Direction;
	if (!DeprojectMousePositionToWorld(Origin, Direction))
	{
		return false;
	}

	// Parallel to the plane: no intersection to find.
	if (FMath::IsNearlyZero(Direction.Z))
	{
		return false;
	}

	const double Distance = (Target->SurfaceZ - Origin.Z) / Direction.Z;

	// Behind the camera. Without this a click on the sky lands on the plane's mirror
	// image, dropping a node far off in the opposite direction.
	if (Distance <= 0.0)
	{
		return false;
	}

	// Near the horizon the ray is almost parallel to the plane and this distance runs
	// away, so a click a few pixels too high lands kilometres out. Refuse rather than
	// place, and say so: silently building a road the size of a county is worse.
	if (Distance > MaxPlaceDistance)
	{
		UE_LOG(LogRoadBuild, Log,
			TEXT("Click ignored: the road plane is %.0f uu away there, past MaxPlaceDistance of %.0f. "
				 "Aim closer to the ground, or fly higher."),
			Distance, MaxPlaceDistance);
		return false;
	}

	OutPosition = FVector2D(
		Origin.X + Direction.X * Distance,
		Origin.Y + Direction.Y * Distance);
	return true;
}

void ARoadBuildController::OnBuildClick()
{
	FVector2D Where;
	if (Target == nullptr || !CursorOnRoadPlane(Where))
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

		UE_LOG(LogRoadBuild, Log, TEXT("Segment %d -> %d"), PendingNode, Node);
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

	if (!bDrawBuildPreview || Target == nullptr)
	{
		return;
	}

	// Thickness is in world units, so these are sized against the road rather than the
	// screen: a marker about a quarter of the default road's width, and a band thin
	// enough not to hide the surface under it. Single digits would be sub-pixel here -
	// indistinguishable from nothing being drawn at all.
	constexpr double MarkerRadius = 200.0;
	constexpr float LineThickness = 40.0f;

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
