#include "RoadBuildHUD.h"

#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Present/RoadNetworkActor.h"
#include "RoadBuildController.h"

void ARoadBuildHUD::DrawHUD()
{
	Super::DrawHUD();

	if (Canvas == nullptr)
	{
		return;
	}

	const ARoadBuildController* Controller = GetBuildController();
	if (Controller == nullptr)
	{
		return;
	}

	const ARoadNetworkActor* Target = Controller->GetTarget();
	if (Target == nullptr)
	{
		return;
	}

	// The controller's own answer, not a second search of our own. The overlay and the
	// click have to agree about what the next click does, and the only way to guarantee
	// that is for there to be one decision rather than two that happen to match.
	FRoadSnapResult Snap;
	const bool bHaveSnap = Controller->ResolveSnap(Snap);

	// Network is null until the first node is placed. Rings need one; the marker does
	// not, and is worth more then than at any other time - it is the only thing on screen
	// showing where the first click will land.
	if (bDrawNodes && Target->Network != nullptr)
	{
		DrawNodes(*Target, Controller->GetPendingNode(), Snap);
	}

	if (bDrawCursor && bHaveSnap)
	{
		DrawSnapMarker(*Target, Snap);
	}
}

ARoadBuildController* ARoadBuildHUD::GetBuildController() const
{
	return Cast<ARoadBuildController>(GetOwningPlayerController());
}

void ARoadBuildHUD::DrawNodes(const ARoadNetworkActor& Target, int32 PendingNode, const FRoadSnapResult& Snap)
{
	const TArray<FRoadNode>& Nodes = Target.Network->GetNodes();

	// Only a Node snap highlights a ring. A Segment snap is a cut between two nodes and
	// belongs to neither of them, so highlighting either would name the wrong thing.
	const int32 SnapTo = (Snap.Kind == ERoadSnapKind::Node) ? Snap.Node.Index : INDEX_NONE;

	for (int32 Index = 0; Index < Nodes.Num(); ++Index)
	{
		const FRoadNode& Node = Nodes[Index];
		if (!Node.bAlive)
		{
			continue;
		}

		FVector2D Screen;
		if (!ProjectPlanePoint(Node.Position, Target.SurfaceZ, Screen))
		{
			continue;
		}

		// Degree is the whole point of drawing these: it is what separates a junction
		// from a straight-through node, and the pavement looks identical either way.
		const int32 Degree = Node.Incident.Num();
		FLinearColor Colour = (Degree == 0) ? StubColour
			: (Degree >= 3) ? JunctionColour
			: EndColour;

		// Pending outranks snap. When the cursor sits over the pending node itself the
		// click does nothing at all - PendingNode == Node creates no segment - so
		// colouring it as a snap target would promise a connection that cannot happen.
		if (Index == SnapTo)
		{
			Colour = SnapColour;
		}
		if (Index == PendingNode)
		{
			Colour = PendingColour;
		}

		DrawRing(Screen, NodeRingRadius, Colour, NodeRingThickness);

		if (Index == PendingNode)
		{
			DrawRing(Screen, NodeRingRadius * 0.5f, Colour, NodeRingThickness);
		}

		if (bDrawNodeIndices && GEngine != nullptr)
		{
			DrawText(FString::FromInt(Index), Colour,
				static_cast<float>(Screen.X) + NodeRingRadius + 3.0f,
				static_cast<float>(Screen.Y) - NodeRingRadius,
				GEngine->GetSmallFont());
		}
	}
}

void ARoadBuildHUD::DrawSnapMarker(const ARoadNetworkActor& Target, const FRoadSnapResult& Snap)
{
	FVector2D Screen;
	if (!ProjectPlanePoint(Snap.Position, Target.SurfaceZ, Screen))
	{
		return;
	}

	// Drawn at the SNAPPED position. Under the top-down orthographic view a Free snap
	// puts this exactly under the mouse pointer and it says nothing; the moment a rule
	// claims the cursor the marker jumps to where the click will really land, and the gap
	// between pointer and marker is the snap made visible.
	const FLinearColor Colour =
		(Snap.Kind == ERoadSnapKind::Node) ? SnapColour :
		(Snap.Kind == ERoadSnapKind::Segment) ? SplitColour : FLinearColor::White;

	const float X = static_cast<float>(Screen.X);
	const float Y = static_cast<float>(Screen.Y);
	DrawLine(X - CursorSize, Y, X + CursorSize, Y, Colour, 1.0f);
	DrawLine(X, Y - CursorSize, X, Y + CursorSize, Colour, 1.0f);

	if (Snap.Kind != ERoadSnapKind::Segment || Target.Network == nullptr)
	{
		return;
	}

	const FRoadSegment* Segment = Target.Network->GetSegment(Snap.Segment);
	const FRoadNode* EndA = Segment != nullptr ? Target.Network->GetNode(Segment->A) : nullptr;
	const FRoadNode* EndB = Segment != nullptr ? Target.Network->GetNode(Segment->B) : nullptr;
	if (EndA == nullptr || EndB == nullptr)
	{
		return;
	}

	const FVector2D Along = (EndB->Position - EndA->Position).GetSafeNormal();
	if (Along.IsNearlyZero())
	{
		return;
	}

	// The tick's direction is taken from a point just beside the split rather than from
	// the segment's endpoints, because a long road usually has at least one end off
	// screen and ProjectPlanePoint culls those - the cut mark would then vanish on
	// exactly the segments most worth cutting.
	FVector2D Beside;
	if (!ProjectPlanePoint(Snap.Position + FVector2D(-Along.Y, Along.X) * 100.0, Target.SurfaceZ, Beside))
	{
		return;
	}

	FVector2D Across = Beside - Screen;
	if (!Across.Normalize())
	{
		return;
	}

	// Sized in pixels off the ring radius, so the cut mark reads at the same weight as
	// the nodes it will become one of.
	const float Half = NodeRingRadius;
	DrawLine(
		X - static_cast<float>(Across.X) * Half, Y - static_cast<float>(Across.Y) * Half,
		X + static_cast<float>(Across.X) * Half, Y + static_cast<float>(Across.Y) * Half,
		Colour, NodeRingThickness);
}

void ARoadBuildHUD::DrawRing(const FVector2D& Centre, float Radius, const FLinearColor& Colour, float Thickness)
{
	const int32 Sides = FMath::Clamp(NodeRingSides, 3, 64);
	const double Step = 2.0 * UE_DOUBLE_PI / Sides;

	FVector2D Previous(Centre.X + Radius, Centre.Y);
	for (int32 Side = 1; Side <= Sides; ++Side)
	{
		const double Angle = Step * Side;
		const FVector2D Point(
			Centre.X + Radius * FMath::Cos(Angle),
			Centre.Y + Radius * FMath::Sin(Angle));

		DrawLine(
			static_cast<float>(Previous.X), static_cast<float>(Previous.Y),
			static_cast<float>(Point.X), static_cast<float>(Point.Y),
			Colour, Thickness);

		Previous = Point;
	}
}

bool ARoadBuildHUD::ProjectPlanePoint(const FVector2D& Where, double SurfaceZ, FVector2D& OutScreen) const
{
	if (Canvas == nullptr)
	{
		return false;
	}

	const FVector Projected = Project(FVector(Where.X, Where.Y, SurfaceZ), /*bClampToZeroPlane*/ true);

	// An exact comparison, and deliberately so: this is a sentinel, not a measurement.
	// Project writes literal zero when the clip W says the point is behind the camera,
	// and also leaves zero when there is no scene view to project through at all. Any
	// other value - however small - is a real depth. See the header for why an ortho
	// view makes `Z <= 0` the wrong test.
	if (Projected.Z == 0.0)
	{
		return false;
	}

	// Cull off-screen nodes with a margin wide enough that a ring straddling the edge
	// still draws its visible half.
	constexpr double Margin = 64.0;
	if (Projected.X < -Margin || Projected.Y < -Margin
		|| Projected.X > Canvas->ClipX + Margin || Projected.Y > Canvas->ClipY + Margin)
	{
		return false;
	}

	OutScreen = FVector2D(Projected.X, Projected.Y);
	return true;
}
