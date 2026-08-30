#include "RoadBuildEditorTool.h"

#include "BaseBehaviors/ClickDragBehavior.h"
#include "BaseBehaviors/MouseHoverBehavior.h"
#include "EngineUtils.h"
#include "InteractiveToolManager.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "ScopedTransaction.h"
#include "SceneManagement.h"
#include "Tool/ApronDrawTool.h"
#include "Tool/RoadDrawTool.h"
#include "Tool/StandPlaceTool.h"
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
		FViewportPreviewSink(FPrimitiveDrawInterface* InPDI, double InPlaneZ)
			: PDI(InPDI), PlaneZ(InPlaneZ) {}

		virtual void Marker(const FVector2D& At, EPreviewStyle Style) override
		{
			// A ring in WORLD units here, unlike the HUD's pixels: the viewport gives no
			// screen size to work in, and a marker sized against the road at least stays
			// meaningful when the camera moves.
			constexpr int32 Sides = 16;
			constexpr double Radius = 120.0;

			FVector Previous = Lift(At + FVector2D(Radius, 0.0));
			for (int32 Side = 1; Side <= Sides; ++Side)
			{
				const double Angle = 2.0 * UE_DOUBLE_PI * Side / Sides;
				const FVector Point = Lift(At + FVector2D(
					Radius * FMath::Cos(Angle), Radius * FMath::Sin(Angle)));
				PDI->DrawLine(Previous, Point, Colour(Style), SDPG_Foreground, 2.0f);
				Previous = Point;
			}
		}

		virtual void Line(const FVector2D& From, const FVector2D& To, EPreviewStyle Style) override
		{
			PDI->DrawLine(Lift(From), Lift(To), Colour(Style), SDPG_Foreground, 3.0f);
		}

		virtual void CrossMark(const FVector2D& At, const FVector2D& Along, EPreviewStyle Style) override
		{
			if (Along.IsNearlyZero())
			{
				return;
			}

			const FVector2D Across(-Along.Y, Along.X);
			PDI->DrawLine(Lift(At - Across * 150.0), Lift(At + Across * 150.0),
				Colour(Style), SDPG_Foreground, 3.0f);
		}

		virtual void Label(const FVector2D& At, const FString& Text, EPreviewStyle Style) override
		{
			// Deliberately nothing. A PrimitiveDrawInterface draws geometry, not text, and a
			// refusal reason rendered as a squiggle is worse than one left to the log. The
			// runtime HUD shows these; here the tool still refuses, it just says so quietly.
		}

	private:
		FVector Lift(const FVector2D& Plane) const { return FVector(Plane.X, Plane.Y, PlaneZ); }

		static FLinearColor Colour(EPreviewStyle Style)
		{
			switch (Style)
			{
			case EPreviewStyle::Snap:    return FLinearColor(1.0f, 0.9f, 0.15f);
			case EPreviewStyle::Doomed:  return FLinearColor(1.0f, 0.15f, 0.1f);
			case EPreviewStyle::Heal:    return FLinearColor(0.3f, 1.0f, 0.5f);
			case EPreviewStyle::Refused: return FLinearColor(1.0f, 0.25f, 0.2f);
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
	Tool->SetKind(Kind);
	return Tool;
}

void URoadBuildEditorTool::Setup()
{
	UInteractiveTool::Setup();

	switch (Kind)
	{
	case ERoadBuildToolKind::Apron: Build = MakeUnique<FApronDrawTool>(); break;
	case ERoadBuildToolKind::Stand: Build = MakeUnique<FStandPlaceTool>(); break;
	case ERoadBuildToolKind::Road:
	default:                        Build = MakeUnique<FRoadDrawTool>(); break;
	}

	Target = ResolveTarget();

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
	if (Build.IsValid() && Target != nullptr)
	{
		Build->OnDeactivate(MakeContext(FInputDeviceRay(FRay())));
	}
	Build.Reset();

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
	if (Target == nullptr || FMath::IsNearlyZero(Ray.Direction.Z))
	{
		return false;
	}

	const double Distance = (Target->SurfaceZ - Ray.Origin.Z) / Ray.Direction.Z;
	if (Distance <= 0.0)
	{
		// Behind the camera, or above the horizon. Under an editor camera looking up at the
		// sky this is most of the screen, so it is a refusal rather than a rarity.
		return false;
	}

	const FVector Hit = Ray.Origin + Ray.Direction * Distance;
	OutPosition = FVector2D(Hit.X, Hit.Y);
	return true;
}

FToolContext URoadBuildEditorTool::MakeContext(const FInputDeviceRay& At) const
{
	FToolContext Context;
	Context.Target = Target;
	Context.bRemoveModifier = bRemoveHeld;
	Context.bInsertModifier = bInsertHeld;

	FVector2D Plane = HoverPosition;
	if (!RayToPlane(At.WorldRay, Plane))
	{
		Plane = HoverPosition;
	}
	Context.Cursor = Plane;

	// The same snap chain the runtime tool uses, over the same graph. Resolved here rather
	// than inside the tool so both drivers hand it the same shape of answer.
	if (Target != nullptr && Target->Network != nullptr)
	{
		FRoadSnapSettings Settings;
		Settings.NodeRadius = Context.SnapRadius;
		Settings.SegmentRadius = Context.SnapRadius;

		static const FRoadSnapChain Chain;
		Context.Snap = Chain.Resolve(*Target->Network, Plane, Settings);
	}
	else
	{
		Context.Snap.Position = Plane;
	}

	return Context;
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
	if (!bPressed || !Build.IsValid())
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

		Build->OnDragBegin(MakeContext(DragPos));
	}

	Build->OnDrag(MakeContext(DragPos));
}

void URoadBuildEditorTool::OnClickRelease(const FInputDeviceRay& ReleasePos)
{
	if (!bPressed || !Build.IsValid())
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
		Build->OnDragEnd(MakeContext(ReleasePos));
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

	Build->OnClick(MakeContext(ReleasePos));
	GEditor->EndTransaction();
}

void URoadBuildEditorTool::OnTerminateDragSequence()
{
	if (bDragging)
	{
		// Escape during a drag. Cancel the transaction rather than committing a half-aimed
		// stand, and tell the tool so it drops whatever it was holding.
		GEditor->CancelTransaction(0);
		if (Build.IsValid())
		{
			Build->OnCancel(MakeContext(FInputDeviceRay(FRay())));
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

	if (Build.IsValid() && Target != nullptr)
	{
		Build->Tick(MakeContext(DevicePos));
	}
	return true;
}

void URoadBuildEditorTool::Render(IToolsContextRenderAPI* RenderAPI)
{
	if (!Build.IsValid() || Target == nullptr || RenderAPI == nullptr)
	{
		return;
	}

	FPrimitiveDrawInterface* PDI = RenderAPI->GetPrimitiveDrawInterface();
	if (PDI == nullptr)
	{
		return;
	}

	FViewportPreviewSink Sink(PDI, Target->SurfaceZ);
	Build->BuildPreview(MakeContext(FInputDeviceRay(FRay())), Sink);
}

#undef LOCTEXT_NAMESPACE
