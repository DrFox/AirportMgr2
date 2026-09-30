#include "Tool/RoadDrawTool.h"

#include "Tool/RemoveGesture.h"
#include "Tool/RoadGuideAnchor.h"

#include "Model/BuildPurse.h"

#include "AirsideLog.h"

#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Model/StandAdmission.h"
#include "Model/TaxiwayRestriction.h"
#include "Model/TaxiwayStrip.h"
#include "Solve/IcaoCode.h"
#include "Solve/RoadGeom.h"
#include "UObject/UObjectGlobals.h"
#include "Solve/GuideArbiter.h"
#include "Tool/RoadHeal.h"
#include "Tool/PavementAxis.h"
#include "Tool/RoadNaming.h"
#include "Profiles/RoadProfile.h"

#define LOCTEXT_NAMESPACE "Airside"

namespace
{
	/**
	 * The pavements the profile a click would lay offers - URoadProfile::AllowedPavements, the
	 * data ERoadSurface's two steps became. THE SAME PROFILE the ghost and ConnectNodes resolve
	 * (IRoadEditTarget::ResolveProfileFor), so the row cannot offer a surface the network then
	 * refuses. No profile at all reads as empty, Pavement::Offered's "all four" - reached only by
	 * a fake target: ConnectNodes refuses a road with no profile, and a taxiway falls back to the
	 * actor's RuntimeProfile, which Fill gives the road list.
	 * ENFORCED BY: Airside.Present.GrassRoadLaid (the row's grass reaches the laid segment)
	 */
	TConstArrayView<EPavement> RoadAllowedPavements(const FToolContext& Context, ERoadKind Kind, int32 WidthIndex)
	{
		const URoadProfile* Profile = Context.Target != nullptr
			? Context.Target->ResolveProfileFor(Kind, WidthIndex) : nullptr;
		return Profile != nullptr ? TConstArrayView<EPavement>(Profile->AllowedPavements) : TConstArrayView<EPavement>();
	}

	/**
	 * The snap as this tool should ACT on it: its position moved onto the guide when the chain
	 * claimed nothing.
	 *
	 * A SNAP BEATS A GUIDE. When the chain claimed a node or a segment the player is attaching
	 * to something REAL - closing a junction, splitting a run - and that is a statement about
	 * the graph, where an alignment is only an aid. A guide allowed to override it would make a
	 * junction impossible to close while any guide was live, which is far worse than a guide
	 * occasionally not applying.
	 *
	 * RETURNS THE WHOLE RESULT so the ghost, all three placement judgements and the click take
	 * the same value. Handing some of them a position and others the raw snap is how a preview
	 * comes to promise what a click does not do.
	 *
	 * PREFIXED because this module is a unity build and "GuidedSnap" is exactly the name a
	 * second tool would also choose - see the SegmentEnds collision in SnapGuideChain.cpp.
	 */
	FRoadSnapResult RoadGuidedSnap(const FToolContext& Context)
	{
		FRoadSnapResult Guided = Context.Snap;
		if (Guided.Kind == ERoadSnapKind::Free)
		{
			Guided.Position = Context.GuidedCursor();
		}
		return Guided;
	}

	/** Position of a live node, or the cursor when there is not one. */
	FVector2D NodePosition(const FToolContext& Context, int32 NodeIndex)
	{
		if (Context.Network() != nullptr)
		{
			const TArray<FRoadNode>& Nodes = Context.Network()->GetNodes();
			if (Nodes.IsValidIndex(NodeIndex) && Nodes[NodeIndex].bAlive)
			{
				return Nodes[NodeIndex].Position;
			}
		}
		return Context.Cursor;
	}

	/**
	 * Put a node where the snap says, whichever kind it is, and report whether it is new.
	 *
	 * The three outcomes are the whole difference between continuing a road, closing a
	 * junction on a node already there, and cutting a new junction into a road already
	 * drawn - and the snap chain has already decided which.
	 */
	int32 ResolveToNode(const FToolContext& Context, bool& bOutCreated)
	{
		bOutCreated = true;

		switch (Context.Snap.Kind)
		{
		case ERoadSnapKind::Node:
			bOutCreated = false;
			return Context.Snap.Node.Index;

		case ERoadSnapKind::Segment:
			return Context.Target->SplitSegment(Context.Snap.Segment.Index, Context.Snap.Position);

		case ERoadSnapKind::Free:
		default:
			return Context.Target->PlaceNode(RoadGuidedSnap(Context).Position);
		}
	}
}

// --- Idle ---------------------------------------------------------------------------

TUniquePtr<IRoadDrawState> FRoadIdleState::OnClick(const FToolContext& Context)
{
	bool bCreated = false;
	const int32 Started = ResolveToNode(Context, bCreated);
	if (Started == INDEX_NONE)
	{
		return nullptr;
	}

	// No RebuildMesh() here any more - PlaceNode/SplitSegment, whichever ResolveToNode just
	// called, now notify the facade's own OnChanged on commit (issue #77).
	return MakeUnique<FRoadChainingState>(Started, bCreated, Kind, WidthIndex, Surface);
}

TUniquePtr<IRoadDrawState> FRoadIdleState::OnCancel(const FToolContext& Context)
{
	// Nothing part-drawn to back out of.
	return nullptr;
}

void FRoadIdleState::BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const
{
	// Only what the next click would attach to. There is no road in progress to show.
	if (Context.Snap.Kind == ERoadSnapKind::Node)
	{
		Sink.Marker(Context.Snap.Position, EPreviewStyle::Snap);
	}
}

// --- Chaining -----------------------------------------------------------------------

TUniquePtr<IRoadDrawState> FRoadChainingState::OnClick(const FToolContext& Context)
{
	// Judged BEFORE anything is created. Validating afterwards would leave a stray node
	// behind on every refused click - the road would not appear, but the graph would have
	// grown anyway.
	FRoadNodeId FromId;
	if (Context.Target->MakeLiveNodeId(From, FromId))
	{
		// Guarded the same way BuildPreview is. The preview and the click used to disagree
		// here - preview checked GetNetwork() for null, the click dereferenced it - and the
		// click is where a null actually crashed (2026-09-06, RoadSlotMap.h:61 reading 0x40).
		// Dropped to idle rather than silently ignored so the player is not left chaining
		// from a node the network cannot see.
		const URoadNetwork* Network = Context.Network();
		if (Network == nullptr)
		{
			UE_LOG(LogAirside, Warning,
				TEXT("Road chaining from node %d refused: the target's network is null although the node is live"), From);
			return MakeUnique<FRoadIdleState>(Kind, WidthIndex, Surface);
		}
		const ERoadPlacement Judgement =
			RoadPlacement::Validate(*Network, FromId, RoadGuidedSnap(Context), Context.Limits);
		if (Judgement != ERoadPlacement::Valid)
		{
			return nullptr;
		}
		// THE STRIP, here too and for the same reason: judged before ResolveToNode makes a
		// node or splits a segment, so a refused click leaves nothing behind. ConnectNodes asks
		// the same question again below and would refuse the same road - but only after the
		// split. ONE EVALUATOR, the readout's (BuildPreview).
		const FString Why = Context.Target->WhySegmentRefused(From, RoadGuidedSnap(Context), Kind, WidthIndex);
		if (!Why.IsEmpty())
		{
			// LOGGED, unlike Validate's refusals above: this is new at stage 3, and "why will it
			// not lay my road" is the first thing a repro asks.
			UE_LOG(LogAirside, Log, TEXT("Road click refused: %s"), *Why);
			return nullptr;
		}
	}

	bool bNextCreated = false;
	const int32 To = ResolveToNode(Context, bNextCreated);
	if (To == INDEX_NONE)
	{
		return nullptr;
	}

	if (To != From && !Context.Target->ConnectNodes(From, To, Kind, WidthIndex, Surface))
	{
		// The facade already logged why. Drop the chain rather than leaving the player
		// clicking against a connection that will not form. NO RebuildMesh() here any more -
		// it used to cover the node or split ResolveToNode just made a few lines up, which
		// notifies on its OWN commit now (PlaceNode/SplitSegment, issue #77); the refused
		// ConnectNodes itself changed nothing further that needs showing.
		return MakeUnique<FRoadIdleState>(Kind, WidthIndex, Surface);
	}

	// No RebuildMesh() here either - ResolveToNode and ConnectNodes each notify the facade's
	// own OnChanged on commit now (issue #77).

	// Chain on from the node just reached, so a road is drawn click by click rather than
	// a pair of clicks per segment.
	return MakeUnique<FRoadChainingState>(To, bNextCreated, Kind, WidthIndex, Surface);
}

TUniquePtr<IRoadDrawState> FRoadChainingState::OnCancel(const FToolContext& Context)
{
	// A chain that placed a node and drew nothing from it leaves that node with no road on
	// it. Removed here because this gesture created it and this gesture is being abandoned
	// - and only if it is still bare, because a node that picked up a segment is part of
	// the network now, whoever made it.
	if (bCreated && Context.Network() != nullptr)
	{
		const TArray<FRoadNode>& Nodes = Context.Network()->GetNodes();
		if (Nodes.IsValidIndex(From) && Nodes[From].bAlive && Nodes[From].Incident.Num() == 0)
		{
			// No RebuildMesh() on success any more - DeleteNode notifies on commit (issue #77).
			Context.Target->DeleteNode(From);
		}
	}

	return MakeUnique<FRoadIdleState>(Kind, WidthIndex, Surface);
}

void FRoadChainingState::BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const
{
	Sink.Marker(NodePosition(Context, From), EPreviewStyle::Pending);

	if (Context.Snap.Kind == ERoadSnapKind::Node)
	{
		Sink.Marker(Context.Snap.Position, EPreviewStyle::Snap);
	}

	// The reason a click will be refused. The ghost already says THAT it will be, by
	// turning red; a colour cannot say which of four rules objected.
	FRoadNodeId FromId;
	if (Context.Network() != nullptr && Context.Target->MakeLiveNodeId(From, FromId))
	{
		// THE GUIDED POINT, once, for every readout below: it is what ResolveToNode commits,
		// so the length, the refusal and the price all describe the segment a click builds.
		const FRoadSnapResult Guided = RoadGuidedSnap(Context);
		const FVector2D Start = NodePosition(Context, From);
		const ERoadPlacement Judgement =
			RoadPlacement::Validate(*Context.Network(), FromId, Guided, Context.Limits);
		// THE STRIP JUDGE after the geometric one - a corner the solver cannot draw is the
		// more basic fault, and Validate's reasons stay first. Asked only once Validate passes.
		const FString StripWhy = Judgement == ERoadPlacement::Valid
			? Context.Target->WhySegmentRefused(From, Guided, Kind, WidthIndex) : FString();
		const bool bAllowed = Judgement == ERoadPlacement::Valid && StripWhy.IsEmpty();

		// THE LENGTH, as the runway's preview has always said its own (reported 2026-09-27:
		// taxiways and roads said nothing). Mid-segment so it never sits on the refusal or the
		// price at the cursor end, and shown when refused too - "how long is it" matters most
		// when the answer is "too short". Straight distance is exact: a segment is a chord.
		const double Length = FVector2D::Distance(Start, Guided.Position);
		if (Length > 0.0)
		{
			Sink.Label((Start + Guided.Position) * 0.5, FString::Printf(TEXT("%.0f m"), Length / 100.0),
				bAllowed ? EPreviewStyle::Pending : EPreviewStyle::Refused);
		}

		if (Judgement != ERoadPlacement::Valid)
		{
			Sink.Label(Guided.Position, RoadPlacement::Describe(Judgement),
				EPreviewStyle::Refused);
		}
		else if (!StripWhy.IsEmpty())
		{
			Sink.Label(Guided.Position, StripWhy, EPreviewStyle::Refused);
		}
		else if (const IBuildPurse* Purse = Context.Target->GetPurse())
		{
			// THE PRICE BEFORE THE CLICK. NO NEW SINK MESSAGE: Label already exists and
			// EPreviewStyle::Refused already means "something the gesture cannot do, with the
			// reason" - which is exactly what an unaffordable road is. The purse formats the
			// money, so no currency symbol ever enters this plugin. Quoted at the GUIDED point:
			// it read the raw Snap.Position until 2026-09-27, pricing a different length from the
			// one the click builds whenever a snap guide was active.
			const FBuildQuote Quote = Context.Target->QuoteForConnect(
				From, Guided.Position, Kind, WidthIndex, Surface);
			if (!Quote.IsFree())
			{
				Sink.Label(Guided.Position, Purse->Describe(Quote).ToString(),
					Purse->CanAfford(Quote) ? EPreviewStyle::Pending : EPreviewStyle::Refused);
			}
		}
	}
}

// --- The tool -----------------------------------------------------------------------

FRoadDrawTool::FRoadDrawTool(ERoadKind InKind)
	: State(MakeUnique<FRoadIdleState>(InKind, /*InWidthIndex*/ 0))   // WidthIndex's default - see its comment
	, Kind(InKind)
{
}

FText FRoadDrawTool::GetDisplayName() const
{
	// TWO NAMES FOR ONE TOOL, and they must match the registry's own Name for each entry -
	// Airside.Tool.BuildSession asserts the two cannot drift, which is exactly the class of
	// bug the registry exists to make impossible elsewhere.
	//
	// "Road" was once refused here on the grounds that a key labelled Road promised a
	// vehicle tool that did not exist (2026-09-07). It exists now: key 9 lays the service
	// road profile, with a GroundVehicle guideline, and key 1 keeps the taxiway.
	return Kind == ERoadKind::ServiceRoad
		? LOCTEXT("RoadTool", "Road")
		: LOCTEXT("TaxiwayTool", "Taxiway");
}

bool FRoadDrawTool::IsIdle() const
{
	return State.IsValid() && State->IsIdle();
}

bool FRoadDrawTool::DescribeGuideAnchor(const URoadNetwork* Network, IRoadEditTarget* Target,
	FGuideAnchor& Out) const
{
	// THE WIDTH THIS GESTURE WOULD LAY - the same question the ghost asks, through the same one
	// resolver, so a guide cannot disagree with the pavement it is guiding. A null Target is a
	// supported state and leaves the widths at zero, which means "no width" rather than "unknown".
	//
	// FILLED BEFORE THE DECLINE BELOW, because a FREE START has a width too: the first click of
	// a road laid flush against an apron edge is displaced by exactly this figure, and the base
	// that answers the free start knows nothing about profiles.
	Out.Point = EDragPoint::Centreline;
	if (Target != nullptr)
	{
		if (const URoadProfile* Profile = Target->ResolveProfileFor(Kind, WidthIndex))
		{
			Out.HalfWidthLeft = Profile->GetHalfWidthLeft();
			Out.HalfWidthRight = Profile->GetHalfWidthRight();
		}
	}

	// NOTHING PENDING MEANS NOTHING TO EXTEND. The first click of a chain has no direction to
	// speak of, and an ANGULAR guide offered there would be squaring to an edge that does not
	// exist - which is why the base's answer here is a FREE START and not this function's own
	// anchor: it puts the cursor in Origin and lets the arbiter drop every angular candidate.
	// Delegating rather than returning false is what opts this tool in; see
	// IBuildTool::DescribeGuideAnchor.
	const int32 Pending = GetPendingNode();
	if (Network == nullptr || Pending == INDEX_NONE)
	{
		return IBuildTool::DescribeGuideAnchor(Network, Target, Out);
	}

	const FRoadNodeId FromId = Network->NodeIdAt(Pending);
	const FRoadNode* From = Network->GetNode(FromId);
	if (From == nullptr)
	{
		// A PENDING NODE THE GRAPH CANNOT SEE is not a free start - the gesture HAS begun, and
		// IsIdle() says so, so the base declines too. Routed through it anyway rather than a
		// bare false, so the two exits cannot come to disagree about what "no anchor" means.
		return IBuildTool::DescribeGuideAnchor(Network, Target, Out);
	}

	Out.Origin = From->Position;

	// THE SEGMENT ALREADY ARRIVING AT THE PENDING NODE. With exactly one incident segment the
	// answer is unambiguous - that is the road being extended. At a junction there are several
	// and none of them is "the" incoming one, so no reference is offered rather than an
	// arbitrary one: a guide that squared to whichever segment happened to be stored first
	// would change with an edit nobody connected to guides at all.
	//
	// THROUGH THE ONE FUNCTION now, issue #303: this used to walk every live segment in the
	// network on every MakeContext to answer the identical question FEditTool's own drag anchor
	// asks through FromId's own FRoadNode::Incident - an O(N) scan repeated for an O(degree)
	// read. See RoadGuideAnchor::DescribeIncomingArm.
	RoadGuideAnchor::DescribeIncomingArm(*Network, *From, FromId, Out);

	RoadGuideAnchor::AddNodeCandidates(*Network, Out.Origin, Pending, Out);

	return true;
}

int32 FRoadDrawTool::GetPendingNode() const
{
	return State.IsValid() ? State->GetPendingNode() : INDEX_NONE;
}

void FRoadDrawTool::Remove(const FToolContext& Context)
{
	if (!RemoveGesture::Apply(Context))
	{
		return;
	}

	// A deletion can take the node the chain was running from, so the chain ends rather
	// than being checked. Its start may not exist any more.
	// WITH THIS TOOL'S OWN KIND. Dropping to a default-constructed idle state would silently
	// put the Road tool back into taxiway mode after a Ctrl+click removal, and the next
	// click would lay a 23 m aircraft lane where the player was drawing a road.
	State = MakeUnique<FRoadIdleState>(Kind, WidthIndex, Surface);
}

void FRoadDrawTool::OnClick(const FToolContext& Context)
{
	if (Context.Target == nullptr || !State.IsValid())
	{
		return;
	}

	if (Context.bRemoveModifier)
	{
		Remove(Context);
		return;
	}

	// UPGRADE LAYS NOTHING: the click changes the piece under it, and no node or chain is made.
	if (Mode == EToolMode::Upgrade)
	{
		Upgrade(Context);
		return;
	}

	// Shift on a road inserts a node and stops there. A plain click splits too, but also
	// starts a chain from the new node - right when drawing a road INTO an existing one,
	// and a nuisance when all you wanted was somewhere to drag from.
	if (Context.bInsertModifier && Context.Snap.Kind == ERoadSnapKind::Segment)
	{
		// No RebuildMesh() on success any more - SplitSegment notifies on commit (issue #77).
		Context.Target->SplitSegment(Context.Snap.Segment.Index, Context.Snap.Position);
		return;
	}

	if (TUniquePtr<IRoadDrawState> Next = State->OnClick(Context))
	{
		State = MoveTemp(Next);
	}
}

void FRoadDrawTool::OnReselect(const FToolContext& Context)
{
	// KEY-AGAIN CYCLES THE WIDTH, the gesture FRunwayTool::OnReselect already gives
	// runways. Borrowed rather than given a key of its own for the reason that tool states:
	// the number keys are spoken for, and "press the tool's key again" is a gesture a
	// player already knows from it.
	//
	// A SERVICE ROAD CYCLES ITS TIERS TOO (2026-09-23), Narrow / Standard / Wide. It used to
	// refuse here - "one authored cross-section" - and reaching for the taxiway list would
	// have laid a 23 m lane for vans. The seam now keys the list by Kind, so a road index can
	// only ever name a road tier.
	const TCHAR* What = Kind == ERoadKind::ServiceRoad ? TEXT("Road") : TEXT("Taxiway");

	if (Context.Target == nullptr)
	{
		// A DIFFERENT REFUSAL from an empty content set, said differently - the same
		// distinction FRunwayTool::NextWidth draws, and for the same reason: this is a
		// caller bug, not a fresh project, and one shared message would blame the content
		// set for a null target.
		UE_LOG(LogAirside, Warning,
			TEXT("%s width unchanged: no edit target in context, so there is nothing "
			     "to ask for widths"), What);
		return;
	}

	// SHIFT STEPS THE SURFACE, the modifier FRunwayTool::OnReselect gives its own surface, so the
	// two tools answer one gesture one way. BEFORE the width count: the surface needs no width
	// profiles (only the kind's own profile's list), so a project with none can still pick grass.
	if (Context.bInsertModifier)
	{
		StepAxis(Context, TEXT("Surface"), What);
		return;
	}

	const int32 Count = Context.Target->GetWidthCount(Kind);
	if (Count <= 0)
	{
		// SAID OUT LOUD. Returning in silence is indistinguishable from a key that never
		// arrived: the player presses the tool's key again, nothing widens, and nothing
		// anywhere says why. A content set with no profiles for this kind is a real state - it
		// is what a project that has not run build_road_profiles.py has.
		UE_LOG(LogAirside, Warning,
			TEXT("%s width unchanged: the content set declares no %s profiles, so "
			     "there is nothing to cycle through. Author them with "
			     "Tools/Python/build_road_profiles.py."), What, Kind == ERoadKind::ServiceRoad ? TEXT("road") : TEXT("taxiway"));
		return;
	}

	// FROM WHAT IS LIT, then round it. The bar's row shows the lit option, so the key steps on
	// from THAT - a press that jumped back to the narrowest from a lit Code C would read as the
	// row and the key disagreeing. Nothing lit (the level's tuning is off-list) starts at the
	// narrowest, which is what the first press always did before the row existed.
	StepAxis(Context, TEXT("Width"), What);
}

void FRoadDrawTool::StepAxis(const FToolContext& Context, FName AxisId, const TCHAR* What)
{
	// BY Id, not by row index, because the Width row is absent when the content set declares no
	// widths and Surface then becomes row 0 - FRunwayTool::StepAxis resolves its rows the same
	// way for the same reason.
	TArray<FToolVariantAxis> Axes;
	GetVariantAxes(Context, Axes);
	const int32 Axis = Axes.IndexOfByPredicate([AxisId](const FToolVariantAxis& A) { return A.Id == AxisId; });
	const int32 Next = Axis != INDEX_NONE ? NextEnabledVariant(Axes[Axis]) : INDEX_NONE;
	if (Next == INDEX_NONE)
	{
		// "every width is locked" for the width row, word for word what this line said before
		// the surface row existed - the log is what "the key does nothing" is diagnosed from.
		const FString Lower = AxisId.ToString().ToLower();
		UE_LOG(LogAirside, Warning, TEXT("%s %s unchanged: every %s is locked"), What, *Lower, *Lower);
		return;
	}
	SelectVariant(Context, Axis, Next);
}

int32 NextEnabledVariant(const FToolVariantAxis& Axis)
{
	const int32 Count = Axis.Options.Num();
	const int32 From = Axis.Current == INDEX_NONE ? -1 : Axis.Current;
	for (int32 Step = 1; Step <= Count; ++Step)
	{
		const int32 Candidate = (From + Step) % Count;
		if (Axis.Options[Candidate].bEnabled)
		{
			return Candidate;
		}
	}
	return INDEX_NONE;
}

FText VariantWidthLabel(double TotalWidth)
{
	// ONE DECIMAL AT MOST: ICAO code B is 10.5 m and code C 15 m, and "15.0 m" beside "10.5 m"
	// reads as a precision the rest of the row does not have.
	FNumberFormattingOptions Metres;
	Metres.MinimumFractionalDigits = 0;
	Metres.MaximumFractionalDigits = 1;
	return FText::Format(LOCTEXT("VariantWidthMetres", "{0} m"), FText::AsNumber(TotalWidth / 100.0, &Metres));
}

void FRoadDrawTool::GetVariantAxes(const FToolContext& Context, TArray<FToolVariantAxis>& Out) const
{
	if (Context.Target == nullptr)
	{
		return;
	}
	// MODE FIRST (strip stage 6): it says what the rows below it DO - lay at this width, or
	// re-width what is clicked - so it heads them. ModeAxis's words, shared with every tool
	// that gains an upgrade.
	ModeAxis::AppendAxis(Out, Mode);
	AddWidthAxis(Context, Out);

	// SURFACE ALWAYS, after Width: choosable with or without width profiles, from THE
	// PROFILE'S LIST (RoadAllowedPavements) - a road tool must not offer a concrete service
	// road, the reason #356 gave roads a two-step enum, now data on the profile. Names and
	// order are Pavement::AppendAxis's, shared with the runway row - see its comment.
	Pavement::AppendAxis(Out, Surface, RoadAllowedPavements(Context, Kind, WidthIndex));
}

void FRoadDrawTool::AddWidthAxis(const FToolContext& Context, TArray<FToolVariantAxis>& Out) const
{
	const int32 Count = Context.Target->GetWidthCount(Kind);
	if (Count <= 0)
	{
		// NO AXIS rather than an empty one: the row hides instead of drawing a heading over
		// nothing. OnReselect's own branch says why in the log.
		return;
	}

	// LIT IS WHAT THE TOOL HOLDS, and nothing else, since 2026-09-28: WidthIndex starts on the
	// narrowest rather than unset, so the level-default match that used to light a preset for an
	// unset tool - and lit nothing when the level's width was off the set - had no case left.
	FToolVariantAxis& Axis = Out.AddDefaulted_GetRef();
	Axis.Id = TEXT("Width");
	Axis.Label = LOCTEXT("VariantAxisWidth", "Width");
	Axis.Current = WidthIndex != INDEX_NONE ? FMath::Clamp(WidthIndex, 0, Count - 1) : INDEX_NONE;

	// IN UPGRADE, "Keep width" LEADS AND IS THE DEFAULT (review fix 4) - see UpgradeWidthIndex.
	// Every standard width moves one option along; SelectVariant undoes the shift.
	if (Mode == EToolMode::Upgrade)
	{
		FToolVariant& Keep = Axis.Options.AddDefaulted_GetRef();
		Keep.Id = TEXT("Keep");
		Keep.Label = LOCTEXT("VariantWidthKeep", "Keep width");
		Axis.Current = UpgradeWidthIndex == INDEX_NONE ? 0 : FMath::Clamp(UpgradeWidthIndex, 0, Count - 1) + 1;
	}
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const URoadProfile* Profile = Context.Target->ResolveWidthProfile(Kind, Index);
		const double Width = Profile != nullptr ? Profile->GetTotalWidth() : 0.0;

		FToolVariant& Option = Axis.Options.AddDefaulted_GetRef();
		// BY WIDTH IN UU, not by index or asset name: stable across calls, distinct within a
		// kind (the set is ordered narrow to wide, ServiceRoadWidth pins it), and a content edit
		// that changes a width changes the Id, which is exactly when the row must rebuild.
		Option.Id = FName(*FString::Printf(TEXT("W%d"), FMath::RoundToInt(Width)));
		Option.Label = VariantWidthLabel(Width);

	}
}

bool FRoadDrawTool::RestoreSurface(EPavement InSurface)
{
	// NOT CHECKED AGAINST THE PROFILE'S OFFERED LIST: there is no context to ask for the profile
	// here (FBuildSession restores at construction). A surface the profile does not offer can
	// only come from a hand-edited ini, and would light no option on the row and be refused at
	// the lay by URoadEditFacade's own Offered check - wrong, visibly, and nothing laid.
	Surface = InSurface;
	if (State.IsValid())
	{
		State->Surface = Surface;
	}
	return true;
}

bool FRoadDrawTool::SelectVariant(const FToolContext& Context, int32 Axis, int32 Option)
{
	// THROUGH GetVariantAxes, virtually, so a lock is honoured wherever it was set - the row that
	// greys a width and the pick that refuses it read one answer.
	TArray<FToolVariantAxis> Axes;
	GetVariantAxes(Context, Axes);
	if (!Axes.IsValidIndex(Axis) || !Axes[Axis].Options.IsValidIndex(Option)
		|| !Axes[Axis].Options[Option].bEnabled)
	{
		return false;
	}

	const TCHAR* What = Kind == ERoadKind::ServiceRoad ? TEXT("Road") : TEXT("Taxiway");

	if (Axes[Axis].Id == ModeAxis::AxisId())
	{
		const EToolMode Next = ModeAxis::ModeAt(Option);
		if (Next != Mode)
		{
			// A PART-DRAWN CHAIN ENDS on the switch, as a deactivate ends it: in Upgrade the
			// next click changes a road rather than continuing one, and a chain left pending
			// would reappear, half-drawn, on the way back to Build.
			if (State.IsValid() && !State->IsIdle())
			{
				if (TUniquePtr<IRoadDrawState> Cancelled = State->OnCancel(Context)) { State = MoveTemp(Cancelled); }
			}
			State = MakeUnique<FRoadIdleState>(Kind, WidthIndex, Surface);
			Context.Target->HideGhost();
			Mode = Next;
			Hover = FUpgradeHover();
			UpgradeWidthIndex = INDEX_NONE;   // every entry to Upgrade starts on "Keep width"
		}
		UE_LOG(LogAirside, Log, TEXT("%s mode -> %s"), What, *ModeAxis::OptionId(Mode).ToString());
		return true;
	}

	// BY THE ROW'S Id - see StepAxis on why an index cannot say which row it is.
	if (Axes[Axis].Id == TEXT("Surface"))
	{
		// AN INDEX INTO THE OFFERED LIST, never into the enum - the row AppendAxis built, from
		// the same profile's list, so Option is in range (checked against Axes above).
		Surface = Pavement::Offered(RoadAllowedPavements(Context, Kind, WidthIndex))[Option];
		UE_LOG(LogAirside, Log, TEXT("%s surface -> %s"), What, Pavement::Name(Surface));

		// The part-drawn chain hears it too, for WidthIndex's reason below.
		if (State.IsValid())
		{
			State->Surface = Surface;
		}
		return true;
	}

	// UPGRADE'S WIDTH ROW: option 0 is "Keep width", the rest one past their Build index.
	if (Mode == EToolMode::Upgrade)
	{
		UpgradeWidthIndex = Option == 0 ? INDEX_NONE : Option - 1;
		const URoadProfile* Chosen = UpgradeWidthIndex != INDEX_NONE ? Context.Target->ResolveWidthProfile(Kind, UpgradeWidthIndex) : nullptr;
		UE_LOG(LogAirside, Log, TEXT("%s upgrade width -> %s"), What, Chosen != nullptr
			? *FString::Printf(TEXT("%.1f m"), Chosen->GetTotalWidth() / 100.0) : TEXT("keep"));
		return true;
	}

	const int32 Count = Axes[Axis].Options.Num();
	WidthIndex = Option;

	// The width is otherwise visible only in the ghost, and only once a chain is started -
	// so a player who has not clicked yet has no way to tell the key did anything. MOVED HERE
	// from OnReselect so a bar click logs the same line a key press does.
	const URoadProfile* Profile = Context.Target->ResolveWidthProfile(Kind, WidthIndex);
	UE_LOG(LogAirside, Log, TEXT("%s width -> %d of %d, %.1f m"),
		What, WidthIndex + 1, Count, Profile != nullptr ? Profile->GetTotalWidth() / 100.0 : 0.0);

	// The part-drawn chain, if any, must hear about it: the state carries its own copy so
	// it can build its successor, and a chain left on the old width would finish at a
	// width the ghost has stopped showing.
	if (State.IsValid())
	{
		State->WidthIndex = WidthIndex;
	}
	return true;
}

void FRoadDrawTool::OnCancel(const FToolContext& Context)
{
	if (Context.Target == nullptr || !State.IsValid())
	{
		return;
	}

	if (TUniquePtr<IRoadDrawState> Next = State->OnCancel(Context))
	{
		State = MoveTemp(Next);
	}
}

void FRoadDrawTool::Tick(const FToolContext& Context)
{
	if (Context.Target == nullptr)
	{
		return;
	}

	// NO ROAD GHOST IN UPGRADE: nothing is being laid; PreviewUpgrade outlines the change.
	if (Mode == EToolMode::Upgrade)
	{
		Context.Target->HideGhost();
		return;
	}

	const int32 Pending = GetPendingNode();

	// No ghost while a deletion is being aimed: the preview would otherwise offer to build
	// the very thing that is about to be removed.
	if (Pending == INDEX_NONE || Context.bRemoveModifier)
	{
		Context.Target->HideGhost();
		return;
	}

	FRoadNodeId FromId;
	if (Context.Network() == nullptr || !Context.Target->MakeLiveNodeId(Pending, FromId))
	{
		Context.Target->HideGhost();
		return;
	}

	// Shown even when illegal, coloured rather than withheld: hiding it would answer "why
	// can I not build here" with nothing at all.
	const ERoadPlacement Judgement =
		RoadPlacement::Validate(*Context.Network(), FromId, RoadGuidedSnap(Context), Context.Limits);
	// Red inside a strip too - the readout's own WhySegmentRefused, so the colour and the
	// label cannot disagree.
	const bool bValid = Judgement == ERoadPlacement::Valid
		&& Context.Target->WhySegmentRefused(Pending, RoadGuidedSnap(Context), Kind, WidthIndex).IsEmpty();
	Context.Target->UpdateGhost(Pending, RoadGuidedSnap(Context), bValid, Kind, WidthIndex);
}

void FRoadDrawTool::OnDeactivate(const FToolContext& Context)
{
	// Abandon the part-drawn chain rather than leaving it to reappear when this tool is
	// picked again - a click landing on a road started minutes ago and forgotten.
	//
	// OnCancel ONLY DOES THIS WHEN Context.Target IS SET (top-level FRoadDrawTool::OnCancel
	// returns early otherwise), but BuildSession::SelectTool's own doc says OnDeactivate must
	// tolerate a default-constructed FToolContext - the session holds no IRoadEditTarget of
	// its own. Every other draw tool resets its part-drawn state unconditionally on
	// deactivate (FOutlineDrawTool, FPlotPlaceTool, FRunwayTool, FStandPlotTool); this one
	// must too, or a chain started with a target survives a later deactivate that has none,
	// and reappears - unfinished and un-cancellable by anything outside a click - next time
	// this tool is picked.
	OnCancel(Context);
	State = MakeUnique<FRoadIdleState>(Kind, WidthIndex, Surface);

	if (Context.Target != nullptr)
	{
		Context.Target->HideGhost();
	}
}

void FRoadDrawTool::PreviewRemoval(const FToolContext& Context, IToolPreviewSink& Sink) const
{
	// THE SHARED ANSWER - see RemoveGesture. This routine and the deletion below it used to
	// be the only pair, and now FEditTool offers the same gesture; two copies of an
	// 80-line plan is how a preview comes to show something the click will not do.
	RemoveGesture::Describe(Context, Sink);
}

void FRoadDrawTool::BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const
{
	if (Context.Target == nullptr)
	{
		return;
	}

	if (Context.bRemoveModifier)
	{
		PreviewRemoval(Context, Sink);
		return;
	}

	if (Mode == EToolMode::Upgrade)
	{
		PreviewUpgrade(Context, Sink);
		return;
	}

	// Where the click lands on the PLANE, which under an angled view is not where the
	// mouse pointer is drawn - and the shallower the view, the further apart they are.
	Sink.Marker(RoadGuidedSnap(Context).Position, EPreviewStyle::Pending);

	// THE DASHED LINE TO WHAT IT IS LINED UP WITH, one per winner - the same emission the plot
	// gesture makes, and deliberately the same shape: two tools drawing one meaning two
	// different ways would be presentation drifting apart inside the plugin.
	//
	// FROM THE POINT THE CLICK WOULD TAKE, so the line touches the marker above rather than
	// floating beside it.
	if (Context.Guide.bActive)
	{
		Sink.Guides(Context.Guide, RoadGuidedSnap(Context).Position);
	}

	if (Context.Snap.Kind == ERoadSnapKind::Segment && Context.Network() != nullptr)
	{
		const TArray<FRoadSegment>& Segments = Context.Network()->GetSegments();
		const int32 Index = Context.Snap.Segment.Index;
		if (Segments.IsValidIndex(Index) && Segments[Index].bAlive)
		{
			const FRoadNode* EndA = Context.Network()->GetNode(Segments[Index].A);
			const FRoadNode* EndB = Context.Network()->GetNode(Segments[Index].B);
			if (EndA != nullptr && EndB != nullptr)
			{
				Sink.CrossMark(Context.Snap.Position,
					(EndB->Position - EndA->Position).GetSafeNormal(), EPreviewStyle::Snap);
			}
		}
	}

	if (State.IsValid())
	{
		State->BuildPreview(Context, Sink);
	}
}

// --- Upgrade mode (strip stage 6) ---------------------------------------------------------

int32 FRoadDrawTool::UpgradeTargetUnder(const FToolContext& Context) const
{
	const URoadNetwork* Network = Context.Network();
	if (Network == nullptr)
	{
		return INDEX_NONE;
	}
	if (Context.Snap.Kind == ERoadSnapKind::Segment && Network->GetSegment(Context.Snap.Segment) != nullptr)
	{
		return Context.Snap.Segment.Index;
	}
	// THE PAVEMENT UNDER THE CURSOR, TaxiwayStrip's footprint - the one description of a
	// segment's ground. Every live segment, linearly (N was 34 on M_Test, 2026-09-28), only
	// while Upgrade is the mode and only when the snap found nothing.
	const TArray<FRoadSegment>& Segments = Network->GetSegments();
	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		const FRoadSegmentId Id = Network->SegmentIdAt(Index);
		TaxiwayStrip::FSegmentShape Shape;
		if (!Id.IsSet() || Network->IsRunwaySegment(Id) || !TaxiwayStrip::ShapeOf(*Network, Id, Shape)) { continue; }
		if (RoadGeom::PointInPolygon(TaxiwayStrip::FootprintOf(Shape), Context.Cursor))
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

void FRoadDrawTool::Upgrade(const FToolContext& Context)
{
	const int32 Segment = UpgradeTargetUnder(Context);
	const TCHAR* What = Kind == ERoadKind::ServiceRoad ? TEXT("Road") : TEXT("Taxiway");
	if (Segment == INDEX_NONE)
	{
		// SAID, for "the click did nothing": Upgrade changes a piece, and there was none here.
		UE_LOG(LogAirside, Log, TEXT("%s upgrade: nothing under the cursor to upgrade"), What);
		return;
	}
	// The facade refuses and logs its own reason; the hover already showed the same one.
	Context.Target->UpgradeSegment(Segment, Kind, UpgradeWidthIndex, Surface);
	Hover = FUpgradeHover();
}

void FRoadDrawTool::PreviewUpgrade(const FToolContext& Context, IToolPreviewSink& Sink) const
{
	Sink.Marker(Context.Cursor, EPreviewStyle::Pending);
	const URoadNetwork* Network = Context.Network();
	const int32 Segment = UpgradeTargetUnder(Context);
	if (Network == nullptr || Segment == INDEX_NONE)
	{
		return;
	}
	const FRoadSegmentId Id = Network->SegmentIdAt(Segment);
	const FRoadSegment& Piece = Network->GetSegments()[Segment];
	const URoadProfile* Had = Network->ProfileFor(Piece);

	// THE MEMO'S KEY: the graph it was asked of, its edit revision and entity count, and the epoch
	// (IRoadEditTarget::GetEditEpoch), which moves on every edit the first two miss - a stand
	// removed and another placed leaves the count alone, and a stand is what the strip closure
	// below reads. The purse is deliberately NOT in it: the money half is asked fresh, see below.
	const uint32 Epoch = Context.Target->GetEditEpoch();
	const bool bSame = Hover.Network == Network && Hover.Revision == Network->GetEditRevision()
		&& Hover.Entities == Network->GetEntities().Num() && Hover.Epoch == Epoch
		&& Hover.Segment == Segment && Hover.Had == Had
		&& Hover.HadSurface == Piece.Surface && Hover.Width == UpgradeWidthIndex && Hover.Surface == Surface;
	if (!bSame)
	{
		// FOR TESTS ONLY - see GetHoverBuildCountForTest.
		++HoverBuildCountForTest;

		Hover = FUpgradeHover();
		Hover.Network = Network;
		Hover.Revision = Network->GetEditRevision();
		Hover.Entities = Network->GetEntities().Num();
		Hover.Epoch = Epoch;
		Hover.Segment = Segment;
		Hover.Had = Had;
		Hover.HadSurface = Piece.Surface;
		Hover.Width = UpgradeWidthIndex;
		Hover.Surface = Surface;

		// THE SITE HALF of the evaluator the click asks (IRoadEditTarget::WhyUpgradeRefused is its
		// composition with the money half) - the half this memo may hold, since the model and the
		// picks are what it is keyed on. Issue #439: the memo used to hold the WHOLE answer, and
		// its "cannot afford" outlived the balance that produced it.
		const FString Why = Context.Target->WhyUpgradeSiteRefused(Segment, Kind, UpgradeWidthIndex, Surface);
		URoadProfile* New = UpgradeWidthIndex != INDEX_NONE ? Context.Target->ResolveWidthProfile(Kind, UpgradeWidthIndex) : nullptr;
		const URoadProfile* Becomes = New != nullptr ? New : Had;
		const bool bTaxiway = TaxiwayStrip::HasStrip(*Network, Id);
		const double NewWidth = Becomes != nullptr ? Becomes->GetTotalWidth() : 0.0;
		const EIcaoCode NewLetter = IcaoCode::TaxiwayLetterForWidth(NewWidth);
		const FString Named = bTaxiway ? FString::Printf(TEXT("Code %s"), IcaoCode::ToLetter(NewLetter))
			: VariantWidthLabel(NewWidth).ToString();

		EIcaoCode Operates = NewLetter;
		// The strip the outline draws: StripWidthOf's answer - of the ghost when the width
		// changes (set below), of the live network otherwise. Never computed a second way.
		double GhostStrip = bTaxiway ? TaxiwayStrip::StripWidthOf(*Network, Id) : 0.0;
		if (!Why.IsEmpty())
		{
			Hover.bRefused = true;
			Hover.Text = FString::Printf(TEXT("Upgrade refused: %s"), *Why);
		}
		else if (Becomes == Had && Surface == Piece.Surface)
		{
			Hover.Text = FString::Printf(TEXT("already %s, %s"), *Named, Pavement::Name(Surface));
		}
		else
		{
			// WHICH WAY IT GOES, never "Upgrade to" (review fix 4): Upgrade is the MODE's word, and a
			// label that said it over a narrowing is how the trap read as a promotion.
			const double HadWidth = Had != nullptr ? Had->GetTotalWidth() : NewWidth;
			const TCHAR* Verb = NewWidth > HadWidth + 0.5 ? TEXT("Widen to")
				: NewWidth < HadWidth - 0.5 ? TEXT("Narrow to") : TEXT("Re-surface");
			Hover.Text = FMath::IsNearlyEqual(NewWidth, HadWidth, 0.5)
				? FString::Printf(TEXT("%s %s as %s"), Verb, *Named, Pavement::Name(Surface))
				: FString::Printf(TEXT("%s %s, %s"), Verb, *Named, Pavement::Name(Surface));
			if (bTaxiway && New != nullptr && New != Had)
			{
				// THE GRAPH AS IT WOULD BE: a duplicate carrying the new profile, asked the same
				// questions the rebuild and admission will ask of the real one after the click.
				// THE RESTRICTION PASS RUN ON IT, quietly, as the rebuild will run it on the real one -
				// so the closures below read the strip at the letter it will OPERATE (StripWidthOf's
				// ruling), and a neighbour the new pavement restricts is restricted here too.
				URoadNetwork* Ghost = DuplicateObject<URoadNetwork>(Network, GetTransientPackage());
				Ghost->SetSegmentProfile(Id, New);
				TaxiwayRestriction::Apply(*Ghost, /*bLog=*/false);
				TArray<FString> Effects;
				TaxiwayRestriction::FObstruction Worst;
				if (const TOptional<EIcaoCode> Restricted = TaxiwayRestriction::RestrictionOf(*Ghost, Id, &Worst))
				{
					Operates = Restricted.GetValue();
					Effects.Add(FString::Printf(TEXT("restricts to Code %s (%s)"), IcaoCode::ToLetter(Operates),
						*TaxiwayRestriction::Describe(*Ghost, Worst)));
				}
				GhostStrip = TaxiwayStrip::StripWidthOf(*Ghost, Id);
				// BY STAND NUMBER (strip stage 5), the one painted at its turn-off - not the entity
				// index, which a delete recycles. Integration of stages 5 and 6, 2026-09-29.
				const TArray<FEntityInstance>& Now = Network->GetEntities();
				const TArray<FEntityInstance>& Then = Ghost->GetEntities();
				for (int32 E = 0; E < Now.Num() && E < Then.Num(); ++E)
				{
					if (!Now[E].bAlive || !Now[E].IsStand()) { continue; }
					const bool bWas = StandAdmission::StripClosure(*Network, Now[E]).IsSet();
					const bool bWill = StandAdmission::StripClosure(*Ghost, Then[E]).IsSet();
					if (bWill && !bWas) { Effects.Add(FString::Printf(TEXT("closes stand %d"), Now[E].StandNumber)); }
					if (bWas && !bWill) { Effects.Add(FString::Printf(TEXT("reopens stand %d"), Now[E].StandNumber)); }
				}
				if (Effects.Num() > 0)
				{
					Hover.Text += TEXT(": ") + FString::Join(Effects, TEXT(", "));
				}
			}
		}

		// THE OUTLINE: the strip it would operate at for a taxiway, the pavement for a road.
		TaxiwayStrip::FSegmentShape Shape;
		if (TaxiwayStrip::ShapeOf(*Network, Id, Shape) && Becomes != nullptr)
		{
			Shape.HalfWidth = Becomes->GetMaxHalfWidth() + GhostStrip;
			Hover.Outline = TaxiwayStrip::FootprintOf(Shape);
		}
	}

	// THE MONEY HALF, FRESH EVERY CALL, hit or miss above - the purse moves through no edit for the
	// key to see. One quote and a compare; asked only when the site half let the upgrade through
	// (a refused site says so whatever the purse holds, as the composition orders it), and empty
	// for an upgrade that changes nothing, so "already so" survives an empty purse.
	// An unaffordable upgrade OVERRIDES the memoised label - the site text, effects and all, is
	// what it would say once the money is there - and draws as refused, outline included.
	FString Unaffordable;
	if (!Hover.bRefused)
	{
		Unaffordable = Context.Target->WhyUpgradeUnaffordable(Segment, Kind, UpgradeWidthIndex, Surface);
	}
	const bool bRefusedNow = Hover.bRefused || !Unaffordable.IsEmpty();
	if (Hover.Outline.Num() > 0)
	{
		Sink.Polygon(Hover.Outline, bRefusedNow ? EPreviewStyle::Refused : EPreviewStyle::Guide);
	}
	Sink.Label(Context.Cursor,
		Unaffordable.IsEmpty() ? Hover.Text : FString::Printf(TEXT("Upgrade refused: %s"), *Unaffordable),
		bRefusedNow ? EPreviewStyle::Refused : EPreviewStyle::Pending);
}

#undef LOCTEXT_NAMESPACE
