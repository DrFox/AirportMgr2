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
	if (Target == nullptr || Target->Network == nullptr)
	{
		return;
	}

	// Resolved once and passed down rather than asked for twice: the rings and the
	// crosshair must agree about what the next click would do, and two independent
	// searches are two things that can disagree.
	FVector2D Cursor = FVector2D::ZeroVector;
	const bool bHaveCursor = Controller->CursorOnRoadPlane(Cursor);
	const int32 SnapTo = bHaveCursor
		? Target->FindNodeNear(Cursor, Controller->GetPickRadius())
		: INDEX_NONE;

	if (bDrawNodes)
	{
		DrawNodes(*Target, Controller->GetPendingNode(), SnapTo);
	}

	if (bDrawCursor && bHaveCursor)
	{
		DrawCursor(*Target, Cursor, SnapTo != INDEX_NONE);
	}
}

ARoadBuildController* ARoadBuildHUD::GetBuildController() const
{
	return Cast<ARoadBuildController>(GetOwningPlayerController());
}

void ARoadBuildHUD::DrawNodes(const ARoadNetworkActor& Target, int32 PendingNode, int32 SnapTo)
{
	const TArray<FRoadNode>& Nodes = Target.Network->GetNodes();

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

void ARoadBuildHUD::DrawCursor(const ARoadNetworkActor& Target, const FVector2D& Cursor, bool bWouldSnap)
{
	FVector2D Screen;
	if (!ProjectPlanePoint(Cursor, Target.SurfaceZ, Screen))
	{
		return;
	}

	// Where the click lands on the PLANE, which under a perspective view is not where the
	// mouse pointer is drawn. Under the top-down orthographic build view the two coincide,
	// and that agreement is itself worth being able to see.
	const FLinearColor Colour = bWouldSnap ? SnapColour : FLinearColor::White;
	const float X = static_cast<float>(Screen.X);
	const float Y = static_cast<float>(Screen.Y);

	DrawLine(X - CursorSize, Y, X + CursorSize, Y, Colour, 1.0f);
	DrawLine(X, Y - CursorSize, X, Y + CursorSize, Colour, 1.0f);
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
