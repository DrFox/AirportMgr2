#include "Tool/BuildSession.h"

#include "Tool/ApronDrawTool.h"
#include "Tool/GuidelineDrawTool.h"
#include "Tool/RoadDrawTool.h"
#include "Tool/RoadEditTarget.h"
#include "Tool/RouteTool.h"
#include "Tool/RunwayTool.h"
#include "Tool/StandPlaceTool.h"

#define LOCTEXT_NAMESPACE "BuildSession"

TConstArrayView<FToolRegistration> ToolRegistry()
{
	// A function-local static rather than a file-scope global: constructed exactly once,
	// on first use, in the order written here - which IS the key order, 1 through 6. Never
	// mutated after that first construction, so handing out a view over it is safe from
	// any thread that only reads.
	//
	// Name repeats each tool's own GetDisplayName() text rather than inventing a second
	// label for the same thing - Airside.Tool.BuildSession asserts the two cannot drift,
	// which is exactly the class of bug this table exists to make impossible elsewhere.
	static const FToolRegistration Registry[] =
	{
		{ EKeys::One,   LOCTEXT("Road",      "Road"),      [] { return MakeUnique<FRoadDrawTool>(); } },
		{ EKeys::Two,   LOCTEXT("Apron",     "Apron"),     [] { return MakeUnique<FApronDrawTool>(); } },
		{ EKeys::Three, LOCTEXT("Stand",     "Stand"),     [] { return MakeUnique<FStandPlaceTool>(); } },
		{ EKeys::Four,  LOCTEXT("Route",     "Route"),     [] { return MakeUnique<FRouteTool>(); } },
		{ EKeys::Five,  LOCTEXT("Guideline", "Guidelines"), [] { return MakeUnique<FGuidelineDrawTool>(); } },
		{ EKeys::Six,   LOCTEXT("Runway",    "Runway"),    [] { return MakeUnique<FRunwayTool>(); } },
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
	if (!Tools.IsValidIndex(Index) || Index == ActiveTool)
	{
		return;
	}

	// The outgoing tool abandons whatever it had part-drawn. Left alone it would reappear
	// on the next selection as a chain the player started minutes ago and has forgotten.
	if (IBuildTool* Outgoing = GetActiveTool())
	{
		Outgoing->OnDeactivate(DeactivateContext);
	}

	ActiveTool = Index;
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
	const FBuildSessionTunables& Tunables, bool bRemoveModifier, bool bInsertModifier) const
{
	FToolContext Context;
	Context.Target = Target;
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
	if (IBuildTool* Tool = GetActiveTool())
	{
		Tool->OnCancel(Context);
	}
}

#undef LOCTEXT_NAMESPACE
