#include "RoadBuildHUD.h"

#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNode.h"
#include "Present/RoadNetworkActor.h"
#include "RoadBuildController.h"
#include "Tool/GraphOverlay.h"
#include "Tool/GuidelineOverlay.h"
#include "Tool/PreviewPalette.h"

ARoadBuildHUD::ARoadBuildHUD()
{
	// Seeded from the one canonical table instead of typed here - see PreviewPalette.h.
	// Assigning in the constructor rather than as a member-initialiser still leaves this a
	// designer-overridable default on the CDO; only where the literal values LIVE has moved.
	// Explicit list, not a reflection-based enumeration of EPreviewStyle, matching this
	// project's other registries (e.g. BuildActionsTest reads EActionSection the same way) -
	// FRoadBuildHUDLooksTest is what catches a style left out of this list.
	for (const EPreviewStyle Style : {
		EPreviewStyle::Pending, EPreviewStyle::Snap, EPreviewStyle::Doomed, EPreviewStyle::Heal,
		EPreviewStyle::Refused, EPreviewStyle::Guideline, EPreviewStyle::Route,
		EPreviewStyle::RunwayHoldingPosition, EPreviewStyle::IntermediateHoldingPosition,
		EPreviewStyle::Hover, EPreviewStyle::Selected, EPreviewStyle::NodeStub,
		EPreviewStyle::NodeThrough, EPreviewStyle::NodeJunction, EPreviewStyle::StandPose,
		EPreviewStyle::ServiceAnchor })
	{
		Looks.Add(Style, PreviewPalette::DefaultLook(Style));
	}
}

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
	//
	// Two calls, not one: bDrawNodes and bDrawStands are INDEPENDENT toggles, and
	// GraphOverlay::Describe would force them to rise and fall together. See GraphOverlay.h
	// for why the split exists and why RoadBuildEditorTool::DrawPersistentState - which has
	// no such toggle - calls Describe instead.
	if (bDrawNodes && Target->Network != nullptr)
	{
		GraphOverlay::DescribeNodes(*Target->Network, *this);
	}

	if (bDrawStands && Target->Network != nullptr)
	{
		GraphOverlay::DescribeStands(*Target->Network, *this);
	}

	// Text labels only - the rings above already drew the geometry. Kept as their own loops,
	// each gated on its own flag, rather than folded into GraphOverlay: the overlay is a
	// shared fact both the runtime and the editor draw, and index/id text is a HUD-only
	// debugging aid the editor viewport has no use for (PrimitiveDrawInterface draws no text
	// at all - see FViewportPreviewSink::Label).
	if (bDrawNodes && bDrawNodeIndices && Target->Network != nullptr)
	{
		DrawNodeIndices(*Target);
	}

	if (bDrawStands && bDrawAnchorIds && Target->Network != nullptr)
	{
		DrawAnchorIds(*Target);
	}

	// The routing graph, under EVERY tool rather than only the route tool. Drawn BEFORE the
	// tool's own preview so the gesture sits on top of the context instead of under it.
	//
	// Not a tool's job: it is true whatever the gesture, and while it lived inside
	// the old route tool the graph you were building for was invisible while you built it.
	if (Controller->bShowGuidelines && Target->Network != nullptr)
	{
		GuidelineOverlay::Draw(*Target->Network, *this);
	}

	// The tool describes what it would do; this class decides what that looks like. The
	// context it is asked with is the same one a click would act on, so the overlay and
	// the click cannot disagree.
	if (IBuildTool* Tool = Controller->GetActiveTool())
	{
		Tool->BuildPreview(Controller->MakeToolContext(), *this);

		// The tool name and the clock moved to UBuildBarWidget; this class draws only in
		// world space now.
	}
}

ARoadBuildController* ARoadBuildHUD::GetBuildController() const
{
	return Cast<ARoadBuildController>(GetOwningPlayerController());
}

const FPreviewLook& ARoadBuildHUD::LookFor(EPreviewStyle Style) const
{
	// No entry missing from a live style is a bug in the constructor's seeding list, not a
	// runtime possibility a caller should branch on - see PreviewPalette.h's FPreviewLook
	// comment and FRoadBuildHUDLooksTest, which is what actually catches a style left out.
	// checkNoEntry() is the same backstop StyleColour's switch used to give a style with no
	// case; worse than a compile error, but still better than silently drawing Pending's
	// look for a style nobody wrote a look for.
	if (const FPreviewLook* Look = Looks.Find(Style))
	{
		return *Look;
	}

	checkNoEntry();
	static const FPreviewLook Fallback;
	return Fallback;
}

void ARoadBuildHUD::Marker(const FVector2D& At, EPreviewStyle Style)
{
	FVector2D Screen;
	if (!ProjectPlanePoint(At, PlaneZ, Screen))
	{
		return;
	}

	const FPreviewLook& Look = LookFor(Style);
	const float Radius = NodeRingRadius * Look.RadiusScale;
	const float Thickness = PreviewThickness * Look.ThicknessScale;

	DrawRing(Screen, Radius, Look.Colour, Thickness);
	if (Look.bDoubleRing)
	{
		// A heavier second ring, so a marked one reads as marked rather than merely
		// recoloured - see FPreviewLook's own comment for which styles set this.
		DrawRing(Screen, NodeRingRadius * 1.6f, Look.Colour, PreviewThickness);
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

	// The route is what was asked for and the graph is what it ran over, so the route is
	// drawn heavier than everything and the graph lighter than everything - see
	// PreviewPalette::DefaultLook's Guideline/Route cases for the ThicknessScale numbers.
	const FPreviewLook& Look = LookFor(Style);
	const float Weight = PreviewThickness * Look.ThicknessScale;

	DrawLine(
		static_cast<float>(ScreenA.X), static_cast<float>(ScreenA.Y),
		static_cast<float>(ScreenB.X), static_cast<float>(ScreenB.Y),
		Look.Colour, Weight);
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

	const FLinearColor Colour = LookFor(Style).Colour;
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

	DrawText(Text, LookFor(Style).Colour,
		static_cast<float>(Screen.X) + NodeRingRadius * 1.8f,
		static_cast<float>(Screen.Y) + NodeRingRadius,
		GEngine->GetSmallFont());
}

void ARoadBuildHUD::DrawNodeIndices(const ARoadNetworkActor& Target)
{
	if (GEngine == nullptr)
	{
		return;
	}

	// The rings themselves come from GraphOverlay::Describe - see DrawHUD. Degree still
	// decides the colour here so the label reads the same as the ring it labels.
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

		const int32 Degree = Node.Incident.Num();
		const EPreviewStyle Style = (Degree == 0) ? EPreviewStyle::NodeStub
			: (Degree >= 3) ? EPreviewStyle::NodeJunction
			: EPreviewStyle::NodeThrough;
		const FLinearColor Colour = LookFor(Style).Colour;

		DrawText(FString::FromInt(Index), Colour,
			static_cast<float>(Screen.X) + NodeRingRadius + 3.0f,
			static_cast<float>(Screen.Y) - NodeRingRadius,
			GEngine->GetSmallFont());
	}
}

void ARoadBuildHUD::DrawAnchorIds(const ARoadNetworkActor& Target)
{
	if (GEngine == nullptr)
	{
		return;
	}

	// The anchor rings themselves come from GraphOverlay::Describe - see DrawHUD. This walks
	// the SAME ResolvedAnchors, read from the INSTANCE rather than recomputed from the
	// definition, for the id text alone.
	for (const FEntityInstance& Entity : Target.Network->GetEntities())
	{
		if (!Entity.bAlive)
		{
			continue;
		}

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

			// Offset by the anchor ring's actual radius so the id sits beside the ring
			// GraphOverlay just drew for it, not a separately-tuned distance from it.
			const FPreviewLook& AnchorLook = LookFor(EPreviewStyle::ServiceAnchor);
			const float AnchorRadius = NodeRingRadius * AnchorLook.RadiusScale;
			DrawText(Anchor.Id.ToString(), AnchorLook.Colour,
				static_cast<float>(Screen.X) + AnchorRadius + 3.0f,
				static_cast<float>(Screen.Y) - AnchorRadius,
				GEngine->GetSmallFont());
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
