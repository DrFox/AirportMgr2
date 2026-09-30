// Node, segment and guideline surgery, plus the undo/mutator plumbing (Actor, EnsureNetwork,
// EnsureHistory, HistoryForEdit, MakeLiveNodeId/MakeLiveSegmentId, the ghost/traffic
// forwarders) every one of them shares. Undo/redo, aprons, stands, ClearNetwork and FindRoute
// are a second translation unit of this SAME class - see RoadEditFacadeSurfaces.cpp's own
// banner comment for why.

#include "Present/RoadEditFacade.h"

#include "Build/BuildCost.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Content/AirsideSettings.h"
#include "Misc/ScopeExit.h"
#include "Model/BuildPurse.h"

#include "AirsideLog.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadSlotMap.h"
#include "Model/TaxiwayStrip.h"
#include "Present/RoadNetworkActor.h"
#include "Profiles/RoadProfile.h"
#include "Solve/GuidelineGeom.h"
#include "Solve/RoadGeom.h"
#include "Tool/RoadPlacement.h"
#include "Solve/RunwayDesignator.h"
#include "Tool/GuidelineDrawTool.h"
#include "Present/RoadEditHistory.h"

ARoadNetworkActor& URoadEditFacade::Actor() const
{
	// A REFERENCE, not a pointer every caller re-checks: this facade REQUIRES an owning
	// actor (see the class comment), so a null Outer here is a construction error, not a
	// state to handle gracefully. checkf makes that loud at the first call rather than a
	// later null-network read that looks like an empty level.
	ARoadNetworkActor* Found = GetTypedOuter<ARoadNetworkActor>();
	checkf(Found != nullptr, TEXT("URoadEditFacade has no owning ARoadNetworkActor - it must ")
		TEXT("be a CreateDefaultSubobject of one, never constructed standalone"));
	return *Found;
}

URoadNetwork& URoadEditFacade::EnsureNetwork()
{
	ARoadNetworkActor& Owner = Actor();
	if (Owner.Network == nullptr)
	{
		// Loud, not fatal: a facade that belongs to a class default object is a pointer that
		// was copied from the CDO during duplication (see ARoadNetworkActor::PostInitProperties).
		// Before that fix every PIE click built a private airport on the CDO in silence.
		if (Owner.HasAnyFlags(RF_ClassDefaultObject))
		{
			UE_LOG(LogRoadMesh, Error,
				TEXT("EnsureNetwork on %s: an edit is reaching the CLASS DEFAULT OBJECT. Some instance ")
				TEXT("holds the CDO's facade instead of its own - see ARoadNetworkActor::PostInitProperties."),
				*Owner.GetName());
		}
		Owner.Network = NewObject<URoadNetwork>(&Owner);
	}
	return *Owner.Network;
}

URoadEditHistory& URoadEditFacade::EnsureHistory()
{
	ARoadNetworkActor& Owner = Actor();
	if (Owner.History == nullptr)
	{
		Owner.History = NewObject<URoadEditHistory>(&Owner);
	}

	// Re-applied on every edit rather than only at creation, so lowering the depth in the
	// details panel mid-session takes effect instead of waiting for a restart.
	Owner.History->MaxDepth = FMath::Max(Owner.MaxUndoDepth, 1);
	return *Owner.History;
}

URoadEditHistory* URoadEditFacade::HistoryForEdit()
{
	// The editor's transaction system does the Memento's job already - see the header. In a
	// game world there is no transaction system, so the history is the only undo there is.
	const UWorld* World = GetWorld();
	if (World != nullptr && !World->IsGameWorld())
	{
		return nullptr;
	}

	return &EnsureHistory();
}

void URoadEditFacade::ClearHistory()
{
	// NO EnsureHistory: a fresh, empty history behaves identically to a null one everywhere
	// else in this class (HistoryForEdit, CanUndo, CanRedo), so creating one just to clear it
	// would be a wasted allocation on every design-time load, where nothing has ever edited yet.
	if (URoadEditHistory* History = Actor().History)
	{
		History->Clear();
	}

	// THE EDITOR WORLD'S ROLLBACK POINT GOES WITH IT: it is the history-less twin of the pending
	// snapshot Clear() just abandoned, and for the same reason - a load is a new baseline, and
	// restoring a point taken before one would put the old airport back over the loaded one.
	EditorRollbackPoint = nullptr;
}

const URoadNetwork* URoadEditFacade::GetNetwork() const
{
	return Actor().Network;
}

double URoadEditFacade::GetMinimumRunwayLength() const
{
	return Actor().MinimumRunwayLength;
}

int32 URoadEditFacade::GetRunwayProfileCount() const
{
	return Actor().GetRunwayProfileCount();
}

URoadProfile* URoadEditFacade::ResolveRunwayProfile(int32 Index) const
{
	return Actor().ResolveRunwayProfile(Index);
}

int32 URoadEditFacade::GetWidthCount(ERoadKind Kind) const
{
	return Actor().GetWidthCount(Kind);
}

URoadProfile* URoadEditFacade::ResolveWidthProfile(ERoadKind Kind, int32 Index) const
{
	return Actor().ResolveWidthProfile(Kind, Index);
}

const UEntityDefinition* URoadEditFacade::GetEntityDefinition(EPlaceableEntity Kind) const
{
	// RESOLVED, not the raw field: PlaceEntity places from ResolveEntityDefinition()'s
	// content-default fallback, so the preview a tool draws from this must resolve the
	// SAME object or the two can disagree about what a click will actually place.
	return Actor().ResolveEntityDefinition(Kind);
}

TArray<PlotYard::FKitSpec> URoadEditFacade::ResolveDepotKits() const
{
	// THE ACTOR'S OWN RESOLVER, not a call to DepotKitSpecs here: content resolves in exactly
	// one function per CLAUDE.md, and ARoadNetworkActor::ResolveDepotKits is it - UPlotPresenter
	// reaches the same method, so a tool's preview and the presenter's built depot cannot come
	// from two different resolutions of the content set.
	return Actor().ResolveDepotKits();
}

void URoadEditFacade::UpdateGhost(int32 FromNodeIndex, const FRoadSnapResult& Snap, bool bValid,
	ERoadKind Kind, int32 WidthIndex)
{
	Actor().UpdateGhost(FromNodeIndex, Snap, bValid, Kind, WidthIndex);
}

void URoadEditFacade::HideGhost()
{
	Actor().HideGhost();
}

void URoadEditFacade::RebuildMesh()
{
	Actor().RebuildMesh();
}

bool URoadEditFacade::DispatchAgent(const FRoutePlan& Plan, const FAirframe& Airframe,
	ETraversalClass Class)
{
	return Actor().DispatchAgent(Plan, Airframe, Class);
}

bool URoadEditFacade::DispatchAgent(const FRoutePlan& Plan, const FVehicle& Vehicle,
	ETraversalClass Class)
{
	return Actor().DispatchAgent(Plan, Vehicle, Class);
}

bool URoadEditFacade::MakeLiveNodeId(int32 Index, FRoadNodeId& OutId) const
{
	// NodeIdAt already returns unset for a dead or out-of-range slot - see RoadSlot::HandleAt.
	// FRoadNodeId::IsSet() would only report that a handle was assigned, which is the
	// check this codebase renamed precisely to stop people reaching for it here; IsSet()
	// on a handle NodeIdAt produced is the liveness check, since a dead slot never gets one.
	const URoadNetwork* Network = GetNetwork();
	OutId = Network != nullptr ? Network->NodeIdAt(Index) : FRoadNodeId();
	return OutId.IsSet();
}

bool URoadEditFacade::MakeLiveSegmentId(int32 Index, FRoadSegmentId& OutId) const
{
	const URoadNetwork* Network = GetNetwork();
	OutId = Network != nullptr ? Network->SegmentIdAt(Index) : FRoadSegmentId();
	return OutId.IsSet();
}

void URoadEditFacade::NotifyChanged(EChangeKind Kind)
{
	// COUNTED BEFORE THE BATCH FOLD BELOW, so an edit inside an open batch still moves the epoch
	// a tool reads (IRoadEditTarget::GetEditEpoch): the model HAS changed, whenever the rebuild
	// that follows it is owed.
	++EditEpoch;

	// FOLDED, NOT BROADCAST, WHILE A BATCH IS OPEN - see the class comment's REBUILD BATCHES.
	if (RebuildBatchDepth > 0)
	{
		PendingBatchKind = PendingBatchKind.IsSet() ? CombineChangeKinds(*PendingBatchKind, Kind) : Kind;
		++FoldedNotifyCount;
		return;
	}
	OnChanged.Broadcast(Kind);
}

void URoadEditFacade::BeginRebuildBatch()
{
	++RebuildBatchDepth;
}

void URoadEditFacade::EndRebuildBatch()
{
	// UNMATCHED IS A CALLER BUG, and only reachable by calling this by hand - FRoadRebuildBatch
	// pairs the two. Refused rather than letting the depth go negative, which would make the
	// NEXT batch's close fire at depth -1 -> 0 one Begin early.
	if (RebuildBatchDepth <= 0)
	{
		UE_LOG(LogRoadMesh, Error, TEXT("EndRebuildBatch with no batch open - ignored (use FRoadRebuildBatch, not the raw calls)"));
		return;
	}
	if (--RebuildBatchDepth > 0)
	{
		return;
	}

	// THE OUTERMOST CLOSE: one notify, through NotifyChanged (depth is zero now, so it
	// broadcasts), keeping OnChanged.Broadcast's single call site. Nothing folded, nothing owed.
	if (!PendingBatchKind.IsSet())
	{
		return;
	}
	const EChangeKind Kind = *PendingBatchKind;
	const int32 Folded = FoldedNotifyCount;
	PendingBatchKind.Reset();
	FoldedNotifyCount = 0;
	// Logged, so "did the bulk edit rebuild, and as what" is one grep, the way every other
	// mutator's success line answers it for a single edit.
	UE_LOG(LogRoadMesh, Log, TEXT("Rebuild batch closed: %d notify(s) folded into one %s rebuild"),
		Folded, Kind == EChangeKind::Topology ? TEXT("Topology")
			: Kind == EChangeKind::Geometry ? TEXT("Geometry") : TEXT("Markings"));
	NotifyChanged(Kind);
}

void URoadEditFacade::WarnIfDerivedStale(const TCHAR* Who) const
{
	if (RebuildBatchDepth > 0 && PendingBatchKind.IsSet())
	{
		UE_LOG(LogRoadMesh, Warning,
			TEXT("%s inside a rebuild batch that has deferred a rebuild: the guideline graph it reads ")
			TEXT("predates the batch's own edits - close the batch first (see URoadEditFacade's REBUILD BATCHES)"),
			Who);
	}
}

bool URoadEditFacade::CanAfford(const FBuildQuote& Quote) const
{
	// NO PURSE MEANS FREE, and that is the design-time answer: URoadBuildEdMode has no runtime
	// and no money, and a default that refused would make the editor mode unable to build.
	return Purse == nullptr || Quote.IsFree() || Purse->CanAfford(Quote);
}

bool URoadEditFacade::AffordOrRefuse(const FBuildQuote& Quote)
{
	if (CanAfford(Quote)) // announces: the one check that broadcasts OnRefused (rule 32)
	{
		return true;
	}
	OnRefused.Broadcast(Quote, EBuildRefusal::CannotAfford);
	return false;
}

FBuildQuote URoadEditFacade::QuoteForSegment(int32 SegmentIndex) const
{
	const URoadNetwork* Network = Actor().Network;
	if (Network == nullptr || !Network->GetSegments().IsValidIndex(SegmentIndex))
	{
		return FBuildQuote();
	}
	const FRoadSegment& Segment = Network->GetSegments()[SegmentIndex];
	const URoadProfile* Profile = Network->ProfileFor(Segment);
	if (Profile == nullptr)
	{
		return FBuildQuote();
	}
	// PavementOf, not Segment.Surface: a runway's ground is FRunwayFacts', and reading the
	// field directly would refund a grass RUNWAY at the tarmac rate - the same over-refund
	// Segment.Surface alone would give a grass road, and delete-and-redraw would print money.
	return BuildCost::ForSegment(*Profile, BuildCost::SegmentLengthUu(*Network, Segment),
		Network->PavementOf(Network->SegmentIdAt(SegmentIndex)));
}

FBuildQuote URoadEditFacade::QuoteForAllPavement() const
{
	FBuildQuote Total;
	const URoadNetwork* Network = Actor().Network;
	if (Network == nullptr)
	{
		return Total;
	}

	for (int32 Index = 0; Index < Network->GetSegments().Num(); ++Index)
	{
		if (!Network->GetSegments()[Index].bAlive)
		{
			continue;
		}
		const FBuildQuote Each = QuoteForSegment(Index);
		// EVERY SEGMENT'S OWN LINE, kept rather than folded into one total: a discount keyed on
		// one profile must not also discount a neighbour segment laid from a different one -
		// see FBuildLine::Source.
		Total.Lines.Append(Each.Lines);
	}
	Total.What = NSLOCTEXT("BuildCost", "MovedPavement", "Moved pavement");
	return Total;
}

FBuildQuote URoadEditFacade::QuoteForApron(TConstArrayView<FVector2D> Outline,
	TOptional<EPavement> Pavement) const
{
	const UAirsideSettings* Settings = GetDefault<UAirsideSettings>();
	// NO PAVEMENT for a bare apron: FApronSurface carries no EPavement of its own (RoadApron.h's
	// own comment), so its callers pass it unset and it quotes at its authored rate. A stand's
	// pad passes its own (QuoteStand) since shared-pavement Task 8.
	return BuildCost::ForApron(Outline,
		Settings != nullptr ? Settings->ApronCostPerSquareMetre : 0.0, Pavement);
}

void URoadEditFacade::CommitPurchase(FRoadEditScope& Edit, const FBuildQuote& Quote)
{
	if (Purse != nullptr && !Quote.IsFree())
	{
		const int32 ChargeId = Purse->Charge(Quote);
		if (URoadEditHistory* History = HistoryForEdit())
		{
			// ONTO THE PENDING SNAPSHOT, before the scope's destructor pushes it: the snapshot
			// IS the edit as far as undo is concerned, so the charge to reverse has to travel
			// with it. In an editor world HistoryForEdit is null and the id is simply dropped,
			// which is correct there - the transaction system owns undo and nothing was paid.
			History->SetPendingCharge(ChargeId, Quote);
		}
	}
	CommitAndNotify(Edit);
}

void URoadEditFacade::CommitDisposal(FRoadEditScope& Edit, const FBuildQuote& Quote)
{
	if (Purse != nullptr && !Quote.IsFree())
	{
		// CREDIT, not Reverse: tearing something out is a NEW transaction valuing the geometry
		// at today's price, not the undoing of the one that built it. See IBuildPurse::Credit -
		// that asymmetry is what keeps a BuiltFor field out of FRoadSegment.
		Purse->Credit(Quote);
	}
	CommitAndNotify(Edit);
}

void URoadEditFacade::CommitAndNotify(FRoadEditScope& Edit, EChangeKind Kind)
{
	Edit.Commit();
	NotifyChanged(Kind);
}

void URoadEditFacade::AdoptNetwork(URoadNetwork& NewNetwork)
{
	Actor().Network = &NewNetwork;

	// The preview may be describing a node that no longer exists in the replacement, and its
	// cache compares only the cursor and the start node - neither of which a network swap
	// changes. See this method's own header comment for the four call sites this replaced.
	Actor().HideGhost();
	NotifyChanged();
}

bool URoadEditFacade::RollBackOpenEdit(URoadEditHistory* Use)
{
	URoadNetwork* Live = Actor().Network;
	if (Live == nullptr)
	{
		return false;
	}

	if (Use != nullptr)
	{
		if (!Use->RollbackEdit(*Live))
		{
			return false;
		}
	}
	else if (EditorRollbackPoint != nullptr)
	{
		Live->RestoreFrom(*EditorRollbackPoint);
	}
	else
	{
		return false;
	}

	// THE NETWORK IS THE SAME OBJECT, so this assigns it to itself - AdoptNetwork is called for
	// its OTHER two jobs, which a restore needs exactly as a swap does: hide a ghost that may be
	// describing a node the restore has just taken away, and tell everything derived that the
	// graph changed shape. Not a second copy of that tail (issue #299 spent a change removing four).
	AdoptNetwork(*Live);
	return true;
}

void URoadEditFacade::AnnounceReplaced(ENetworkReplace Phase)
{
	// Logged, so "did the drivers hear that the airport was replaced" is one grep - the question
	// #426 was, for a load, answered by reading five call sites.
	UE_LOG(LogRoadMesh, Log, TEXT("Network replaced: %s"),
		Phase == ENetworkReplace::Discarding ? TEXT("discarding the live graph") : TEXT("replacement adopted"));
	OnReplaced.Broadcast(Phase);
}

bool URoadEditFacade::RestoreInPlace(TFunctionRef<bool(URoadNetwork&)> Deserialise)
{
	ARoadNetworkActor& Owner = Actor();
	if (Owner.Network == nullptr)
	{
		// NOTHING TO RESTORE INTO, and nothing announced: no replacement happened. A caller that
		// wants a load to make a network makes one first (AirportOps' LoadFromSlot refuses).
		UE_LOG(LogRoadMesh, Warning, TEXT("RestoreInPlace refused: no live network to restore into"));
		return false;
	}

	// THE ORDER IS THE HEADER'S, step for step - see RestoreInPlace there for why each is where it is.
	AnnounceReplaced(ENetworkReplace::Discarding);
	const bool bRestored = Deserialise(*Owner.Network);
	if (bRestored)
	{
		Owner.RepairLoadedNetwork(ELoadedFrom::SaveGame);
		// THROUGH THE FACADE'S OWN DOOR (issue #191) - the undo stack is this class's to manage.
		ClearHistory();
	}
	else
	{
		UE_LOG(LogRoadMesh, Warning, TEXT("RestoreInPlace: the restore failed - rebuilding what the network holds, history kept"));
	}
	AdoptNetwork(*Owner.Network);
	AnnounceReplaced(ENetworkReplace::Adopted);
	return bRestored;
}

bool URoadEditFacade::ApplyInteractiveMutation(const TCHAR* Label,
	TFunctionRef<bool(URoadNetwork&)> Mutate, bool bChangesGraphShape,
	TFunctionRef<bool(const URoadNetwork&)> Verify)
{
	ARoadNetworkActor& Owner = Actor();

	// JOINS A DRAG ALREADY IN PROGRESS, so the whole drag is one undo step; on its own it is
	// one edit of its own. IsEditing is what tells the two apart - see this method's own
	// header comment.
	URoadEditHistory* Use = HistoryForEdit();
	const bool bOwnsEdit = Use != nullptr && !Use->IsEditing();
	if (bOwnsEdit)
	{
		Use->BeginEdit(*Owner.Network, Label);
	}

	// THE SAME QUESTION, FOR A WORLD WITH NO HISTORY (issue #437): is there an open edit to join,
	// or does this call own one? The drag's rollback point answers it - BeginInteractiveEdit took
	// it - and a bare call with none open takes its own and lets it go on every way out, so a
	// Verify failure always has a state to go back to. Never taken in a game world: there the
	// history's pending snapshot is the same thing, and a second copy would be pure waste.
	const bool bOwnsPoint = Use == nullptr && EditorRollbackPoint == nullptr;
	if (bOwnsPoint)
	{
		EditorRollbackPoint = FRoadEditScope::SnapshotForRollback(*Owner.Network);
	}
	ON_SCOPE_EXIT
	{
		if (bOwnsPoint)
		{
			EditorRollbackPoint = nullptr;
		}
	};

	const bool bMutated = Mutate(*Owner.Network);
	if (!bMutated)
	{
		if (bOwnsEdit)
		{
			// Nothing was touched, so ABANDON is right here and Revert would be wrong - see
			// URoadEditHistory::RollbackEdit on the distinction.
			Use->AbandonEdit();
		}
		return false;
	}

	if (!Verify(*Owner.Network))
	{
		// REVERTED, NOT ABANDONED, and in EVERY world rather than gated on there being a history
		// or on bOwnsEdit - see this method's own header comment for why a Verify failure cannot
		// simply be refused, and for the editor world this used to leave merged.
		if (!RollBackOpenEdit(Use))
		{
			UE_LOG(LogRoadMesh, Error,
				TEXT("'%s' failed its Verify and there was no open edit to roll back to - the network "
					 "keeps the change, which nothing else will undo"), Label);
		}
		return false;
	}

	if (bOwnsEdit)
	{
		Use->CommitEdit();
	}

	// GEOMETRY ONLY WHILE AN INTERACTIVE EDIT IS STILL OPEN, AND ONLY WHEN THIS MUTATION DOES
	// NOT CHANGE THE GRAPH'S SHAPE - see this method's own header comment (the bare-call trap,
	// issue #165/#190, and bChangesGraphShape, issue #299) for the full reasoning.
	if (!bChangesGraphShape && bInteractiveEditOpen)
	{
		bGeometryChangedDuringEdit = true;
		NotifyChanged(EChangeKind::Geometry);
	}
	else
	{
		NotifyChanged(EChangeKind::Topology);
	}

	return true;
}

int32 URoadEditFacade::PlaceNode(FVector2D Where)
{
	// The network is made BEFORE the scope, so the snapshot is of an empty graph rather
	// than of nothing at all - otherwise the first node of a session is the one edit that
	// cannot be undone.
	URoadNetwork& Net = EnsureNetwork();
	FRoadEditScope Edit(HistoryForEdit(), &Net, TEXT("place node"));

	const FRoadNodeId Node = Net.AddNode(Where);
	if (!Node.IsSet())
	{
		// Every refusal after a scope opens rolls it back (Check-Architecture rule 40, issue #437):
		// whether this one wrote anything is the question the next edit to this function gets wrong.
		Edit.Rollback();
		UE_LOG(LogRoadMesh, Warning, TEXT("PlaceNode refused at (%f, %f)"), Where.X, Where.Y);
		return INDEX_NONE;
	}

	// Success is logged as well as refusal: a lone node draws no mesh, so without this line
	// "I clicked and nothing happened" and "I clicked and a node was placed" read the same
	// in the log - and the census line below it counts slots, not live nodes.
	// Names the actor and world too: the 2026-09-06 PIE bug was caught by this line saying
	// "on Default__RoadNetworkActor in no world" - see ARoadNetworkActor::PostInitProperties.
	UE_LOG(LogRoadMesh, Log, TEXT("Node %d placed at (%.0f, %.0f), generation %d - on %s in %s"),
		Node.Index, Where.X, Where.Y, Node.Generation,
		*Actor().GetName(), Actor().GetWorld() ? *Actor().GetWorld()->GetName() : TEXT("no world"));
	CommitAndNotify(Edit);
	return Node.Index;
}

URoadProfile* URoadEditFacade::ResolveProfileFor(ERoadKind Kind, int32 WidthIndex)
{
	// EXTRACTED FROM ConnectNodes (below, this same class) SO THE GHOST AND THE CLICK CANNOT
	// DISAGREE. The preview has to price what a click would actually lay, and a tool resolving
	// the profile for itself would be a second answer to "which profile is this?" - the exact
	// shape of bug the registry and the action table exist to prevent elsewhere. THE RULE ITSELF
	// moved back here (issue #298) from ARoadNetworkActor, which now composes its own
	// ResolveProfileFor by forwarding to this - see the header's own comment for the width rule.
	//
	// Until 2026-09-23 a service road ignored the index outright: it had one authored
	// cross-section, and a TAXIWAY index reaching it would have laid 23 m for vans. The index is
	// now resolved against the ROAD list (Owner.ResolveWidthProfile keys by kind), so it can
	// only ever name a road tier.
	ARoadNetworkActor& Owner = Actor();
	URoadProfile* Chosen = nullptr;
	if (WidthIndex != INDEX_NONE)
	{
		Chosen = Owner.ResolveWidthProfile(Kind, WidthIndex);
	}
	else if (Kind == ERoadKind::ServiceRoad)
	{
		Chosen = Owner.ResolveServiceRoadProfile();
	}
	if (Chosen == nullptr && Kind != ERoadKind::ServiceRoad)
	{
		// No index, or an index the content set cannot answer. Either way the level's own
		// tuning is the honest fallback here - unlike the service road, a taxiway always has one.
		Chosen = Owner.ResolveProfile();
	}
	return Chosen;
}

FBuildSessionTunables URoadEditFacade::MakeTunables(double ViewWorldWidth)
{
	ARoadNetworkActor& Owner = Actor();

	// The corner-fit rule needs the width of the road about to be drawn, which only the
	// actor's own profile resolver knows - refreshed on PlacementLimits itself, not just the
	// Tunables copy, so PlanNodeDeletion (which reads PlacementLimits directly, not through
	// here) judges a rejoin against the same width a click just did. NewRoadHalfWidth is
	// deliberately not a UPROPERTY - see FRoadPlacementLimits - so this is a cache refresh, the
	// same shape as RuntimeProfile, not a write to authored state.
	//
	// THE STRIP JUDGE DOES NOT READ THIS (stage 3, 2026-09-29). It is the TAXIWAY default
	// whatever kind is being laid, so a 6 m service road would be judged 24 m wide and refused
	// at twice its reach from every taxiway. WhySegmentRefused takes its half-width from
	// ResolveProfileFor(Kind, WidthIndex) instead; this stays for the corner-fit checks that
	// other tests pin to it.
	if (const URoadProfile* ProfileForLimits = Owner.ResolveProfile())
	{
		Owner.PlacementLimits.NewRoadHalfWidth = ProfileForLimits->GetMaxHalfWidth();
	}

	FBuildSessionTunables Tunables;
	Tunables.Snap = Owner.Snap;
	Tunables.GuideSources = Owner.GuideSources;
	Tunables.Limits = Owner.PlacementLimits;

	// RESOLVED HERE, THE ONE CHOKEPOINT BOTH DRIVERS READ (#292 review finding, same shape as
	// Limits above): FStandPlotTool::DescribeLetter's ghost preview reads FToolContext::
	// Envelopes rather than IcaoCode::FloorEnvelopeForLetter, so it agrees with the drawn-stand
	// commit path and the live point-placement path, which both call UAirsideSettings::
	// ResolveLetterEnvelope directly (Present/ may reach Content/; Tool/ may not).
	Tunables.Envelopes = UAirsideSettings::ResolveLetterEnvelopeTable();

	// THE SERVICE ROAD'S HALF-WIDTH, through the actor's one resolver for its profile (user
	// ruling 2026-09-27): the plot ghost's ServiceEdge line sits this far out past a stand's far
	// edge - see FToolContext::ServiceRoadHalfWidth. No profile resolved leaves it 0, on the edge.
	if (const URoadProfile* ServiceRoad = Owner.ResolveServiceRoadProfile())
	{
		Tunables.ServiceRoadHalfWidth = ServiceRoad->GetMaxHalfWidth();
	}

	// ViewWorldWidth > 0: the caller has no view-scale UPROPERTY of its own to read (the
	// editor tool) and wants a radius that stays clickable at any zoom - the same 2% floor
	// URoadBuildEditorTool::MakeContextAt used to compute for itself. 0: the caller (the
	// runtime driver) has its own ToolPickRadius and overwrites this right after - see
	// ARoadBuildController::MakeToolContext.
	if (ViewWorldWidth > 0.0)
	{
		const double Floor = FMath::Max(150.0, ViewWorldWidth * 0.02);
		Tunables.ToolPickRadius = Floor;

		// A FLOOR ON THE AUTHORED VALUE, not an overwrite of it: the road-snap radii are
		// per-airport now (ARoadNetworkActor::Snap, issue #93), and folding them down to a
		// fixed 150/150 here would be a THIRD place they came from, on top of the level
		// author's own choice and the class default. Without the floor, "has to be a screen
		// distance, not a world one" - the reason MakeContextAt computed this at all - goes
		// straight back to being sub-pixel at 20000 uu of view width: max() keeps whichever
		// of the two is more generous, so a wide-open airport with untouched defaults still
		// snaps by screen size, and an airport whose author widened NodeRadius past the
		// floor keeps that choice.
		Tunables.Snap.NodeRadius = FMath::Max(Tunables.Snap.NodeRadius, Floor);
		Tunables.Snap.SegmentRadius = FMath::Max(Tunables.Snap.SegmentRadius, Floor);
	}
	else
	{
		Tunables.ToolPickRadius = FBuildSessionTunables().ToolPickRadius;
	}

	return Tunables;
}

FBuildQuote URoadEditFacade::QuoteForConnect(int32 FromIndex, FVector2D To, ERoadKind Kind,
	int32 WidthIndex, EPavement Surface) const
{
	const URoadNetwork* Network = Actor().Network;
	// THROUGH THE ACTOR'S OWN FORWARDER, not this class's ResolveProfileFor directly - that
	// method is non-const (it may reach Actor().ResolveProfile(), which lazily fills
	// RuntimeProfile, a decision that method's own comment records deliberately), and this
	// method is const. Actor() hands back a non-const reference from a const method, so a quote
	// can still ask the question - through ARoadNetworkActor::ResolveProfileFor, which forwards
	// straight back to THIS object's own (non-const) ResolveProfileFor above - without that
	// decision having to be reversed for it.
	const URoadProfile* Profile = Actor().ResolveProfileFor(Kind, WidthIndex);
	if (Network == nullptr || Profile == nullptr || !Network->GetNodes().IsValidIndex(FromIndex))
	{
		return FBuildQuote();
	}
	return BuildCost::ForSegment(*Profile,
		FVector2D::Distance(Network->GetNodes()[FromIndex].Position, To), Surface);
}

FString URoadEditFacade::WhySegmentRefused(int32 FromIndex, const FRoadSnapResult& To, ERoadKind Kind, int32 WidthIndex) const
{
	const URoadNetwork* Network = Actor().Network;
	FRoadNodeId From;
	if (Network == nullptr || !MakeLiveNodeId(FromIndex, From))
	{
		return FString();
	}
	// THROUGH THE ACTOR'S FORWARDER, QuoteForConnect's reason: this is const and the facade's
	// own ResolveProfileFor is not. No profile = nothing to judge; ConnectNodes refuses that
	// case itself, with its own message.
	const URoadProfile* Profile = Actor().ResolveProfileFor(Kind, WidthIndex);
	if (Profile == nullptr)
	{
		return FString();
	}

	TaxiwayStrip::FSegmentShape Shape;
	Shape.A = Network->GetNodes()[From.Index].Position;
	Shape.B = To.Position;
	Shape.Control = (Shape.A + Shape.B) * 0.5;   // straight - AddStraightSegment's own control
	Shape.HalfWidth = Profile->GetMaxHalfWidth();

	TaxiwayStrip::FSegmentEnd AtA;
	AtA.Node = From;
	AtA.At = Shape.A;
	TaxiwayStrip::FSegmentEnd AtB;
	AtB.At = To.Position;
	if (To.Kind == ERoadSnapKind::Node)
	{
		AtB.Node = To.Node;
	}
	else if (To.Kind == ERoadSnapKind::Segment)
	{
		AtB.Segment = To.Segment;
	}

	const TaxiwayStrip::FStripVerdict Verdict =
		TaxiwayStrip::JudgeSegment(*Network, Shape, Kind == ERoadKind::Taxiway, AtA, AtB);
	return Verdict.bRefused ? Verdict.Text : FString();
}

bool URoadEditFacade::ConnectNodes(int32 FromIndex, int32 ToIndex, ERoadKind Kind, int32 WidthIndex,
	EPavement Surface)
{
	if (FromIndex == ToIndex)
	{
		UE_LOG(LogRoadMesh, Warning, TEXT("ConnectNodes refused: node %d cannot join itself"), FromIndex);
		return false;
	}

	FRoadNodeId From;
	FRoadNodeId To;
	if (!MakeLiveNodeId(FromIndex, From) || !MakeLiveNodeId(ToIndex, To))
	{
		UE_LOG(LogRoadMesh, Warning,
			TEXT("ConnectNodes refused: %d -> %d, one of them is not a live node"), FromIndex, ToIndex);
		return false;
	}

	ARoadNetworkActor& Owner = Actor();

	// RESOLVED PER KIND, here rather than in the tool - see ERoadKind for why a tool never
	// names an asset.
	//
	// A ROAD WITH NO PROFILE IS REFUSED, not laid as a taxiway. The same choice PlaceRunway
	// makes below and for the same reason - the right shape on screen and the wrong
	// behaviour at every junction, with nothing to say so - but worse here than there: a
	// taxiway profile carries an AIRCRAFT guideline, so the silent fallback would admit
	// aeroplanes onto a lane laid for vans. There is no transient fallback to reach for
	// either; see ARoadNetworkActor::ResolveServiceRoadProfile.
	//
	// WHICH WIDTH is ARoadNetworkActor::ResolveProfileFor's rule, stated once there: a chosen
	// index names a standard width of THIS kind (a taxiway code or a road tier), INDEX_NONE
	// the kind's default.
	URoadProfile* Chosen = ResolveProfileFor(Kind, WidthIndex);
	if (Chosen == nullptr && Kind == ERoadKind::ServiceRoad)
	{
		UE_LOG(LogRoadMesh, Warning,
			TEXT("ConnectNodes refused: %d -> %d, no service road profile. Author "
				 "DA_RoadProfile_ServiceRoad with Tools/Python/build_road_profiles.py, or set "
				 "ServiceRoadProfile on the actor."), FromIndex, ToIndex);
		return false;
	}

	// A SURFACE THE PROFILE DOES NOT OFFER is refused here, before the scope, for the price's
	// reason below: URoadNetwork::SetSegmentSurface would refuse it only after
	// AddStraightSegment, leaving a tarmac road charged at Surface's rate. The tool's row is
	// built from the same list, so this is reached only by a caller that skipped the row.
	// ENFORCED BY: Airside.Present.BuildPurseConnectRefusesUnofferedPavement (concrete on a
	// taxiway: refused, nothing laid, nothing charged); Airside.Present.GrassRoadLaid (a row
	// pick of grass connects, no refusal)
	if (Chosen != nullptr && !Pavement::Offered(Chosen->AllowedPavements).Contains(Surface))
	{
		UE_LOG(LogRoadMesh, Warning, TEXT("ConnectNodes refused: %s is not offered by profile %s"),
			Pavement::Name(Surface), *Chosen->GetName());
		return false;
	}

	// INSIDE A TAXIWAY'S CLEARANCE STRIP (strip stage 3), BEFORE THE PRICE, so a refused road
	// is never quoted. THE SAME WhySegmentRefused the draw tool's readout and click ask - a
	// Node snap at To, which is what the click's Segment snap has become by now (its split made
	// the node), so preview and commit judge one geometry.
	// ENFORCED BY: Airside.Tool.RoadRefusedInsideStrip
	{
		FRoadSnapResult AtTo;
		AtTo.Kind = ERoadSnapKind::Node;
		AtTo.Node = To;
		AtTo.Position = Owner.Network->GetNodes()[To.Index].Position;
		const FString Why = WhySegmentRefused(FromIndex, AtTo, Kind, WidthIndex);
		if (!Why.IsEmpty())
		{
			UE_LOG(LogRoadMesh, Log, TEXT("ConnectNodes refused: %s"), *Why);
			return false;
		}
	}

	// PRICED AND REFUSED BEFORE THE SCOPE, not at commit. An FRoadEditScope that is not
	// committed discards its undo snapshot but does NOT roll the network back by itself, so a
	// refusal after AddStraightSegment that forgot Rollback() would leave the taxiway built and
	// unpaid for - see CommitPurchase's own comment. This one is priced first regardless: it is
	// the refusal a player can hit and be told about, the one the preview has already shown, and
	// one known before the first write costs no snapshot and no restore.
	const FBuildQuote Quote = BuildCost::ForSegment(*Chosen,
		FVector2D::Distance(Owner.Network->GetNodes()[From.Index].Position,
			Owner.Network->GetNodes()[To.Index].Position), Surface);
	if (!AffordOrRefuse(Quote))
	{
		UE_LOG(LogRoadMesh, Log, TEXT("ConnectNodes refused: cannot afford %s"),
			*Quote.What.ToString());
		return false;
	}

	// Created after the guards above, all of which refuse without mutating anything, so a
	// rejected connection never costs a snapshot.
	FRoadEditScope Edit(HistoryForEdit(), Owner.Network,
		Kind == ERoadKind::ServiceRoad ? TEXT("connect road nodes") : TEXT("connect nodes"));

	// Straight only. The model stores a Bezier control point, but AddSegment still
	// interpolates its interior samples in a straight line, so a curve authored here
	// would render as a chord until slice 2b samples the curve properly.
	const FRoadSegmentId Segment = Owner.Network->AddStraightSegment(From, To, Chosen);
	if (!Segment.IsSet())
	{
		Edit.Rollback();   // rule 40 - see PlaceNode
		UE_LOG(LogRoadMesh, Warning, TEXT("ConnectNodes refused: %d -> %d"), FromIndex, ToIndex);
		return false;
	}

	// INSIDE THE SCOPE, before the commit, so the surface is part of the one undo step and
	// the rebuild the commit fires already sees grass - see IRoadEditTarget::ConnectNodes.
	Owner.Network->SetSegmentSurface(Segment, Surface);

	UE_LOG(LogRoadMesh, Log, TEXT("Segment %d connected: node %d -> node %d, %s"),
		Segment.Index, FromIndex, ToIndex, Pavement::Name(Surface));
	CommitPurchase(Edit, Quote);
	return true;
}

FBuildQuote URoadEditFacade::QuoteForRunway(FVector2D From, FVector2D To, const URoadProfile* Profile,
	EPavement Pavement) const
{
	return Profile != nullptr
		? BuildCost::ForSegment(*Profile, FVector2D::Distance(From, To), Pavement) : FBuildQuote();
}

bool URoadEditFacade::PlaceRunway(FVector2D From, FVector2D To, URoadProfile* RunwayProfile, const FRunwayFacts& Facts)
{
	ARoadNetworkActor& Owner = Actor();
	if (Owner.Network == nullptr)
	{
		return false;
	}

	if (RunwayProfile == nullptr)
	{
		// Refused rather than defaulted. Falling back to ResolveProfile here would lay a
		// taxiway at runway length and call it a runway - the right shape on screen and the
		// wrong behaviour at every exit, with nothing to say so.
		UE_LOG(LogRoadMesh, Warning,
			TEXT("PlaceRunway refused: no runway profile. Check Project Settings > Plugins > "
				 "Airside, the content set's RunwayProfiles."));
		return false;
	}

	const double Length = FVector2D::Distance(From, To);
	if (Length < Owner.MinimumRunwayLength)
	{
		UE_LOG(LogRoadMesh, Warning,
			TEXT("PlaceRunway refused: %.0f uu is under the %.0f uu minimum"),
			Length, Owner.MinimumRunwayLength);
		return false;
	}

	// After the guards, all of which refuse without mutating, so a rejected runway never
	// costs a snapshot - the same rule ConnectNodes follows.
	// Priced from the two ENDS the caller asked for, before anything is mutated - see
	// ConnectNodes above for why the refusal cannot wait until commit.
	// THROUGH QuoteForRunway, the function the runway tool's ghost prices with - one answer to
	// "what does this strip cost", so the preview and the charge cannot drift. Priced at
	// Facts.Surface - the runway's ground is FRunwayFacts', not the profile's.
	const FBuildQuote Quote = QuoteForRunway(From, To, RunwayProfile, Facts.Surface);
	if (!AffordOrRefuse(Quote))
	{
		UE_LOG(LogRoadMesh, Log, TEXT("PlaceRunway refused: cannot afford %s"),
			*Quote.What.ToString());
		return false;
	}

	FRoadEditScope Edit(HistoryForEdit(), Owner.Network, TEXT("place runway"));

	const FRoadNodeId A = Owner.Network->AddNode(From);
	const FRoadNodeId B = Owner.Network->AddNode(To);
	if (!A.IsSet() || !B.IsSet())
	{
		// A REFUSAL AFTER A WRITE: the first AddNode may have landed. Before a scope could roll
		// back this left a bare node behind with no undo step to reach it (issue #437).
		Edit.Rollback();
		return false;
	}

	// STRAIGHT, and the model cannot express otherwise here: AddStraightSegment puts the
	// control point on the midpoint, which IsStraight tests for exactly.
	const FRoadSegmentId Segment = Owner.Network->AddStraightSegment(A, B, RunwayProfile);
	if (!Segment.IsSet())
	{
		// Both nodes are in by now; the rollback takes them out - see above.
		Edit.Rollback();
		return false;
	}

	// The facts go on in the SAME edit as the pavement, so an undo takes both back: a
	// runway that came back as tarmac after Ctrl+Z on its classification would be a
	// runway the player never placed.
	Owner.Network->SetRunwayFacts(Segment, Facts);

	CommitPurchase(Edit, Quote);

	UE_LOG(LogRoadMesh, Log, TEXT("Runway %s placed, %.0f uu long, %.0f uu wide, %s, %s approach"),
		*RunwayDesignator::ToPairText(To - From), Length, RunwayProfile->GetTotalWidth(),
		Pavement::Name(Facts.Surface), RunwayApproachName(Facts.Approach));
	return true;
}

bool URoadEditFacade::SetRunwayFacts(int32 SegmentIndex, const FRunwayFacts& InFacts)
{
	FRunwayFacts Facts = InFacts;
	URoadNetwork* Network = Actor().Network;
	if (Network == nullptr)
	{
		return false;
	}
	FRoadSegmentId Segment;
	if (!MakeLiveSegmentId(SegmentIndex, Segment) || !Network->IsRunwaySegment(Segment))
	{
		// Refused BEFORE the snapshot, like every other guard on this seam: an uncommitted scope
		// does not roll back on its own (Rollback() is explicit), and a refusal that can be known
		// first costs no snapshot and no restore.
		return false;
	}
	// InUse 0 means "keep the strip's" (URoadNetwork::SetRunwayFacts) - resolved HERE too, so a
	// surface-only reclassify that changes nothing is still "already so" rather than an undo step.
	const FRunwayFacts Was = Network->RunwayFactsFor(Segment);
	if (Facts.InUse == 0)
	{
		Facts.InUse = Was.InUse;
	}
	if (Facts.Use == ERunwayUse::Unset)
	{
		Facts.Use = Was.Use;
	}
	if (Was == Facts)
	{
		// Already so. True, because the runway IS what was asked for - but no edit, since
		// an undo step that changes nothing is a Ctrl+Z the player has to press twice.
		return true;
	}

	FRoadEditScope Edit(HistoryForEdit(), Network, TEXT("set runway facts"));
	Network->SetRunwayFacts(Segment, Facts);
	CommitAndNotify(Edit);

	UE_LOG(LogRoadMesh, Log, TEXT("Runway at segment %d reclassified: %s, %s approach (the whole strip)"),
		SegmentIndex, Pavement::Name(Facts.Surface), RunwayApproachName(Facts.Approach));
	if (Facts.InUse != Was.InUse)
	{
		// The line to grep when "the planes still land the old way": a flip reached the model.
		// Planned flights keep their plan (ruling 2) - only the next one reads this.
		// The pair from the clicked segment's own axis: a runway is straight, so any member
		// names the strip, and ToPairText is low-first whichever way the segment was drawn.
		const FRoadSegment* Piece = Network->GetSegment(Segment);
		const FRoadNode* A = Piece != nullptr ? Network->GetNode(Piece->A) : nullptr;
		const FRoadNode* B = Piece != nullptr ? Network->GetNode(Piece->B) : nullptr;
		const FString Pair = A != nullptr && B != nullptr
			? RunwayDesignator::ToPairText(B->Position - A->Position) : FString(TEXT("?"));
		UE_LOG(LogRoadMesh, Log, TEXT("Runway %s in use: %s (was %s)"), *Pair,
			*RunwayDesignator::ToText(Facts.InUse), *RunwayDesignator::ToText(Was.InUse));
	}
	if (RunwayUse::Resolve(Facts.Use) != RunwayUse::Resolve(Was.Use))
	{
		// The line to grep when "the second runway is still empty": the mode reached the model.
		// Like the direction, planned flights keep their plan; the next one reads this.
		UE_LOG(LogRoadMesh, Log, TEXT("Runway at segment %d takes: %s (was %s)"), SegmentIndex,
			RunwayUse::Name(Facts.Use), RunwayUse::Name(Was.Use));
	}
	return true;
}

namespace
{
	/** What an upgrade would lay: Kind's standard width WidthIndex, or - INDEX_NONE - the
	 *  segment's own profile, a surface-only upgrade. Through ResolveWidthProfile (const, the
	 *  content list), not ResolveProfileFor: that folds in each kind's DEFAULT, and an upgrade
	 *  never means "whatever a fresh road would be". */
	const URoadProfile* UpgradeTarget(const ARoadNetworkActor& Owner, const URoadNetwork& Network,
		const FRoadSegment& Segment, ERoadKind Kind, int32 WidthIndex)
	{
		return WidthIndex == INDEX_NONE ? Network.ProfileFor(Segment) : Owner.ResolveWidthProfile(Kind, WidthIndex);
	}
}

FString URoadEditFacade::WhyUpgradeRefused(int32 SegmentIndex, ERoadKind Kind, int32 WidthIndex, EPavement Surface) const
{
	return WhyUpgradeRefusedImpl(SegmentIndex, Kind, WidthIndex, Surface, nullptr);
}

FString URoadEditFacade::WhyUpgradeRefusedImpl(int32 SegmentIndex, ERoadKind Kind, int32 WidthIndex, EPavement Surface,
	FBuildQuote* OutUnaffordable) const
{
	// THE TWO HALVES, SITE FIRST, and nothing else (issue #439, WhyStandRefusedImpl's split): the
	// Upgrade hover asks them one at a time - it remembers the first and never the second - while
	// the click asks them together here, so the pair and the whole cannot be two evaluators.
	// THE PRICE GATE USED TO SIT BEFORE THE STRIP GATE; it now follows it, so an upgrade that is
	// both unaffordable and refused by a neighbour's strip says the strip. Earning the money could
	// not make it committable, and "cannot afford" would promise that it could.
	// ENFORCED BY: Airside.Present.UpgradeRefusalIsTheTwoHalves
	const FString Site = WhyUpgradeSiteRefused(SegmentIndex, Kind, WidthIndex, Surface);
	if (!Site.IsEmpty())
	{
		return Site;
	}
	return WhyUpgradeUnaffordableImpl(SegmentIndex, Kind, WidthIndex, Surface, OutUnaffordable);
}

FString URoadEditFacade::WhyUpgradeSiteRefused(int32 SegmentIndex, ERoadKind Kind, int32 WidthIndex, EPavement Surface) const
{
	const URoadNetwork* Network = Actor().Network;
	FRoadSegmentId Id;
	if (Network == nullptr || !MakeLiveSegmentId(SegmentIndex, Id))
	{
		return TEXT("there is no road there to upgrade");
	}
	if (Network->IsRunwaySegment(Id))
	{
		// A runway's width and surface are its whole strip's - the runway tool's, chain-wide.
		return TEXT("a runway is upgraded with the runway tool");
	}
	const FRoadSegment& Segment = Network->GetSegments()[SegmentIndex];
	const bool bIsTaxiway = TaxiwayStrip::IsAircraftOnly(*Network, Id);
	if (bIsTaxiway != (Kind == ERoadKind::Taxiway))
	{
		return bIsTaxiway ? TEXT("that is a taxiway - upgrade it with the taxiway tool")
			: TEXT("that is a service road - upgrade it with the road tool");
	}
	const URoadProfile* Old = Network->ProfileFor(Segment);
	const URoadProfile* New = UpgradeTarget(Actor(), *Network, Segment, Kind, WidthIndex);
	if (Old == nullptr || New == nullptr)
	{
		return TEXT("no such width in the content set");
	}
	// URoadNetwork::SetSegmentProfile's own refusals, asked HERE so the commit below can never
	// meet them (review fix 5): a runway cross-section, or one of the other kind - a content
	// list that put either among Kind's widths would otherwise half-apply the upgrade.
	if (New->bContinuousThroughJunctions || TaxiwayStrip::IsAircraftOnlyProfile(New) != bIsTaxiway)
	{
		return FString::Printf(TEXT("%s is not a %s cross-section"), *New->GetName(), bIsTaxiway ? TEXT("taxiway") : TEXT("service road"));
	}
	if (!Pavement::Offered(New->AllowedPavements).Contains(Surface))
	{
		return FString::Printf(TEXT("%s is not offered on this width"), Pavement::Name(Surface));
	}
	if (New == Old && Surface == Segment.Surface)
	{
		return FString();   // already so - UpgradeSegment answers true with no edit
	}

	// NO PRICE GATE HERE - WhyUpgradeUnaffordable's, asked fresh (issue #439): this function is the
	// half a tool may remember, and the purse is no part of the model it can key on.

	// WIDENING INTO A NEIGHBOUR IS LAYING PAVEMENT (plan Task 2): the new edge is judged as if
	// laid now - its own nodes, itself ignored - as a ROAD (bIsTaxiway false), so only its
	// PAVEMENT in another taxiway's strip refuses. What its grown strip swallows is what the
	// restriction pass and stand admission answer, not a refusal - the point of this stage.
	// ONLY WHEN IT WIDENS: a surface change or a narrowing lays no new ground, and a segment
	// already inside a strip (a map from before stage 3) must still be re-surfaceable.
	if (New->GetMaxHalfWidth() > Old->GetMaxHalfWidth())
	{
		TaxiwayStrip::FSegmentShape Shape;
		if (TaxiwayStrip::ShapeOf(*Network, Id, Shape))
		{
			Shape.HalfWidth = New->GetMaxHalfWidth();
			TaxiwayStrip::FSegmentEnd AtA;
			AtA.Node = Segment.A;
			AtA.At = Shape.A;
			TaxiwayStrip::FSegmentEnd AtB;
			AtB.Node = Segment.B;
			AtB.At = Shape.B;
			const FRoadSegmentId Self[] = { Id };
			const TaxiwayStrip::FStripVerdict Verdict = TaxiwayStrip::JudgeSegment(*Network, Shape, false, AtA, AtB, Self);
			if (Verdict.bRefused)
			{
				return Verdict.Text;
			}
		}
	}
	return FString();
}

FString URoadEditFacade::WhyUpgradeUnaffordable(int32 SegmentIndex, ERoadKind Kind, int32 WidthIndex, EPavement Surface) const
{
	return WhyUpgradeUnaffordableImpl(SegmentIndex, Kind, WidthIndex, Surface, nullptr);
}

FString URoadEditFacade::WhyUpgradeUnaffordableImpl(int32 SegmentIndex, ERoadKind Kind, int32 WidthIndex, EPavement Surface,
	FBuildQuote* OutUnaffordable) const
{
	// THE SEGMENT AND ITS TWO PROFILES, resolved again rather than handed over: a caller asking
	// this half alone of an upgrade the site half refuses (a dead slot, a width that does not
	// resolve) gets "" - nothing to price, the refusal is the site half's to state.
	const URoadNetwork* Network = Actor().Network;
	FRoadSegmentId Id;
	if (Network == nullptr || !MakeLiveSegmentId(SegmentIndex, Id))
	{
		return FString();
	}
	const FRoadSegment& Segment = Network->GetSegments()[SegmentIndex];
	const URoadProfile* Old = Network->ProfileFor(Segment);
	const URoadProfile* New = UpgradeTarget(Actor(), *Network, Segment, Kind, WidthIndex);
	if (Old == nullptr || New == nullptr)
	{
		return FString();
	}
	if (New == Old && Surface == Segment.Surface)
	{
		return FString();   // already so - nothing to pay for, and the site half agrees
	}
	const FBuildQuote Quote = BuildCost::ForUpgrade(*Old, Segment.Surface, *New, Surface,
		BuildCost::SegmentLengthUu(*Network, Segment));
	if (!CanAfford(Quote)) // preview: WhyUpgradeRefused is asked by the tool every frame - silent here, UpgradeSegment announces
	{
		if (OutUnaffordable != nullptr)
		{
			*OutUnaffordable = Quote;
		}
		return FString::Printf(TEXT("cannot afford %s"), *Quote.What.ToString());
	}
	return FString();
}

bool URoadEditFacade::UpgradeSegment(int32 SegmentIndex, ERoadKind Kind, int32 WidthIndex, EPavement Surface)
{
	// SetRunwayFacts' SHAPE: every guard before the snapshot (an uncommitted scope discards its
	// undo step but does not roll the network back), already-so answers true with no edit, and
	// ONE scope for both writes so one Ctrl+Z reverts width and surface together.
	// THE SAME EVALUATOR THE TOOL ASKED, plus which quote could not be afforded - the one refusal here
	// the preview could show but a player might still click through, announced so it is not only a log line.
	FBuildQuote Unaffordable;
	const FString Why = WhyUpgradeRefusedImpl(SegmentIndex, Kind, WidthIndex, Surface, &Unaffordable);
	if (!Why.IsEmpty())
	{
		if (Unaffordable.Lines.Num() > 0)
		{
			OnRefused.Broadcast(Unaffordable, EBuildRefusal::CannotAfford);
		}
		UE_LOG(LogRoadMesh, Log, TEXT("UpgradeSegment refused: segment %d, %s"), SegmentIndex, *Why);
		return false;
	}
	URoadNetwork* Network = Actor().Network;
	const FRoadSegmentId Id = Network->SegmentIdAt(SegmentIndex);
	const FRoadSegment& Segment = Network->GetSegments()[SegmentIndex];
	const URoadProfile* Old = Network->ProfileFor(Segment);
	URoadProfile* New = const_cast<URoadProfile*>(UpgradeTarget(Actor(), *Network, Segment, Kind, WidthIndex));
	const EPavement WasSurface = Segment.Surface;
	if (New == Old && Surface == WasSurface)
	{
		return true;
	}
	const FBuildQuote Quote = BuildCost::ForUpgrade(*Old, WasSurface, *New, Surface,
		BuildCost::SegmentLengthUu(*Network, Segment));
	const double WasWidth = Old->GetTotalWidth();

	FRoadEditScope Edit(HistoryForEdit(), Network, TEXT("upgrade segment"));
	// HONOURED, not discarded (review fix 5, CLAUDE.md's out-parameter rule): WhyUpgradeRefused
	// asked both setters' refusals already, so either failing here is a new refusal it does not
	// know about. An uncommitted scope rolls nothing back by itself, so the scope is told to: the
	// width the first write landed comes back with the whole model, bitwise, rather than by a
	// second write of the old profile (issue #437 - that hand unwind needed a const_cast to say
	// "the old profile" and could only ever undo the one field its author remembered).
	// ENFORCED BY: Airside.Present.UpgradeSecondWriteRefusedRollsBack
	if (!Network->SetSegmentProfile(Id, New))
	{
		Edit.Rollback();
		UE_LOG(LogRoadMesh, Warning, TEXT("UpgradeSegment refused: segment %d would not take %s"), SegmentIndex, *New->GetName());
		return false;
	}
	if (!Network->SetSegmentSurface(Id, Surface))
	{
		Edit.Rollback();
		UE_LOG(LogRoadMesh, Warning, TEXT("UpgradeSegment refused: segment %d would not take %s"), SegmentIndex, Pavement::Name(Surface));
		return false;
	}
	CommitPurchase(Edit, Quote);

	// The line to grep when "the upgrade did nothing": what reached the model, both halves.
	UE_LOG(LogRoadMesh, Log, TEXT("Segment %d upgraded: %.1f m %s -> %.1f m %s"), SegmentIndex,
		WasWidth / 100.0, Pavement::Name(WasSurface), New->GetTotalWidth() / 100.0, Pavement::Name(Surface));
	return true;
}

bool URoadEditFacade::AddReverseTurn(int32 NodeIndex, int32 FromFarIndex, int32 IntoFarIndex)
{
	URoadNetwork* Network = Actor().Network;
	if (Network == nullptr)
	{
		return false;
	}
	FReverseTurn Turn;
	Turn.Node = Network->NodeIdAt(NodeIndex);
	Turn.FromFar = Network->NodeIdAt(FromFarIndex);
	Turn.IntoFar = Network->NodeIdAt(IntoFarIndex);
	const FRoadNode* Node = Network->GetNode(Turn.Node);
	// BOTH PAIRS MUST BE ARMS OF THE NODE - refused here, where the caller can still be told,
	// rather than recorded and dropped by the next rebuild with only a log line to show for it.
	auto IsArm = [Network, Node, &Turn](FRoadNodeId Far)
	{
		return Node != nullptr && Node->Incident.ContainsByPredicate(
			[Network, &Turn, Far](const FRoadSegmentId& Seg) { return Network->GetOtherEnd(Seg, Turn.Node) == Far; });
	};
	if (!Turn.Node.IsSet() || !Turn.FromFar.IsSet() || !Turn.IntoFar.IsSet() || Turn.FromFar == Turn.IntoFar
		|| !IsArm(Turn.FromFar) || !IsArm(Turn.IntoFar))
	{
		UE_LOG(LogRoadMesh, Warning, TEXT("AddReverseTurn refused: nodes %d / %d / %d are not a junction and two of its arms"),
			NodeIndex, FromFarIndex, IntoFarIndex);
		return false;
	}

	// FREE, like a guideline link: it lays no pavement, only lines on the road already there.
	FRoadEditScope Edit(HistoryForEdit(), Network, TEXT("add reverse turn"));
	Network->AddReverseTurn(Turn);
	CommitAndNotify(Edit);
	return true;
}

int32 URoadEditFacade::ConnectGuidelines(int32 FromNodeIndex, int32 ToNodeIndex)
{
	// Indexes the GUIDELINE graph - stale inside a batch that deferred its rebuild.
	WarnIfDerivedStale(TEXT("ConnectGuidelines"));
	URoadNetwork* Network = Actor().Network;
	if (Network == nullptr)
	{
		return INDEX_NONE;
	}

	const TArray<FGuidelineNode>& Nodes = Network->GetGuidelineNodes();
	if (!Nodes.IsValidIndex(FromNodeIndex) || !Nodes.IsValidIndex(ToNodeIndex))
	{
		return INDEX_NONE;
	}

	const FGuidelineNodeId From = Network->GuidelineNodeIdAt(FromNodeIndex);
	const FGuidelineNodeId To = Network->GuidelineNodeIdAt(ToNodeIndex);
	if (!From.IsSet() || !To.IsSet())
	{
		// A valid INDEX whose slot is no longer alive - GuidelineNodeIdAt reports that with
		// an unset handle rather than a reason, so this used to fall through to Validate()
		// below and log from there. Logged here now with the same text Validate() would have
		// given (NoStart, "nothing to link here"), so a dead node still says why (2026-09-13
		// review of #79).
		UE_LOG(LogRoadMesh, Warning, TEXT("ConnectGuidelines refused: %s"),
			FGuidelineDrawTool::Describe(EGuidelineLink::NoStart));
		return INDEX_NONE;
	}

	if (FGuidelineDrawTool::Validate(*Network, From, To) != EGuidelineLink::Valid)
	{
		UE_LOG(LogRoadMesh, Warning, TEXT("ConnectGuidelines refused: %s"),
			FGuidelineDrawTool::Describe(FGuidelineDrawTool::Validate(*Network, From, To)));
		return INDEX_NONE;
	}

	FRoadEditScope Edit(HistoryForEdit(), Network, TEXT("link guidelines"));

	FGuidelineEdge Edge;
	Edge.A = From;
	Edge.B = To;

	// Straight, like a derived guideline on a straight segment: Control at the midpoint.
	// A curve would need a gesture to author it and would render as a chord regardless.
	Edge.Control = (Nodes[FromNodeIndex].Position + Nodes[ToNodeIndex].Position) * 0.5;

	// EVERY class. A hand-drawn link exists because the graph is missing a connection, and
	// guessing a narrower rule would make it silently useless to whoever needed it - with
	// no way to tell, because a refusal to route looks the same as no link at all.
	Edge.AllowedTraffic = FTrafficMask::All();
	Edge.Direction = EGuidelineDir::Bidirectional;

	// The player's, so the builder steps aside for it.
	Edge.bDerived = false;

	// And WHAT its ends are, not merely where they are now. Without this it survives every
	// rebuild attached to nodes the new derivation abandoned - drawn, and routing nothing.
	Edge.EndRefA = Nodes[FromNodeIndex].Origin;
	Edge.EndRefB = Nodes[ToNodeIndex].Origin;

	const FGuidelineEdgeId Added = Network->AddGuidelineEdge(MoveTemp(Edge));
	if (!Added.IsSet())
	{
		// Whether AddGuidelineEdge writes before it refuses is not this function's business:
		// Rollback is called regardless (rule 40, issue #437 - see PlaceNode), because "does this
		// refusal write?" is the question the next edit to this function gets wrong. The scope
		// then has nothing left for ~FRoadEditScope to abandon.
		Edit.Rollback();
		UE_LOG(LogRoadMesh, Warning,
			TEXT("ConnectGuidelines refused: node %d -> node %d, AddGuidelineEdge declined"),
			FromNodeIndex, ToNodeIndex);
		return INDEX_NONE;
	}

	// COMMITTED (#125): this used to fall off the end of the function with the scope still
	// open, so ~FRoadEditScope called AbandonEdit instead - no undo step pushed for a
	// hand-drawn link, and OnChanged never fired, so nothing rebuilt the overlay or the mesh
	// to show it. Every other mutator on this seam commits before returning; this one simply
	// never had the line.
	CommitAndNotify(Edit);
	return Added.Index;
}

bool URoadEditFacade::DisconnectGuideline(int32 EdgeIndex)
{
	// Indexes the GUIDELINE graph - stale inside a batch that deferred its rebuild.
	WarnIfDerivedStale(TEXT("DisconnectGuideline"));
	URoadNetwork* Network = Actor().Network;
	const FGuidelineEdgeId Id = Network != nullptr ? Network->GuidelineEdgeIdAt(EdgeIndex) : FGuidelineEdgeId();
	if (!Id.IsSet())
	{
		return false;
	}

	// A derived edge belongs to the pavement, and the next rebuild would put it straight
	// back. Obeying here would be indistinguishable from ignoring the click. Checked BEFORE
	// DeleteSlot, which has no notion of this refusal - it only ever sees a doomed handle
	// that is or is not there.
	if (Network->GetGuidelineEdges()[EdgeIndex].bDerived)
	{
		UE_LOG(LogRoadMesh, Warning,
			TEXT("DisconnectGuideline refused: edge %d is derived from a road, not hand-drawn"),
			EdgeIndex);
		return false;
	}

	// COMMITTED (#125): this used to return RemoveGuidelineEdge's result directly with the
	// scope still open, so ~FRoadEditScope called AbandonEdit on a SUCCESSFUL removal - no
	// undo step for it, and OnChanged never fired, so a disconnected guideline stayed on
	// screen until something else happened to rebuild it. Now DeleteSlot's own
	// CommitAndNotify does that (#103 review: this had grown the same guard-scope-remove-
	// commit shape as DeleteApron/DeleteEntity and belonged with them).
	return DeleteSlot(true, TEXT("unlink guidelines"),
		[Id](URoadNetwork& Net) { return Net.RemoveGuidelineEdge(Id); });
}

bool URoadEditFacade::SetDriveSide(EDriveSide Side)
{
	URoadNetwork* Network = Actor().Network;
	// Refused BEFORE the scope: a no-op that committed inside one would push an undo step that
	// does nothing, and a refusal known first costs no snapshot and no restore.
	if (Network == nullptr || Network->GetDriveSide() == Side)
	{
		return false;
	}
	FRoadEditScope Edit(HistoryForEdit(), Network, TEXT("drive side"));
	Network->SetDriveSide(Side);
	// TOPOLOGY, not Markings: every lane moves, so the guideline graph is re-derived - which
	// is the whole edit. The centre-line paint does not move (it sits on offset 0), but the
	// rebuild repaints it anyway, and that is cheaper than a second change kind for one edit.
	CommitAndNotify(Edit, EChangeKind::Topology);

	int32 Lanes = 0;
	for (const FGuidelineEdge& Edge : Network->GetGuidelineEdges())
	{
		Lanes += (Edge.bAlive && Edge.bDerived && Edge.DerivedFrom.IsSet()
			&& Edge.Direction != EGuidelineDir::Bidirectional) ? 1 : 0;
	}
	UE_LOG(LogRoadMesh, Log, TEXT("Drive side -> %s, %d road lane edge(s) re-derived"),
		Side == EDriveSide::Left ? TEXT("Left") : TEXT("Right"), Lanes);
	return true;
}

bool URoadEditFacade::AddEntityModule(FEntityInstanceId Entity, EDepotModule Module)
{
	URoadNetwork* Network = Actor().Network;
	// REFUSED BEFORE THE SCOPE, SetDriveSide's reason: a refusal known first costs no snapshot and no
	// restore, and a refused write that committed would push an undo step that does nothing.
	const FEntityInstance* Instance = Network != nullptr ? Network->GetEntity(Entity) : nullptr;
	if (Instance == nullptr || !Instance->bAlive || !Instance->IsDepot())
	{
		UE_LOG(LogRoadMesh, Warning, TEXT("AddEntityModule refused: entity %d is not a live depot"), Entity.Index);
		return false;
	}
	// REFUSED WHILE A DRAG IS OPEN: ClearHistory below would drop the drag's pending snapshot, and its
	// EndInteractiveEdit would then land a whole drag with no undo step. The inspector's Buy is a click the
	// player makes between drags, but nothing in the UI makes that impossible, so the facade says no.
	// ENFORCED BY: AirportOps.Present.Facility.ShedRefusedDuringADrag
	if (bInteractiveEditOpen)
	{
		UE_LOG(LogRoadMesh, Warning, TEXT("AddEntityModule refused: depot %d - an interactive edit is open"), Entity.Index);
		return false;
	}
	{
		// A SCOPE AND CommitAndNotify, as the facade's other mutators notify - then closed, so its
		// destructor has pushed the step BEFORE the history is cleared below.
		FRoadEditScope Edit(HistoryForEdit(), Network, TEXT("buy module"));
		Network->AddEntityModule(Entity, Module);
		CommitAndNotify(Edit, EChangeKind::Topology);
	}
	ClearHistory();
	UE_LOG(LogRoadMesh, Log, TEXT("Depot %d: %s added; undo history cleared (a purchase is a checkpoint)"),
		Entity.Index, *UEnum::GetValueAsString(Module));
	return true;
}

int32 URoadEditFacade::RemoveUnseatedModules(FEntityInstanceId Entity, EDepotModule Module, int32 Count)
{
	URoadNetwork* Network = Actor().Network;
	const FEntityInstance* Instance = Network != nullptr ? Network->GetEntity(Entity) : nullptr;
	if (Instance == nullptr || !Instance->bAlive || !Instance->IsDepot())
	{
		UE_LOG(LogRoadMesh, Warning, TEXT("RemoveUnseatedModules refused: entity %d is not a live depot"), Entity.Index);
		return 0;
	}
	// REFUSED WHILE A DRAG IS OPEN, AddEntityModule's reason: the ClearHistory below would drop the drag's pending
	// snapshot. Deferred, not lost: the repair asks again on the next network change it hears.
	if (bInteractiveEditOpen)
	{
		UE_LOG(LogRoadMesh, Warning, TEXT("RemoveUnseatedModules deferred: depot %d - an interactive edit is open"), Entity.Index);
		return 0;
	}
	// NO SCOPE: a repair is not a step the player can undo (see the header). Notified as a Topology change, AddEntityModule's
	// kind, so the yard and the drop count are rebuilt from the list as it now stands.
	const int32 Removed = Network->RemoveEntityModules(Entity, Module, Count);
	if (Removed == 0)
	{
		return 0;
	}
	NotifyChanged(EChangeKind::Topology);
	ClearHistory();
	UE_LOG(LogRoadMesh, Log, TEXT("Depot %d: %d unseated %s removed; undo history cleared (a repair is a checkpoint)"),
		Entity.Index, Removed, *UEnum::GetValueAsString(Module));
	return Removed;
}

bool URoadEditFacade::SetIntermediateHoldingPosition(int32 NodeIndex, bool bSet)
{
	// Indexes the GUIDELINE graph - stale inside a batch that deferred its rebuild.
	WarnIfDerivedStale(TEXT("SetIntermediateHoldingPosition"));
	URoadNetwork* Network = Actor().Network;
	if (Network == nullptr)
	{
		return false;
	}
	const TArray<FGuidelineNode>& Nodes = Network->GetGuidelineNodes();
	const FGuidelineNodeId Node = Network->GuidelineNodeIdAt(NodeIndex);
	if (!Node.IsSet())
	{
		return false;
	}

	// WHAT THE NODE SAYS NOW, read BEFORE the mutation, so a no-op can be recognised after it. A
	// click that clears an already-clear position changes nothing, and committing it would give
	// the player an undo step that visibly does nothing and has to be pressed twice to get past -
	// see URoadEditHistory's "Edit lifecycle" comment. Also what a refusal names below: the kind
	// is the reason.
	const EHoldingPositionKind Before = Nodes[NodeIndex].HoldingPosition;
	FRoadEditScope Edit(HistoryForEdit(), Network, TEXT("holding point"));

	// THE MODEL'S OWN REFUSAL, asked once, HERE (issue #437). This function used to re-type
	// URoadNetwork::SetIntermediateHoldingPosition's two rules - a runway-holding position and a
	// road's taxiway-crossing stop line are derived on every build, not the player's - above the
	// scope, because a scope could not roll back and a refusal inside one had to be one that had
	// written nothing: "a future mutation followed by a return false inside a scope would leave a
	// changed graph with no undo entry for it". Two copies of a model rule in the facade is how
	// the next kind of derived position gets refused in the model and honoured here. The scope can
	// roll back now, so whatever the model wrote before it said no goes back with the rest.
	// ENFORCED BY: Airside.Present.DerivedHoldingRefusedThroughFacade
	if (!Network->SetIntermediateHoldingPosition(Node, bSet))
	{
		Edit.Rollback();
		UE_LOG(LogRoadMesh, Warning,
			TEXT("SetIntermediateHoldingPosition refused at guideline node %d (set %d): it is a derived "
				 "holding position (kind %d) - a runway's, or a road's stop line at a taxiway crossing - "
				 "and not the player's"),
			NodeIndex, bSet, static_cast<int32>(Before));
		return false;
	}
	if (Network->GetGuidelineNodes()[NodeIndex].HoldingPosition == Before)
	{
		// Succeeded and changed nothing. Leaving the scope uncommitted abandons the pending
		// snapshot - the stacks are untouched and the live graph is not restored, which is
		// exactly right here because nothing was altered to restore. True is still returned:
		// the caller asked for a state, and that state holds.
		UE_LOG(LogRoadMesh, Verbose,
			TEXT("Holding point at guideline node %d already as asked - no undo step pushed"), NodeIndex);
		return true;
	}
	// THROUGH CommitAndNotify, NOT a bare Commit() (issue #179). The old comment here said a
	// holding position changes no pavement and no mesh - true when it was written, false
	// since FHoldingPositionMarkingBuilder started painting Node.HoldingPosition ==
	// Intermediate as a dashed bar in URoadSurfacePresenter::RebuildMarkings. A bare Commit()
	// left that paint layer stale until an unrelated edit happened to rebuild it - the exact
	// "notification split-brain" issue #77 closed, reintroduced by a justification that
	// predated the paint layer it was talking about.
	//
	// EChangeKind::Markings, NOT the Topology default: this changes neither the pavement nor
	// the graph's SHAPE, so re-deriving the guideline graph over it (what Topology does, via
	// FRoadGuidelineBuilder::Build) would be wasted work that also reallocates every live
	// FGuidelineNodeId, including the node this call just toggled - see EChangeKind's own
	// comment, which was written from three tests that failed the day Topology was tried here.
	//
	// EXCEPT A SET THAT LANDS AT A STRIP EDGE (taxiway strip stage 4): the hold is realised down
	// the arm, on a node the builder splits there, so the graph's shape DOES change and only a
	// re-derive puts the bar where it belongs. The clicked node's handle does not survive that,
	// which is the cost the Markings ruling above avoided - paid only when there is a split.
	const bool bReshapes = bSet && FRoadGuidelineBuilder::IntermediateHoldMovesOffEnd(*Network, Node);
	CommitAndNotify(Edit, bReshapes ? EChangeKind::Topology : EChangeKind::Markings);
	UE_LOG(LogRoadMesh, Log, TEXT("Holding point %s at guideline node %d"),
		bSet ? TEXT("set") : TEXT("cleared"), NodeIndex);
	return true;
}

int32 URoadEditFacade::FindNodeNear(FVector2D Where, double Radius) const
{
	const URoadNetwork* Network = GetNetwork();
	if (Network == nullptr || Radius <= 0.0)
	{
		return INDEX_NONE;
	}

	return RoadSlot::NearestAlive<FRoadNode>(Network->GetNodes(), Where, Radius,
		[](const FRoadNode& Node) { return Node.Position; });
}

int32 URoadEditFacade::SplitSegment(int32 SegmentIndex, FVector2D At)
{
	FRoadSegmentId Doomed;
	if (!MakeLiveSegmentId(SegmentIndex, Doomed))
	{
		UE_LOG(LogRoadMesh, Warning,
			TEXT("SplitSegment refused: %d is not a live segment"), SegmentIndex);
		return INDEX_NONE;
	}

	ARoadNetworkActor& Owner = Actor();
	FRoadEditScope Edit(HistoryForEdit(), Owner.Network, TEXT("split segment"));

	// The surgery itself moved to URoadNetwork::SplitSegment (#104): it is genuinely graph
	// surgery, touches nothing this facade owns, and a Model/ test and the presenter's ghost
	// preview both used to reach into this class's own static SplitSegmentIn for it - see
	// that method's comment, now on URoadNetwork::SplitSegment, for why it was here at all.
	const FRoadNodeId Middle = Owner.Network->SplitSegment(Doomed, At);
	if (!Middle.IsSet())
	{
		Edit.Rollback();   // rule 40 - see PlaceNode
		UE_LOG(LogRoadMesh, Warning,
			TEXT("SplitSegment refused: segment %d at (%f, %f)"), SegmentIndex, At.X, At.Y);
		return INDEX_NONE;
	}

	CommitAndNotify(Edit);
	return Middle.Index;
}

void URoadEditFacade::BeginInteractiveEdit(const FString& Label)
{
	// REFUSED INSIDE A REBUILD BATCH - see the class comment's REBUILD BATCHES: a drag spans
	// frames and repaints every one, a batch is one synchronous call. Refused BEFORE any field
	// is touched, so bInteractiveEditOpen stays false and the matching EndInteractiveEdit
	// no-ops on its own guard.
	if (RebuildBatchDepth > 0)
	{
		UE_LOG(LogRoadMesh, Error,
			TEXT("BeginInteractiveEdit('%s') refused: a rebuild batch is open, and a drag cannot live inside one"),
			*Label);
		return;
	}

	URoadEditHistory* Use = HistoryForEdit();
	URoadNetwork* Network = Actor().Network;
	if (Network != nullptr && Use != nullptr && !Use->IsEditing())
	{
		Use->BeginEdit(*Network, Label);
	}
	else if (Network != nullptr && Use == nullptr && !bInteractiveEditOpen)
	{
		// THE HISTORY-LESS WORLD'S SNAPSHOT (issue #437): the state a Verify failure in the middle
		// of this drag - a drop-to-merge the solver cannot corner - has to come back to. Taken here,
		// once, rather than per frame: a drag is one edit, and its frames must not each copy the
		// network. NOT taken again when a drag is already open, the way the branch above keeps the
		// pending snapshot of an edit that is still going.
		EditorRollbackPoint = FRoadEditScope::SnapshotForRollback(*Network);
	}

	// WHAT THE PAVEMENT WAS WORTH BEFORE THE DRAG. Without this the cost model has a hole big
	// enough to drive through: build ten metres of taxiway, drag its end two kilometres, and
	// the extra pavement is free - MoveNode creates no segment, so nothing else charges for it.
	PavementValueAtDragStart = QuoteForAllPavement().BaseAmount();

	// A FRESH EDIT HAS MOVED NOTHING YET - see the field's own comment for what
	// EndInteractiveEdit does with this.
	bGeometryChangedDuringEdit = false;

	// SET UNCONDITIONALLY, WHATEVER Use AND Network TURNED OUT TO BE - see the field's own
	// comment (issue #190). PavementValueAtDragStart and bGeometryChangedDuringEdit above are
	// already set the same way, and this is the flag MoveNode/MoveApronCorner now decide
	// Geometry vs Topology on, so it has to be true for every world BeginInteractiveEdit is
	// called in, not only the one with a History to open.
	bInteractiveEditOpen = true;
}

void URoadEditFacade::EndInteractiveEdit(bool bKeep)
{
	// GATED ON bInteractiveEditOpen, NOT ON History (issue #190) - see that field's own
	// comment. History is null in an editor world by HistoryForEdit()'s own design, and the
	// old `History == nullptr || !History->IsEditing()` guard treated that exactly like "no
	// edit is open", so an editor-mode drag's EndInteractiveEdit call was always a no-op: no
	// bookkeeping reset, and no chance to fire the Topology notify a drag that moved
	// something still owes the derived graph.
	if (!bInteractiveEditOpen)
	{
		return;
	}
	bInteractiveEditOpen = false;

	// THE DRAG IS OVER, kept or not, so nothing may restore to its start any more: let the point go
	// (issue #437). Before the branches below, none of which reads it - the cannot-afford revert is
	// a game-world one and restores from the history's pending snapshot.
	EditorRollbackPoint = nullptr;

	URoadEditHistory* History = Actor().History;
	const bool bHasHistoryEdit = History != nullptr && History->IsEditing();

	if (!bKeep)
	{
		if (bHasHistoryEdit)
		{
			History->AbandonEdit();
		}

		// LATENT STALENESS ON ABANDON (issue #165 follow-up). AbandonEdit only drops the undo
		// SNAPSHOT - it does not put the nodes back, because they were never recorded as a
		// scope's mutation in the first place (MoveNode/MoveApronCorner bypass
		// CommitAndNotify's scope entirely - see the class comment). So a drag that moved
		// something and was then abandoned (Escape cancels a drag rather than committing it)
		// would leave the guideline graph, anchor links, plots and traffic pointed at the
		// PRE-drag positions FOREVER: nothing else will ever notify Topology for this edit.
		// Before #165 every MoveNode/MoveApronCorner notify ran the whole pipeline, so an
		// abandoned drag was still fresh; this restores exactly that guarantee, and ONLY when
		// something actually moved - an edit opened and abandoned with no successful move
		// costs nothing, same as it always has. bHasHistoryEdit's absence changes nothing
		// here: an editor-world abandon has no snapshot to drop either way, only the same
		// catch-up notify to fire.
		if (bGeometryChangedDuringEdit)
		{
			NotifyChanged(EChangeKind::Topology);
		}
		bGeometryChangedDuringEdit = false;
		return;
	}

	if (bHasHistoryEdit)
	{
		// THE DIFFERENCE THE DRAG MADE, priced at today's rates. A drag that lengthened the
		// pavement is a purchase; one that shortened it is a disposal, and is credited at scrap
		// value rather than refunded in full - otherwise dragging a taxiway long and short again
		// would be a loop that returns more than it costs.
		const FBuildQuote After = QuoteForAllPavement();
		const double RawDelta = After.BaseAmount() - PavementValueAtDragStart;
		PavementValueAtDragStart = 0.0;

		// ONE SYNTHETIC LINE, not the network's own segment lines: the delta is a NET figure
		// across the WHOLE network (QuoteForAllPavement's own comment), not one buildable, so
		// it is priced as a single line at rate 1 with the difference itself as Quantity - the
		// first segment met stands in as Source, for a discount to key on only; the AMOUNT is
		// the true difference whichever segment that is.
		// PAVEMENT UNSET ON PURPOSE: After and PavementValueAtDragStart are both already at
		// their own segments' factors (QuoteForAllPavement's lines each carry their own
		// Pavement), so RawDelta is a difference of two ALREADY-FACTORED totals. Setting a
		// Pavement here would apply Pavement::RateFactor a second time to a figure that has
		// already paid it once.
		FBuildQuote Delta;
		Delta.Lines.Add({ After.Lines.IsValidIndex(0) ? After.Lines[0].Source : nullptr,
			EBuildUnit::Each, FMath::Abs(RawDelta), 1.0, {} });
		Delta.What = After.What;

		if (RawDelta > 0.0 && !AffordOrRefuse(Delta))
		{
			// REVERTED, NOT ABANDONED. The node has already moved on every frame of the drag, so
			// dropping the snapshot would leave the longer taxiway standing and unpaid for. This is
			// the one edit that has to be undone rather than merely refused - see
			// URoadEditHistory::RollbackEdit. RollBackOpenEdit (issue #437) is the same "put the
			// network back and catch the ghost up" door ApplyInteractiveMutation's own
			// Verify-failure branch uses, through AdoptNetwork (#299)'s tail.
			if (RollBackOpenEdit(History))
			{
				UE_LOG(LogRoadMesh, Log,
					TEXT("Drag reverted: cannot afford the %.0f of pavement it added"), Delta.BaseAmount());
			}
			else
			{
				// A line that must not read as a revert when nothing was reverted: the pavement stays,
				// unpaid for, and the log is where that has to show.
				UE_LOG(LogRoadMesh, Error,
					TEXT("Drag could NOT be reverted (nothing to roll back to): it added %.0f of pavement the "
						 "player cannot afford, and it stays"), Delta.BaseAmount());
			}
			return;
		}

		if (Purse != nullptr)
		{
			if (RawDelta > 0.0)
			{
				History->SetPendingCharge(Purse->Charge(Delta), Delta);
			}
			else if (RawDelta < 0.0)
			{
				Purse->Credit(Delta);
			}
		}

		History->CommitEdit();
	}
	else
	{
		// THE HISTORY-LESS BRANCH (issue #190) - AN EDITOR WORLD, where HistoryForEdit() never
		// hands BeginInteractiveEdit anything to open. There is no undo snapshot to commit
		// (the editor's own transaction already covers Ctrl+Z - see this class's header) and
		// no economy to charge against outside PIE, so this skips the pricing, CanAfford and
		// Purse steps above entirely - MINUS THE PURSE - and only resets the drag-start
		// reading BeginInteractiveEdit took, before falling through to the same Topology
		// catch-up every committed drag owes the derived graph.
		PavementValueAtDragStart = 0.0;
	}

	// THE ONE TOPOLOGY NOTIFY A DRAG FIRES (issue #165) - AND ONLY IF SOMETHING ACTUALLY
	// MOVED. Every frame of a real drag notified Geometry only, through MoveNode/
	// MoveApronCorner, which left the guideline graph, anchor links, plots and traffic
	// exactly as stale as they were when the drag began - none of them re-derive from a
	// Geometry notify. This is where a committed drag that moved something catches them up,
	// exactly once, no matter how many frames it ran for - IN THE EDITOR WORLD TOO, now that
	// this runs whether or not there was a History edit to commit above (issue #190).
	//
	// GUARDED, because a click-release that opens and closes an interactive edit without a
	// single successful move (the cursor never left the node, or every move attempted was
	// refused) is not a drag at all - before #165 that sequence fired no notify whatsoever,
	// since MoveNode's own notify simply never happened, and an unconditional Topology notify
	// here would be a full rebuild that never used to run.
	if (bGeometryChangedDuringEdit)
	{
		NotifyChanged(EChangeKind::Topology);
	}
	bGeometryChangedDuringEdit = false;
}

bool URoadEditFacade::MoveApronCorner(int32 ApronIndex, int32 CornerIndex, FVector2D To)
{
	ARoadNetworkActor& Owner = Actor();
	if (Owner.Network == nullptr)
	{
		return false;
	}

	const FApronId Apron = Owner.Network->ApronIdAt(ApronIndex);
	const FApronSurface* Live = Owner.Network->GetApron(Apron);
	if (Live == nullptr || !Live->Outline.IsValidIndex(CornerIndex))
	{
		UE_LOG(LogRoadMesh, Warning,
			TEXT("MoveApronCorner refused: apron %d has no corner %d"), ApronIndex, CornerIndex);
		return false;
	}

	// JUDGED ON A COPY, BEFORE ANYTHING IS WRITTEN. A self-intersecting outline has no
	// inside, and the surface builder has no answer for one - so a corner dragged across
	// its own polygon has to be refused rather than fixed up afterwards.
	//
	// AND REFUSED OUT HERE, BEFORE ApplyInteractiveMutation OPENS ANYTHING: this write has no
	// Verify of its own to revert on, so a refusal that can be known first is the whole judgement,
	// and there is nothing to roll back to. (Before issue #437 the reason was harder: an
	// abandoned edit restored nothing, so a mutation followed by a `return false` left a changed
	// graph with no undo entry to reach it.)
	//
	// THROUGH RoadGeom::IsSimplePolygon, the same test FApronDrawTool closes an outline
	// against - one answer to "is this a valid apron", not a second opinion.
	TArray<FVector2D> Proposed = Live->Outline;
	Proposed[CornerIndex] = To;
	if (!RoadGeom::IsSimplePolygon(Proposed))
	{
		UE_LOG(LogRoadMesh, Log,
			TEXT("MoveApronCorner refused: corner %d of apron %d would cross its own outline"),
			CornerIndex, ApronIndex);
		return false;
	}

	// AN APRON CORNER IS NOT IN THE ROAD GRAPH AT ALL, so its notify never has a reason to
	// change the graph's SHAPE - bChangesGraphShape stays at its default, false, the same as
	// MoveNode. See ApplyInteractiveMutation's own header comment for the Geometry/Topology
	// split this joins (the bare-call trap, issues #165/#190/#299).
	return ApplyInteractiveMutation(TEXT("move apron corner"),
		[Apron, CornerIndex, To](URoadNetwork& Net) { return Net.SetApronCorner(Apron, CornerIndex, To); });
}

bool URoadEditFacade::MergeNodes(int32 KeepIndex, int32 AbsorbIndex)
{
	FRoadNodeId Keep;
	FRoadNodeId Absorb;
	if (!MakeLiveNodeId(KeepIndex, Keep) || !MakeLiveNodeId(AbsorbIndex, Absorb))
	{
		UE_LOG(LogRoadMesh, Warning,
			TEXT("MergeNodes refused: %d or %d is not a live node"), KeepIndex, AbsorbIndex);
		return false;
	}
	if (Keep == Absorb)
	{
		return false;
	}

	// JUDGED AFTER, AND ONLY AFTER. RoadPlacement::NodeCornersFit reads a node's CURRENT arms
	// and judges them at a proposed position; it has no way to be asked about an arm set that
	// does not exist yet. So unlike MoveNode - which can and does judge before moving - a
	// merge has to happen before its corners can be measured at all, which is exactly what
	// ApplyInteractiveMutation's Verify parameter exists for: run after Mutate, and REVERT
	// rather than refuse on a false answer (see that method's own header comment).
	// THE ARMS THE MERGE WILL RE-POINT, by id - URoadNetwork::MergeNodes keeps their handles -
	// so the Verify below can judge exactly them against the clearance strip.
	TArray<FRoadSegmentId> Absorbed;
	if (const FRoadNode* AbsorbNode = Actor().Network->GetNode(Absorb))
	{
		Absorbed = AbsorbNode->Incident;
	}

	const bool bMerged = ApplyInteractiveMutation(TEXT("merge nodes"),
		[Keep, Absorb](URoadNetwork& Net) { return Net.MergeNodes(Keep, Absorb); },
		// A node disappeared - the graph's SHAPE changed - so this always notifies Topology,
		// drag or no drag: see ApplyInteractiveMutation's own header comment for why that
		// differs from MoveNode/MoveApronCorner's mid-drag Geometry notify.
		/*bChangesGraphShape*/ true,
		[Keep, KeepIndex, AbsorbIndex, Absorbed](const URoadNetwork& Net)
		{
			const FRoadNode* Merged = Net.GetNode(Keep);
			const bool bFits = Merged != nullptr && RoadPlacement::NodeCornersFit(Net, Keep, Merged->Position);
			if (!bFits)
			{
				UE_LOG(LogRoadMesh, Log,
					TEXT("Merge refused: node %d folded into %d makes a corner the solver cannot "
						 "trim, so the whole edit is reverted."), AbsorbIndex, KeepIndex);
				return false;
			}

			// THE CLEARANCE STRIP, of every arm the merge re-pointed, on the merged graph (final
			// review 3). The edit tool merges on drop whenever its drag snapped to a node,
			// whatever MoveNode answered on the way - so the merge is where the join is judged,
			// by the same JudgeSegment, and a refusal reverts the whole edit. Runway arms are
			// not judged (plan ruling 2).
			// ENFORCED BY: Airside.Present.MergeRefusedIntoStrip
			for (const FRoadSegmentId& Arm : Absorbed)
			{
				if (Net.GetSegment(Arm) == nullptr || Net.IsRunwaySegment(Arm)) { continue; }
				const TaxiwayStrip::FStripVerdict Verdict = TaxiwayStrip::JudgeExisting(Net, Arm);
				if (Verdict.bRefused)
				{
					UE_LOG(LogRoadMesh, Log, TEXT("Merge refused: %s"), *Verdict.Text);
					return false;
				}
			}
			return true;
		});

	if (bMerged)
	{
		UE_LOG(LogRoadMesh, Log, TEXT("Merged node %d into %d"), AbsorbIndex, KeepIndex);
	}
	return bMerged;
}

bool URoadEditFacade::MoveNode(int32 NodeIndex, FVector2D To)
{
	FRoadNodeId Node;
	if (!MakeLiveNodeId(NodeIndex, Node))
	{
		return false;
	}

	ARoadNetworkActor& Owner = Actor();
	const FRoadNode* Live = Owner.Network->GetNode(Node);
	if (Live == nullptr)
	{
		return false;
	}

	// A RUNWAY CHAIN IS STRAIGHT, and this is where that becomes true rather than merely
	// assumed. FRunwayMarkingBuilder paints every marking along ONE frame - an origin, a
	// direction and a length from RunwayExtentAt, which reports only the two ENDS - so a
	// chain bent at an interior node draws a straight centreline down a crooked strip.
	// Reported from play, 2026-09-20: "it bends the runway, but keeps the centre line
	// straight".
	//
	// TWO EVALUATORS IS THE ACTUAL DEFECT. The surface follows the nodes and the markings
	// follow the ends, and nothing made them agree - the same shape as the guideline
	// invariant in CLAUDE.md. Rather than teach the markings to bend, which would be a
	// second crooked-runway feature nobody asked for, the graph stops representing one.
	//
	// IN MoveNode, NOT IN THE EDIT TOOL. It is an invariant of the model, so it holds for
	// every caller rather than for the one gesture that happened to expose it.
	{
		TArray<FRoadNodeId> RunwayNeighbours;
		FRoadSegmentId AnyRunwayArm;
		for (const FRoadSegmentId& Incident : Live->Incident)
		{
			if (Owner.Network->IsRunwaySegment(Incident))
			{
				RunwayNeighbours.Add(Owner.Network->GetOtherEnd(Incident, Node));
				AnyRunwayArm = Incident;
			}
		}

		// THE LINE THE NODE MAY NOT LEAVE, or two unset points when it is free to go
		// anywhere. Two cases, one rule:
		//
		//   - AN INTERIOR NODE (a taxiway exit, runway on both sides) is pinned between its
		//     two runway neighbours, and their line is the strip's.
		//
		//   - A THRESHOLD with exits behind it may still slide, and its line runs through
		//     its one neighbour and where it currently stands - which IS the strip's line,
		//     because the chain is straight, which is the invariant being kept.
		//
		// A THRESHOLD ON A STRIP WITH NO EXITS IS FREE, and that is not an exception: with
		// nothing between the ends there is no interior node to leave behind, so no move can
		// bend anything. It is the drag the runway tool's own handle offers.
		const FRoadNode* LineFrom = nullptr;
		FVector2D LineThrough = FVector2D::ZeroVector;

		if (RunwayNeighbours.Num() >= 2)
		{
			LineFrom = Owner.Network->GetNode(RunwayNeighbours[0]);
			if (const FRoadNode* Second = Owner.Network->GetNode(RunwayNeighbours[1]))
			{
				LineThrough = Second->Position;
			}
			else
			{
				LineFrom = nullptr;
			}
		}
		else if (RunwayNeighbours.Num() == 1 && AnyRunwayArm.IsSet()
			&& Owner.Network->RunwayChainOrSeed(AnyRunwayArm).Num() > 1)
		{
			LineFrom = Owner.Network->GetNode(RunwayNeighbours[0]);
			LineThrough = Live->Position;
		}

		if (LineFrom != nullptr)
		{
			const FVector2D Line = LineThrough - LineFrom->Position;
			const double LengthSquared = Line.SizeSquared();
			if (LengthSquared > UE_DOUBLE_SMALL_NUMBER)
			{
				// The foot of the perpendicular, UNCLAMPED: the length, corner and
				// minimum-runway checks below decide how far along is legal, so the node
				// stops at the last legal spot rather than at some other one chosen here.
				//
				// PROJECTED RATHER THAN REFUSED. Refusing was the first cut and its own test
				// caught it: it also refuses extending a threshold straight along its own
				// line, which bends nothing and is the most ordinary runway edit there is.
				// Sliding costs no more machinery and leaves nothing to explain.
				const double T = FVector2D::DotProduct(To - LineFrom->Position, Line) / LengthSquared;
				To = LineFrom->Position + Line * T;
			}
		}
	}

	// Judged before moving. Every road this node holds gets longer or shorter as it goes,
	// and one pulled under the minimum is one the solver cannot trim back from both ends.
	for (const FRoadSegmentId& Incident : Live->Incident)
	{
		const FRoadNodeId Other = Owner.Network->GetOtherEnd(Incident, Node);
		const FRoadNode* Far = Owner.Network->GetNode(Other);
		if (Far != nullptr && FVector2D::Distance(Far->Position, To) < Owner.PlacementLimits.MinSegmentLength)
		{
			return false;
		}
	}

	// The corners a move would make, here and at every neighbour, refused before anything
	// changes - so a drag cannot build the corner the solver would fail and leave a road
	// undrawn (2026-09-06). Judged at the PROPOSED position without moving first, because a
	// move-then-check would need an undo the drag never asked for.
	if (!RoadPlacement::NodeCornersFit(*Owner.Network, Node, To))
	{
		return false;
	}

	// A RUNWAY MAY NOT BE DRAGGED SHORTER THAN ONE. MinSegmentLength above is the solver's
	// floor - what a piece of pavement needs to be trimmable - and says nothing about
	// whether a STRIP is still a runway. Dragging a threshold in is how you would shorten
	// one, and without this the aircraft admitted to it yesterday would be refused today
	// with nothing to say when it changed.
	//
	// ONE CLAUSE ON MoveNode rather than a MoveRunwayThreshold beside it: a threshold IS a
	// road node, and a second mutator would be a second answer to "may this node move" for
	// the two to drift apart on.
	//
	// THE WHOLE CHAIN, not the arm being moved - a split runway has interior nodes, and its
	// length is the sum of its pieces.
	for (const FRoadSegmentId& Incident : Live->Incident)
	{
		if (!Owner.Network->IsRunwaySegment(Incident))
		{
			continue;
		}

		double Length = 0.0;
		for (const FRoadSegmentId& Piece : Owner.Network->RunwayChainOrSeed(Incident))
		{
			const FRoadSegment* Segment = Owner.Network->GetSegment(Piece);
			const FRoadNode* PieceA = Segment != nullptr ? Owner.Network->GetNode(Segment->A) : nullptr;
			const FRoadNode* PieceB = Segment != nullptr ? Owner.Network->GetNode(Segment->B) : nullptr;
			if (PieceA == nullptr || PieceB == nullptr)
			{
				continue;
			}

			// Measured where the node WOULD land, as NodeCornersFit does just above.
			const FVector2D At = (Segment->A == Node) ? To : PieceA->Position;
			const FVector2D To2 = (Segment->B == Node) ? To : PieceB->Position;
			Length += FVector2D::Distance(At, To2);
		}

		if (Length < Owner.MinimumRunwayLength)
		{
			UE_LOG(LogRoadMesh, Log,
				TEXT("Move refused: it would leave the runway %.0f uu long, under the %.0f "
					 "minimum."), Length, Owner.MinimumRunwayLength);
			return false;
		}
		break;
	}

	// INSIDE A TAXIWAY'S CLEARANCE STRIP (strip stage 3, Review Focus 3): every segment this
	// node drags, judged at the shape it would take - both ends where they would be, the
	// control shifted by half the move exactly as URoadNetwork::SetNodePosition shifts it -
	// with ALL of the node's own arms ignored, since each is being replaced by its moved self.
	// Refused before ApplyInteractiveMutation, so nothing moves and no undo step is pushed.
	// RUNWAY ARMS ARE NOT JUDGED (plan ruling 2: their own strip rules).
	// ENFORCED BY: Airside.Present.MoveNodeRefusedIntoStrip
	{
		const FVector2D Shift = (To - Live->Position) * 0.5;

		// A DRAG SNAPPED ONTO ANOTHER NODE MEETS THAT NODE'S ROADS. The edit tool's drop merges
		// the two, and its node snap copies the target's position bitwise (FRoadSnapResult's
		// contract) - so an exact match is the drag-to-join gesture, and judging the pre-merge
		// frame as two unjoined roads would refuse every join onto a taxiway's end.
		FRoadNodeId LandsOn = Node;
		for (int32 Index = 0; Index < Owner.Network->GetNodes().Num(); ++Index)
		{
			const FRoadNodeId Other = Owner.Network->NodeIdAt(Index);
			if (Other.IsSet() && Other != Node && Owner.Network->GetNodes()[Index].Position == To)
			{
				LandsOn = Other;
				break;
			}
		}

		// Each moved arm's direction OUT of the moved node, for the sibling check below.
		TArray<TPair<FRoadSegmentId, FVector2D>> MovedOut;
		for (const FRoadSegmentId& Incident : Live->Incident)
		{
			TaxiwayStrip::FSegmentShape Shape;
			if (Owner.Network->IsRunwaySegment(Incident) || !TaxiwayStrip::ShapeOf(*Owner.Network, Incident, Shape))
			{
				continue;
			}
			const FRoadSegment* Segment = Owner.Network->GetSegment(Incident);
			const bool bNodeIsA = Segment->A == Node;
			(bNodeIsA ? Shape.A : Shape.B) = To;
			Shape.Control += Shift;

			TaxiwayStrip::FSegmentEnd Moved;
			Moved.Node = LandsOn;
			Moved.At = To;
			TaxiwayStrip::FSegmentEnd Fixed;
			Fixed.Node = Owner.Network->GetOtherEnd(Incident, Node);
			Fixed.At = bNodeIsA ? Shape.B : Shape.A;

			const TaxiwayStrip::FStripVerdict Verdict = TaxiwayStrip::JudgeSegment(*Owner.Network, Shape,
				TaxiwayStrip::HasStrip(*Owner.Network, Incident),
				bNodeIsA ? Moved : Fixed, bNodeIsA ? Fixed : Moved, Live->Incident);
			if (Verdict.bRefused)
			{
				UE_LOG(LogRoadMesh, Log, TEXT("MoveNode refused: %s"), *Verdict.Text);
				return false;
			}
			MovedOut.Emplace(Incident, bNodeIsA
				? GuidelineGeom::Tangent(Shape.A, Shape.Control, Shape.B, 0.0)
				: -GuidelineGeom::Tangent(Shape.A, Shape.Control, Shape.B, 1.0));
		}

		// THE NODE'S OWN ARMS AGAINST EACH OTHER (final review 2): each was Ignored above
		// because each is being replaced, so nothing judged their MOVED angle - a T-junction
		// slid along its taxiway left the road 28 degrees off it, along its strip. The meeting
		// rule per pair, against every sibling that carries a strip.
		for (const TPair<FRoadSegmentId, FVector2D>& Arm : MovedOut)
		{
			for (const TPair<FRoadSegmentId, FVector2D>& Sibling : MovedOut)
			{
				if (Arm.Key == Sibling.Key || !TaxiwayStrip::HasStrip(*Owner.Network, Sibling.Key)) { continue; }
				const double Deg = FMath::RadiansToDegrees(RoadGeom::AngleBetween(Arm.Value, Sibling.Value));
				if (!TaxiwayStrip::MeetsAtAllowedAngle(Deg))
				{
					UE_LOG(LogRoadMesh, Log, TEXT("MoveNode refused: %s"),
						*TaxiwayStrip::MeetingRefusal(*Owner.Network, Sibling.Key, Deg));
					return false;
				}
			}
		}
	}

	// A move changes no node's existence and no segment's endpoints-as-a-set, only where
	// things sit, so bChangesGraphShape stays at its default, false: see
	// ApplyInteractiveMutation's own header comment for the bare-call-trap Geometry/Topology
	// split this joins (issues #165/#190/#299).
	return ApplyInteractiveMutation(TEXT("move node"),
		[Node, To](URoadNetwork& Net) { return Net.SetNodePosition(Node, To); });
}

FRoadDeletionPlan URoadEditFacade::PlanNodeDeletion(int32 NodeIndex) const
{
	FRoadNodeId Node;
	ARoadNetworkActor& Owner = Actor();
	if (Owner.Network == nullptr || !MakeLiveNodeId(NodeIndex, Node))
	{
		return FRoadDeletionPlan();
	}

	// CACHE HIT: same node, and nothing about the graph has moved since the plan was made -
	// see this method's own header comment for why EditRevision alone is enough to know
	// that (#166). FRemoveGesture asks this every frame Ctrl hovers a node; without this,
	// a still hover paid for RoadHeal::PlanNodeDeletion's whole-graph duplicate-and-validate
	// simulation sixty times a second for an answer that could not have changed.
	const uint32 Revision = Owner.Network->GetEditRevision();
	if (bHasLastDeletionPlan && LastDeletionPlanNode == NodeIndex && LastDeletionPlanRevision == Revision)
	{
		return LastDeletionPlan;
	}

	LastDeletionPlan = RoadHeal::PlanNodeDeletion(*Owner.Network, Node, Owner.PlacementLimits);
	LastDeletionPlanNode = NodeIndex;
	LastDeletionPlanRevision = Revision;
	bHasLastDeletionPlan = true;
	++DeletionPlanComputeCount;
	return LastDeletionPlan;
}

bool URoadEditFacade::DeleteNode(int32 NodeIndex)
{
	FRoadNodeId Node;
	if (!MakeLiveNodeId(NodeIndex, Node))
	{
		UE_LOG(LogRoadMesh, Warning, TEXT("DeleteNode refused: %d is not a live node"), NodeIndex);
		return false;
	}

	ARoadNetworkActor& Owner = Actor();
	const FRoadDeletionPlan Plan = RoadHeal::PlanNodeDeletion(*Owner.Network, Node, Owner.PlacementLimits);
	if (!Plan.bValid)
	{
		// Refused whole. Nothing has been touched yet, which is the point of planning
		// before acting rather than unwinding afterwards.
		UE_LOG(LogRoadMesh, Warning,
			TEXT("DeleteNode refused: node %d cannot rejoin node %d (%s). Delete its roads "
				 "one at a time to strand it, then it will delete."),
			NodeIndex, Plan.RefusedNeighbour.Index, RoadPlacement::Describe(Plan.Refusal));
		return false;
	}

	// EVERY SEGMENT THIS TAKES WITH IT, summed before the removal. Deleting a node destroys
	// the roads meeting it - SegmentsIncidentTo is what the deletion plan already shows the
	// player - so crediting only the node would pay back nothing for the pavement that
	// actually disappears.
	// WHAT THE PLAN ACTUALLY TAKES, not every arm of the node: a runway arm is not doomed
	// by deleting something attached to it (FRoadDeletionPlan::bKeepTarget), and crediting
	// the player for a runway still on the ground would be paying them to disconnect a
	// taxiway.
	FBuildQuote Quote;
	for (const FRoadSegmentId& DoomedArm : Plan.Doomed)
	{
		const int32 Incident = DoomedArm.Index;
		const FBuildQuote Each = QuoteForSegment(Incident);
		// EVERY ARM'S OWN LINE, kept rather than folded into one total - a junction of two
		// widths is priced on each arm's own profile, never on a stand-in for the lot.
		Quote.Lines.Append(Each.Lines);
		if (Quote.What.IsEmpty())
		{
			Quote.What = Each.What;
		}
	}

	// THE HEAL, JUDGED BEFORE ANYTHING IS REMOVED (strip stage 3, Review Focus 4): a rejoin
	// that would run through a taxiway's clearance strip is SKIPPED - the delete still
	// happens, the stranded ends are left as they are - and logged, because a silent skip is a
	// gap the player did not ask for with nothing to say why. Judged against the graph as it
	// stands, with the doomed arms ignored: they are what the heal replaces. Refusing the whole
	// delete instead would make a node in a legacy layout undeletable.
	// ENFORCED BY: Airside.Present.HealRefusedAcrossStrip
	TSet<FRoadNodeId> SkipHeal;
	{
		const FRoadNode* AnchorNode = Owner.Network->GetNode(Plan.Anchor);
		const URoadProfile* HealWith = Plan.HealProfile != nullptr ? Plan.HealProfile.Get() : Owner.ResolveProfile();
		// WHAT THE HEAL IS, asked of the doomed arms it replaces through the one taxiway and
		// runway rules (HasStrip, IsRunwaySegment) rather than re-read off the profile here.
		bool bHealIsRunway = false;
		bool bHealIsTaxiway = false;
		for (const FRoadSegmentId& Arm : Plan.Doomed)
		{
			bHealIsRunway |= Owner.Network->IsRunwaySegment(Arm);
			bHealIsTaxiway |= TaxiwayStrip::HasStrip(*Owner.Network, Arm);
		}
		for (const FRoadNodeId& Stranded : Plan.Rejoin)
		{
			const FRoadNode* StrandedNode = Owner.Network->GetNode(Stranded);
			if (AnchorNode == nullptr || StrandedNode == nullptr || HealWith == nullptr
				|| bHealIsRunway)   // a runway relay - plan ruling 2
			{
				continue;
			}
			TaxiwayStrip::FSegmentShape Shape;
			Shape.A = StrandedNode->Position;
			Shape.B = AnchorNode->Position;
			Shape.Control = (Shape.A + Shape.B) * 0.5;   // AddStraightSegment's, below
			Shape.HalfWidth = HealWith->GetMaxHalfWidth();
			TaxiwayStrip::FSegmentEnd AtA;
			AtA.Node = Stranded;
			AtA.At = Shape.A;
			TaxiwayStrip::FSegmentEnd AtB;
			AtB.Node = Plan.Anchor;
			AtB.At = Shape.B;
			const TaxiwayStrip::FStripVerdict Verdict = TaxiwayStrip::JudgeSegment(*Owner.Network, Shape,
				bHealIsTaxiway, AtA, AtB, Plan.Doomed);
			if (Verdict.bRefused)
			{
				UE_LOG(LogRoadMesh, Log, TEXT("Heal skipped: %s (node %d not rejoined to %d)"),
					*Verdict.Text, Stranded.Index, Plan.Anchor.Index);
				SkipHeal.Add(Stranded);
			}
		}
	}

	FRoadEditScope Edit(HistoryForEdit(), Owner.Network, TEXT("delete node"));

	if (Plan.bKeepTarget)
	{
		// THE ARMS, NOT THE NODE. RemoveNode cascades every incident segment, which is
		// exactly what must not happen here - the runway this node carries is staying. So
		// the doomed arms are removed one by one and the node is left standing, because it
		// is the runway's threshold.
		for (const FRoadSegmentId& DoomedArm : Plan.Doomed)
		{
			Owner.Network->RemoveSegment(DoomedArm);
		}
		UE_LOG(LogRoadMesh, Log,
			TEXT("Deleted %d arm(s) at node %d; its runway is left where it is."),
			Plan.Doomed.Num(), NodeIndex);
	}
	// The cascade is the model's: a segment whose endpoint is gone has no geometry.
	else if (!Owner.Network->RemoveNode(Node))
	{
		Edit.Rollback();   // rule 40 - see PlaceNode
		UE_LOG(LogRoadMesh, Warning, TEXT("DeleteNode refused: node %d would not remove"), NodeIndex);
		return false;
	}

	// The heal. Every one of these was judged against the post-deletion graph, so it is
	// being applied to exactly the state it was approved for.
	for (const FRoadNodeId& Stranded : Plan.Rejoin)
	{
		if (SkipHeal.Contains(Stranded))
		{
			continue;   // judged above, logged there
		}

		// THE PLAN'S PROFILE, not the level's default: the road being healed keeps its own
		// cross-section. See FRoadDeletionPlan::HealProfile for what laying the default
		// instead used to do to a runway.
		URoadProfile* Relay = Plan.HealProfile != nullptr
			? Plan.HealProfile.Get()
			: Owner.ResolveProfile();

		const FRoadSegmentId Healed = Owner.Network->AddStraightSegment(Stranded, Plan.Anchor, Relay);
		if (!Healed.IsSet())
		{
			UE_LOG(LogRoadMesh, Error,
				TEXT("DeleteNode healed only partly: node %d could not rejoin %d"),
				Stranded.Index, Plan.Anchor.Index);
		}
		else if (Plan.HealSurface != EPavement::Tarmac)
		{
			// THE PLAN'S SURFACE with the plan's profile - see FRoadDeletionPlan::HealSurface.
			// Refused quietly on a runway relay, whose ground is its facts'.
			Owner.Network->SetSegmentSurface(Healed, Plan.HealSurface);
		}
	}

	for (const FRoadNodeId& Litter : Plan.Swept)
	{
		Owner.Network->RemoveNode(Litter);
	}

	// A SKIPPED HEAL'S ENDS, IF BARE (final review 5): Plan.Swept was planned on the heal
	// happening, so a stranded node or anchor whose only road was a doomed arm is left with
	// none. Swept here, as the plan sweeps any other bare litter.
	if (SkipHeal.Num() > 0)
	{
		TArray<FRoadNodeId> Ends = SkipHeal.Array();
		Ends.Add(Plan.Anchor);
		for (const FRoadNodeId& End : Ends)
		{
			const FRoadNode* Left = Owner.Network->GetNode(End);
			if (Left != nullptr && Left->Incident.Num() == 0)
			{
				Owner.Network->RemoveNode(End);
			}
		}
	}

	CommitDisposal(Edit, Quote);
	return true;
}

bool URoadEditFacade::DeleteSegment(int32 SegmentIndex)
{
	FRoadSegmentId Segment;
	if (!MakeLiveSegmentId(SegmentIndex, Segment))
	{
		UE_LOG(LogRoadMesh, Warning,
			TEXT("DeleteSegment refused: %d is not a live segment"), SegmentIndex);
		return false;
	}

	ARoadNetworkActor& Owner = Actor();

	// Captured before the removal, because afterwards the segment cannot say what it joined.
	const FRoadSegment* Doomed = Owner.Network->GetSegment(Segment);
	const FRoadNodeId EndA = Doomed != nullptr ? Doomed->A : FRoadNodeId();
	const FRoadNodeId EndB = Doomed != nullptr ? Doomed->B : FRoadNodeId();

	// QUOTED BEFORE THE REMOVAL, because afterwards the segment is not there to measure. Its
	// value TODAY rather than what was paid for it - see IBuildPurse::Credit.
	const FBuildQuote Quote = QuoteForSegment(SegmentIndex);

	FRoadEditScope Edit(HistoryForEdit(), Owner.Network, TEXT("delete segment"));

	if (!Owner.Network->RemoveSegment(Segment))
	{
		Edit.Rollback();   // rule 40 - see PlaceNode
		UE_LOG(LogRoadMesh, Warning,
			TEXT("DeleteSegment refused: segment %d would not remove"), SegmentIndex);
		return false;
	}

	// Cleanup, not deletion: an endpoint left with no road holds no geometry, so removing
	// it destroys nothing. An endpoint that still has roads is untouched.
	for (const FRoadNodeId& End : { EndA, EndB })
	{
		if (const FRoadNode* Live = Owner.Network->GetNode(End))
		{
			if (Live->Incident.Num() == 0)
			{
				Owner.Network->RemoveNode(End);
			}
		}
	}

	CommitDisposal(Edit, Quote);
	return true;
}

TArray<int32> URoadEditFacade::SegmentsIncidentTo(int32 NodeIndex) const
{
	TArray<int32> Found;

	FRoadNodeId Node;
	if (!MakeLiveNodeId(NodeIndex, Node))
	{
		return Found;
	}

	const FRoadNode* Live = GetNetwork()->GetNode(Node);
	if (Live == nullptr)
	{
		return Found;
	}

	Found.Reserve(Live->Incident.Num());
	for (const FRoadSegmentId& Incident : Live->Incident)
	{
		Found.Add(Incident.Index);
	}
	return Found;
}

bool URoadEditFacade::GetSegmentEnds(int32 SegmentIndex, FVector2D& OutA, FVector2D& OutB) const
{
	FRoadSegmentId Id;
	if (!MakeLiveSegmentId(SegmentIndex, Id))
	{
		return false;
	}

	const URoadNetwork* Network = GetNetwork();
	const FRoadSegment* Segment = Network->GetSegment(Id);
	const FRoadNode* EndA = Segment != nullptr ? Network->GetNode(Segment->A) : nullptr;
	const FRoadNode* EndB = Segment != nullptr ? Network->GetNode(Segment->B) : nullptr;
	if (EndA == nullptr || EndB == nullptr)
	{
		return false;
	}

	OutA = EndA->Position;
	OutB = EndB->Position;
	return true;
}
