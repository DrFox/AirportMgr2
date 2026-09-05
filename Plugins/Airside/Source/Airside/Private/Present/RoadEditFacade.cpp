// Node, segment and guideline surgery, plus the undo/mutator plumbing (Owner, EnsureNetwork,
// EnsureHistory, HistoryForEdit, MakeLiveNodeId/MakeLiveSegmentId, the ghost/traffic
// forwarders) every one of them shares. Undo/redo, aprons, stands, ClearNetwork and FindRoute
// are a second translation unit of this SAME class - see RoadEditFacadeSurfaces.cpp's own
// banner comment for why.

#include "Present/RoadEditFacade.h"

#include "AirsideLog.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadSlotMap.h"
#include "Present/RoadNetworkActor.h"
#include "Profiles/RoadProfile.h"
#include "Solve/RunwayDesignator.h"
#include "Tool/GuidelineDrawTool.h"
#include "Tool/RoadEditHistory.h"

ARoadNetworkActor* URoadEditFacade::Owner() const
{
	return GetTypedOuter<ARoadNetworkActor>();
}

URoadNetwork& URoadEditFacade::EnsureNetwork()
{
	ARoadNetworkActor& Actor = *Owner();
	if (Actor.Network == nullptr)
	{
		Actor.Network = NewObject<URoadNetwork>(&Actor);
	}
	return *Actor.Network;
}

URoadEditHistory& URoadEditFacade::EnsureHistory()
{
	ARoadNetworkActor& Actor = *Owner();
	if (Actor.History == nullptr)
	{
		Actor.History = NewObject<URoadEditHistory>(&Actor);
	}

	// Re-applied on every edit rather than only at creation, so lowering the depth in the
	// details panel mid-session takes effect instead of waiting for a restart.
	Actor.History->MaxDepth = FMath::Max(Actor.MaxUndoDepth, 1);
	return *Actor.History;
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

const URoadNetwork* URoadEditFacade::GetNetwork() const
{
	const ARoadNetworkActor* Actor = Owner();
	return Actor != nullptr ? Actor->Network : nullptr;
}

double URoadEditFacade::GetMinimumRunwayLength() const
{
	return Owner()->MinimumRunwayLength;
}

const UEntityDefinition* URoadEditFacade::GetStandDefinition() const
{
	// The RAW field, not ResolveStandDefinition()'s content-default fallback - see
	// ARoadNetworkActor::GetStandDefinition's own comment for why that distinction matters.
	return Owner()->StandDefinition;
}

void URoadEditFacade::UpdateGhost(int32 FromNodeIndex, const FRoadSnapResult& Snap, bool bValid)
{
	Owner()->UpdateGhost(FromNodeIndex, Snap, bValid);
}

void URoadEditFacade::HideGhost()
{
	Owner()->HideGhost();
}

void URoadEditFacade::RebuildMesh()
{
	Owner()->RebuildMesh();
}

bool URoadEditFacade::DispatchAgent(const FRoutePlan& Plan, const FAirframe& Airframe)
{
	return Owner()->DispatchAgent(Plan, Airframe);
}

bool URoadEditFacade::MakeLiveNodeId(int32 Index, FRoadNodeId& OutId) const
{
	const URoadNetwork* Network = GetNetwork();
	if (Network == nullptr || !Network->GetNodes().IsValidIndex(Index))
	{
		return false;
	}

	// Build the handle from the slot's own generation and then check liveness properly.
	// FRoadNodeId::IsSet() would only report that a handle was assigned, which is the
	// check this codebase renamed precisely to stop people reaching for it here.
	FRoadNodeId Candidate;
	Candidate.Index = Index;
	Candidate.Generation = Network->GetNodes()[Index].Generation;

	if (!RoadSlot::IsValid<FRoadNodeId, FRoadNode>(Network->GetNodes(), Candidate))
	{
		return false;
	}

	OutId = Candidate;
	return true;
}

bool URoadEditFacade::MakeLiveSegmentId(int32 Index, FRoadSegmentId& OutId) const
{
	const URoadNetwork* Network = GetNetwork();
	if (Network == nullptr || !Network->GetSegments().IsValidIndex(Index))
	{
		return false;
	}

	FRoadSegmentId Candidate;
	Candidate.Index = Index;
	Candidate.Generation = Network->GetSegments()[Index].Generation;

	if (!RoadSlot::IsValid<FRoadSegmentId, FRoadSegment>(Network->GetSegments(), Candidate))
	{
		return false;
	}

	OutId = Candidate;
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
		UE_LOG(LogRoadMesh, Warning, TEXT("PlaceNode refused at (%f, %f)"), Where.X, Where.Y);
		return INDEX_NONE;
	}

	Edit.Commit();
	return Node.Index;
}

bool URoadEditFacade::ConnectNodes(int32 FromIndex, int32 ToIndex)
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

	ARoadNetworkActor& Actor = *Owner();

	// Created after the guards above, all of which refuse without mutating anything, so a
	// rejected connection never costs a snapshot.
	FRoadEditScope Edit(HistoryForEdit(), Actor.Network, TEXT("connect nodes"));

	// Straight only. The model stores a Bezier control point, but AddSegment still
	// interpolates its interior samples in a straight line, so a curve authored here
	// would render as a chord until slice 2b samples the curve properly.
	const FRoadSegmentId Segment = Actor.Network->AddStraightSegment(From, To, Actor.ResolveProfile());
	if (!Segment.IsSet())
	{
		UE_LOG(LogRoadMesh, Warning, TEXT("ConnectNodes refused: %d -> %d"), FromIndex, ToIndex);
		return false;
	}

	Edit.Commit();
	return true;
}

bool URoadEditFacade::PlaceRunway(FVector2D From, FVector2D To, URoadProfile* RunwayProfile)
{
	ARoadNetworkActor& Actor = *Owner();
	if (Actor.Network == nullptr)
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
	if (Length < Actor.MinimumRunwayLength)
	{
		UE_LOG(LogRoadMesh, Warning,
			TEXT("PlaceRunway refused: %.0f uu is under the %.0f uu minimum"),
			Length, Actor.MinimumRunwayLength);
		return false;
	}

	// After the guards, all of which refuse without mutating, so a rejected runway never
	// costs a snapshot - the same rule ConnectNodes follows.
	FRoadEditScope Edit(HistoryForEdit(), Actor.Network, TEXT("place runway"));

	const FRoadNodeId A = Actor.Network->AddNode(From);
	const FRoadNodeId B = Actor.Network->AddNode(To);
	if (!A.IsSet() || !B.IsSet())
	{
		return false;
	}

	// STRAIGHT, and the model cannot express otherwise here: AddStraightSegment puts the
	// control point on the midpoint, which IsStraight tests for exactly.
	const FRoadSegmentId Segment = Actor.Network->AddStraightSegment(A, B, RunwayProfile);
	if (!Segment.IsSet())
	{
		return false;
	}

	Edit.Commit();
	OnChanged.Broadcast();

	UE_LOG(LogRoadMesh, Log, TEXT("Runway %s placed, %.0f uu long, %.0f uu wide"),
		*RunwayDesignator::ToPairText(To - From), Length, RunwayProfile->GetTotalWidth());
	return true;
}

int32 URoadEditFacade::ConnectGuidelines(int32 FromNodeIndex, int32 ToNodeIndex)
{
	URoadNetwork* Network = Owner()->Network;
	if (Network == nullptr)
	{
		return INDEX_NONE;
	}

	const TArray<FGuidelineNode>& Nodes = Network->GetGuidelineNodes();
	if (!Nodes.IsValidIndex(FromNodeIndex) || !Nodes.IsValidIndex(ToNodeIndex))
	{
		return INDEX_NONE;
	}

	FGuidelineNodeId From;
	From.Index = FromNodeIndex;
	From.Generation = Nodes[FromNodeIndex].Generation;

	FGuidelineNodeId To;
	To.Index = ToNodeIndex;
	To.Generation = Nodes[ToNodeIndex].Generation;

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
	return Added.IsSet() ? Added.Index : INDEX_NONE;
}

bool URoadEditFacade::DisconnectGuideline(int32 EdgeIndex)
{
	URoadNetwork* Network = Owner()->Network;
	if (Network == nullptr)
	{
		return false;
	}

	const TArray<FGuidelineEdge>& Edges = Network->GetGuidelineEdges();
	if (!Edges.IsValidIndex(EdgeIndex) || !Edges[EdgeIndex].bAlive)
	{
		return false;
	}

	// A derived edge belongs to the pavement, and the next rebuild would put it straight
	// back. Obeying here would be indistinguishable from ignoring the click.
	if (Edges[EdgeIndex].bDerived)
	{
		UE_LOG(LogRoadMesh, Warning,
			TEXT("DisconnectGuideline refused: edge %d is derived from a road, not hand-drawn"),
			EdgeIndex);
		return false;
	}

	FRoadEditScope Edit(HistoryForEdit(), Network, TEXT("unlink guidelines"));

	FGuidelineEdgeId Id;
	Id.Index = EdgeIndex;
	Id.Generation = Edges[EdgeIndex].Generation;
	return Network->RemoveGuidelineEdge(Id);
}

int32 URoadEditFacade::FindNodeNear(FVector2D Where, double Radius) const
{
	const URoadNetwork* Network = GetNetwork();
	if (Network == nullptr || Radius <= 0.0)
	{
		return INDEX_NONE;
	}

	// Compared squared, so a caller passing a large radius costs no square roots.
	const double RadiusSquared = Radius * Radius;
	double BestSquared = RadiusSquared;
	int32 Best = INDEX_NONE;

	const TArray<FRoadNode>& Nodes = Network->GetNodes();
	for (int32 Index = 0; Index < Nodes.Num(); ++Index)
	{
		if (!Nodes[Index].bAlive)
		{
			continue;
		}

		const double DistanceSquared = FVector2D::DistSquared(Nodes[Index].Position, Where);
		if (DistanceSquared <= BestSquared)
		{
			BestSquared = DistanceSquared;
			Best = Index;
		}
	}

	return Best;
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

	ARoadNetworkActor& Actor = *Owner();
	FRoadEditScope Edit(HistoryForEdit(), Actor.Network, TEXT("split segment"));

	const FRoadNodeId Middle = SplitSegmentIn(*Actor.Network, Doomed, At);
	if (!Middle.IsSet())
	{
		UE_LOG(LogRoadMesh, Warning,
			TEXT("SplitSegment refused: segment %d at (%f, %f)"), SegmentIndex, At.X, At.Y);
		return INDEX_NONE;
	}

	Edit.Commit();
	return Middle.Index;
}

FRoadNodeId URoadEditFacade::SplitSegmentIn(URoadNetwork& Net, FRoadSegmentId Doomed, const FVector2D& At)
{
	const FRoadSegment* Segment = Net.GetSegment(Doomed);
	const FRoadNode* EndA = Segment != nullptr ? Net.GetNode(Segment->A) : nullptr;
	const FRoadNode* EndB = Segment != nullptr ? Net.GetNode(Segment->B) : nullptr;
	if (EndA == nullptr || EndB == nullptr)
	{
		return FRoadNodeId();
	}

	// Copied out before anything mutates. Every pointer above dangles the moment the
	// segment is removed or the arrays reallocate, and the two replacements need all of
	// this after that point.
	const FRoadNodeId KeepA = Segment->A;
	const FRoadNodeId KeepB = Segment->B;
	URoadProfile* KeepProfile = Segment->Profile;
	const FVector2D PositionA = EndA->Position;
	const FVector2D PositionB = EndB->Position;

	// A degeneracy floor, NOT a placement policy: how far from an end a split should be
	// allowed is the snap chain's MinSplitFromEndpoint, which is tuned and can be turned
	// down. This is the point below which the result is not a road at all, and no setting
	// may cross it - a zero-length segment has no direction, so the solver cannot derive
	// a bearing for it and the junction at either end loses an arm.
	constexpr double MinSplitOffset = 1.0;
	if (FVector2D::DistSquared(At, PositionA) < MinSplitOffset * MinSplitOffset
		|| FVector2D::DistSquared(At, PositionB) < MinSplitOffset * MinSplitOffset)
	{
		return FRoadNodeId();
	}

	const FRoadNodeId Middle = Net.AddNode(At);
	if (!Middle.IsSet())
	{
		return FRoadNodeId();
	}

	// Removed, not reshaped. A segment's endpoints are its identity and both of them
	// change here, so the handle must die rather than quietly come to mean half a road.
	if (!Net.RemoveSegment(Doomed))
	{
		Net.RemoveNode(Middle);
		return FRoadNodeId();
	}

	const FRoadSegmentId First = Net.AddStraightSegment(KeepA, Middle, KeepProfile);
	const FRoadSegmentId Second = Net.AddStraightSegment(Middle, KeepB, KeepProfile);

	// Both endpoints were checked live and the middle node was just created, so the only
	// way here is a model invariant having changed underneath. Loud rather than silent:
	// the graph is now missing a road the player can still see the ends of.
	if (!First.IsSet() || !Second.IsSet())
	{
		UE_LOG(LogRoadMesh, Error, TEXT("SplitSegmentIn left a segment half-replaced: first=%d second=%d"),
			First.IsSet() ? 1 : 0, Second.IsSet() ? 1 : 0);
	}

	return Middle;
}

void URoadEditFacade::BeginInteractiveEdit(const FString& Label)
{
	URoadEditHistory* Use = HistoryForEdit();
	URoadNetwork* Network = Owner()->Network;
	if (Network != nullptr && Use != nullptr && !Use->IsEditing())
	{
		Use->BeginEdit(*Network, Label);
	}
}

void URoadEditFacade::EndInteractiveEdit(bool bKeep)
{
	URoadEditHistory* History = Owner()->History;
	if (History == nullptr || !History->IsEditing())
	{
		return;
	}

	if (bKeep)
	{
		History->CommitEdit();
	}
	else
	{
		History->AbandonEdit();
	}
}

bool URoadEditFacade::MoveNode(int32 NodeIndex, FVector2D To)
{
	FRoadNodeId Node;
	if (!MakeLiveNodeId(NodeIndex, Node))
	{
		return false;
	}

	ARoadNetworkActor& Actor = *Owner();
	const FRoadNode* Live = Actor.Network->GetNode(Node);
	if (Live == nullptr)
	{
		return false;
	}

	// Judged before moving. Every road this node holds gets longer or shorter as it goes,
	// and one pulled under the minimum is one the solver cannot trim back from both ends.
	for (const FRoadSegmentId& Incident : Live->Incident)
	{
		const FRoadNodeId Other = Actor.Network->GetOtherEnd(Incident, Node);
		const FRoadNode* Far = Actor.Network->GetNode(Other);
		if (Far != nullptr && FVector2D::Distance(Far->Position, To) < Actor.PlacementLimits.MinSegmentLength)
		{
			return false;
		}
	}

	// Joins a drag already in progress, so the whole drag is one undo step; on its own it
	// is one edit of its own. IsEditing is what tells the two apart.
	URoadEditHistory* Use = HistoryForEdit();
	const bool bOwnsEdit = Use != nullptr && !Use->IsEditing();
	if (bOwnsEdit)
	{
		Use->BeginEdit(*Actor.Network, TEXT("move node"));
	}

	const bool bMoved = Actor.Network->SetNodePosition(Node, To);

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

	return bMoved;
}

FRoadDeletionPlan URoadEditFacade::PlanNodeDeletion(int32 NodeIndex) const
{
	FRoadNodeId Node;
	const ARoadNetworkActor* Actor = Owner();
	if (Actor == nullptr || Actor->Network == nullptr || !MakeLiveNodeId(NodeIndex, Node))
	{
		return FRoadDeletionPlan();
	}
	return RoadHeal::PlanNodeDeletion(*Actor->Network, Node, Actor->PlacementLimits);
}

bool URoadEditFacade::DeleteNode(int32 NodeIndex)
{
	FRoadNodeId Node;
	if (!MakeLiveNodeId(NodeIndex, Node))
	{
		UE_LOG(LogRoadMesh, Warning, TEXT("DeleteNode refused: %d is not a live node"), NodeIndex);
		return false;
	}

	ARoadNetworkActor& Actor = *Owner();
	const FRoadDeletionPlan Plan = RoadHeal::PlanNodeDeletion(*Actor.Network, Node, Actor.PlacementLimits);
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

	FRoadEditScope Edit(HistoryForEdit(), Actor.Network, TEXT("delete node"));

	// The cascade is the model's: a segment whose endpoint is gone has no geometry.
	if (!Actor.Network->RemoveNode(Node))
	{
		UE_LOG(LogRoadMesh, Warning, TEXT("DeleteNode refused: node %d would not remove"), NodeIndex);
		return false;
	}

	// The heal. Every one of these was judged against the post-deletion graph, so it is
	// being applied to exactly the state it was approved for.
	for (const FRoadNodeId& Stranded : Plan.Rejoin)
	{
		if (!Actor.Network->AddStraightSegment(Stranded, Plan.Anchor, Actor.ResolveProfile()).IsSet())
		{
			UE_LOG(LogRoadMesh, Error,
				TEXT("DeleteNode healed only partly: node %d could not rejoin %d"),
				Stranded.Index, Plan.Anchor.Index);
		}
	}

	for (const FRoadNodeId& Litter : Plan.Swept)
	{
		Actor.Network->RemoveNode(Litter);
	}

	Edit.Commit();
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

	ARoadNetworkActor& Actor = *Owner();

	// Captured before the removal, because afterwards the segment cannot say what it joined.
	const FRoadSegment* Doomed = Actor.Network->GetSegment(Segment);
	const FRoadNodeId EndA = Doomed != nullptr ? Doomed->A : FRoadNodeId();
	const FRoadNodeId EndB = Doomed != nullptr ? Doomed->B : FRoadNodeId();

	FRoadEditScope Edit(HistoryForEdit(), Actor.Network, TEXT("delete segment"));

	if (!Actor.Network->RemoveSegment(Segment))
	{
		UE_LOG(LogRoadMesh, Warning,
			TEXT("DeleteSegment refused: segment %d would not remove"), SegmentIndex);
		return false;
	}

	// Cleanup, not deletion: an endpoint left with no road holds no geometry, so removing
	// it destroys nothing. An endpoint that still has roads is untouched.
	for (const FRoadNodeId& End : { EndA, EndB })
	{
		if (const FRoadNode* Live = Actor.Network->GetNode(End))
		{
			if (Live->Incident.Num() == 0)
			{
				Actor.Network->RemoveNode(End);
			}
		}
	}

	Edit.Commit();
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

