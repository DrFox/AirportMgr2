#include "RoadBuildHUD.h"

#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Model/RoadNetwork.h"
#include "Entities/AircraftType.h"
#include "Entities/EntityDefinition.h"
#include "Model/RoadEntity.h"
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

	ARoadBuildController* Controller = GetBuildController();
	if (Controller == nullptr)
	{
		return;
	}

	const ARoadNetworkActor* Target = Controller->GetTarget();
	if (Target == nullptr)
	{
		return;
	}

	PlaneZ = Target->SurfaceZ;

	// The graph first, so the tool's intent overdraws it rather than hiding beneath it.
	if (bDrawNodes && Target->Network != nullptr)
	{
		DrawNodes(*Target);
	}

	if (bDrawStands && Target->Network != nullptr)
	{
		DrawStands(*Target);
	}

	// The tool describes what it would do; this class decides what that looks like. The
	// context it is asked with is the same one a click would act on, so the overlay and
	// the click cannot disagree.
	if (IBuildTool* Tool = Controller->GetActiveTool())
	{
		Tool->BuildPreview(Controller->MakeToolContext(), *this);

		if (bDrawToolName && GEngine != nullptr)
		{
			DrawText(Tool->GetDisplayName().ToString(), PendingColour, 24.0f, 24.0f,
				GEngine->GetSmallFont(), 1.3f);
		}
	}
}

ARoadBuildController* ARoadBuildHUD::GetBuildController() const
{
	return Cast<ARoadBuildController>(GetOwningPlayerController());
}

FLinearColor ARoadBuildHUD::StyleColour(EPreviewStyle Style) const
{
	switch (Style)
	{
	case EPreviewStyle::Snap:    return SnapColour;
	case EPreviewStyle::Doomed:  return DoomedColour;
	case EPreviewStyle::Heal:    return HealColour;
	case EPreviewStyle::Refused: return RefusedColour;
	case EPreviewStyle::Pending:
	default:                     return PendingColour;
	}
}

void ARoadBuildHUD::Marker(const FVector2D& At, EPreviewStyle Style)
{
	FVector2D Screen;
	if (!ProjectPlanePoint(At, PlaneZ, Screen))
	{
		return;
	}

	// A heavier ring than a node normally wears, so a marked one reads as marked rather
	// than merely recoloured.
	DrawRing(Screen, NodeRingRadius, StyleColour(Style), PreviewThickness);
	if (Style == EPreviewStyle::Doomed || Style == EPreviewStyle::Pending)
	{
		DrawRing(Screen, NodeRingRadius * 1.6f, StyleColour(Style), PreviewThickness);
	}
}

void ARoadBuildHUD::Line(const FVector2D& From, const FVector2D& To, EPreviewStyle Style)
{
	FVector2D ScreenA;
	FVector2D ScreenB;
	if (!ProjectPlanePoint(From, PlaneZ, ScreenA) || !ProjectPlanePoint(To, PlaneZ, ScreenB))
	{
		return;
	}

	DrawLine(
		static_cast<float>(ScreenA.X), static_cast<float>(ScreenA.Y),
		static_cast<float>(ScreenB.X), static_cast<float>(ScreenB.Y),
		StyleColour(Style), PreviewThickness);
}

void ARoadBuildHUD::CrossMark(const FVector2D& At, const FVector2D& Along, EPreviewStyle Style)
{
	FVector2D Screen;
	if (!ProjectPlanePoint(At, PlaneZ, Screen) || Along.IsNearlyZero())
	{
		return;
	}

	// The direction is taken from a point just BESIDE the mark rather than from the ends of
	// whatever it lies on, because a long road usually has an end off screen and
	// ProjectPlanePoint culls those - the mark would then vanish on exactly the roads most
	// worth marking.
	FVector2D Beside;
	if (!ProjectPlanePoint(At + FVector2D(-Along.Y, Along.X) * 100.0, PlaneZ, Beside))
	{
		return;
	}

	FVector2D Across = Beside - Screen;
	if (!Across.Normalize())
	{
		return;
	}

	const FLinearColor Colour = StyleColour(Style);
	DrawLine(
		static_cast<float>(Screen.X - Across.X * CrossMarkRadius),
		static_cast<float>(Screen.Y - Across.Y * CrossMarkRadius),
		static_cast<float>(Screen.X + Across.X * CrossMarkRadius),
		static_cast<float>(Screen.Y + Across.Y * CrossMarkRadius),
		Colour, PreviewThickness);
}

void ARoadBuildHUD::Label(const FVector2D& At, const FString& Text, EPreviewStyle Style)
{
	FVector2D Screen;
	if (!ProjectPlanePoint(At, PlaneZ, Screen) || GEngine == nullptr)
	{
		return;
	}

	DrawText(Text, StyleColour(Style),
		static_cast<float>(Screen.X) + NodeRingRadius * 1.8f,
		static_cast<float>(Screen.Y) + NodeRingRadius,
		GEngine->GetSmallFont());
}

void ARoadBuildHUD::DrawNodes(const ARoadNetworkActor& Target)
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

		// Degree is the whole point of drawing these: it is what separates a junction from
		// a straight-through node, and the pavement looks identical either way.
		const int32 Degree = Node.Incident.Num();
		const FLinearColor Colour = (Degree == 0) ? StubColour
			: (Degree >= 3) ? JunctionColour
			: EndColour;

		DrawRing(Screen, NodeRingRadius, Colour, NodeRingThickness);

		if (bDrawNodeIndices && GEngine != nullptr)
		{
			DrawText(FString::FromInt(Index), Colour,
				static_cast<float>(Screen.X) + NodeRingRadius + 3.0f,
				static_cast<float>(Screen.Y) - NodeRingRadius,
				GEngine->GetSmallFont());
		}
	}
}

void ARoadBuildHUD::DrawStands(const ARoadNetworkActor& Target)
{
	for (const FEntityInstance& Entity : Target.Network->GetEntities())
	{
		if (!Entity.bAlive)
		{
			continue;
		}

		FVector2D StopScreen;
		const bool bStopVisible = ProjectPlanePoint(Entity.Position, Target.SurfaceZ, StopScreen);
		if (bStopVisible)
		{
			DrawRing(StopScreen, NodeRingRadius, StandColour, NodeRingThickness);
			DrawRing(StopScreen, NodeRingRadius * 1.6f, StandColour, NodeRingThickness);
		}

		// The aircraft's plan extent, so the anchors have something to be read against and
		// the heading is unmistakable. A stand aimed 180 degrees out looks identical to a
		// correct one until something tries to taxi onto it.
		//
		// Dimensions come from the DEFINITION. An overlay carrying its own would be a
		// second opinion about how big an A320 is.
		const UAircraftType* Design =
			Entity.Definition != nullptr ? Entity.Definition->DesignAircraft.Get() : nullptr;
		if (Design != nullptr)
		{
			TArray<FVector2D> Outline;
			UAircraftType::BuildFootprintLines(Design->Footprint, Outline);

			const double Cos = FMath::Cos(Entity.Heading);
			const double Sin = FMath::Sin(Entity.Heading);

			for (int32 Index = 0; Index + 1 < Outline.Num(); Index += 2)
			{
				auto ToWorld = [&Entity, Cos, Sin](const FVector2D& Local)
				{
					return FVector2D(
						Entity.Position.X + Local.X * Cos - Local.Y * Sin,
						Entity.Position.Y + Local.X * Sin + Local.Y * Cos);
				};

				FVector2D FromScreen;
				FVector2D ToScreen;
				if (ProjectPlanePoint(ToWorld(Outline[Index]), Target.SurfaceZ, FromScreen)
					&& ProjectPlanePoint(ToWorld(Outline[Index + 1]), Target.SurfaceZ, ToScreen))
				{
					DrawLine(
						static_cast<float>(FromScreen.X), static_cast<float>(FromScreen.Y),
						static_cast<float>(ToScreen.X), static_cast<float>(ToScreen.Y),
						StandColour, NodeRingThickness);
				}
			}
		}

		// Where the design aircraft would need each service. Recomputed rather than stored,
		// because these belong to whatever is PARKED here - today the type the stand was
		// sized for, tomorrow whatever actually occupies it - and a stored copy would be a
		// claim about an aircraft that has not arrived.
		if (Design != nullptr)
		{
			const double Cos = FMath::Cos(Entity.Heading);
			const double Sin = FMath::Sin(Entity.Heading);

			for (const FEntityAnchor& Point : Design->ServicePoints)
			{
				const FVector2D World(
					Entity.Position.X + Point.LocalPosition.X * Cos - Point.LocalPosition.Y * Sin,
					Entity.Position.Y + Point.LocalPosition.X * Sin + Point.LocalPosition.Y * Cos);

				FVector2D Screen;
				if (ProjectPlanePoint(World, Target.SurfaceZ, Screen))
				{
					DrawRing(Screen, ServiceAnchorRadius * 0.7f, StandColour, NodeRingThickness);
				}
			}
		}

		// The stand's FIXTURES, read from the INSTANCE rather than recomputed from the
		// definition.
		// These are the guideline nodes vehicles will actually route to; drawing the
		// definition's local positions transformed again would be a second opinion about
		// where they are, and the two could disagree without anything reporting it.
		for (const FResolvedAnchor& Anchor : Entity.ResolvedAnchors)
		{
			const FGuidelineNode* Node = Target.Network->GetGuidelineNode(Anchor.Node);
			if (Node == nullptr)
			{
				continue;
			}

			FVector2D Screen;
			if (!ProjectPlanePoint(Node->Position, Target.SurfaceZ, Screen))
			{
				continue;
			}

			DrawRing(Screen, ServiceAnchorRadius, ServiceAnchorColour, NodeRingThickness);

			if (bDrawAnchorIds && GEngine != nullptr)
			{
				DrawText(Anchor.Id.ToString(), ServiceAnchorColour,
					static_cast<float>(Screen.X) + ServiceAnchorRadius + 3.0f,
					static_cast<float>(Screen.Y) - ServiceAnchorRadius,
					GEngine->GetSmallFont());
			}
		}
	}
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
	// Project writes literal zero when the clip W says the point is behind the camera, and
	// also leaves zero when there is no scene view to project through at all. Any other
	// value - however small - is a real depth. See the header for why `Z <= 0` is wrong.
	if (Projected.Z == 0.0)
	{
		return false;
	}

	// Cull off-screen points with a margin wide enough that a ring straddling the edge
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
