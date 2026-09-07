#include "Tool/BuildSession.h"

#include "Tool/ApronDrawTool.h"
#include "Tool/GuidelineDrawTool.h"
#include "Tool/HoldingPointTool.h"
#include "Tool/RoadDrawTool.h"
#include "Tool/RoadEditTarget.h"
#include "Tool/RunwayTool.h"
#include "Tool/SelectTool.h"
#include "Tool/StandPlaceTool.h"

#define LOCTEXT_NAMESPACE "BuildSession"

TConstArrayView<FToolRegistration> ToolRegistry()
{
	// A function-local static rather than a file-scope global: constructed exactly once,
	// on first use, in the order written here - which IS the key order, 1 through 3, 5, 6
	// then 8 (see the holding-position entry for why seven is skipped; 4 is the Select
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
		{ EKeys::Four,  LOCTEXT("Select",    "Select"),    [] { return MakeUnique<FSelectTool>(); } },

		{ EKeys::One,   LOCTEXT("Taxiway",   "Taxiway"),   [] { return MakeUnique<FRoadDrawTool>(); } },
		{ EKeys::Two,   LOCTEXT("Apron",     "Apron"),     [] { return MakeUnique<FApronDrawTool>(); } },
		{ EKeys::Three, LOCTEXT("Stand",     "Stand"),     [] { return MakeUnique<FStandPlaceTool>(); } },
		{ EKeys::Five,  LOCTEXT("Guideline", "Guidelines"), [] { return MakeUnique<FGuidelineDrawTool>(); } },
		{ EKeys::Six,   LOCTEXT("Runway",    "Runway"),    [] { return MakeUnique<FRunwayTool>(); } },

		// EIGHT, not seven: key 7 is "land an aircraft", which is not a tool and is not in
		// this table - see ARoadBuildController::OnLandAircraft. Numbering around it keeps
		// the printed key on the bar and the key that actually works the same number.
		{ EKeys::Eight, LOCTEXT("HoldingPosition", "Holding point"), [] { return MakeUnique<FHoldingPointTool>(); } },
	};
	return TConstArrayView<FToolRegistration>(Registry);
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
	return Tools.IsValidIndex(ActiveTool) ? Tools[ActiveTool].Get() : nullptr;
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
		return;
	}

	// The outgoing tool abandons whatever it had part-drawn. Left alone it would reappear
	// on the next selection as a chain the player started minutes ago and has forgotten.
	if (IBuildTool* Outgoing = GetActiveTool())
	{
		Outgoing->OnDeactivate(DeactivateContext);
	}

	ActiveTool = Index;

	// A build tool is modal over the airport, not over a thing in it: the selection closes
	// with the panel when one opens, and does not come back when it is cancelled.
	if (Index != 0)
	{
		Selection.Clear();
	}
}

bool FBuildSession::ResolveSnap(const URoadNetwork* Network, const FVector2D& PlaneHit,
	const FRoadSnapSettings& Snap, FRoadSnapResult& Out) const
{
	Out = FRoadSnapResult();
	Out.Position = PlaneHit;

	if (Network != nullptr)
	{
		Out = SnapChain.Resolve(*Network, PlaneHit, Snap);
	}
	return true;
}

FToolContext FBuildSession::MakeContext(IRoadEditTarget* Target, const FVector2D& PlaneHit,
	const FBuildSessionTunables& Tunables, bool bRemoveModifier, bool bInsertModifier,
	int32 HoverAgent) const
{
	FToolContext Context;
	Context.Target = Target;
	Context.HoverAgent = HoverAgent;
	Context.Selection = &Selection;
	Context.Limits = Tunables.Limits;
	Context.SnapRadius = Tunables.ToolPickRadius;
	Context.bRemoveModifier = bRemoveModifier;
	Context.bInsertModifier = bInsertModifier;

	// Resolved ONCE and carried, rather than each consumer asking again. The tool acts on
	// this and the overlay draws it, so what is highlighted and what happens cannot come
	// from two searches that merely tend to agree.
	FRoadSnapResult Snapped;
	ResolveSnap(Target != nullptr ? Target->GetNetwork() : nullptr, PlaneHit, Tunables.Snap, Snapped);

	Context.SetCursor(PlaneHit, Snapped);
	return Context;
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
