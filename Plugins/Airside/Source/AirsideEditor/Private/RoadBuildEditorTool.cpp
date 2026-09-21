#include "RoadBuildEditorTool.h"

#include "RoadBuildEdMode.h"

#include "AirsideEditorLog.h"
#include "BaseBehaviors/ClickDragBehavior.h"
#include "BaseBehaviors/MouseHoverBehavior.h"
#include "CanvasTypes.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "EngineUtils.h"
#include "InteractiveToolManager.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "RoadBuildEdModeCommands.h"
#include "ScopedTransaction.h"
#include "SceneManagement.h"
#include "Solve/RoadGeom.h"
#include "Tool/GraphOverlay.h"
#include "Tool/GuidelineOverlay.h"
#include "Present/PreviewPalette.h"
#include "ToolContextInterfaces.h"

#define LOCTEXT_NAMESPACE "RoadBuildEditorTool"

namespace
{
	// DragThresholdPixels no longer lives here (issue #191/#92-#93): this was a THIRD 4.0,
	// beside RoadBuildController.h's UPROPERTY and BuildGestureCompositionTest.cpp's own
	// comment, nothing keeping the three in agreement - see FBuildGesture::
	// DefaultThresholdPixels, which Gesture.Move below now defaults to.

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

		/**
		 * The SAME table ARoadBuildHUD seeds its UPROPERTYs from - see PreviewPalette.h.
		 *
		 * This used to retype the whole table by hand, and its `default:` silently mapped
		 * Hover and Selected to Pending's green because neither had its own case - the exact
		 * bug PreviewPalette exists to make impossible. The viewport has no per-level
		 * designer override to preserve, so calling straight through is the whole function.
		 */
		static FLinearColor Colour(EPreviewStyle Style)
		{
			return PreviewPalette::Default(Style);
		}

		FPrimitiveDrawInterface* PDI = nullptr;
		double PlaneZ = 0.0;
	};
}

UInteractiveTool* URoadBuildEditorToolBuilder::BuildTool(const FToolBuilderState& SceneState) const
{
	URoadBuildEditorTool* Tool = NewObject<URoadBuildEditorTool>(SceneState.ToolManager);
	Tool->SetToolIndex(ToolIndex);

	// THE MODE'S SESSION, not one of this tool's own. The builder is created with the mode as
	// its outer (URoadBuildEdMode::Enter), so the mode is reachable without threading it
	// through FToolBuilderState. Without this the session - and every runway width chosen in
	// it - is rebuilt on each activation; see URoadBuildEdMode::GetSession.
	if (URoadBuildEdMode* Mode = Cast<URoadBuildEdMode>(GetOuter()))
	{
		Tool->SetSharedSession(&Mode->GetSession());
	}
	else
	{
		UE_LOG(LogAirsideEditor, Warning,
			TEXT("Airside editor tool built without a mode: tool state will not persist "
				 "across activations, so a runway's width resets each time it is picked."));
	}
	return Tool;
}

void URoadBuildEditorTool::Setup()
{
	UInteractiveTool::Setup();

	// RESOLVED BEFORE SelectTool, not after: a SAME-INDEX activation (this tool's own key,
	// pressed again while already active) makes SelectTool see Index == ActiveTool and call
	// OnReselect on the spot - FRunwayTool::OnReselect calls NextWidth, which since issue
	// #78 asks Context.Target for the profile count. Calling SelectTool with Target still
	// null would silently stop the width cycling, the same failure mode
	// URoadBuildEdMode::StartToolAction's own reselect path had.
	Target = ResolveTarget();

	// Session is constructed with all six registry tools already - see FBuildSession's
	// constructor - so selecting this instance's one is a switch, not a make.
	//
	// bRemoveHeld/bInsertHeld CARRIED, not left at their false defaults (issue #191/#92-#93):
	// ARoadBuildController::SelectTool always builds its context through MakeToolContext(),
	// which reads live Shift/Ctrl regardless of whether the press switches tools or reselects
	// the one already active, so a runway picked up while Ctrl is still held from removing a
	// taxiway carries that removal intent straight in. This instance's own modifiers are still
	// their construction-time false/false at the exact moment Setup() runs on a genuine
	// activation - the behaviours that report them are registered a few lines below - but this
	// call site also fires as a RESELECT when ToolIndex is already the session's active tool
	// (a fresh instance built for a second press of the same palette entry, before this one's
	// own Setup has had a chance to see anything held), and reading the field rather than
	// hand-writing false here is what stops that path being a second place a default is typed.
	FToolContext SelectContext;
	SelectContext.Target = Target;
	SelectContext.bRemoveModifier = bRemoveHeld;
	SelectContext.bInsertModifier = bInsertHeld;
	Sess().SelectTool(ToolIndex, SelectContext);

	UE_LOG(LogAirsideEditor, Log, TEXT("Airside ed tool active: %s, target %s"),
		Sess().GetActiveTool() != nullptr ? *Sess().GetActiveTool()->GetDisplayName().ToString() : TEXT("NONE"),
		Target != nullptr ? *Target->GetName() : TEXT("NONE"));

	UClickDragInputBehavior* Drag = NewObject<UClickDragInputBehavior>(this);
	Drag->Initialize(this);

	// Ctrl and shift mean the same here as at runtime - remove, and insert - because they
	// are read by the shared tool, not by this adapter.
	Drag->Modifiers.RegisterModifier(RemoveModifierId, FInputDeviceState::IsCtrlKeyDown);
	Drag->Modifiers.RegisterModifier(InsertModifierId, FInputDeviceState::IsShiftKeyDown);

	// REGISTERED ON BOTH BEHAVIOURS, here and on Hover below. The editor mode does not read
	// keys - it is told about modifier ids it registered - and registering only on Drag would
	// suspend while dragging but not while hovering, so the ghost would show a guide the click
	// then ignored.
	Drag->Modifiers.RegisterModifier(SuspendModifierId, FInputDeviceState::IsAltKeyDown);
	AddInputBehavior(Drag);

	// Hover exists only so the preview follows the cursor between clicks. Without it a
	// half-drawn apron would show nothing until the next press.
	UMouseHoverBehavior* Hover = NewObject<UMouseHoverBehavior>(this);
	Hover->Initialize(this);
	Hover->Modifiers.RegisterModifier(RemoveModifierId, FInputDeviceState::IsCtrlKeyDown);
	Hover->Modifiers.RegisterModifier(InsertModifierId, FInputDeviceState::IsShiftKeyDown);
	Hover->Modifiers.RegisterModifier(SuspendModifierId, FInputDeviceState::IsAltKeyDown);
	AddInputBehavior(Hover);
}

void URoadBuildEditorTool::Shutdown(EToolShutdownType ShutdownType)
{
	// Leaving the tool abandons whatever it had part-drawn, exactly as switching tools does
	// at runtime. A chain resumed after a mode change would be a click landing on something
	// begun before the user went away.
	if (IBuildTool* Tool = Sess().GetActiveTool(); Tool != nullptr && Target != nullptr)
	{
		Tool->OnDeactivate(MakeHoverContext());
	}

	// A mid-drag transaction outlives OnClickDrag/OnClickRelease by design (see
	// DragTransaction's own comment) - so a palette switch or mode exit DURING a drag,
	// which shuts this tool down without ever reaching OnClickRelease's DragEnd branch or
	// OnTerminateDragSequence, is exactly the kind of early-return the RAII was meant to
	// close against. Cancelled, not committed: the drag never finished.
	if (DragTransaction.IsValid())
	{
		DragTransaction->Cancel();
		DragTransaction.Reset();
	}

	UInteractiveTool::Shutdown(ShutdownType);
}

void URoadBuildEditorTool::DeactivateOnUndo()
{
	// Same guard and same call as Shutdown's own deactivate block above - see this method's
	// header comment for why a mid-drag transaction is deliberately NOT handled here too.
	if (IBuildTool* Tool = Sess().GetActiveTool(); Tool != nullptr && Target != nullptr)
	{
		Tool->OnDeactivate(MakeHoverContext());
	}
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

	// CAPPED THE SAME WAY PIE's CursorOnRoadPlane IS (issue #191/#92-#93): a perspective click
	// near the horizon used to run away toward infinity here with NO guard at all, while
	// ARoadBuildController measured every click against MaxPlaceDistanceFactor *
	// ActiveRig().Distance. ViewCentreDistance is Render's own view-centre distance to the
	// plane, refreshed every frame it runs - see that field's own comment for why a negative
	// value (before the first Render, or during an orthographic view) leaves this uncapped,
	// exactly as it always was.
	const double MaxDistance = ViewCentreDistance > 0.0
		? RoadGeom::DefaultMaxPlaceDistanceFactor * ViewCentreDistance
		: TNumericLimits<double>::Max();

	return RoadGeom::RayToPlaneZ(Ray.Origin, Ray.Direction, Target->SurfaceZ,
		MaxDistance, OutPosition);
}

FToolContext URoadBuildEditorTool::MakeContext(const FInputDeviceRay& At) const
{
	// Resolve the ray HERE, where a miss can fall back honestly. There is deliberately no
	// "no ray" sentinel: FRay() defaults its direction to (0,0,1), which points straight
	// down at the road plane and resolves to the WORLD ORIGIN rather than failing. Every
	// preview drew against (0,0) because of it.
	//
	// The fallback lives on FBuildSession (RecordPlaneHit/LastPlaneHit) now, shared with
	// ARoadBuildController's identical fallback - see issue #92.
	FVector2D Plane;
	if (RayToPlane(At.WorldRay, Plane))
	{
		Sess().RecordPlaneHit(Plane);
	}
	else
	{
		Plane = Sess().LastPlaneHit();
	}
	return MakeContextAt(Plane);
}

FToolContext URoadBuildEditorTool::MakeHoverContext() const
{
	return MakeContextAt(Sess().LastPlaneHit());
}

FToolContext URoadBuildEditorTool::MakeContextAt(const FVector2D& Plane) const
{
	// Snap/Limits are the airport's own now, not this tool's - see ARoadNetworkActor::
	// MakeTunables and issue #93. Before this, MakeContextAt built its OWN Snap from a
	// view-derived radius and never set Limits at all, so a corner PIE would refuse as
	// TooSharp the editor mode happily drew, and a click-to-split radius here disagreed
	// with the runtime driver's PickRadius for no reason either driver chose.
	//
	// ViewWorldWidth > 0 asks MakeTunables for the same view-scaled ToolPickRadius/snap
	// floor this used to compute inline (how close counts as "on" something has to be a
	// screen distance, not a world one - at a fixed 150 uu default, closing an apron meant
	// clicking within 1.5 m of its first corner, unhittable when zoomed out over a runway).
	// Passed as a local FBuildSessionTunables rather than pushed onto Session first - the
	// session has no mutable tunables to push into any more, so MakeContextAt stays const.
	const FBuildSessionTunables Tunables = Target != nullptr
		? Target->MakeTunables(ViewWorldWidth) : FBuildSessionTunables();

	// See FBuildSession::MakeContext for why Cursor is the raw hit and Snap rides beside
	// it rather than being folded into it.
	return Sess().MakeContext(Target, Plane, Tunables, bRemoveHeld, bInsertHeld, bSuspendHeld);
}

void URoadBuildEditorTool::OnUpdateModifierState(int ModifierID, bool bIsOn)
{
	if (ModifierID == RemoveModifierId) { bRemoveHeld = bIsOn; }
	if (ModifierID == InsertModifierId) { bInsertHeld = bIsOn; }
	if (ModifierID == SuspendModifierId) { bSuspendHeld = bIsOn; }
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

URoadBuildEditorTool::FScopedRoadBuildTransaction::FScopedRoadBuildTransaction(
	const FText& SessionName, ARoadNetworkActor* InTarget)
{
	GEditor->BeginTransaction(SessionName);
	if (InTarget != nullptr && InTarget->Network != nullptr)
	{
		InTarget->Modify();
		InTarget->Network->Modify();
	}
}

URoadBuildEditorTool::FScopedRoadBuildTransaction::~FScopedRoadBuildTransaction()
{
	if (!bCancelled)
	{
		GEditor->EndTransaction();
	}
}

void URoadBuildEditorTool::FScopedRoadBuildTransaction::Cancel()
{
	if (!bCancelled)
	{
		GEditor->CancelTransaction(0);
		bCancelled = true;
	}
}

void URoadBuildEditorTool::OnClickPress(const FInputDeviceRay& PressPos)
{
	Gesture.Press(PressPos.ScreenPosition);

	FVector2D Plane;
	if (RayToPlane(PressPos.WorldRay, Plane))
	{
		Sess().RecordPlaneHit(Plane);
	}
}

void URoadBuildEditorTool::OnClickDrag(const FInputDeviceRay& DragPos)
{
	IBuildTool* Tool = Sess().GetActiveTool();
	if (!Gesture.IsPressed() || Tool == nullptr)
	{
		return;
	}

	FVector2D Plane;
	if (RayToPlane(DragPos.WorldRay, Plane))
	{
		Sess().RecordPlaneHit(Plane);
	}

	const EGestureStep Step = Gesture.Move(DragPos.ScreenPosition);
	if (Step == EGestureStep::None)
	{
		return;
	}

	if (Step == EGestureStep::DragBegan)
	{
		// One transaction for the whole drag, opened where the gesture becomes real - held
		// on DragTransaction until OnClickRelease's DragEnd branch or
		// OnTerminateDragSequence closes it.
		DragTransaction = MakeUnique<FScopedRoadBuildTransaction>(LOCTEXT("RoadBuildDrag", "Road Build"), Target);
		Tool->OnDragBegin(MakeContext(DragPos));
	}

	Tool->OnDrag(MakeContext(DragPos));
}

void URoadBuildEditorTool::OnClickRelease(const FInputDeviceRay& ReleasePos)
{
	IBuildTool* Tool = Sess().GetActiveTool();
	const EGestureEnd End = Gesture.Release();
	if (Tool == nullptr || End == EGestureEnd::Nothing)
	{
		return;
	}

	FVector2D Plane;
	if (RayToPlane(ReleasePos.WorldRay, Plane))
	{
		Sess().RecordPlaneHit(Plane);
	}

	if (End == EGestureEnd::DragEnd)
	{
		Tool->OnDragEnd(MakeContext(ReleasePos));
		DragTransaction.Reset();
		return;
	}

	// A press that never travelled was a click. Its own transaction, so one click is one
	// Ctrl+Z rather than part of whatever came before.
	FScopedRoadBuildTransaction Transaction(LOCTEXT("RoadBuildClick", "Road Build"), Target);
	Tool->OnClick(MakeContext(ReleasePos));
}

void URoadBuildEditorTool::OnTerminateDragSequence()
{
	if (Gesture.IsDragging())
	{
		// Escape during a drag. Cancel the transaction rather than committing a half-aimed
		// stand, and tell the tool so it drops whatever it was holding.
		if (DragTransaction.IsValid())
		{
			DragTransaction->Cancel();
			DragTransaction.Reset();
		}
		if (IBuildTool* Tool = Sess().GetActiveTool())
		{
			Tool->OnCancel(MakeHoverContext());
		}
	}

	Gesture.Cancel();
}

FInputRayHit URoadBuildEditorTool::BeginHoverSequenceHitTest(const FInputDeviceRay& PressPos)
{
	FVector2D Unused;
	return RayToPlane(PressPos.WorldRay, Unused) ? FInputRayHit(0.0f) : FInputRayHit();
}

bool URoadBuildEditorTool::OnUpdateHover(const FInputDeviceRay& DevicePos)
{
	FVector2D Plane;
	bHoverValid = RayToPlane(DevicePos.WorldRay, Plane);
	if (bHoverValid)
	{
		Sess().RecordPlaneHit(Plane);
	}

	if (IBuildTool* Tool = Sess().GetActiveTool(); Tool != nullptr && Target != nullptr)
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

	// Placed entities are always drawn - they are the airport, not scaffolding.
	GraphOverlay::DescribeStands(*Target->Network, Sink);

	// THE NODE RINGS ARE CONDITIONAL, and asked of the SESSION so this viewport and
	// ARoadBuildHUD cannot answer it differently - which is exactly how the three
	// renderings GraphOverlay.h describes came to drift. Describe() is no longer called
	// here: its whole rationale was "the one caller with no toggle", and this caller now
	// has one.
	if (Sess().WantsRoadNodesDrawn())
	{
		GraphOverlay::DescribeNodes(*Target->Network, Sink);
	}
}

void URoadBuildEditorTool::CancelGesture()
{
	IBuildTool* Tool = Sess().GetActiveTool();
	if (Tool == nullptr || Target == nullptr)
	{
		return;
	}

	// A cancel can still touch the graph - abandoning a chain after one click removes the
	// node it stranded - so it gets a transaction like any other edit.
	FScopedRoadBuildTransaction Transaction(LOCTEXT("RoadBuildCancel", "Road Build Cancel"), Target);

	// Not Sess().CancelActiveGesture, and the reason survives the session moving to the mode:
	// each editor tool INSTANCE is still pinned to ONE palette entry
	// (URoadBuildEditorToolBuilder::ToolIndex), so "return to Select" here would run the
	// Select tool under a palette button that still says Taxiway. In the editor, Escape ends
	// the gesture and the palette changes tools. What changed is only WHERE the session
	// lives, not which tool this instance speaks for.
	Tool->OnCancel(MakeHoverContext());

	Gesture.Cancel();
}

void URoadBuildEditorTool::CommitGesture()
{
	IBuildTool* Tool = Sess().GetActiveTool();
	if (Tool == nullptr || Target == nullptr)
	{
		return;
	}

	// UNDOABLE LIKE ANY OTHER EDIT (issue #185): OnCommit is what places the fuel depot -
	// FPlotPlaceTool::OnCommit calls IRoadEditTarget::PlaceEntityInPlot - so Modify() has to
	// run before that happens, exactly the same transaction shape CancelGesture and a plain
	// click already use above. Ctrl+Z removing a placed depot is the whole point of this
	// verb existing in an editor world rather than PIE's Memento history (see the class
	// comment's "UNDO IS THE EDITOR'S").
	FScopedRoadBuildTransaction Transaction(LOCTEXT("RoadBuildCommit", "Road Build"), Target);
	Tool->OnCommit(MakeHoverContext());
}

void URoadBuildEditorTool::Render(IToolsContextRenderAPI* RenderAPI)
{
	IBuildTool* Tool = Sess().GetActiveTool();
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

		// UNCAPPED (issue #191/#92-#93): every ray under an orthographic projection shares the
		// camera's own direction, so the horizon-runaway RayToPlane's cap guards against - a
		// ray nearly parallel to the plane sending Distance toward infinity - cannot happen
		// here regardless of where on screen a click lands. See ViewCentreDistance's own
		// comment for the perspective case this exempts.
		ViewCentreDistance = -1.0;
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

		// STORED, not just spent on ViewWorldWidth below (issue #191/#92-#93): RayToPlane
		// reads this back to cap a click the same way ARoadBuildController::CursorOnRoadPlane
		// caps one against the build camera's own distance - see ViewCentreDistance's header
		// comment for why this used to be computed and thrown away.
		ViewCentreDistance = Distance;

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

	// Gated on a real hover. Before the first mouse move the session's LastPlaneHit is
	// (0,0), and the idle marker was drawing a corner at the world origin.
	if (bHoverValid)
	{
		Tool->BuildPreview(MakeHoverContext(), Sink);
	}

}

void URoadBuildEditorTool::DrawHUD(FCanvas* Canvas, IToolsContextRenderAPI* RenderAPI)
{
	// SURFACES BuildReadout IN THE EDITOR (issue #185). Nothing here ever called it before -
	// FViewportPreviewSink::Label is "deliberately nothing" because a PrimitiveDrawInterface
	// draws geometry, not text, which is exactly why this uses DrawHUD's own FCanvas instead
	// of Render's PDI: the same split ARoadBuildHUD keeps between its world-space ghost
	// (DrawNodes/DrawStands) and its canvas panel (DrawPlotPanel). Without this, a fuel
	// depot's bay counts, its warnings and whether it could even commit were invisible in the
	// editor while the runtime HUD had always shown them.
	IBuildTool* Tool = Sess().GetActiveTool();
	const FSceneView* SceneView = RenderAPI != nullptr ? RenderAPI->GetSceneView() : nullptr;
	if (Tool == nullptr || Target == nullptr || Canvas == nullptr || SceneView == nullptr || !bHoverValid)
	{
		return;
	}

	// A FRESH COLLECTOR EACH FRAME, never a member: FToolReadoutCollector::Reset's own
	// comment is exactly why - a fact left over from last frame would describe a gesture the
	// player has already changed.
	FToolReadoutCollector Collector;
	Tool->BuildReadout(MakeHoverContext(), Collector);

	TArray<FString> Lines;
	for (const TPair<FString, FString>& Fact : Collector.Readout.Facts)
	{
		Lines.Add(FString::Printf(TEXT("%s: %s"), *Fact.Key, *Fact.Value));
	}
	for (const FString& Warning : Collector.Readout.Warnings)
	{
		Lines.Add(Warning);
	}

	// THE HINT, not a second opinion about whether Enter would do anything - bCommittable IS
	// the answer, same as it is for PIE's Build button (BuildActions.cpp's edit.build reads
	// this exact field). THE KEY COMES OFF THE COMMAND ITSELF, not typed here, for the reason
	// ARoadBuildHUD::CommitPromptText's own comment gives about two sources of truth.
	if (Collector.Readout.bCommittable)
	{
		const TSharedPtr<FUICommandInfo>& BuildCommand = FRoadBuildEdModeCommands::Get().Build;
		Lines.Add(BuildCommand.IsValid()
			? FString::Printf(TEXT("Build  [%s]"), *BuildCommand->GetInputText().ToString())
			: TEXT("Build"));
	}

	if (Lines.Num() == 0)
	{
		return;
	}

	UFont* Font = GEngine != nullptr ? GEngine->GetMediumFont() : nullptr;
	if (Font == nullptr)
	{
		return;
	}

	// THE SAME CURSOR THE GHOST IS DRAWN AT (LastPlaneHit), projected with the view's OWN
	// camera rather than re-deriving one - RenderAPI->GetSceneView() is exactly what the
	// engine's own tools use for this (UMeshInspectorTool::DrawHUD, MeshModelingToolsExp).
	FVector2D PixelPos;
	const FVector WorldPos(Sess().LastPlaneHit(), Target->SurfaceZ);
	if (!SceneView->WorldToPixel(WorldPos, PixelPos))
	{
		return;
	}

	// DPI-SCALED, like DrawShadowedString's every other caller in the engine: PixelPos is a
	// physical pixel from the scene view, and Canvas expects DPI-independent coordinates.
	const float DPIScale = Canvas->GetDPIScale();
	float Y = static_cast<float>(PixelPos.Y) / DPIScale;
	for (const FString& Line : Lines)
	{
		Canvas->DrawShadowedString(static_cast<float>(PixelPos.X) / DPIScale, Y, *Line, Font, FLinearColor::White);
		Y += Font->GetMaxCharHeight();
	}
}

#undef LOCTEXT_NAMESPACE
