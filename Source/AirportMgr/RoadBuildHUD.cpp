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
	// Seeded from the one canonical table instead of typed here - see PreviewPalette.h and
	// the header's comment on each of these UPROPERTYs. Assigning in the constructor rather
	// than as member-initialisers still leaves every one of them a designer-overridable
	// default on the CDO; only where the literal value LIVES has moved.
	StubColour = PreviewPalette::Default(EPreviewStyle::NodeStub);
	EndColour = PreviewPalette::Default(EPreviewStyle::NodeThrough);
	JunctionColour = PreviewPalette::Default(EPreviewStyle::NodeJunction);
	StandColour = PreviewPalette::Default(EPreviewStyle::StandPose);
	ServiceAnchorColour = PreviewPalette::Default(EPreviewStyle::ServiceAnchor);

	PendingColour = PreviewPalette::Default(EPreviewStyle::Pending);
	SnapColour = PreviewPalette::Default(EPreviewStyle::Snap);
	DoomedColour = PreviewPalette::Default(EPreviewStyle::Doomed);
	HealColour = PreviewPalette::Default(EPreviewStyle::Heal);
	RefusedColour = PreviewPalette::Default(EPreviewStyle::Refused);
	GuidelineColour = PreviewPalette::Default(EPreviewStyle::Guideline);
	RouteColour = PreviewPalette::Default(EPreviewStyle::Route);
	HoverColour = PreviewPalette::Default(EPreviewStyle::Hover);
	SelectedColour = PreviewPalette::Default(EPreviewStyle::Selected);
	RunwayHoldingPositionColour = PreviewPalette::Default(EPreviewStyle::RunwayHoldingPosition);
	IntermediateHoldingPositionColour = PreviewPalette::Default(EPreviewStyle::IntermediateHoldingPosition);
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
	// One call for both nodes and stands - GraphOverlay::Describe draws the committed graph
	// exactly as GuidelineOverlay::Draw draws the routing graph below, with no per-feature
	// toggle inside the overlay itself. RoadBuildEditorTool::DrawPersistentState makes the
	// identical call, which is the whole point: see GraphOverlay.h.
	if ((bDrawNodes || bDrawStands) && Target->Network != nullptr)
	{
		GraphOverlay::Describe(*Target->Network, *this);
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

FLinearColor ARoadBuildHUD::StyleColour(EPreviewStyle Style) const
{
	// No `default:` - see PreviewPalette.h. A style added to EPreviewStyle without a case
	// here now warns at compile time instead of silently reusing Pending's colour, which is
	// what the old default: did for Hover and Selected before this issue.
	switch (Style)
	{
	case EPreviewStyle::Pending:  return PendingColour;
	case EPreviewStyle::Snap:    return SnapColour;
	case EPreviewStyle::Doomed:  return DoomedColour;
	case EPreviewStyle::Heal:    return HealColour;
	case EPreviewStyle::Refused: return RefusedColour;
	case EPreviewStyle::Guideline: return GuidelineColour;
	case EPreviewStyle::Route:   return RouteColour;
	case EPreviewStyle::Hover:   return HoverColour;
	case EPreviewStyle::Selected: return SelectedColour;
	case EPreviewStyle::RunwayHoldingPosition: return RunwayHoldingPositionColour;
	case EPreviewStyle::IntermediateHoldingPosition: return IntermediateHoldingPositionColour;
	case EPreviewStyle::NodeStub:      return StubColour;
	case EPreviewStyle::NodeThrough:   return EndColour;
	case EPreviewStyle::NodeJunction:  return JunctionColour;
	case EPreviewStyle::StandPose:     return StandColour;
	case EPreviewStyle::ServiceAnchor: return ServiceAnchorColour;
	}

	checkNoEntry();
	return PendingColour;
}

void ARoadBuildHUD::Marker(const FVector2D& At, EPreviewStyle Style)
{
	FVector2D Screen;
	if (!ProjectPlanePoint(At, PlaneZ, Screen))
	{
		return;
	}

	// A heavier ring than a node normally wears, so a marked one reads as marked rather
	// than merely recoloured. Guideline nodes are the exception: there are hundreds of
	// them and they are context, so they get a dot rather than a ring that would swamp
	// every mark a tool actually wants read.
	const bool bContext = (Style == EPreviewStyle::Guideline);
	float Radius = bContext ? NodeRingRadius * 0.35f : NodeRingRadius;
	float Thickness = bContext ? PreviewThickness * 0.5f : PreviewThickness;

	// GraphOverlay's own styles keep the sizing DrawNodes/DrawStands drew them with before
	// the two calls were unified: PreviewThickness is tuned for a tool's own sparse preview
	// lines, and NodeRingRadius would swamp the small anchor rings a stand carries eight of.
	if (Style == EPreviewStyle::NodeStub || Style == EPreviewStyle::NodeThrough
		|| Style == EPreviewStyle::NodeJunction)
	{
		Thickness = NodeRingThickness;
	}
	else if (Style == EPreviewStyle::ServiceAnchor)
	{
		Radius = ServiceAnchorRadius;
		Thickness = NodeRingThickness;
	}

	DrawRing(Screen, Radius, StyleColour(Style), Thickness);
	if (Style == EPreviewStyle::Doomed || Style == EPreviewStyle::Pending
		|| Style == EPreviewStyle::Selected || Style == EPreviewStyle::StandPose)
	{
		// The double ring a stand's committed pose always wore in DrawStands - "the aircraft
		// stop position" is drawn heavier than everything else at the pose, per that
		// UPROPERTY's own doc comment.
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

	// The route is what was asked for and the graph is what it ran over, so the route is
	// drawn heavier than everything and the graph lighter than everything.
	const float Weight =
		Style == EPreviewStyle::Guideline ? PreviewThickness * 0.5f :
		Style == EPreviewStyle::Route     ? PreviewThickness * 2.0f :
											PreviewThickness;

	DrawLine(
		static_cast<float>(ScreenA.X), static_cast<float>(ScreenA.Y),
		static_cast<float>(ScreenB.X), static_cast<float>(ScreenB.Y),
		StyleColour(Style), Weight);
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

void ARoadBuildHUD::DrawNodeIndices(const ARoadNetworkActor& Target)
{
	// The rings themselves come from GraphOverlay::Describe - see DrawHUD. Degree still
	// decides the colour here so the label reads the same as the ring it labels.
	const TArray<FRoadNode>& Nodes = Target.Network->GetNodes();

	for (int32 Index = 0; Index < Nodes.Num(); ++Index)
	{
		const FRoadNode& Node = Nodes[Index];
		if (!Node.bAlive || GEngine == nullptr)
		{
			continue;
		}

		FVector2D Screen;
		if (!ProjectPlanePoint(Node.Position, Target.SurfaceZ, Screen))
		{
			continue;
		}

		const int32 Degree = Node.Incident.Num();
		const FLinearColor Colour = (Degree == 0) ? StubColour
			: (Degree >= 3) ? JunctionColour
			: EndColour;

		DrawText(FString::FromInt(Index), Colour,
			static_cast<float>(Screen.X) + NodeRingRadius + 3.0f,
			static_cast<float>(Screen.Y) - NodeRingRadius,
			GEngine->GetSmallFont());
	}
}

void ARoadBuildHUD::DrawAnchorIds(const ARoadNetworkActor& Target)
{
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
			if (!ProjectPlanePoint(Node->Position, Target.SurfaceZ, Screen) || GEngine == nullptr)
			{
				continue;
			}

			DrawText(Anchor.Id.ToString(), ServiceAnchorColour,
				static_cast<float>(Screen.X) + ServiceAnchorRadius + 3.0f,
				static_cast<float>(Screen.Y) - ServiceAnchorRadius,
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
