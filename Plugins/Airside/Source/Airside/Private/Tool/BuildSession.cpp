#include "Tool/BuildSession.h"

#include "AirsideLog.h"
#include "Model/RoadNetwork.h"
#include "Tool/ApronDrawTool.h"
#include "Tool/GuidelineDrawTool.h"
#include "Tool/HoldingPointTool.h"
#include "Tool/PlotPlaceTool.h"
#include "Tool/RoadDrawTool.h"
#include "Tool/RoadEditTarget.h"
#include "Tool/RunwayTool.h"
#include "Tool/SelectTool.h"
#include "Tool/StandPlotTool.h"

#define LOCTEXT_NAMESPACE "BuildSession"

TConstArrayView<FToolRegistration> ToolRegistry()
{
	// A function-local static rather than a file-scope global: constructed exactly once,
	// on first use, in the order written here - which IS the key order, 1 through 3, 5, 6,
	// 8, 9 then 0 (see the holding-position entry for why seven is skipped; 4 is the Select
	// tool, added at index 0 - see FSelectTool). Never
	// mutated after that first construction, so handing out a view over it is safe from
	// any thread that only reads.
	//
	// Name repeats each tool's own GetDisplayName() text rather than inventing a second
	// label for the same thing - Airside.Tool.BuildSession asserts the two cannot drift,
	// which is exactly the class of bug this table exists to make impossible elsewhere.
	static const FToolRegistration Registry[] =
	{
		// INDEX 0 IS THE DEFAULT STATE (spec 2026-09-07-entity-inspector §2): the session
		// opens here and CancelActiveGesture returns here. Key 4 because that was the route
		// tool's, whose slot this fills; the printed keys 1-3 keep their meaning.
		{ EKeys::Four,  TEXT("Select"),   LOCTEXT("Select",    "Select"),
			LOCTEXT("SelectTooltip", "Click an aircraft or a stand to inspect it. Escape deselects."),
			[] { return MakeUnique<FSelectTool>(); },
			EEditHandleKind::None },

		{ EKeys::One,   TEXT("Taxiway"),  LOCTEXT("Taxiway",   "Taxiway"),
			LOCTEXT("TaxiwayTooltip", "Draw taxiways: click to chain, ctrl to remove, shift to insert a node. The taxiway key pressed again cycles the width. Press M to edit placed nodes."),
			[] { return MakeUnique<FRoadDrawTool>(ERoadKind::Taxiway); },
			EEditHandleKind::AirsideNode, /*bShowsRoadNodes*/ true },
		{ EKeys::Two,   TEXT("Apron"),    LOCTEXT("Apron",     "Apron"),
			LOCTEXT("ApronTooltip", "Draw a polygon of pavement; click the first corner again to close it."),
			[] { return MakeUnique<FApronDrawTool>(); },
			EEditHandleKind::ApronCorner },
		{ EKeys::Three, TEXT("Stand"),    LOCTEXT("Stand",     "Stand"),
			LOCTEXT("StandTooltip", "Place an aircraft stand: click a taxiway to start the entrance, drag along it for width, away from it for depth, then press Build. Bigger stands take bigger aircraft."),
			[] { return MakeUnique<FStandPlotTool>(); },
			EEditHandleKind::None },
		{ EKeys::Five,  TEXT("Guideline"), LOCTEXT("Guideline", "Guidelines"),
			LOCTEXT("GuidelineTooltip", "Draw a routing link the derivation never made: click a node, click another."),
			[] { return MakeUnique<FGuidelineDrawTool>(); },
			EEditHandleKind::None },
		{ EKeys::Six,   TEXT("Runway"),   LOCTEXT("Runway",    "Runway"),
			// "IN PLAY" DROPPED (issue #191/#92-#93): the reselect cycle used to work only in
			// PIE - this tooltip said so - because the editor's reselect context carried no
			// modifiers at all. URoadBuildEdMode::StartToolAction now reads the active tool's
			// own Ctrl/Shift state into it, so the qualifier stopped being true.
			LOCTEXT("RunwayTooltip", "Click one threshold, then the other. Press the runway key again to cycle the width, Shift for the surface, Ctrl for the approach."),
			[] { return MakeUnique<FRunwayTool>(); },
			EEditHandleKind::RunwayThreshold },

		// EIGHT, not seven: key 7 is "land an aircraft", which is not a tool and is not in
		// this table - see ARoadBuildController::LandAircraftNearViewFocus. Numbering around it keeps
		// the printed key on the bar and the key that actually works the same number.
		{ EKeys::Eight, TEXT("HoldingPosition"), LOCTEXT("HoldingPosition", "Holding point"),
			LOCTEXT("HoldingPositionTooltip", "Click a taxiway junction node to place an intermediate holding position; click it again to remove it. Runway holding positions are derived from the runway."),
			[] { return MakeUnique<FHoldingPointTool>(); },
			EEditHandleKind::None },

		// NINE: the SAME FRoadDrawTool, laying the service road cross-section instead of the
		// taxiway one. One tool, two entries - see FRoadDrawTool's own constructor comment
		// for why this is not a second class.
		{ EKeys::Nine,  TEXT("Road"),     LOCTEXT("Road",      "Road"),
			LOCTEXT("RoadTooltip", "Draw service roads for ground vehicles: click to chain, ctrl to remove, shift to insert a node. Press M to edit placed nodes."),
			[] { return MakeUnique<FRoadDrawTool>(ERoadKind::ServiceRoad); },
			EEditHandleKind::ServiceRoadNode, /*bShowsRoadNodes*/ true },

		// ZERO, after nine: it is the next key along a keyboard's top row, and every other
		// number is spoken for.
		//
		// A DIFFERENT TOOL FROM KEY 3, though the same gesture: FPlotPlaceTool here, snapped
		// to a service road; FStandPlotTool there, snapped to a taxiway, sharing the first
		// click through PlotGesture. Until 2026-09-23 a stand kept press-drag-release on the
		// belief that it had no plot - its extent was its design aircraft's. Drawn stands
		// reversed that: the rectangle's size now DECIDES the letter, so it is the one fact
		// about the stand rather than a second opinion (drawn-stands spec).
		//
		// AND A STAGED GESTURE, not the apron's freeform one it briefly borrowed. That reuse
		// was cheap and PIE showed what it cost - see the plot gesture design doc.
		{ EKeys::Zero,  TEXT("FuelDepot"), LOCTEXT("FuelDepot", "Fuel depot"),
			LOCTEXT("FuelDepotTooltip", "Place a fuel depot: click a service road to anchor it, drag along the road for width, away from it for depth, then press Build."),
			[] { return MakeUnique<FPlotPlaceTool>(EPlaceableEntity::FuelDepot); },
			EEditHandleKind::None },
	};
	return TConstArrayView<FToolRegistration>(Registry);
}

TConstArrayView<FBuildVerbRegistration> BuildVerbRegistry()
{
	// A function-local static, like ToolRegistry() and for the same reason: built once, on
	// first use, never mutated after - so handing out a view over it is safe from any thread
	// that only reads. See FBuildVerbRegistration's own comment for why Build/Cancel/Guidelines
	// are not entries here.
	static const FBuildVerbRegistration Registry[] =
	{
		// NO KEY: Ctrl already means this while held (FBuildSession::MakeContext ORs the two).
		// This entry is the STICKY form, for work that outlasts a comfortable reach - moved
		// here from BuildActions.cpp's own hand-written row, unchanged in behaviour.
		{ EKeys::Invalid, TEXT("Remove"), LOCTEXT("Remove", "Remove"),
			LOCTEXT("RemoveTooltip", "Sticky: a gesture removes rather than builds, the same as holding Ctrl."),
			[](FBuildSession& Session, const FToolContext& Context)
			{
				Session.ToggleGestureMode(EGestureMode::Remove, Context);
			},
			[](const FBuildSession& Session) { return Session.GetGestureMode() == EGestureMode::Remove; },
			[](const FBuildSession&) { return true; } },

		// NO KEY, for the same reason as Remove above: Shift already means this while held.
		{ EKeys::Invalid, TEXT("Insert"), LOCTEXT("Insert", "Insert"),
			LOCTEXT("InsertTooltip", "Sticky: a gesture inserts without starting anything, the same as holding Shift."),
			[](FBuildSession& Session, const FToolContext& Context)
			{
				Session.ToggleGestureMode(EGestureMode::Insert, Context);
			},
			[](const FBuildSession& Session) { return Session.GetGestureMode() == EGestureMode::Insert; },
			[](const FBuildSession&) { return true; } },

		// EDIT IS THE ONE WITH A KEY, because it is the one you enter deliberately and stay in -
		// M, not E (Q/E is camera turn, polled every frame in PIE's UpdateView), and mnemonic
		// for move and merge. GREYED when the lit tool exposes no handles, so the bar (and, once
		// wired, the editor palette) answers "why can I not edit this" rather than lighting over
		// a mode that would do nothing at all - see FToolRegistration::EditHandles, the one list
		// this reads instead of a second copy of which tools are editable.
		{ EKeys::M, TEXT("EditMode"), LOCTEXT("EditMode", "Edit"),
			LOCTEXT("EditModeTooltip", "Move placed nodes and apron corners. Greyed when the lit tool has none."),
			[](FBuildSession& Session, const FToolContext& Context)
			{
				Session.ToggleGestureMode(EGestureMode::Edit, Context);
			},
			[](const FBuildSession& Session) { return Session.GetGestureMode() == EGestureMode::Edit; },
			[](const FBuildSession& Session)
			{
				const TConstArrayView<FToolRegistration> Tools = ToolRegistry();
				const int32 Index = Session.GetActiveToolIndex();
				return Tools.IsValidIndex(Index) && Tools[Index].EditHandles != EEditHandleKind::None;
			} },
	};
	return TConstArrayView<FBuildVerbRegistration>(Registry);
}

FBuildSession::FBuildSession()
{
	for (const FToolRegistration& Registration : ToolRegistry())
	{
		Tools.Add(Registration.Make());
	}
}

IBuildTool* FBuildSession::GetActiveTool() const
{
	// THE ONE PLACE THE MODE IS HONOURED, and what makes "Edit suppresses the build tool"
	// structural rather than a rule nine tools each have to remember. Both drivers reach
	// every tool through this function - nine call sites in ARoadBuildController, eight in
	// URoadBuildEditorTool - so returning the edit tool here is the whole switch, and
	// neither driver needed a line changed for it.
	if (Mode == EGestureMode::Edit)
	{
		return &EditTool;
	}
	return Tools.IsValidIndex(ActiveTool) ? Tools[ActiveTool].Get() : nullptr;
}

void FBuildSession::SetGestureMode(EGestureMode InMode, const FToolContext& DeactivateContext)
{
	if (InMode == Mode)
	{
		return;
	}

	// The outgoing tool abandons whatever it had part-drawn, exactly as SelectTool does and
	// for the same reason: switching to Edit mid-chain and back would otherwise resume a
	// road the player stopped drawing in order to go and fix something else.
	if (IBuildTool* Outgoing = GetActiveTool())
	{
		Outgoing->OnDeactivate(DeactivateContext);
	}

	Mode = InMode;

	// A MODE CHANGE ALWAYS CHANGES WHAT MakeContext WOULD ANSWER - EditHandles at least - so
	// GetFrameContext's cache would already miss on GetActiveTool() changing to/from the edit
	// tool. Cleared explicitly anyway rather than left to that side effect: issue #303 wants
	// the session's own mutators to retire their own cache, the same way a setter would
	// invalidate a derived member it keeps.
	InvalidateFrameContextCache();

	// NAMED, not a number: this line is how "which mode am I actually in" gets answered from
	// the log rather than guessed at, which is the failure the report that prompted the merge
	// took the long way round.
	const TCHAR* Named =
		Mode == EGestureMode::Edit   ? TEXT("Edit")   :
		Mode == EGestureMode::Remove ? TEXT("Remove") :
		Mode == EGestureMode::Insert ? TEXT("Insert") : TEXT("Build");
	UE_LOG(LogAirside, Log, TEXT("Gesture mode -> %s"), Named);
}

void FBuildSession::SelectTool(int32 Index, const FToolContext& DeactivateContext)
{
	if (!Tools.IsValidIndex(Index))
	{
		return;
	}
	if (Index == ActiveTool)
	{
		// The key the tool is already lit under: a reselect, not a switch. The context is
		// the caller's, so its modifiers are the ones held with the key.
		if (IBuildTool* Active = GetActiveTool())
		{
			Active->OnReselect(DeactivateContext);
		}

		// GetFrameContext's key cannot see this: GetActiveTool() returns the SAME pointer, but
		// OnReselect can still drop a sticky Remove/Insert modifier below - the exact edge case
		// ARoadBuildController::SelectTool's own comment names for issue #190's cache, which
		// issue #303's cache inherits unchanged.
		InvalidateFrameContextCache();
		return;
	}

	// The outgoing tool abandons whatever it had part-drawn. Left alone it would reappear
	// on the next selection as a chain the player started minutes ago and has forgotten.
	if (IBuildTool* Outgoing = GetActiveTool())
	{
		Outgoing->OnDeactivate(DeactivateContext);
	}

	ActiveTool = Index;

	// A REAL SWITCH ALSO CHANGES GetActiveTool()'s POINTER, which GetFrameContext's key already
	// includes - but OnDeactivate above can itself change what the OUTGOING tool would answer
	// (its own stage), and cleared here rather than relied upon is cheaper to read than to prove.
	InvalidateFrameContextCache();

	// A STICKY BUILD MODIFIER WAS CHOSEN FOR THE TOOL IT WAS LIT UNDER, so picking another
	// drops it - what stops a Remove left on from the road tool deleting the first stand the
	// player clicks. Moved here from ARoadBuildController::SelectTool with the modes.
	//
	// EDIT SURVIVES, and the difference is not an exception grudgingly carved out: Remove and
	// Insert modify what a BUILD gesture does, and choosing a new build tool is choosing a new
	// gesture, so the modifier belonged to the old one. Edit is not a build gesture at all -
	// the lit tool only says which handles it exposes, so switching tools WHILE editing is the
	// ordinary way to go from moving taxiway nodes to moving apron corners.
	if (Mode == EGestureMode::Remove || Mode == EGestureMode::Insert)
	{
		Mode = EGestureMode::Build;
	}

	// A build tool is modal over the airport, not over a thing in it: the selection closes
	// with the panel when one opens, and does not come back when it is cancelled.
	if (Index != 0)
	{
		Selection.Clear();
	}
}

bool FBuildSession::ResolveSnap(const URoadNetwork* Network, const FRoadSnapQuery& Query,
	const FRoadSnapSettings& Snap, FRoadSnapResult& Out) const
{
	Out = FRoadSnapResult();
	Out.Position = Query.Cursor;

	if (Network != nullptr)
	{
		Out = SnapChain.Resolve(*Network, Query, Snap);
	}
	return true;
}

FToolContext FBuildSession::MakeContext(IRoadEditTarget* Target, const FVector2D& PlaneHit,
	const FBuildSessionTunables& Tunables, bool bRemoveModifier, bool bInsertModifier,
	bool bSuspendGuides, int32 HoverAgent) const
{
	// COUNTED FIRST, unconditionally: see MakeContextCallCountForTest's own comment. This is
	// the one function every MakeToolContext call on either driver funnels through, which is
	// what makes it the right chokepoint to count "how many contexts did that operation build"
	// rather than adding a counter to each of the several call sites that reach here.
	++ContextBuildCountForTest;

	FToolContext Context;
	Context.Target = Target;
	Context.HoverAgent = HoverAgent;
	Context.Selection = &Selection;
	Context.Limits = Tunables.Limits;
	Context.Envelopes = Tunables.Envelopes;
	Context.SnapRadius = Tunables.ToolPickRadius;
	// THE STICKY MODE ORS WITH THE HELD KEY. The bar's Remove button and a held Ctrl mean
	// the same thing to a tool, and either lights the same button - the arrangement
	// ARoadBuildController::MakeToolContext used to make with its own EClickModifier, moved
	// here when the modes became one enum so the editor mode gets it too.
	Context.bRemoveModifier = bRemoveModifier || Mode == EGestureMode::Remove;
	Context.bInsertModifier = bInsertModifier || Mode == EGestureMode::Insert;
	Context.bSuspendGuides = bSuspendGuides;

	// WHERE FToolRegistration::EditHandles IS CONSUMED - the one reader, so the tool table
	// stays the one place that mapping is written. The LIT tool decides, not the edit tool:
	// that is what makes Taxiway light taxiway nodes and Apron light apron corners while a
	// single FEditTool serves both.
	//
	// None outside Edit, so nothing can act on a handle kind while FEditTool is not even
	// the tool running.
	const TConstArrayView<FToolRegistration> Registry = ToolRegistry();
	Context.EditHandles = (Mode == EGestureMode::Edit && Registry.IsValidIndex(ActiveTool))
		? Registry[ActiveTool].EditHandles
		: EEditHandleKind::None;

	// Resolved ONCE and carried, rather than each consumer asking again. The tool acts on
	// this and the overlay draws it, so what is highlighted and what happens cannot come
	// from two searches that merely tend to agree.
	FRoadSnapResult Snapped;
	const URoadNetwork* Network = Target != nullptr ? Target->GetNetwork() : nullptr;

	FRoadSnapQuery Query;
	Query.Cursor = PlaneHit;

	// WHAT THE ACTIVE TOOL IS MOVING, so a drag stops snapping to the node in its own hand.
	// Asked here rather than inside the chain because only the tool knows, and only this
	// function holds both the tool and the query. The tool hands over a slot INDEX and this
	// makes the generation-checked handle - the one place a dead slot is refused.
	if (const IBuildTool* Snapping = GetActiveTool(); Snapping != nullptr && Network != nullptr)
	{
		const int32 Exclude = Snapping->GetSnapExclusion();
		if (Exclude != INDEX_NONE)
		{
			Query.ExcludeNode = Network->NodeIdAt(Exclude);
		}
	}

	ResolveSnap(Network, Query, Tunables.Snap, Snapped);

	Context.SetCursor(PlaneHit, Snapped);

	// THE GUIDE, RESOLVED HERE AND NOWHERE ELSE - beside Snap, from the same place, for the
	// same recorded reason (snap-guides design, section 2). The active tool says what it is
	// dragging; this asks the chain what that lines up with and hands the tool the answer, so
	// a tool can neither resolve a guide of its own nor remember one between frames.
	//
	// THE NETWORK GUARD IS NOT COSMETIC: every source takes a URoadNetwork& because stage 2's
	// four of them must query it. With no target there is nothing to guide against and
	// nothing on screen to guide, so the default inactive FResult is the right answer.
	FGuideAnchor Anchor;
	SnapGuide::FResult Guide;
	// ALT SUSPENDS EVERY SOURCE AT ONCE, checked first so no source does any work while the
	// player is holding it. The line below that stores LastGuide unconditionally is what makes
	// releasing Alt start afresh rather than resume the winner it was holding - a held winner
	// surviving a suspend would be the hysteresis rule working against the gesture that asked
	// it to stop.
	const IBuildTool* Tool = GetActiveTool();
	if (!bSuspendGuides && Tool != nullptr && Network != nullptr
		&& Tool->DescribeGuideAnchor(Network, Target, Anchor))
	{
		// A FREE START SWINGS AROUND THE CURSOR, and this is the only place that can say so: the
		// tool is handed the target and never the half-built context, so it declares that it
		// wants the cursor rather than fetching one. See FGuideAnchor::bFreeStart - with the
		// origin ON the cursor every angular candidate sits out inside Arbitrate, which is what
		// leaves exactly the positional guides without a special case anywhere in the chain.
		//
		// THE RAW PLANE HIT, not the snap, for the reason SetCursor draws the same distinction
		// eight lines up: a guide resolved from a snapped position would answer about a point
		// the player did not aim at.
		if (Anchor.bFreeStart)
		{
			Anchor.Origin = PlaneHit;
		}

		Guide = GuideChain.Resolve(*Network, Anchor, PlaneHit, LastGuide, Tunables.GuideSources);
	}

	// ASSIGNED EVEN WHEN NOTHING RESOLVED, which is the clearing half: a gesture that ends
	// must not leave its winner behind for the next one to inherit and then hold on to
	// through the hysteresis rule itself.
	LastGuide = Guide;
	Context.Guide = Guide;
	return Context;
}

const FToolContext& FBuildSession::GetFrameContext(IRoadEditTarget* Target, const FVector2D& PlaneHit,
	const FBuildSessionTunables& Tunables, bool bRemoveModifier, bool bInsertModifier,
	bool bSuspendGuides, int32 HoverAgent) const
{
	FFrameContextKey NewKey;
	NewKey.Target = Target;
	NewKey.Tool = GetActiveTool();
	NewKey.PlaneHit = PlaneHit;
	NewKey.Tunables = Tunables;
	NewKey.bRemoveModifier = bRemoveModifier;
	NewKey.bInsertModifier = bInsertModifier;
	NewKey.bSuspendGuides = bSuspendGuides;
	NewKey.HoverAgent = HoverAgent;

	if (bHasFrameContextCache && NewKey == LastFrameContextKey)
	{
		// SAME QUESTION AS LAST CALL - issue #303. Whichever driver or entry point is asking,
		// two calls with an identical key can only differ if something changed the network
		// without moving any of them - see InvalidateFrameContextCache for how that is covered.
		return CachedFrameContext;
	}

	CachedFrameContext = MakeContext(Target, PlaneHit, Tunables, bRemoveModifier, bInsertModifier,
		bSuspendGuides, HoverAgent);
	LastFrameContextKey = NewKey;
	bHasFrameContextCache = true;
	return CachedFrameContext;
}

void FBuildSession::CancelActiveGesture(const FToolContext& Context)
{
	IBuildTool* Tool = GetActiveTool();
	if (Tool == nullptr)
	{
		return;
	}
	if (!Tool->IsIdle())
	{
		Tool->OnCancel(Context);

		// OnCancel abandons the tool's own stage - not visible to GetFrameContext's key, the
		// same reason ARoadBuildController::OnCancelGesture pairs this call with
		// InvalidateToolReadoutCache.
		InvalidateFrameContextCache();
		return;
	}
	// Idle, and not in Select: cancel means "put the tool down". Two cancels from mid-gesture
	// reach Select; one from an idle build tool does. In Select itself an idle cancel is a
	// no-op rather than a toggle to anything.
	if (ActiveTool != 0)
	{
		SelectTool(0, Context);
	}
}

#undef LOCTEXT_NAMESPACE
