// Node, segment and guideline surgery, plus the undo/mutator plumbing (Actor, EnsureNetwork,
// EnsureHistory, HistoryForEdit, MakeLiveNodeId/MakeLiveSegmentId, the ghost/traffic
// forwarders) every one of them shares. Undo/redo, aprons, stands, ClearNetwork and FindRoute
// are a second translation unit of this SAME class - see RoadEditFacadeSurfaces.cpp's own
// banner comment for why.

#include "Present/RoadEditFacade.h"

#include "Build/BuildCost.h"
#include "Content/AirsideSettings.h"
#include "Model/BuildPurse.h"

#include "AirsideLog.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadSlotMap.h"
#include "Present/RoadNetworkActor.h"
#include "Profiles/RoadProfile.h"
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

int32 URoadEditFacade::GetTaxiwayProfileCount() const
{
	return Actor().GetTaxiwayProfileCount();
}

URoadProfile* URoadEditFacade::ResolveTaxiwayProfile(int32 Index) const
{
	return Actor().ResolveTaxiwayProfile(Index);
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
	OnChanged.Broadcast(Kind);
}

bool URoadEditFacade::CanAfford(const FBuildQuote& Quote) const
{
	// NO PURSE MEANS FREE, and that is the design-time answer: URoadBuildEdMode has no runtime
	// and no money, and a default that refused would make the editor mode unable to build.
	return Purse == nullptr || Quote.IsFree() || Purse->CanAfford(Quote);
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
	return BuildCost::ForSegment(*Profile, BuildCost::SegmentLengthUu(*Network, Segment));
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
		Total.BaseAmount += Each.BaseAmount;
		if (!Total.Source.IsValid())
		{
			// The first profile met stands in for the lot, for discounts only - the AMOUNT is
			// the true sum whichever one it is.
			Total.Source = Each.Source;
		}
	}
	Total.What = NSLOCTEXT("BuildCost", "MovedPavement", "Moved pavement");
	return Total;
}

FBuildQuote URoadEditFacade::QuoteForApron(TConstArrayView<FVector2D> Outline) const
{
	const UAirsideSettings* Settings = GetDefault<UAirsideSettings>();
	return BuildCost::ForApron(Outline,
		Settings != nullptr ? Settings->ApronCostPerSquareMetre : 0.0);
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
	// FORWARDED SINCE 2026-09-20. The rule moved to ARoadNetworkActor::ResolveProfileFor, beside
	// the Resolve* family it is composed of, so the guide anchor could ask it through
	// IRoadEditTarget without this becoming a third copy - see that function's own comment.
	return Actor().ResolveProfileFor(Kind, WidthIndex);
}

FBuildQuote URoadEditFacade::QuoteForConnect(int32 FromIndex, FVector2D To, ERoadKind Kind,
	int32 WidthIndex) const
{
	const URoadNetwork* Network = Actor().Network;
	// THROUGH THE ACTOR, not through this - ResolveProfileFor is non-const because
	// ARoadNetworkActor::ResolveProfile lazily fills RuntimeProfile, a decision that header
	// records deliberately. Actor() hands back a non-const reference from a const method, so a
	// quote can ask the question without that decision having to be reversed for it.
	const URoadProfile* Profile = Actor().ResolveProfileFor(Kind, WidthIndex);
	if (Network == nullptr || Profile == nullptr || !Network->GetNodes().IsValidIndex(FromIndex))
	{
		return FBuildQuote();
	}
	return BuildCost::ForSegment(*Profile,
		FVector2D::Distance(Network->GetNodes()[FromIndex].Position, To));
}

bool URoadEditFacade::ConnectNodes(int32 FromIndex, int32 ToIndex, ERoadKind Kind, int32 WidthIndex)
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
	// A CHOSEN WIDTH WINS OVER THE DEFAULT, and only for a taxiway: WidthIndex names one of
	// the content set's standard widths (the tool cycles it on key-again), INDEX_NONE means
	// "whatever this kind defaults to". The default for a taxiway is the ACTOR's own
	// profile, which ResolveProfile keeps the content set out of on purpose - so a player
	// who never touches the cycle lays exactly the road this level was tuned for.
	//
	// A service road ignores the index outright: it has one authored cross-section, and an
	// index reaching it would lay a taxiway's width on a lane meant for vans.
	URoadProfile* Chosen = ResolveProfileFor(Kind, WidthIndex);
	if (Chosen == nullptr && Kind == ERoadKind::ServiceRoad)
	{
		UE_LOG(LogRoadMesh, Warning,
			TEXT("ConnectNodes refused: %d -> %d, no service road profile. Author "
				 "DA_RoadProfile_ServiceRoad with Tools/Python/build_road_profiles.py, or set "
				 "ServiceRoadProfile on the actor."), FromIndex, ToIndex);
		return false;
	}

	// PRICED AND REFUSED BEFORE THE SCOPE, not at commit. An FRoadEditScope that is not
	// committed discards its undo snapshot but does NOT roll the network back, so a refusal
	// after AddStraightSegment would leave the taxiway built and unpaid for - see
	// CommitPurchase's own comment.
	const FBuildQuote Quote = BuildCost::ForSegment(*Chosen,
		FVector2D::Distance(Owner.Network->GetNodes()[From.Index].Position,
			Owner.Network->GetNodes()[To.Index].Position));
	if (!CanAfford(Quote))
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
		UE_LOG(LogRoadMesh, Warning, TEXT("ConnectNodes refused: %d -> %d"), FromIndex, ToIndex);
		return false;
	}

	UE_LOG(LogRoadMesh, Log, TEXT("Segment %d connected: node %d -> node %d"), Segment.Index, FromIndex, ToIndex);
	CommitPurchase(Edit, Quote);
	return true;
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
	const FBuildQuote Quote = BuildCost::ForSegment(*RunwayProfile, FVector2D::Distance(From, To));
	if (!CanAfford(Quote))
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
		return false;
	}

	// STRAIGHT, and the model cannot express otherwise here: AddStraightSegment puts the
	// control point on the midpoint, which IsStraight tests for exactly.
	const FRoadSegmentId Segment = Owner.Network->AddStraightSegment(A, B, RunwayProfile);
	if (!Segment.IsSet())
	{
		return false;
	}

	// The facts go on in the SAME edit as the pavement, so an undo takes both back: a
	// runway that came back as tarmac after Ctrl+Z on its classification would be a
	// runway the player never placed.
	Owner.Network->SetRunwayFacts(Segment, Facts);

	CommitPurchase(Edit, Quote);

	UE_LOG(LogRoadMesh, Log, TEXT("Runway %s placed, %.0f uu long, %.0f uu wide, %s, %s approach"),
		*RunwayDesignator::ToPairText(To - From), Length, RunwayProfile->GetTotalWidth(),
		RunwaySurfaceName(Facts.Surface), RunwayApproachName(Facts.Approach));
	return true;
}

bool URoadEditFacade::SetRunwayFacts(int32 SegmentIndex, const FRunwayFacts& Facts)
{
	URoadNetwork* Network = Actor().Network;
	if (Network == nullptr)
	{
		return false;
	}
	FRoadSegmentId Segment;
	if (!MakeLiveSegmentId(SegmentIndex, Segment) || !Network->IsRunwaySegment(Segment))
	{
		// Refused BEFORE the snapshot, like every other guard on this seam - see
		// SetIntermediateHoldingPosition for why a refusal inside the scope is not a rollback.
		return false;
	}
	if (Network->RunwayFactsFor(Segment) == Facts)
	{
		// Already so. True, because the runway IS what was asked for - but no edit, since
		// an undo step that changes nothing is a Ctrl+Z the player has to press twice.
		return true;
	}

	FRoadEditScope Edit(HistoryForEdit(), Network, TEXT("set runway facts"));
	Network->SetRunwayFacts(Segment, Facts);
	CommitAndNotify(Edit);

	UE_LOG(LogRoadMesh, Log, TEXT("Runway at segment %d reclassified: %s, %s approach (the whole strip)"),
		SegmentIndex, RunwaySurfaceName(Facts.Surface), RunwayApproachName(Facts.Approach));
	return true;
}

int32 URoadEditFacade::ConnectGuidelines(int32 FromNodeIndex, int32 ToNodeIndex)
{
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
		// Nothing mutated - AddGuidelineEdge refuses without touching the graph, so leaving
		// the scope uncommitted here costs nothing to abandon. ~FRoadEditScope calls
		// AbandonEdit.
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

bool URoadEditFacade::SetIntermediateHoldingPosition(int32 NodeIndex, bool bSet)
{
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

	// HOISTED ABOVE THE SCOPE, so every refusal really does happen before the snapshot.
	// URoadNetwork::SetIntermediateHoldingPosition refuses a runway-holding position, and
	// checking it only in there would mean the one guard most likely to fire fired INSIDE
	// the edit.
	//
	// THERE IS NO ROLLBACK, so do not read the backstop below as one. ~FRoadEditScope calls
	// AbandonEdit, which DISCARDS the pending snapshot and leaves the live graph exactly as
	// the body left it - the stacks are untouched, the model is not restored. Returning
	// false from inside a scope is safe here only because the model refuses WITHOUT
	// MUTATING, so there is nothing to put back. A future mutation followed by a return
	// false inside a scope would leave a changed graph with no undo entry for it, which is
	// a corruption no later undo can reach - hence the guard living out here.
	if (Nodes[NodeIndex].HoldingPosition == EHoldingPositionKind::Runway)
	{
		UE_LOG(LogRoadMesh, Warning,
			TEXT("SetIntermediateHoldingPosition refused before the snapshot at guideline node %d: "
				 "it is a runway-holding position, derived from the runway and not the player's"),
			NodeIndex);
		return false;
	}
	// What the node says now, read BEFORE the mutation, so a no-op can be recognised after
	// it. A click that clears an already-clear position changes nothing, and committing it
	// would give the player an undo step that visibly does nothing and has to be pressed
	// twice to get past - see URoadEditHistory's "Edit lifecycle" comment.
	const EHoldingPositionKind Before = Nodes[NodeIndex].HoldingPosition;
	// After the guards, which refuse without mutating - a rejected position costs no snapshot.
	FRoadEditScope Edit(HistoryForEdit(), Network, TEXT("holding point"));
	if (!Network->SetIntermediateHoldingPosition(Node, bSet))
	{
		// The BACKSTOP, and reaching it means a guard above missed something - the node
		// liveness check and the runway-kind check together are meant to cover every
		// refusal the model can make. Distinct text from the hoisted guard's, so the log
		// says WHICH of the two fired rather than leaving the reader to guess.
		UE_LOG(LogRoadMesh, Warning,
			TEXT("SetIntermediateHoldingPosition refused inside the edit at guideline node %d (set %d) - "
				 "the hoisted guard should have caught this"),
			NodeIndex, bSet);
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
	CommitAndNotify(Edit, EChangeKind::Markings);
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
		UE_LOG(LogRoadMesh, Warning,
			TEXT("SplitSegment refused: segment %d at (%f, %f)"), SegmentIndex, At.X, At.Y);
		return INDEX_NONE;
	}

	CommitAndNotify(Edit);
	return Middle.Index;
}

void URoadEditFacade::BeginInteractiveEdit(const FString& Label)
{
	URoadEditHistory* Use = HistoryForEdit();
	URoadNetwork* Network = Actor().Network;
	if (Network != nullptr && Use != nullptr && !Use->IsEditing())
	{
		Use->BeginEdit(*Network, Label);
	}

	// WHAT THE PAVEMENT WAS WORTH BEFORE THE DRAG. Without this the cost model has a hole big
	// enough to drive through: build ten metres of taxiway, drag its end two kilometres, and
	// the extra pavement is free - MoveNode creates no segment, so nothing else charges for it.
	PavementValueAtDragStart = QuoteForAllPavement().BaseAmount;

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
		FBuildQuote Delta = QuoteForAllPavement();
		const double After = Delta.BaseAmount;
		Delta.BaseAmount = After - PavementValueAtDragStart;
		PavementValueAtDragStart = 0.0;

		if (Delta.BaseAmount > 0.0 && !CanAfford(Delta))
		{
			// REVERTED, NOT ABANDONED. The node has already moved on every frame of the drag, so
			// dropping the snapshot would leave the longer taxiway standing and unpaid for. This is
			// the one edit that has to be undone rather than merely refused - see
			// URoadEditHistory::RevertEdit.
			if (URoadNetwork* Reverted = History->RevertEdit())
			{
				Actor().Network = Reverted;
				HideGhost();
				NotifyChanged();
			}
			UE_LOG(LogRoadMesh, Log,
				TEXT("Drag reverted: cannot afford the %.0f of pavement it added"), Delta.BaseAmount);
			return;
		}

		if (Purse != nullptr)
		{
			if (Delta.BaseAmount > 0.0)
			{
				History->SetPendingCharge(Purse->Charge(Delta), Delta);
			}
			else if (Delta.BaseAmount < 0.0)
			{
				Delta.BaseAmount = -Delta.BaseAmount;
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
	// AND REFUSED OUT HERE, ABOVE THE SCOPE, for the reason SetIntermediateHoldingPosition
	// records at length: ~FRoadEditScope abandons the snapshot and restores nothing, so a
	// mutation followed by a `return false` inside a scope leaves a changed graph with no
	// undo entry to reach it.
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

	// JOINS A DRAG ALREADY IN PROGRESS, so the whole drag is one undo step; on its own it is
	// one edit of its own. IsEditing is what tells the two apart - the arrangement MoveNode
	// uses, and NOT an FRoadEditScope: a scope calls BeginEdit unconditionally, which
	// ensure-fails on the pending snapshot an interactive edit has already taken.
	URoadEditHistory* Use = HistoryForEdit();
	const bool bOwnsEdit = Use != nullptr && !Use->IsEditing();
	if (bOwnsEdit)
	{
		Use->BeginEdit(*Owner.Network, TEXT("move apron corner"));
	}

	const bool bMoved = Owner.Network->SetApronCorner(Apron, CornerIndex, To);

	if (bOwnsEdit)
	{
		if (bMoved)
		{
			Use->CommitEdit();
		}
		else
		{
			Use->AbandonEdit();
		}
	}

	// GEOMETRY ONLY WHILE AN INTERACTIVE EDIT IS STILL OPEN - THE BARE-CALL TRAP (review
	// follow-up on #165). bInteractiveEditOpen here means this call joined a drag that
	// BeginInteractiveEdit started and EndInteractiveEdit has not yet closed - the ONLY case
	// where something downstream (EndInteractiveEdit's own Topology notify) is guaranteed to
	// catch the derived graph up later. Anything else has no EndInteractiveEdit coming and
	// must do the whole job itself, Topology, right here: a BARE call (bOwnsEdit was true -
	// this very call opened and closed its own tiny history edit above, with no surrounding
	// BeginInteractiveEdit at all) never set the flag in the first place.
	//
	// bInteractiveEditOpen, NOT `Use != nullptr && Use->IsEditing()` (issue #190) - that test
	// was always false in an editor world, where HistoryForEdit() is a deliberate no-op (see
	// its own comment), so an editor-mode drag notified Topology on every frame regardless of
	// URoadBuildEdMode's own Begin/EndInteractiveEdit calls bracketing it exactly as PIE's do.
	// An apron corner is not in the road graph at all (see OnDragBegin's own comment), so a
	// Topology rebuild here does not cost this call anything a Geometry one would have saved
	// beyond what a genuine mid-drag frame already skips.
	if (bMoved)
	{
		if (bInteractiveEditOpen)
		{
			bGeometryChangedDuringEdit = true;
			NotifyChanged(EChangeKind::Geometry);
		}
		else
		{
			NotifyChanged(EChangeKind::Topology);
		}
	}
	return bMoved;
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

	ARoadNetworkActor& Owner = Actor();

	// JOINS A DRAG ALREADY IN PROGRESS, so drop-to-merge is ONE undo step with the move that
	// carried the node there - the arrangement MoveNode already uses, and the reason
	// IsEditing is what tells the two apart. On its own it is one edit of its own.
	URoadEditHistory* Use = HistoryForEdit();
	const bool bOwnsEdit = Use != nullptr && !Use->IsEditing();
	if (bOwnsEdit)
	{
		Use->BeginEdit(*Owner.Network, TEXT("merge nodes"));
	}

	if (!Owner.Network->MergeNodes(Keep, Absorb))
	{
		if (bOwnsEdit && Use != nullptr)
		{
			// Nothing was touched, so ABANDON is right here and Revert would be wrong - see
			// URoadEditHistory::RevertEdit on the distinction.
			Use->AbandonEdit();
		}
		return false;
	}

	// JUDGED AFTER, AND ONLY AFTER. RoadPlacement::NodeCornersFit reads a node's CURRENT arms
	// and judges them at a proposed position; it has no way to be asked about an arm set that
	// does not exist yet. So unlike MoveNode - which can and does judge before moving - a
	// merge has to happen before its corners can be measured at all.
	//
	// WHICH IS WHY THIS REVERTS RATHER THAN REFUSING. AbandonEdit drops the snapshot and
	// leaves the model as the edit left it, which here would be a merged junction the solver
	// cannot surface. RevertEdit hands back the state the edit started from, exactly as
	// EndInteractiveEdit does for a drag nobody can pay for.
	const FRoadNode* Merged = Owner.Network->GetNode(Keep);
	if (Merged != nullptr && !RoadPlacement::NodeCornersFit(*Owner.Network, Keep, Merged->Position))
	{
		if (Use != nullptr)
		{
			if (URoadNetwork* Reverted = Use->RevertEdit())
			{
				Owner.Network = Reverted;
				HideGhost();
				NotifyChanged();
			}
		}
		UE_LOG(LogRoadMesh, Log,
			TEXT("Merge refused: node %d folded into %d makes a corner the solver cannot "
				 "trim, so the whole edit is reverted."), AbsorbIndex, KeepIndex);
		return false;
	}

	UE_LOG(LogRoadMesh, Log, TEXT("Merged node %d into %d"), AbsorbIndex, KeepIndex);

	if (bOwnsEdit && Use != nullptr)
	{
		Use->CommitEdit();
	}

	// Pavement changed AND a node was removed - the graph's SHAPE changed - so this notifies
	// Topology (the default), unlike SetIntermediateHoldingPosition (issue #179), which
	// notifies the cheaper EChangeKind::Markings because it changes neither.
	NotifyChanged();
	return true;
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

	// Joins a drag already in progress, so the whole drag is one undo step; on its own it
	// is one edit of its own. IsEditing is what tells the two apart.
	URoadEditHistory* Use = HistoryForEdit();
	const bool bOwnsEdit = Use != nullptr && !Use->IsEditing();
	if (bOwnsEdit)
	{
		Use->BeginEdit(*Owner.Network, TEXT("move node"));
	}

	const bool bMoved = Owner.Network->SetNodePosition(Node, To);

	if (bOwnsEdit)
	{
		if (bMoved)
		{
			Use->CommitEdit();
		}
		else
		{
			Use->AbandonEdit();
		}
	}

	// UNCONDITIONAL on bOwnsEdit, deliberately: a drag calls this every frame and only the
	// FIRST frame owns the edit (see IsEditing above), but every frame that actually moves
	// the node must still rebuild - that per-frame notification during a drag is the whole
	// reason this is not folded into CommitAndNotify, which fires once per committed edit.
	//
	// GEOMETRY ONLY WHILE AN INTERACTIVE EDIT IS STILL OPEN - THE BARE-CALL TRAP (review
	// follow-up on #165). A move changes no node's existence and no segment's
	// endpoints-as-a-set, only where things sit, so a frame that is part of a real drag can
	// rebuild the surface alone and skip the guideline graph, anchor links, plots and
	// traffic - EndInteractiveEdit fires the one Topology notify that catches those up once
	// the drag commits (see bGeometryChangedDuringEdit's own comment). But that promise only
	// holds while bInteractiveEditOpen is true, meaning THIS call joined a drag
	// BeginInteractiveEdit opened and EndInteractiveEdit has not yet closed. A BARE call has
	// no EndInteractiveEdit coming at all (bOwnsEdit was true - this call opened and closed
	// its own tiny history edit above, the un-wrapped `Actor->MoveNode(...)` every test
	// before this review makes) and never set the flag, so it falls to the else branch and
	// does the whole job itself, Topology, right here.
	//
	// bInteractiveEditOpen, NOT `Use != nullptr && Use->IsEditing()` (issue #190) - that test
	// was always false in an editor world, where HistoryForEdit() is a deliberate no-op (see
	// its own comment): Use was null for every editor-mode move regardless of whether
	// URoadBuildEdMode had a real drag open, so every editor drag frame notified Topology in
	// full and EndInteractiveEdit's own History-gated early return meant nothing ever fired
	// the one catch-up notify a committed drag owes the derived graph.
	if (bMoved)
	{
		if (bInteractiveEditOpen)
		{
			bGeometryChangedDuringEdit = true;
			NotifyChanged(EChangeKind::Geometry);
		}
		else
		{
			NotifyChanged(EChangeKind::Topology);
		}
	}

	return bMoved;
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
		Quote.BaseAmount += Each.BaseAmount;
		if (!Quote.Source.IsValid())
		{
			// The first profile met stands for the lot. A junction of two widths is priced on
			// one of them for discount purposes only - the AMOUNT is the true sum either way.
			Quote.Source = Each.Source;
			Quote.What = Each.What;
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
		UE_LOG(LogRoadMesh, Warning, TEXT("DeleteNode refused: node %d would not remove"), NodeIndex);
		return false;
	}

	// The heal. Every one of these was judged against the post-deletion graph, so it is
	// being applied to exactly the state it was approved for.
	for (const FRoadNodeId& Stranded : Plan.Rejoin)
	{
		// THE PLAN'S PROFILE, not the level's default: the road being healed keeps its own
		// cross-section. See FRoadDeletionPlan::HealProfile for what laying the default
		// instead used to do to a runway.
		URoadProfile* Relay = Plan.HealProfile != nullptr
			? Plan.HealProfile.Get()
			: Owner.ResolveProfile();

		if (!Owner.Network->AddStraightSegment(Stranded, Plan.Anchor, Relay).IsSet())
		{
			UE_LOG(LogRoadMesh, Error,
				TEXT("DeleteNode healed only partly: node %d could not rejoin %d"),
				Stranded.Index, Plan.Anchor.Index);
		}
	}

	for (const FRoadNodeId& Litter : Plan.Swept)
	{
		Owner.Network->RemoveNode(Litter);
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
