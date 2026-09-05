// Undo/redo, aprons, stands, ClearNetwork and FindRoute - the rest of URoadEditFacade's
// body, split into a second translation unit purely to keep each .cpp under this task's
// self-review budget (issue #32 caps a new .cpp at 700 lines). This is still ONE class:
// RoadEditFacade.cpp holds node/segment/guideline surgery and the undo/mutator plumbing
// they share; everything here is the facade's remaining public surface. See
// Present/RoadEditFacade.h for the class itself.

#include "Present/RoadEditFacade.h"

#include "AirsideLog.h"
#include "Algo/Reverse.h"
#include "Entities/EntityDefinition.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteSearch.h"
#include "Present/RoadNetworkActor.h"
#include "Solve/RoadGeom.h"
#include "Tool/RoadEditHistory.h"

bool URoadEditFacade::Undo()
{
	ARoadNetworkActor& Owner = Actor();
	if (Owner.Network == nullptr || Owner.History == nullptr)
	{
		return false;
	}

	URoadNetwork* Restored = Owner.History->Undo(*Owner.Network);
	if (Restored == nullptr)
	{
		return false;
	}

	// Adopted outright rather than copied: the history has already let go of it.
	Owner.Network = Restored;

	// The preview may be describing a node that no longer exists, and its cache compares
	// only the cursor and the start node - neither of which an undo changes.
	Owner.HideGhost();
	OnChanged.Broadcast();
	return true;
}

bool URoadEditFacade::Redo()
{
	ARoadNetworkActor& Owner = Actor();
	if (Owner.Network == nullptr || Owner.History == nullptr)
	{
		return false;
	}

	URoadNetwork* Restored = Owner.History->Redo(*Owner.Network);
	if (Restored == nullptr)
	{
		return false;
	}

	Owner.Network = Restored;
	Owner.HideGhost();
	OnChanged.Broadcast();
	return true;
}

bool URoadEditFacade::CanUndo() const
{
	const URoadEditHistory* History = Actor().History;
	return History != nullptr && History->CanUndo();
}

bool URoadEditFacade::CanRedo() const
{
	const URoadEditHistory* History = Actor().History;
	return History != nullptr && History->CanRedo();
}

FString URoadEditFacade::PeekUndoLabel() const
{
	const URoadEditHistory* History = Actor().History;
	return History != nullptr ? History->PeekUndoLabel() : FString();
}

int32 URoadEditFacade::AddApron(const TArray<FVector2D>& Outline)
{
	if (Outline.Num() < 3)
	{
		UE_LOG(LogRoadMesh, Warning,
			TEXT("AddApron refused: %d corners, three is the minimum"), Outline.Num());
		return INDEX_NONE;
	}

	// The triangulator's contract is a SIMPLE polygon. Fed a figure-eight it produces
	// overlapping triangles rather than an error, so the refusal has to happen here.
	if (!RoadGeom::IsSimplePolygon(Outline))
	{
		UE_LOG(LogRoadMesh, Warning, TEXT("AddApron refused: the outline crosses itself"));
		return INDEX_NONE;
	}

	FApronSurface Surface;
	Surface.Outline = Outline;

	// Corrected, not refused. FApronSurface asks for counter-clockwise and the shoelace
	// sign says which way round this is; reversing is an answer, refusing is a complaint.
	if (RoadGeom::PolygonArea(Surface.Outline) < 0.0)
	{
		Algo::Reverse(Surface.Outline);
	}

	URoadNetwork& Net = EnsureNetwork();
	FRoadEditScope Edit(HistoryForEdit(), &Net, TEXT("add apron"));

	const FApronId Added = Net.AddApron(MoveTemp(Surface));
	if (!Added.IsSet())
	{
		UE_LOG(LogRoadMesh, Warning, TEXT("AddApron refused by the model"));
		return INDEX_NONE;
	}

	Edit.Commit();
	return Added.Index;
}

bool URoadEditFacade::DeleteApron(int32 ApronIndex)
{
	URoadNetwork* Network = Actor().Network;
	if (Network == nullptr || !Network->GetAprons().IsValidIndex(ApronIndex)
		|| !Network->GetAprons()[ApronIndex].bAlive)
	{
		return false;
	}

	FApronId Doomed;
	Doomed.Index = ApronIndex;
	Doomed.Generation = Network->GetAprons()[ApronIndex].Generation;

	FRoadEditScope Edit(HistoryForEdit(), Network, TEXT("delete apron"));

	if (!Network->RemoveApron(Doomed))
	{
		return false;
	}

	Edit.Commit();
	return true;
}

int32 URoadEditFacade::FindApronAt(FVector2D Where) const
{
	const URoadNetwork* Network = GetNetwork();
	if (Network == nullptr)
	{
		return INDEX_NONE;
	}

	// Walked backwards so the most recently added apron wins where two overlap, which is
	// what "the one on top" means to someone who just drew it.
	const TArray<FApronSurface>& Aprons = Network->GetAprons();
	for (int32 Index = Aprons.Num() - 1; Index >= 0; --Index)
	{
		if (Aprons[Index].bAlive && RoadGeom::PointInPolygon(Aprons[Index].Outline, Where))
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

int32 URoadEditFacade::PlaceStand(FVector2D Where, double Heading)
{
	ARoadNetworkActor& Owner = Actor();
	UEntityDefinition* Stand = Owner.ResolveStandDefinition();
	if (Stand == nullptr)
	{
		UE_LOG(LogRoadMesh, Warning,
			TEXT("PlaceStand refused: no StandDefinition. Author DA_Stand_CodeC with "
				 "Tools/Python/build_stand_asset.py, or set one on the actor."));
		return INDEX_NONE;
	}

	// Moved down from URoadNetwork::PlaceEntity along with Anchors itself: HasUsableAnchorIds
	// is a UEntityDefinition method, and Model/ no longer calls into Entities/ at all - see
	// Tool/RoadEditTarget.h's header comment for the other half of that seam. Complained
	// about, not refused: a half-authored definition should be visible in the log rather
	// than fatal at the call site. But it IS a real fault - lookup is by id, so two anchors
	// sharing one are indistinguishable and a query for either returns the first, which
	// sends the fuel truck to the belt loader and reports success.
	if (!UEntityDefinition::HasUsableAnchorIds(Stand))
	{
		UE_LOG(LogRoadMesh, Error,
			TEXT("PlaceStand: %s has anchors with empty or duplicate ids. Anchor lookups on "
				 "this entity will be ambiguous."),
			*Stand->GetName());
	}

	URoadNetwork& Net = EnsureNetwork();
	FRoadEditScope Edit(HistoryForEdit(), &Net, TEXT("place stand"));

	// The design wingspan is read HERE, in the one caller allowed to see the definition, and
	// handed down - see PlaceEntity's comment on why Model/ cannot read it for itself.
	const double DesignWingspan =
		Stand->DesignAircraft != nullptr ? Stand->DesignAircraft->Footprint.Wingspan : 0.0;
	const FEntityInstanceId Placed = Net.PlaceEntity(Stand, Stand->Anchors, Where, Heading, DesignWingspan);
	if (!Placed.IsSet())
	{
		return INDEX_NONE;
	}

	Edit.Commit();
	return Placed.Index;
}

bool URoadEditFacade::DeleteEntity(int32 EntityIndex)
{
	URoadNetwork* Network = Actor().Network;
	if (Network == nullptr || !Network->GetEntities().IsValidIndex(EntityIndex)
		|| !Network->GetEntities()[EntityIndex].bAlive)
	{
		return false;
	}

	FEntityInstanceId Doomed;
	Doomed.Index = EntityIndex;
	Doomed.Generation = Network->GetEntities()[EntityIndex].Generation;

	FRoadEditScope Edit(HistoryForEdit(), Network, TEXT("delete stand"));

	if (!Network->RemoveEntity(Doomed))
	{
		return false;
	}

	Edit.Commit();
	return true;
}

int32 URoadEditFacade::FindEntityAt(FVector2D Where, double Radius) const
{
	const URoadNetwork* Network = GetNetwork();
	if (Network == nullptr || Radius <= 0.0)
	{
		return INDEX_NONE;
	}

	// Picked by the entity's own position - its stop mark - rather than by any anchor. An
	// anchor is where a vehicle parks; the stand is the thing being pointed at.
	double BestSquared = Radius * Radius;
	int32 Best = INDEX_NONE;

	const TArray<FEntityInstance>& Entities = Network->GetEntities();
	for (int32 Index = 0; Index < Entities.Num(); ++Index)
	{
		if (!Entities[Index].bAlive)
		{
			continue;
		}

		const double DistanceSquared = FVector2D::DistSquared(Entities[Index].Position, Where);
		if (DistanceSquared <= BestSquared)
		{
			BestSquared = DistanceSquared;
			Best = Index;
		}
	}
	return Best;
}

void URoadEditFacade::ClearNetwork()
{
	ARoadNetworkActor& Owner = Actor();
	Owner.HideGhost();

	// Undoable, because clearing everything by accident is the worst thing the tool can do
	// and the only one with nothing left on screen to hint at what was lost.
	if (Owner.Network != nullptr)
	{
		FRoadEditScope Edit(HistoryForEdit(), Owner.Network, TEXT("clear network"));
		Edit.Commit();
	}

	// A fresh network rather than a drain: node removal bumps generations and prunes
	// incident lists, and none of that bookkeeping is worth doing on the way to empty.
	Owner.Network = NewObject<URoadNetwork>(&Owner);
	OnChanged.Broadcast();
}

FRoutePlan URoadEditFacade::FindRoute(
	FGuidelineNodeId Start, FGuidelineNodeId Goal, ETraversalClass Class, double Wingspan) const
{
	const URoadNetwork* Network = GetNetwork();
	if (Network == nullptr)
	{
		return FRoutePlan();
	}

	FRouteQuery Query;
	Query.Start = Start;
	Query.Goal = Goal;
	Query.Class = Class;
	Query.Wingspan = Wingspan;

	return RouteSearch::Find(*Network, Query);
}
