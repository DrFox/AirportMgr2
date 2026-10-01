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
#include "Present/AirsideBuildingsActor.h"
#include "Present/PlotPresenter.h"
#include "Present/RoadNetworkActor.h"
#include "RoadBuildEdModeCommands.h"
#include "ScopedTransaction.h"
#include "SceneManagement.h"
#include "Solve/RoadGeom.h"
#include "Tool/GridOverlay.h"
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
			// NO PDI is a supported caller now (issue #304): CachePreviewLabelsForTest builds one
			// of these with PDI=nullptr purely to run BuildPreview for its Label() calls headlessly
			// - see that function's own comment. Marker/Line/CrossMark below guard it for exactly
			// that caller; Render's own construction always has a real PDI (checked before this
			// class is ever built there), so nothing changes for the 3D viewport.
			if (PDI == nullptr)
			{
				return;
			}

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
			// See Marker's own comment on the null guard.
			if (PDI == nullptr)
			{
				return;
			}

			// bScreenSpace = TRUE. Thickness is otherwise WORLD units: 3 uu is 3 cm, a
			// hairline over an airport, which is why these read as far thinner than the
			// runtime HUD's pixel-width lines.
			PDI->DrawLine(Lift(From), Lift(To), Colour(Style), SDPG_Foreground,
				3.0f, 0.0f, true);
		}

		virtual void CrossMark(const FVector2D& At, const FVector2D& Along, EPreviewStyle Style) override
		{
			// See Marker's own comment on the null guard.
			if (PDI == nullptr || Along.IsNearlyZero())
			{
				return;
			}

			const FVector2D Across(-Along.Y, Along.X);
			const double Arm = RadiusAt(At) * 1.25;
			PDI->DrawLine(Lift(At - Across * Arm), Lift(At + Across * Arm),
				Colour(Style), SDPG_Foreground, 3.0f, 0.0f, true);
		}

		/**
		 * COLLECTS, no longer "deliberately nothing" (issue #304). A PrimitiveDrawInterface
		 * still draws geometry, not text - Marker/Line/CrossMark above still need one - but
		 * DrawHUD's FCanvas can draw text, and until now had no way to reach what a tool's
		 * preview described here: every refusal reason ("too short: %.0f m", WhyStandRefused,
		 * every guide description, the purse quote) existed only in PIE's ARoadBuildHUD::Label.
		 * Render's own call - the one with a real PDI - reads CollectedLabels() straight back
		 * afterwards and caches it on PendingLabels for DrawHUD; see Render's own comment.
		 */
		virtual void Label(const FVector2D& At, const FString& Text, EPreviewStyle Style) override
		{
			Labels.Add({ At, Text, Style });
		}

		/** What Label collected this call - read by Render (a real Sink) and
		 *  CachePreviewLabelsForTest (a null-PDI one standing in for it). */
		const TArray<FEditorPreviewLabel>& CollectedLabels() const { return Labels; }

	private:
		FVector Lift(const FVector2D& Plane) const { return FVector(Plane.X, Plane.Y, PlaneZ); }
		FVector Camera = FVector::ZeroVector;
		double PerDistance = 0.0;
		double FixedRadius = 0.0;
		TArray<FEditorPreviewLabel> Labels;

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
	// HeldInput's remove/insert CARRIED, not left at their false defaults (issue #191/#92-#93):
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
	SelectContext.bRemoveModifier = HeldInput.bRemoveModifier;
	SelectContext.bInsertModifier = HeldInput.bInsertModifier;
	Sess().SelectTool(ToolIndex, SelectContext);

	// GHOST BAYS FOLLOW THE LIT TOOL, asked of the session like the node rings in Render. At
	// Setup rather than every frame, unlike ARoadBuildController::PlayerTick: here the lit
	// tool only changes by a new tool instance being set up, so this IS every change.
	SetPlotGhostsVisible(Sess().WantsPlotGhostsDrawn());

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

void URoadBuildEditorTool::SetPlotGhostsVisible(bool bVisible) const
{
	const UWorld* World = GetToolManager() != nullptr ? GetToolManager()->GetWorld() : nullptr;
	if (AAirsideBuildingsActor* Buildings = AAirsideBuildingsActor::Find(World))
	{
		if (UPlotPresenter* Plots = Buildings->GetPlotPresenter())
		{
			Plots->SetGhostsVisible(bVisible);
		}
	}
}

void URoadBuildEditorTool::Shutdown(EToolShutdownType ShutdownType)
{
	// Leaving the tool abandons whatever it had part-drawn, exactly as switching tools does
	// at runtime. A chain resumed after a mode change would be a click landing on something
	// begun before the user went away.
	if (IBuildTool* Tool = Sess().GetActiveTool(); Tool != nullptr && Target != nullptr)
	{
		Tool->OnDeactivate(MakeHoverContext());
		Sess().InvalidateFrameContextCache();
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

	// BACK TO VISIBLE on the way out: a level with no Road Build tool active is being
	// designed, and UPlotPresenter's default says so. The next tool's Setup, if any, sets its
	// own answer straight after.
	SetPlotGhostsVisible(true);

	UInteractiveTool::Shutdown(ShutdownType);
}

void URoadBuildEditorTool::DeactivateOnUndo()
{
	// THE SESSION'S OWN ANSWER TO A REPLACED NETWORK (FBuildSession::OnNetworkReplaced, #426) - the
	// same call PIE's ARoadBuildController::OnNetworkReplaced makes when the facade announces an undo,
	// rather than a hand copy of it: the active tool deactivated, the frame context retired. ADOPTED, as
	// an undo is: the transactor restored this graph's own slots, so the selection keeps its index. See
	// this method's header comment for why a mid-drag transaction is deliberately NOT handled here too.
	Sess().OnNetworkReplaced(MakeHoverContext(), ENetworkReplace::Adopted);
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
	ARoadNetworkActor* Road = ARoadNetworkActor::FindOrCreate(World);
	// See URoadBuildEdMode::MakeReselectContext: the buildings actor is created beside it.
	AAirsideBuildingsActor::FindOrCreate(World, Road);
	return Road;
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
	//
	// THROUGH THE SESSION'S CACHE, not a fresh MakeContext every time - issue #303. OnUpdateHover,
	// Render and DrawHUD each funnel through this one function, and within one frame they ask it
	// with the SAME plane hit (OnUpdateHover records it; Render/DrawHUD read it back via
	// MakeHoverContext) and the same tunables/modifiers, so the whole snap + guide pipeline ran
	// up to three times over for one cursor position before this. A key MISS - the cursor moved,
	// the view zoomed, the lit tool changed - still rebuilds exactly as MakeContext always did;
	// see FBuildSession::GetFrameContext.
	return Sess().GetFrameContext(Target, Plane, Tunables, HeldInput);
}

void URoadBuildEditorTool::OnUpdateModifierState(int ModifierID, bool bIsOn)
{
	if (ModifierID == RemoveModifierId) { HeldInput.bRemoveModifier = bIsOn; }
	if (ModifierID == InsertModifierId) { HeldInput.bInsertModifier = bIsOn; }
	if (ModifierID == SuspendModifierId) { HeldInput.bSuspendGuides = bIsOn; }
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

	// A drag step can move the graph (issue #303's cache has no other way to see it) with the
	// cursor unmoved from the position that step itself just read - the same reasoning
	// ARoadBuildController::UpdateDrag pairs with InvalidateToolReadoutCache.
	Sess().InvalidateFrameContextCache();
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

		// A drag end commits the graph the cache last saw mid-drag - see OnClickDrag's own
		// invalidation for the same reasoning.
		Sess().InvalidateFrameContextCache();
		return;
	}

	// A press that never travelled was a click. Its own transaction, so one click is one
	// Ctrl+Z rather than part of whatever came before.
	FScopedRoadBuildTransaction Transaction(LOCTEXT("RoadBuildClick", "Road Build"), Target);
	Tool->OnClick(MakeContext(ReleasePos));

	// A click can build or remove, exactly the class of change issue #303's cache cannot see on
	// its own - the editor twin of ARoadBuildController::OnPrimaryReleased's own invalidation.
	Sess().InvalidateFrameContextCache();
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

		// A mid-drag cancel can un-build whatever the drag had staged - see OnClickDrag's own
		// invalidation.
		Sess().InvalidateFrameContextCache();
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

	// A cancel can heal a deletion it undoes or drop a part-drawn chain's stray node - either
	// way the graph the cache last saw may no longer be current, exactly what
	// ARoadBuildController::OnCancelGesture pairs with InvalidateToolReadoutCache for.
	Sess().InvalidateFrameContextCache();

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

	// A commit places whatever the gesture staged - the editor twin of
	// ARoadBuildController::OnBuild's own invalidation.
	Sess().InvalidateFrameContextCache();
}

void URoadBuildEditorTool::ApplyVerb(const FBuildVerbRegistration& Verb)
{
	if (Target == nullptr)
	{
		return;
	}

	// A TRANSACTION LIKE ANY OTHER EDIT, for the same reason CancelGesture's own comment gives:
	// entering or leaving a mode DEACTIVATES whatever tool the session is leaving
	// (FBuildSession::SetGestureMode calls Outgoing->OnDeactivate), and OnDeactivate can itself
	// touch the graph - a draw tool mid-chain drops the node it stranded. Wrapping every path
	// that can reach OnDeactivate, not only the ones that obviously place or remove something,
	// is what CancelGesture already does and what this verb dispatch must match.
	FScopedRoadBuildTransaction Transaction(LOCTEXT("RoadBuildVerb", "Road Build"), Target);

	// THE EDITOR'S OWN DOOR ONTO BuildVerbRegistry() (issue #304): Remove/Insert/Edit reach
	// FBuildSession::ToggleGestureMode through here, exactly the way CancelGesture/CommitGesture
	// reach CancelActiveGesture/IBuildTool::OnCommit above. GetActiveTool() already returns
	// FEditTool the instant the session's mode is Edit (FBuildSession::GetActiveTool's own
	// comment), so no OTHER change makes a node drag or an apron-corner drag reachable here -
	// this is the one missing door, not a second implementation of what is behind it.
	Verb.Apply(Sess(), MakeHoverContext());
	Sess().InvalidateFrameContextCache();
}

bool URoadBuildEditorTool::SelectVariant(int32 Axis, int32 Option)
{
	if (Target == nullptr)
	{
		UE_LOG(LogAirsideEditor, Warning, TEXT("Variant row %d -> option %d ignored: the tool has no target"), Axis, Option);
		return false;
	}

	// A TRANSACTION, ApplyVerb's reason: FRoadDrawTool::SelectVariant's Mode switch cancels a
	// part-drawn chain (State->OnCancel), which can remove the node it stranded - an edit the
	// editor's undo must be able to reach, like every other path into OnCancel here.
	FScopedRoadBuildTransaction Transaction(LOCTEXT("RoadBuildVariant", "Road Build"), Target);

	// THE SESSION'S DOOR, the one PIE's ARoadBuildController::SelectActiveVariant calls - so a
	// pick is remembered for the next launch (FBuildSession::RememberSurfaces) in both drivers
	// the same way, which is how a stand's pavement stops being whatever PIE last wrote (#440).
	const bool bTook = Sess().SelectActiveVariant(MakeHoverContext(), Axis, Option);
	const IBuildTool* Active = Sess().GetActiveTool();
	UE_LOG(LogAirsideEditor, Log, TEXT("Variant: %s row %d -> option %d (%s)"),
		Active != nullptr ? *Active->GetDisplayName().ToString() : TEXT("no tool"),
		Axis, Option, bTook ? TEXT("taken") : TEXT("refused"));
	if (!bTook)
	{
		// A refusal changed nothing (SelectVariant's contract), so it leaves no undo step behind.
		Transaction.Cancel();
	}

	// A pick changes what the next click lays - the width, the surface, Build against Upgrade -
	// without moving any input the frame-context key holds.
	Sess().InvalidateFrameContextCache();
	return bTook;
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

	// ONE FUNCTION DESCRIBES THE FRAME, and the headless seam calls the same one (see
	// DescribeFrame): the seam used to be a hand-kept copy of the tail of this function, so a Render
	// that cached nothing left Airside.Editor.DrawHUDShowsRefusalLabels green (review 2026-09-30).
	DescribeFrame(*Tool, PDI, Camera.Position, PerDistance, FixedRadius);
}

void URoadBuildEditorTool::DescribeFrame(IBuildTool& Tool, FPrimitiveDrawInterface* PDI,
	const FVector& CameraPosition, double PerDistance, double FixedRadius)
{
	if (Target == nullptr)
	{
		PendingLabels.Reset();
		return;
	}

	// PDI MAY BE NULL (issue #304): CachePreviewLabelsForTest runs this with none, purely so a
	// headless test collects the labels a real Render would - FViewportPreviewSink's Marker/Line/
	// CrossMark null-guard it, and Label collects either way.
	FViewportPreviewSink Sink(PDI, Target->SurfaceZ, CameraPosition, PerDistance, FixedRadius);

	// The COMMITTED graph first, then the tool's intent on top. The runtime HUD does this
	// in ARoadBuildHUD::DrawNodes/DrawStands; in the editor nothing did, so existing nodes
	// and stands were invisible and there was no way to see what a snap would attach to.
	DrawPersistentState(Sink);

	// Gated on a real hover. Before the first mouse move the session's LastPlaneHit is
	// (0,0), and the idle marker was drawing a corner at the world origin.
	//
	// PendingLabels IS CACHED HERE, FROM THIS SAME CALL - review round 2 of issue #304. The
	// first version of this fix ran a SECOND, independent Tool->BuildPreview from DrawHUD to
	// collect labels, discarding what THIS call already produced into Sink - doubling the cost
	// of every tool's preview every frame for text this Sink was going to describe anyway.
	// FViewportPreviewSink::Label collects regardless of whether PDI is real, so the one call
	// Render already makes (to draw markers/lines with a real PDI) is also the one that fills
	// the cache DrawHUD reads. THE CLAIM "BuildPreview RUNS ONCE A FRAME, FROM HERE" is enforced
	// by Check-Architecture rule 'editor-preview-described-once', which reads every function in this
	// file - BuildPreview( only here, DescribeFrame( only from Render and the seam - where the test
	// that used to count it (Airside.Editor.RenderCachesLabelsOnce) only ever counted its own seam.
	if (bHoverValid)
	{
		// ONE CONTEXT FOR BOTH, so the grid drawn is the grid the ghost is landing on. The grid
		// first, under the gesture - see GridOverlay; the PIE HUD makes the same two calls.
		const FToolContext HoverContext = MakeHoverContext();
		GridOverlay::Describe(HoverContext, Sink);
		Tool.BuildPreview(HoverContext, Sink);
		PendingLabels = Sink.CollectedLabels();
	}
	else
	{
		PendingLabels.Reset();
	}
}

void URoadBuildEditorTool::CachePreviewLabelsForTest()
{
	// STANDS IN FOR Render (same precedent as SetViewCentreDistanceForTest/HoverFrameContextForTest:
	// a real IToolsContextRenderAPI/FPrimitiveDrawInterface needs a live viewport this headless
	// harness does not have) BY CALLING RENDER'S OWN DescribeFrame - not a second copy of its tail.
	// NO PDI: PDI-dependent drawing does not matter for labels, so the SAME single BuildPreview call
	// collects the SAME labels a real Render call would, whatever else it also draws.
	IBuildTool* Tool = Sess().GetActiveTool();
	if (Tool == nullptr)
	{
		PendingLabels.Reset();
		return;
	}
	DescribeFrame(*Tool, nullptr, FVector::ZeroVector, 0.0, 0.0);
}

TArray<FString> URoadBuildEditorTool::CollectPreviewLabelTextForTest() const
{
	// READS THE CACHE ONLY - no BuildPreview call here any more (review round 2 of issue #304).
	// This is DrawHUD's own half of the contract CachePreviewLabelsForTest's comment describes:
	// whatever populated PendingLabels (Render, for real; CachePreviewLabelsForTest, in a test),
	// this reads it back rather than asking the tool a second time.
	TArray<FString> Out;
	Out.Reserve(PendingLabels.Num());
	for (const FEditorPreviewLabel& Label : PendingLabels)
	{
		Out.Add(Label.Text);
	}
	return Out;
}

void URoadBuildEditorTool::DrawHUD(FCanvas* Canvas, IToolsContextRenderAPI* RenderAPI)
{
	// SURFACES BuildReadout IN THE EDITOR (issue #185), AND EVERY PREVIEW LABEL (issue #304) -
	// PrimitiveDrawInterface draws geometry, not text, which is exactly why both use DrawHUD's
	// own FCanvas instead of Render's PDI: the same split ARoadBuildHUD keeps between its
	// world-space ghost (DrawNodes/DrawStands) and its canvas panel (DrawPlotPanel). Before
	// #185, a fuel depot's bay counts, warnings and whether it could even commit were invisible
	// in the editor while the runtime HUD had always shown them. Before #304,
	// FViewportPreviewSink::Label was "deliberately nothing" for the identical reason and every
	// refusal reason a tool describes - "too short: %.0f m", WhyStandRefused, every guide
	// description - existed only in PIE; see DescribeFrame's own comment on PendingLabels, which this
	// function only READS - it does not run BuildPreview a second time (rule 'editor-preview-described-once').
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

	// EVERY LABEL THE TOOL'S OWN PREVIEW DESCRIBES (issue #304) - "too short: %.0f m",
	// WhyStandRefused, every guide description, the purse quote. READ FROM PendingLabels, NOT
	// RE-COLLECTED: review round 2 of issue #304 found this function running a SECOND,
	// independent Tool->BuildPreview here to gather them, discarding what Render's own call had
	// already produced into its Sink - doubling every tool's preview cost every frame for text
	// Render's Sink was going to describe anyway. See DescribeFrame's own comment on PendingLabels.
	// ENFORCED BY: Check-Architecture rule 'editor-preview-described-once' (BuildPreview( is called from DescribeFrame alone, and DescribeFrame( from
	// Render and the headless seam alone - so DrawHUD asks for no description of its own)
	if (Lines.Num() == 0 && PendingLabels.Num() == 0)
	{
		return;
	}

	UFont* Font = GEngine != nullptr ? GEngine->GetMediumFont() : nullptr;
	if (Font == nullptr)
	{
		return;
	}

	// DPI-SCALED, like DrawShadowedString's every other caller in the engine: WorldToPixel
	// returns a physical pixel from the scene view, and Canvas expects DPI-independent
	// coordinates.
	const float DPIScale = Canvas->GetDPIScale();

	if (Lines.Num() > 0)
	{
		// THE SAME CURSOR THE GHOST IS DRAWN AT (LastPlaneHit), projected with the view's OWN
		// camera rather than re-deriving one - RenderAPI->GetSceneView() is exactly what the
		// engine's own tools use for this (UMeshInspectorTool::DrawHUD, MeshModelingToolsExp).
		FVector2D PixelPos;
		const FVector WorldPos(Sess().LastPlaneHit(), Target->SurfaceZ);
		if (SceneView->WorldToPixel(WorldPos, PixelPos))
		{
			float Y = static_cast<float>(PixelPos.Y) / DPIScale;
			for (const FString& Line : Lines)
			{
				Canvas->DrawShadowedString(static_cast<float>(PixelPos.X) / DPIScale, Y, *Line, Font, FLinearColor::White);
				Y += Font->GetMaxCharHeight();
			}
		}
	}

	// EACH LABEL AT ITS OWN PLANE POSITION, not stacked with Lines above: a refusal reason
	// belongs beside the point it describes (FRunwayTool::BuildPreview's Far threshold, say),
	// which is not necessarily where the cursor sits - PIE's own ARoadBuildHUD::Label makes the
	// identical choice, projecting the label's own At rather than the readout's cursor. COLOURED
	// BY STYLE, not the readout's flat white, the same palette FViewportPreviewSink's Marker/
	// Line/CrossMark already draw the rest of the preview in.
	for (const FEditorPreviewLabel& Label : PendingLabels)
	{
		FVector2D LabelPixelPos;
		if (!SceneView->WorldToPixel(FVector(Label.At, Target->SurfaceZ), LabelPixelPos))
		{
			continue;
		}
		Canvas->DrawShadowedString(static_cast<float>(LabelPixelPos.X) / DPIScale,
			static_cast<float>(LabelPixelPos.Y) / DPIScale, *Label.Text, Font,
			PreviewPalette::Default(Label.Style));
	}
}

#undef LOCTEXT_NAMESPACE
