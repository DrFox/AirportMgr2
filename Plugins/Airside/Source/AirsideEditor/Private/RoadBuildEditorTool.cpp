#include "RoadBuildEditorTool.h"

#include "BaseBehaviors/ClickDragBehavior.h"
#include "BaseBehaviors/MouseHoverBehavior.h"
#include "EngineUtils.h"
#include "InteractiveToolManager.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNode.h"
#include "Present/RoadNetworkActor.h"
#include "ScopedTransaction.h"
#include "SceneManagement.h"
#include "Solve/RoadGeom.h"
#include "Tool/GuidelineOverlay.h"
#include "Tool/StandPreview.h"
#include "ToolContextInterfaces.h"

#define LOCTEXT_NAMESPACE "RoadBuildEditorTool"

namespace
{
	/** Below this, a press that moved is still a click. Pixels, as at runtime. */
	constexpr double DragThresholdPixels = 4.0;

	/**
	 * Draws what a build tool describes, into the editor viewport.
	 *
	 * The runtime counterpart is ARoadBuildHUD. Both implement the same sink and the tools
	 * cannot tell them apart, which is the whole reason the sink takes ROAD PLANE
	 * coordinates and a MEANING rather than screen positions and a colour.
	 */
	class FViewportPreviewSink : public IToolPreviewSink
	{
	public:
		FViewportPreviewSink(FPrimitiveDrawInterface* InPDI, double InPlaneZ,
			const FVector& InCamera, double InPerDistance, double InFixedRadius)
			: PDI(InPDI), PlaneZ(InPlaneZ), Camera(InCamera)
			, PerDistance(InPerDistance), FixedRadius(InFixedRadius) {}

		/**
		 * Radius that holds a constant SIZE ON SCREEN for a point at this distance.
		 *
		 * A world-space circle shrinks with distance, so sizing every marker from one
		 * view-centre number left near ones huge and far ones specks - which is exactly
		 * what the nodes looked like.
		 */
		double RadiusAt(const FVector2D& Plane) const
		{
			if (FixedRadius > 0.0)
			{
				return FixedRadius;
			}
			return PerDistance * FVector::Dist(Camera, Lift(Plane));
		}

		virtual void Marker(const FVector2D& At, EPreviewStyle Style) override
		{
			// A ring in WORLD units here, unlike the HUD's pixels: the viewport gives no
			// screen size to work in, and a marker sized against the road at least stays
			// meaningful when the camera moves.
			// A FRACTION OF THE SCREEN, not a fixed world size. At 120 uu this ring was
			// 1.2 m across, which is sub-pixel over an airport and looked like the preview
			// was not following the mouse at all.
			constexpr int32 Sides = 16;
			const double Radius = RadiusAt(At);

			FVector Previous = Lift(At + FVector2D(Radius, 0.0));
			for (int32 Side = 1; Side <= Sides; ++Side)
			{
				const double Angle = 2.0 * UE_DOUBLE_PI * Side / Sides;
				const FVector Point = Lift(At + FVector2D(
					Radius * FMath::Cos(Angle), Radius * FMath::Sin(Angle)));
				PDI->DrawLine(Previous, Point, Colour(Style), SDPG_Foreground,
					2.0f, 0.0f, true);
				Previous = Point;
			}
		}

		virtual void Line(const FVector2D& From, const FVector2D& To, EPreviewStyle Style) override
		{
			// bScreenSpace = TRUE. Thickness is otherwise WORLD units: 3 uu is 3 cm, a
			// hairline over an airport, which is why these read as far thinner than the
			// runtime HUD's pixel-width lines.
			PDI->DrawLine(Lift(From), Lift(To), Colour(Style), SDPG_Foreground,
				3.0f, 0.0f, true);
		}

		virtual void CrossMark(const FVector2D& At, const FVector2D& Along, EPreviewStyle Style) override
		{
			if (Along.IsNearlyZero())
			{
				return;
			}

			const FVector2D Across(-Along.Y, Along.X);
			const double Arm = RadiusAt(At) * 1.25;
			PDI->DrawLine(Lift(At - Across * Arm), Lift(At + Across * Arm),
				Colour(Style), SDPG_Foreground, 3.0f, 0.0f, true);
		}

		virtual void Label(const FVector2D& At, const FString& Text, EPreviewStyle Style) override
		{
			// Deliberately nothing. A PrimitiveDrawInterface draws geometry, not text, and a
			// refusal reason rendered as a squiggle is worse than one left to the log. The
			// runtime HUD shows these; here the tool still refuses, it just says so quietly.
		}

	private:
		FVector Lift(const FVector2D& Plane) const { return FVector(Plane.X, Plane.Y, PlaneZ); }
		FVector Camera = FVector::ZeroVector;
		double PerDistance = 0.0;
		double FixedRadius = 0.0;

		static FLinearColor Colour(EPreviewStyle Style)
		{
			switch (Style)
			{
			case EPreviewStyle::Snap:    return FLinearColor(1.0f, 0.9f, 0.15f);
			case EPreviewStyle::Doomed:  return FLinearColor(1.0f, 0.15f, 0.1f);
			case EPreviewStyle::Heal:    return FLinearColor(0.3f, 1.0f, 0.5f);
			case EPreviewStyle::Refused: return FLinearColor(1.0f, 0.25f, 0.2f);
			case EPreviewStyle::Guideline: return FLinearColor(0.35f, 0.45f, 0.6f);
			case EPreviewStyle::Route:   return FLinearColor(0.2f, 0.85f, 1.0f);
			case EPreviewStyle::Pending:
			default:                     return FLinearColor(0.2f, 1.0f, 0.3f);
			}
		}

		FPrimitiveDrawInterface* PDI = nullptr;
		double PlaneZ = 0.0;
	};
}

UInteractiveTool* URoadBuildEditorToolBuilder::BuildTool(const FToolBuilderState& SceneState) const
{
	URoadBuildEditorTool* Tool = NewObject<URoadBuildEditorTool>(SceneState.ToolManager);
	Tool->SetToolIndex(ToolIndex);
	return Tool;
}

void URoadBuildEditorTool::Setup()
{
	UInteractiveTool::Setup();

	// Session is constructed with all six registry tools already - see FBuildSession's
	// constructor - so selecting this instance's one is a switch, not a make.
	Session.SelectTool(ToolIndex);

	Target = ResolveTarget();

	UE_LOG(LogTemp, Log, TEXT("Airside ed tool active: %s, target %s"),
		Session.GetActiveTool() != nullptr ? *Session.GetActiveTool()->GetDisplayName().ToString() : TEXT("NONE"),
		Target != nullptr ? *Target->GetName() : TEXT("NONE"));

	UClickDragInputBehavior* Drag = NewObject<UClickDragInputBehavior>(this);
	Drag->Initialize(this);

	// Ctrl and shift mean the same here as at runtime - remove, and insert - because they
	// are read by the shared tool, not by this adapter.
	Drag->Modifiers.RegisterModifier(RemoveModifierId, FInputDeviceState::IsCtrlKeyDown);
	Drag->Modifiers.RegisterModifier(InsertModifierId, FInputDeviceState::IsShiftKeyDown);
	AddInputBehavior(Drag);

	// Hover exists only so the preview follows the cursor between clicks. Without it a
	// half-drawn apron would show nothing until the next press.
	UMouseHoverBehavior* Hover = NewObject<UMouseHoverBehavior>(this);
	Hover->Initialize(this);
	Hover->Modifiers.RegisterModifier(RemoveModifierId, FInputDeviceState::IsCtrlKeyDown);
	Hover->Modifiers.RegisterModifier(InsertModifierId, FInputDeviceState::IsShiftKeyDown);
	AddInputBehavior(Hover);
}

void URoadBuildEditorTool::Shutdown(EToolShutdownType ShutdownType)
{
	// Leaving the tool abandons whatever it had part-drawn, exactly as switching tools does
	// at runtime. A chain resumed after a mode change would be a click landing on something
	// begun before the user went away.
	if (IBuildTool* Tool = Session.GetActiveTool(); Tool != nullptr && Target != nullptr)
	{
		Tool->OnDeactivate(MakeHoverContext());
	}

	UInteractiveTool::Shutdown(ShutdownType);
}

ARoadNetworkActor* URoadBuildEditorTool::ResolveTarget() const
{
	UWorld* World = GetToolManager() != nullptr ? GetToolManager()->GetWorld() : nullptr;
	if (World == nullptr)
	{
		return nullptr;
	}

	// Found or created. Having to drag one in by hand before anything works was a
	// convenience gap, not a design requirement - and in the editor there is nothing to
	// warn at.
	return ARoadNetworkActor::FindOrCreate(World);
}

bool URoadBuildEditorTool::RayToPlane(const FRay& Ray, FVector2D& OutPosition) const
{
	if (Target == nullptr)
	{
		return false;
	}

	// No max-distance guard, unlike the runtime driver's CursorOnRoadPlane: that guard is
	// measured against the build camera's "current view distance", a concept this editor
	// tool has no equivalent of (ViewWorldWidth is a WIDTH, not a distance, and is only
	// refreshed in Render). An effectively infinite cap keeps this call refusing only for
	// the two reasons it always did - parallel, or behind the camera.
	return RoadGeom::RayToPlaneZ(Ray.Origin, Ray.Direction, Target->SurfaceZ,
		TNumericLimits<double>::Max(), OutPosition);
}

FToolContext URoadBuildEditorTool::MakeContext(const FInputDeviceRay& At)
{
	// Resolve the ray HERE, where a miss can fall back honestly. There is deliberately no
	// "no ray" sentinel: FRay() defaults its direction to (0,0,1), which points straight
	// down at the road plane and resolves to the WORLD ORIGIN rather than failing. Every
	// preview drew against (0,0) because of it.
	FVector2D Plane;
	if (!RayToPlane(At.WorldRay, Plane))
	{
		Plane = HoverPosition;
	}
	return MakeContextAt(Plane);
}

FToolContext URoadBuildEditorTool::MakeHoverContext()
{
	return MakeContextAt(HoverPosition);
}

FToolContext URoadBuildEditorTool::MakeContextAt(const FVector2D& Plane)
{
	// How close counts as "on" something has to be a screen distance, not a world one.
	// At the fixed 150 uu default, closing an apron meant clicking within 1.5 m of its
	// first corner - unhittable when zoomed out over a runway. Pushed into Session before
	// asking it to resolve and build, rather than passed as an argument MakeContext has no
	// room for (its signature is shared with the runtime driver's call) - the same "push
	// the tunable, then act" shape ARoadBuildController::MakeToolContext uses.
	const double SnapRadius = FMath::Max(150.0, ViewWorldWidth * 0.02);
	Session.ToolPickRadius = SnapRadius;
	Session.Snap.NodeRadius = SnapRadius;
	Session.Snap.SegmentRadius = SnapRadius;

	// Through SetCursor, like the runtime driver, so the raw hit and the snapped answer
	// cannot drift into meaning the same thing in one driver and different things in the
	// other - which they did, and only one of them was right.
	return Session.MakeContext(Target, Plane, bRemoveHeld, bInsertHeld);
}

void URoadBuildEditorTool::OnUpdateModifierState(int ModifierID, bool bIsOn)
{
	if (ModifierID == RemoveModifierId) { bRemoveHeld = bIsOn; }
	if (ModifierID == InsertModifierId) { bInsertHeld = bIsOn; }
}

FInputRayHit URoadBuildEditorTool::CanBeginClickDragSequence(const FInputDeviceRay& PressPos)
{
	FVector2D Unused;
	if (!RayToPlane(PressPos.WorldRay, Unused))
	{
		return FInputRayHit();
	}


	// Any point on the road plane is fair game. A depth of zero puts this behind anything
	// else that claims the click, which is what we want: a gizmo should still win.
	return FInputRayHit(0.0f);
}

void URoadBuildEditorTool::OnClickPress(const FInputDeviceRay& PressPos)
{
	bPressed = true;
	bDragging = false;
	PressScreen = PressPos.ScreenPosition;

	RayToPlane(PressPos.WorldRay, HoverPosition);
}

void URoadBuildEditorTool::OnClickDrag(const FInputDeviceRay& DragPos)
{
	IBuildTool* Tool = Session.GetActiveTool();
	if (!bPressed || Tool == nullptr)
	{
		return;
	}

	RayToPlane(DragPos.WorldRay, HoverPosition);

	if (!bDragging)
	{
		if (FVector2D::Distance(DragPos.ScreenPosition, PressScreen) < DragThresholdPixels)
		{
			return;
		}

		bDragging = true;

		// One transaction for the whole drag, opened where the gesture becomes real.
		GEditor->BeginTransaction(LOCTEXT("RoadBuildDrag", "Road Build"));
		if (Target != nullptr && Target->Network != nullptr)
		{
			Target->Modify();
			Target->Network->Modify();
		}

		Tool->OnDragBegin(MakeContext(DragPos));
	}

	Tool->OnDrag(MakeContext(DragPos));
}

void URoadBuildEditorTool::OnClickRelease(const FInputDeviceRay& ReleasePos)
{
	IBuildTool* Tool = Session.GetActiveTool();
	if (!bPressed || Tool == nullptr)
	{
		bPressed = false;
		return;
	}

	RayToPlane(ReleasePos.WorldRay, HoverPosition);

	const bool bWasDragging = bDragging;
	bPressed = false;
	bDragging = false;

	if (bWasDragging)
	{
		Tool->OnDragEnd(MakeContext(ReleasePos));
		GEditor->EndTransaction();
		return;
	}

	// A press that never travelled was a click. Its own transaction, so one click is one
	// Ctrl+Z rather than part of whatever came before.
	GEditor->BeginTransaction(LOCTEXT("RoadBuildClick", "Road Build"));
	if (Target != nullptr && Target->Network != nullptr)
	{
		Target->Modify();
		Target->Network->Modify();
	}

	Tool->OnClick(MakeContext(ReleasePos));
	GEditor->EndTransaction();
}

void URoadBuildEditorTool::OnTerminateDragSequence()
{
	if (bDragging)
	{
		// Escape during a drag. Cancel the transaction rather than committing a half-aimed
		// stand, and tell the tool so it drops whatever it was holding.
		GEditor->CancelTransaction(0);
		if (IBuildTool* Tool = Session.GetActiveTool())
		{
			Tool->OnCancel(MakeHoverContext());
		}
	}

	bPressed = false;
	bDragging = false;
}

FInputRayHit URoadBuildEditorTool::BeginHoverSequenceHitTest(const FInputDeviceRay& PressPos)
{
	FVector2D Unused;
	return RayToPlane(PressPos.WorldRay, Unused) ? FInputRayHit(0.0f) : FInputRayHit();
}

bool URoadBuildEditorTool::OnUpdateHover(const FInputDeviceRay& DevicePos)
{
	bHoverValid = RayToPlane(DevicePos.WorldRay, HoverPosition);

	if (IBuildTool* Tool = Session.GetActiveTool(); Tool != nullptr && Target != nullptr)
	{
		Tool->Tick(MakeContext(DevicePos));
	}
	return true;
}

void URoadBuildEditorTool::DrawPersistentState(IToolPreviewSink& Sink) const
{
	if (Target == nullptr || Target->Network == nullptr)
	{
		return;
	}

	// The routing graph is committed state, so it belongs here beside the nodes and stands
	// rather than inside whichever tool happens to be selected.
	//
	// ALWAYS ON in the editor - there is no toggle. The runtime driver binds G for it, but
	// a key here would mean a new command plus a palette entry, and that pairing is where
	// this module has already shipped three separate "the list nothing reads" defects. A
	// visibility change is not the place to take that on.
	GuidelineOverlay::Draw(*Target->Network, Sink);

	for (const FRoadNode& Node : Target->Network->GetNodes())
	{
		if (!Node.bAlive)
		{
			continue;
		}

		// Same reading as the HUD: degree separates a junction from a straight-through
		// node, and the pavement looks identical either way.
		const int32 Degree = Node.Incident.Num();
		const EPreviewStyle Style = (Degree == 0) ? EPreviewStyle::Refused
			: (Degree >= 3) ? EPreviewStyle::Snap
			: EPreviewStyle::Heal;

		Sink.Marker(Node.Position, Style);
	}

	for (const FEntityInstance& Entity : Target->Network->GetEntities())
	{
		if (Entity.bAlive)
		{
			// The full stand - aircraft footprint, service points, fixtures - not the
			// ring and tick this used to draw.
			StandPreview::Describe(Entity.Definition, Entity.Position, Entity.Heading, Sink);
		}
	}
}

void URoadBuildEditorTool::CancelGesture()
{
	IBuildTool* Tool = Session.GetActiveTool();
	if (Tool == nullptr || Target == nullptr)
	{
		return;
	}

	// A cancel can still touch the graph - abandoning a chain after one click removes the
	// node it stranded - so it gets a transaction like any other edit.
	GEditor->BeginTransaction(LOCTEXT("RoadBuildCancel", "Road Build Cancel"));
	if (Target->Network != nullptr)
	{
		Target->Modify();
		Target->Network->Modify();
	}

	Tool->OnCancel(MakeHoverContext());
	GEditor->EndTransaction();

	bPressed = false;
	bDragging = false;
}

void URoadBuildEditorTool::Render(IToolsContextRenderAPI* RenderAPI)
{
	IBuildTool* Tool = Session.GetActiveTool();
	if (Tool == nullptr || Target == nullptr || RenderAPI == nullptr)
	{
		return;
	}

	FPrimitiveDrawInterface* PDI = RenderAPI->GetPrimitiveDrawInterface();
	if (PDI == nullptr)
	{
		return;
	}

	const FViewCameraState Camera = RenderAPI->GetCameraState();
	if (Camera.bIsOrthographic)
	{
		ViewWorldWidth = Camera.OrthoWorldCoordinateWidth;
	}
	else
	{
		// Distance to the plane at the VIEW CENTRE, never to the cursor. Keyed to the
		// cursor, every marker resized as the mouse moved - the scale has to depend on
		// where the camera is, not on where the pointer happens to be.
		const FVector Forward = Camera.Orientation.GetForwardVector();
		const double Drop = FMath::Abs(Camera.Position.Z - Target->SurfaceZ);

		double Distance = Drop;
		if (!FMath::IsNearlyZero(Forward.Z))
		{
			const double Along = (Target->SurfaceZ - Camera.Position.Z) / Forward.Z;
			if (Along > 0.0)
			{
				Distance = Along;
			}
		}

		ViewWorldWidth = 2.0 * Distance
			* FMath::Tan(FMath::DegreesToRadians(Camera.HorizontalFOVDegrees * 0.5f));
	}
	ViewWorldWidth = FMath::Clamp(ViewWorldWidth, 100.0, 1.0e7);

	// Perspective: a marker's radius is a fraction of the view width AT ITS OWN DEPTH.
	// Orthographic: depth is irrelevant, so one fixed size is correct.
	const double PerDistance = Camera.bIsOrthographic
		? 0.0
		: 0.012 * 2.0 * FMath::Tan(FMath::DegreesToRadians(Camera.HorizontalFOVDegrees * 0.5f));
	const double FixedRadius = Camera.bIsOrthographic ? ViewWorldWidth * 0.012 : 0.0;

	FViewportPreviewSink Sink(PDI, Target->SurfaceZ, Camera.Position, PerDistance, FixedRadius);

	// The COMMITTED graph first, then the tool's intent on top. The runtime HUD does this
	// in ARoadBuildHUD::DrawNodes/DrawStands; in the editor nothing did, so existing nodes
	// and stands were invisible and there was no way to see what a snap would attach to.
	DrawPersistentState(Sink);

	// Gated on a real hover. Before the first mouse move HoverPosition is (0,0), and the
	// idle marker was drawing a corner at the world origin.
	if (bHoverValid)
	{
		Tool->BuildPreview(MakeHoverContext(), Sink);
	}

}

#undef LOCTEXT_NAMESPACE
