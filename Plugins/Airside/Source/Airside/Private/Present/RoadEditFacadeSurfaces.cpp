// Undo/redo, aprons, stands, ClearNetwork and FindRoute - the rest of URoadEditFacade's
// body, split into a second translation unit purely to keep each .cpp under this task's
// self-review budget (issue #32 caps a new .cpp at 700 lines). This is still ONE class:
// RoadEditFacade.cpp holds node/segment/guideline surgery and the undo/mutator plumbing
// they share; everything here is the facade's remaining public surface. See
// Present/RoadEditFacade.h for the class itself.

#include "Present/RoadEditFacade.h"

#include "Build/BuildCost.h"
#include "Model/BuildPurse.h"

#include "AirsideLog.h"
#include "Algo/Reverse.h"
#include "Build/AnchorLink.h"
#include "Entities/EntityDefinition.h"
#include "Model/RoadNetwork.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadSlotMap.h"
#include "Model/RouteSearch.h"
#include "Present/RoadNetworkActor.h"
#include "Solve/PlotFit.h"
#include "Solve/RoadGeom.h"
#include "Tool/RoadEditHistory.h"

bool URoadEditFacade::Travel(TFunctionRef<URoadNetwork*(URoadEditHistory&, URoadNetwork&)> Step)
{
	ARoadNetworkActor& Owner = Actor();
	if (Owner.Network == nullptr || Owner.History == nullptr)
	{
		return false;
	}

	URoadNetwork* Restored = Step(*Owner.History, *Owner.Network);
	if (Restored == nullptr)
	{
		return false;
	}

	// Adopted outright rather than copied: the history has already let go of it.
	Owner.Network = Restored;

	// The preview may be describing a node that no longer exists, and its cache compares
	// only the cursor and the start node - neither of which an undo/redo changes.
	Owner.HideGhost();
	NotifyChanged();
	return true;
}

bool URoadEditFacade::Undo()
{
	// READ BEFORE TRAVELLING. Undo moves that snapshot onto the redo stack, so afterwards the
	// entry this names is no longer the one on top - see PeekUndoChargeId.
	URoadEditHistory* History = Actor().History;
	const int32 ChargeId = History != nullptr ? History->PeekUndoChargeId() : INDEX_NONE;

	if (!Travel([](URoadEditHistory& H, URoadNetwork& Network) { return H.Undo(Network); }))
	{
		return false;
	}

	// REVERSED BY ID, so the player gets back exactly what the build took rather than a figure
	// recomputed from geometry that undo has just removed. Demolition is the other case and
	// works the other way round - see IBuildPurse::Credit.
	if (Purse != nullptr && ChargeId != INDEX_NONE)
	{
		Purse->Reverse(ChargeId);
	}
	return true;
}

bool URoadEditFacade::Redo()
{
	URoadEditHistory* History = Actor().History;
	const FBuildQuote Quote = History != nullptr ? History->PeekRedoQuote() : FBuildQuote();

	// REFUSED WHEN THE MONEY HAS GONE SINCE, exactly as a fresh build would be. Redoing a
	// taxiway the player can no longer afford would hand it to them for nothing, and undo
	// refunding while redo rebuilt free is a loop that prints money.
	if (!CanAfford(Quote))
	{
		UE_LOG(LogRoadMesh, Log, TEXT("Redo refused: cannot afford %s again"),
			*Quote.What.ToString());
		return false;
	}

	if (!Travel([](URoadEditHistory& H, URoadNetwork& Network) { return H.Redo(Network); }))
	{
		return false;
	}

	if (Purse != nullptr && !Quote.IsFree() && History != nullptr)
	{
		// A NEW id: the one the step used to carry names an entry the undo already reversed,
		// and the ledger refuses a second reversal of it on purpose.
		History->SetUndoTopCharge(Purse->Charge(Quote));
	}
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

	// Priced on the CORRECTED outline, so a clockwise-drawn apron costs the same as the same
	// shape drawn the other way - see BuildCost::PolygonAreaSquareMetres, which takes the
	// absolute area for that reason. Refused before the scope: an abandoned scope drops the
	// undo snapshot but does not roll the model back.
	const FBuildQuote Quote = QuoteForApron(Surface.Outline);
	if (!CanAfford(Quote))
	{
		UE_LOG(LogRoadMesh, Log, TEXT("AddApron refused: cannot afford %s"), *Quote.What.ToString());
		return INDEX_NONE;
	}

	URoadNetwork& Net = EnsureNetwork();
	FRoadEditScope Edit(HistoryForEdit(), &Net, TEXT("add apron"));

	const FApronId Added = Net.AddApron(MoveTemp(Surface));
	if (!Added.IsSet())
	{
		UE_LOG(LogRoadMesh, Warning, TEXT("AddApron refused by the model"));
		return INDEX_NONE;
	}

	CommitPurchase(Edit, Quote);
	return Added.Index;
}

bool URoadEditFacade::DeleteSlot(bool bDoomed, const TCHAR* Label,
	TFunctionRef<bool(URoadNetwork&)> Remove, const FBuildQuote& Quote)
{
	URoadNetwork* Network = Actor().Network;
	if (Network == nullptr || !bDoomed)
	{
		return false;
	}

	FRoadEditScope Edit(HistoryForEdit(), Network, Label);

	if (!Remove(*Network))
	{
		return false;
	}

	// A DEFAULT-CONSTRUCTED QUOTE IS FREE, which is DisconnectGuideline's answer and the
	// reason that caller needed no change: a guideline is not pavement, and unlinking one
	// destroys no surface to be paid for.
	CommitDisposal(Edit, Quote);
	return true;
}

bool URoadEditFacade::DeleteApron(int32 ApronIndex)
{
	const URoadNetwork* Network = Actor().Network;
	const FApronId Doomed = Network != nullptr ? Network->ApronIdAt(ApronIndex) : FApronId();
	// Quoted from the outline while the apron is still there to measure.
	const FApronSurface* Surface = Doomed.IsSet() ? Network->GetApron(Doomed) : nullptr;
	const FBuildQuote Quote = Surface != nullptr ? QuoteForApron(Surface->Outline) : FBuildQuote();

	return DeleteSlot(Doomed.IsSet(), TEXT("delete apron"),
		[Doomed](URoadNetwork& Net) { return Net.RemoveApron(Doomed); }, Quote);
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

int32 URoadEditFacade::PlaceEntity(FVector2D Where, double Heading, EPlaceableEntity Kind)
{
	ARoadNetworkActor& Owner = Actor();

	// RESOLVED BY KIND, in one place - see ARoadNetworkActor::ResolveEntityDefinition, which
	// the tool's preview goes through too so the two cannot pick different objects.
	UEntityDefinition* Definition = Owner.ResolveEntityDefinition(Kind);
	if (Definition == nullptr)
	{
		// REFUSED, never substituted. Falling back to the other kind would drop a stand where
		// a building was asked for, which reads on screen as the tool working. The message
		// names the ASSET AND THE SCRIPT that authors it, which is what turned "the stand
		// tool does nothing" into a one-line fix the first time.
		// ONE FORMAT STRING with the missing thing substituted, not a ternary between two
		// literals: UE 5.8's format-string sanitiser needs a compile-time TCHAR array, and a
		// ternary is not one. The message still names the ASSET AND THE SCRIPT that authors
		// it, which is what turned "the stand tool does nothing" into a one-line fix.
		const TCHAR* Missing = Kind == EPlaceableEntity::FuelDepot
			? TEXT("FuelDepotDefinition (author DA_FuelDepot)")
			: TEXT("StandDefinition (author DA_Stand_CodeC)");
		UE_LOG(LogRoadMesh, Warning,
			TEXT("PlaceEntity refused: no %s with Tools/Python/build_stand_asset.py, or set "
				 "one on the actor."), Missing);
		return INDEX_NONE;
	}

	// Moved down from URoadNetwork::PlaceEntity along with Anchors itself: HasUsableAnchorIds
	// is a UEntityDefinition method, and Model/ no longer calls into Entities/ at all - see
	// Tool/RoadEditTarget.h's header comment for the other half of that seam. Complained
	// about, not refused: a half-authored definition should be visible in the log rather
	// than fatal at the call site. But it IS a real fault - lookup is by id, so two anchors
	// sharing one are indistinguishable and a query for either returns the first, which
	// sends the fuel truck to the belt loader and reports success.
	if (!UEntityDefinition::HasUsableAnchorIds(Definition))
	{
		UE_LOG(LogRoadMesh, Error,
			TEXT("PlaceEntity: %s has anchors with empty or duplicate ids. Anchor lookups on "
				 "this entity will be ambiguous."),
			*Definition->GetName());
	}

	// Priced from the definition and refused before anything is placed - see ConnectNodes for
	// why an abandoned scope is not a rollback.
	const FBuildQuote Quote = BuildCost::ForEntity(*Definition);
	if (!CanAfford(Quote))
	{
		UE_LOG(LogRoadMesh, Log, TEXT("PlaceEntity refused: cannot afford %s"),
			*Quote.What.ToString());
		return INDEX_NONE;
	}

	URoadNetwork& Net = EnsureNetwork();
	FRoadEditScope Edit(HistoryForEdit(), &Net,
		Kind == EPlaceableEntity::FuelDepot ? TEXT("place fuel depot") : TEXT("place stand"));

	// The design wingspan is read HERE, in the one caller allowed to see the definition, and
	// handed down - see PlaceEntity's comment on why Model/ cannot read it for itself.
	const double DesignWingspan =
		Definition->DesignAircraft != nullptr ? Definition->DesignAircraft->Footprint.Wingspan : 0.0;
	// PoseRole travels with DesignWingspan and for the same reason: this is the one caller
	// allowed to see the definition, so it reads both and hands them down.
	const FEntityInstanceId Placed = Net.PlaceEntity(Definition, Definition->Anchors, Where,
		Heading, DesignWingspan, Definition->PoseRole, Definition->Trucks);
	if (!Placed.IsSet())
	{
		return INDEX_NONE;
	}

	CommitPurchase(Edit, Quote);
	return Placed.Index;
}

int32 URoadEditFacade::PlaceEntityInPlot(const TArray<FVector2D>& Outline,
	FVector2D FrontageA, FVector2D FrontageB,
	const TArray<EDepotModule>& Modules, EPlaceableEntity Kind)
{
	ARoadNetworkActor& Owner = Actor();

	// RESOLVED BY KIND, through the same one place PlaceEntity uses, so a plot and a plop
	// cannot end up placing different objects for the same key.
	UEntityDefinition* Definition = Owner.ResolveEntityDefinition(Kind);
	if (Definition == nullptr)
	{
		UE_LOG(LogRoadMesh, Warning,
			TEXT("PlaceEntityInPlot refused: no FuelDepotDefinition (author DA_FuelDepot) "
				 "with Tools/Python/build_stand_asset.py, or set one on the actor."));
		return INDEX_NONE;
	}

	URoadNetwork& Net = EnsureNetwork();

	// The triangulator's contract is a SIMPLE polygon, and it is this layer that owes it -
	// fed a figure-eight it produces overlapping triangles rather than an error. The gesture
	// cannot produce one now that a plot is a rectangle, which is exactly why this stays:
	// the guarantee belongs to whoever feeds the triangulator, not to whoever happens to be
	// calling this month.
	if (!RoadGeom::IsSimplePolygon(Outline))
	{
		UE_LOG(LogRoadMesh, Warning,
			TEXT("PlaceEntityInPlot refused: the outline crosses itself"));
		return INDEX_NONE;
	}

	// COUNTER-CLOCKWISE, exactly the correction AddApron makes and for the same reason: the
	// pad goes through the same triangulator, which orients its triangles from the winding,
	// and the surface is not two-sided. Stored clockwise, the pad faces DOWN - the fence and
	// the modules still stand up, because PlotFit derives the interior side from the signed
	// area, and the concrete is simply absent. That shipped on 2026-09-15 and took a
	// screenshot to find.
	//
	// KEPT EVEN THOUGH THE GESTURE NOW HANDS IN A COUNTER-CLOCKWISE RECTANGLE. It costs a
	// shoelace sum, and the alternative is a facade that is correct only for the one caller
	// that happens to get the winding right.
	TArray<FVector2D> Wound = Outline;
	if (RoadGeom::PolygonArea(Wound) < 0.0)
	{
		Algo::Reverse(Wound);

		// The frontage travels with it. It was given in the ORIGINAL winding order, and
		// PlotFit reads which side the interior is on from that direction - left alone
		// across a reversal, it would lay every bay across the road instead of into the plot.
		Swap(FrontageA, FrontageB);
	}

	// THE FRONTAGE IS GIVEN, NOT SEARCHED FOR. A road-snapped rectangle knows which of its
	// edges is on the road by construction, so searching would be a second opinion about a
	// fact the gesture already established - and that is why FAnchorLink::FindFrontageEdge
	// was deleted rather than left sitting there looking authoritative.
	//
	// The plot also touches a road BY CONSTRUCTION, so the old "no road within reach"
	// refusal went with it. The Idle stage reports a cursor near no service road before a
	// click is even possible, which is earlier and cheaper than refusing at commit.
	const PlotFit::FPlotFit Fit = PlotFit::FitBays(Wound, FrontageA, FrontageB);
	if (!Fit.bFits)
	{
		UE_LOG(LogRoadMesh, Warning,
			TEXT("PlaceEntityInPlot refused: the plot is smaller than one %.0f m bay."),
			PlotFit::BayWidthUu / 100.0);
		return INDEX_NONE;
	}

	FRoadEditScope Edit(HistoryForEdit(), &Net, TEXT("place fuel depot"));

	FEntityPlacement Placement;
	Placement.Definition = Definition;
	Placement.Anchors = Definition->Anchors;

	// THE GATE IS THE POSE, and there is exactly one of it however many sheds the plot
	// holds - BuildFuelDepot's ruling that two lead-ins from one small building into one
	// road is a duplicate painted line. It sits at the middle of the frontage edge, which
	// is the one point on the plot the road is reliably nearest.
	Placement.Position = (FrontageA + FrontageB) * 0.5;

	// The bays all face the same way, so the first one's heading IS the installation's.
	Placement.Heading = Fit.Bays[0].Heading;
	Placement.PoseRole = Definition->PoseRole;
	Placement.Outline = Wound;
	Placement.Modules = Modules;

	// TRAILING MODULES ARE DROPPED, not squeezed in. A player who chose four modules for a
	// three-bay plot gets three and is told so; scaling the bays to fit would silently
	// change the size they drew, which is the one thing a drawn plot must never do.
	if (Placement.Modules.Num() > Fit.Bays.Num())
	{
		UE_LOG(LogRoadMesh, Warning,
			TEXT("PlaceEntityInPlot: %d modules chosen but only %d bays fit; dropped %d."),
			Placement.Modules.Num(), Fit.Bays.Num(),
			Placement.Modules.Num() - Fit.Bays.Num());
		Placement.Modules.SetNum(Fit.Bays.Num());
	}

	const FEntityInstanceId Placed = Net.PlaceEntity(Placement);
	if (!Placed.IsSet())
	{
		return INDEX_NONE;
	}

	CommitAndNotify(Edit);
	return Placed.Index;
}

bool URoadEditFacade::DeleteEntity(int32 EntityIndex)
{
	const URoadNetwork* Network = Actor().Network;
	const FEntityInstanceId Doomed = Network != nullptr ? Network->EntityIdAt(EntityIndex) : FEntityInstanceId();

	// KEYED ON KIND, the same way PlaceEntity's own label is (#103 review): the label lost
	// this distinction when the field-by-field version collapsed into DeleteSlot, and the
	// issue named it - a fuel depot removed under "delete stand" is the wrong word in the
	// undo history and the log. Doomed.IsSet() means the index is still live to read.
	const FEntityInstance* Entity = Doomed.IsSet() ? Network->GetEntity(Doomed) : nullptr;
	const TCHAR* Label = (Entity != nullptr && Entity->PoseRole == EServiceRole::Fuel)
		? TEXT("delete fuel depot") : TEXT("delete stand");

	// Quoted from the DEFINITION, which is what was paid for, while the instance still names
	// it - after the removal there is nothing left to ask.
	const FBuildQuote Quote = (Entity != nullptr && Entity->Definition != nullptr)
		? BuildCost::ForEntity(*Entity->Definition) : FBuildQuote();

	return DeleteSlot(Doomed.IsSet(), Label,
		[Doomed](URoadNetwork& Net) { return Net.RemoveEntity(Doomed); }, Quote);
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
	return RoadSlot::NearestAlive<FEntityInstance>(Network->GetEntities(), Where, Radius,
		[](const FEntityInstance& Entity) { return Entity.Position; });
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
	NotifyChanged();
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

	// VEHICLES ALWAYS ROUTE WITH THE TABLE; AIRCRAFT NEVER DO. Spec §4. A van sent to a job
	// should go round the queue that is there when it is dispatched, and it has no clearance
	// to violate by doing so. An aircraft's route is fixed at clearance and stays fixed: a
	// taxi clearance that quietly re-routed itself round traffic between being read out and
	// being flown is not a clearance, and the only thing that may change an aircraft's route
	// afterwards is UGroundTraffic::ReplanAt - a deadlock, or the graph itself changing.
	//
	// QueryingAgent stays 0: nothing has been dispatched yet, so there is no agent whose own
	// claims should be discounted from the cost.
	// TrafficModelProvider, not Actor().GetTraffic()->GetModel() directly: this class must
	// not reach past Network/History for anything else (#104, and see the class comment) -
	// ARoadNetworkActor wires the provider once, right after it creates Traffic, the same
	// way it wires OnChanged right after creating Facade.
	if (Class != ETraversalClass::Aircraft && TrafficModelProvider)
	{
		if (const UGroundTraffic* Model = TrafficModelProvider())
		{
			Query.Occupancy = &Model->GetOccupancy();
			Query.CongestionWeight = Model->Rules.CongestionWeight;
		}
	}

	return RouteSearch::Find(*Network, Query);
}
