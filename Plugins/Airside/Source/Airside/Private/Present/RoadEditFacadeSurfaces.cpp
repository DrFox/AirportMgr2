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
#include "Content/AirsideSettings.h"
#include "Build/DepotKit.h"
#include "Build/PlotLayoutStrategy.h"
#include "Entities/EntityDefinition.h"
#include "Model/RoadNetwork.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadSlotMap.h"
#include "Model/RoutePolicy.h"
#include "Model/RouteSearch.h"
#include "Present/RoadNetworkActor.h"
#include "Profiles/RoadProfile.h"
#include "Solve/IcaoCode.h"
#include "Solve/PlotYard.h"
#include "Solve/RoadGeom.h"
#include "Solve/StandBox.h"
#include "Tool/PlotGesture.h"
#include "Present/RoadEditHistory.h"

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

	// Adopted outright rather than copied: the history has already let go of it. See
	// AdoptNetwork's own header comment (#299) for the hide-ghost-and-notify tail this shares
	// with the two RevertEdit sites and ClearNetwork.
	AdoptNetwork(*Restored);
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

namespace
{
	/**
	 * How far two outlines must interpenetrate, uu, before OutlinesOverlap calls it an
	 * overlap. One centimetre: the PlotFit::CornerInsetUu precedent, and for the same reason -
	 * a boundary-exact question answered by floating point is a coin flip, and the question
	 * meant is "do these interiors share ground", not "is this point on that line".
	 */
	constexpr double OverlapToleranceUu = 1.0;

	/**
	 * Do two outlines share INTERIOR - more than OverlapToleranceUu of ground in common?
	 * Touching along an edge or at a corner is NOT overlapping.
	 *
	 * SEPARATING AXES, NOT CONTAINMENT (final review I4). This was "any vertex of one inside
	 * the other, or an edge of one crossing an edge of the other", through RoadGeom::
	 * PointInPolygon and RoadGeom::SegmentsCross. Both are undefined exactly on a boundary
	 * (RoadGeom.h says so of the first), and a row of stands drawn off one taxiway grid puts
	 * every neighbour's corner exactly on the last one's edge, to within the ulps of two
	 * independent sums: the second stand of a row was refused "overlaps stand 0" by a coin
	 * flip. Two convex outlines are disjoint exactly when some edge normal of either separates
	 * their projections, so asking that axis by axis with a tolerance makes "touching" a
	 * margin rather than a knife edge - and a real overlap, of any shape (one inside the
	 * other, or crossed like a plus sign, the two cases the old test needed both halves for),
	 * still has no separating axis and is still refused.
	 *
	 * CONVEX INPUTS, which is every outline that reaches here: a stand is StandBox's
	 * rectangle, a depot plot the gesture's rectangle. A non-convex one would be judged by its
	 * own edges' normals, so the answer errs toward "overlaps" (refusal), never toward
	 * letting two interiors share ground.
	 */
	bool OutlinesOverlap(TArrayView<const FVector2D> A, TArrayView<const FVector2D> B)
	{
		// Is there an edge normal of Edges along which A and B's projections are apart, or
		// meet within the tolerance?
		const auto SeparatedByEdgesOf = [A, B](TArrayView<const FVector2D> Edges)
		{
			const int32 Num = Edges.Num();
			for (int32 Index = 0; Index < Num; ++Index)
			{
				const FVector2D Edge = Edges[(Index + 1) % Num] - Edges[Index];
				const double Length = Edge.Size();
				if (Length <= UE_DOUBLE_SMALL_NUMBER)
				{
					continue;
				}
				// Unit length, so the tolerance below is in uu along every axis alike.
				const FVector2D Axis(-Edge.Y / Length, Edge.X / Length);

				double MinA = TNumericLimits<double>::Max(), MaxA = TNumericLimits<double>::Lowest();
				for (const FVector2D& P : A)
				{
					const double D = FVector2D::DotProduct(P, Axis);
					MinA = FMath::Min(MinA, D);
					MaxA = FMath::Max(MaxA, D);
				}
				double MinB = TNumericLimits<double>::Max(), MaxB = TNumericLimits<double>::Lowest();
				for (const FVector2D& P : B)
				{
					const double D = FVector2D::DotProduct(P, Axis);
					MinB = FMath::Min(MinB, D);
					MaxB = FMath::Max(MaxB, D);
				}
				if (MaxA - MinB <= OverlapToleranceUu || MaxB - MinA <= OverlapToleranceUu)
				{
					return true;
				}
			}
			return false;
		};
		return !SeparatedByEdgesOf(A) && !SeparatedByEdgesOf(B);
	}
}

PlotYard::FReservation URoadEditFacade::ReserveForPlot(TArrayView<const FVector2D> Outline,
	FVector2D FrontageA, FVector2D FrontageB, EPlaceableEntity Kind) const
{
	++PlotEvaluatorCount;

	// THE SAME RESOLUTION FPlotPlaceTool::ReservationFor makes: a definition the target
	// cannot resolve falls back to the scatter layout, which is what an unauthored plot type
	// would have drawn anyway - see that function's own comment on why a second source of
	// truth for the layout shipped once already (DA_FuelDepot vs. the tool's own Kind map).
	const UEntityDefinition* Definition = GetEntityDefinition(Kind);
	const EPlotLayout Layout = Definition != nullptr ? Definition->Layout : EPlotLayout::Scatter;
	const TArray<PlotYard::FKitSpec> Specs = ResolveDepotKits();

	// THE GATE IS THE FRONTAGE MIDPOINT, which is also what PlaceEntityInPlot stores as the
	// entity's own Position below - DepotYardSeed keys off exactly that pose, so this solve
	// and the one the presenter re-derives from the built entity roll the same yard.
	FPlotSite Site;
	Site.Outline = Outline;
	Site.FrontageA = FrontageA;
	Site.FrontageB = FrontageB;
	Site.Gate = (FrontageA + FrontageB) * 0.5;
	Site.Seed = DepotYardSeed(Site.Gate);

	return PlotLayoutFor(Layout)->Solve(Site, Specs);
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

	// NOT OVER A STAND. WhyStandRefused has always refused a stand drawn over a depot; the
	// reverse had no check, so a depot plot laid across a stand's apron placed its fence and
	// tanks under the parked aircraft (final review). The SAME OutlinesOverlap, so a depot
	// flush against a stand's edge places exactly as a stand flush against a depot does.
	// Stands only: depot-on-depot was never refused and is not this fix's to change.
	for (int32 Index = 0; Index < Net.GetEntities().Num(); ++Index)
	{
		const FEntityInstance& Entity = Net.GetEntities()[Index];
		if (Entity.bAlive && Entity.IsStand() && Entity.IsPlotted() && OutlinesOverlap(Outline, Entity.Outline))
		{
			UE_LOG(LogRoadMesh, Warning,
				TEXT("PlaceEntityInPlot refused: the plot overlaps stand %d"), Index);
			return INDEX_NONE;
		}
	}

	// COUNTER-CLOCKWISE, exactly the correction AddApron makes and for the same reason: the
	// pad goes through the same triangulator, which orients its triangles from the winding,
	// and the surface is not two-sided. Stored clockwise, the pad faces DOWN - the fence and
	// the modules still stand up, because PlotYard derives the interior side from the signed
	// area (PlotYard::InwardOf), and the concrete is simply absent. That shipped on
	// 2026-09-15 and took a screenshot to find.
	//
	// KEPT EVEN THOUGH THE GESTURE NOW HANDS IN A COUNTER-CLOCKWISE RECTANGLE. It costs a
	// shoelace sum, and the alternative is a facade that is correct only for the one caller
	// that happens to get the winding right.
	TArray<FVector2D> Wound = Outline;
	if (RoadGeom::PolygonArea(Wound) < 0.0)
	{
		Algo::Reverse(Wound);

		// The frontage travels with it. It was given in the ORIGINAL winding order, and
		// PlotYard::InwardOf reads which side the interior is on from that direction - left
		// alone across a reversal, it would lay every module across the road instead of into
		// the plot.
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
	//
	// ONE EVALUATOR - issue #182. This used to judge the commit against PlotFit::FitBays, a
	// 4 m x 12 m bay grid with its own point-in-polygon test, while FPlotPlaceTool judged the
	// SAME plot's ghost and readout against PlotYard::Reserve - two solvers, free to disagree
	// on exactly the plots where FitBays's own comment said its winding-number test earned
	// its keep. ReserveForPlot runs the IDENTICAL call the tool's preview makes, so whatever
	// the player was shown is what gets judged here.
	const PlotYard::FReservation Reservation = ReserveForPlot(Wound, FrontageA, FrontageB, Kind);
	if (Reservation.Stands.Num() == 0)
	{
		// RESERVES NOTHING IS THE ONE REFUSAL, not "fewer than asked for" - the 2026-09-20
		// module-kits design rules a kit's ceiling fixed at draw time and a partial fit
		// ordinary (see its section 3.3): a plot that holds three sheds and no tank still
		// builds, exactly as it did before reservation existed. Only a plot that would place
		// NOTHING AT ALL is refused, which is what FPlotPlaceTool's own readout already warns
		// about ("This plot holds nothing") - the same evaluator, the same threshold.
		UE_LOG(LogRoadMesh, Warning,
			TEXT("PlaceEntityInPlot refused: this plot has no room to reserve anything."));
		return INDEX_NONE;
	}

	// PRICED AND REFUSED BEFORE THE SCOPE OPENS - issue #193. This call used to end in
	// CommitAndNotify, whose own header says it is "for an edit that moves no pavement... a
	// bare node, naming a runway, splitting a segment" - but a plotted depot moves an apron
	// pad AND places the same UEntityDefinition PlaceEntity charges BuildCost::ForEntity for,
	// so a plotted depot was free while the identical stand placed with PlaceEntity was paid
	// for, and undo's money accounting never saw the plot at all. Two quotes, one for the
	// entity and one for the pad it sits on (the same rate AddApron charges, at the WOUND
	// outline QuoteForApron already reasons about), summed the way QuoteForAllPavement and
	// DeleteNode's own doomed-segment total already sum several FBuildQuotes into one -
	// there is no third pricing mechanism to invent here, only this file's existing two.
	// CanAfford runs BEFORE Net.PlaceEntity below for the same reason PlaceEntity's own quote
	// does: an abandoned FRoadEditScope drops the undo SNAPSHOT, not the mutation, so a
	// refusal after the entity is placed would leave it built and unpaid for.
	FBuildQuote Quote = BuildCost::ForEntity(*Definition);
	const FBuildQuote ApronQuote = QuoteForApron(Wound);
	Quote.BaseAmount += ApronQuote.BaseAmount;
	Quote.What = FText::Format(NSLOCTEXT("BuildCost", "DepotPlusPad", "{0} + {1}"),
		Quote.What, ApronQuote.What);
	if (!CanAfford(Quote))
	{
		UE_LOG(LogRoadMesh, Log, TEXT("PlaceEntityInPlot refused: cannot afford %s"),
			*Quote.What.ToString());
		return INDEX_NONE;
	}

	FRoadEditScope Edit(HistoryForEdit(), &Net, TEXT("place fuel depot"));

	FEntityPlacement Placement;
	Placement.Definition = Definition;
	Placement.Anchors = Definition->Anchors;

	// THE GATE IS THE POSE, and there is exactly one of it however many modules the plot
	// holds - BuildFuelDepot's ruling that two lead-ins from one small building into one
	// road is a duplicate painted line. It sits at the middle of the frontage edge, which
	// is the one point on the plot the road is reliably nearest - and the same point
	// ReserveForPlot just seeded the yard from.
	Placement.Position = (FrontageA + FrontageB) * 0.5;

	// Every module the yard places is squared to the SAME frontage, so the inward normal IS
	// the installation's own heading - the fact PlotFit::FitBays's uniform Bay.Heading used
	// to state and PlotYard::InwardOf states now, without a solve of its own.
	const FVector2D Inward = PlotYard::InwardOf(Wound, FrontageA, FrontageB);
	Placement.Heading = RoadGeom::Bearing(Inward);
	Placement.PoseRole = Definition->PoseRole;
	Placement.Outline = Wound;

	// THE TRUCKS' HOME, SET BACK INTO THE YARD far enough that the link onto the road can turn
	// at a radius the largest service vehicle can steer - FAnchorLink::PoseSetbackFor. On the
	// gate, as it was until 2026-09-22, the square turn onto the road had the kerb's 300 uu and
	// came out at R = 206 against a lock of 699: the truck crabbed out of every depot.
	Placement.PoseSetbackUu = FAnchorLink::PoseSetbackFor(Net, Placement.Position, Inward,
		UAirsideSettings::ResolveLargestServiceVehicle(), Owner.ServiceLinkRadius);
	const FVector2D Home = Placement.Position + Inward.GetSafeNormal() * Placement.PoseSetbackUu;
	if (!RoadGeom::PointInPolygon(Wound, Home))
	{
		// SAID, NOT REFUSED: the depot still works, its trucks just start outside the fence.
		// Only a plot shallower than the turn needs can do this.
		UE_LOG(LogRoadMesh, Warning,
			TEXT("PlaceEntityInPlot: the trucks' home, %.0f uu in from the gate so they can turn "
				 "onto the road, is outside this plot - draw it deeper."), Placement.PoseSetbackUu);
	}
	UE_LOG(LogRoadMesh, Log, TEXT("PlaceEntityInPlot: trucks' home set %.0f uu in from the gate"),
		Placement.PoseSetbackUu);

	// STORED WHOLE, NEVER TRUNCATED TO WHAT FITS - issue #182 again. FitBays's bay count used
	// to cap Modules here, which was a SECOND capacity rule competing with the reservation
	// that decides what actually stands (the four-point gesture spec's own section 8 named
	// this as a second opinion before this evaluator existed to replace it). The reservation
	// is recomputed, never saved (2026-09-20 module-kits design section 3.4), so the entity
	// keeps exactly what the player chose and UPlotPresenter lights only the stands the SAME
	// solve, run again from the built entity, actually reserved.
	Placement.Modules = Modules;

	const FEntityInstanceId Placed = Net.PlaceEntity(Placement);
	if (!Placed.IsSet())
	{
		return INDEX_NONE;
	}

	// CommitPurchase, NOT CommitAndNotify (issue #193): the charge above must actually change
	// hands, and the pending-charge id must land on the undo snapshot through the SAME path
	// PlaceEntity's own commit uses - CommitPurchase is that one door, not a second one this
	// call would otherwise need to keep in step with it.
	CommitPurchase(Edit, Quote);
	return Placed.Index;
}

namespace
{
	/**
	 * Does the straight chord A-B (a segment's two node positions) enter Outline's interior -
	 * either endpoint inside, or a crossing with any edge?
	 *
	 * THE CHORD, NOT THE CURVE. A segment may carry a Bezier Control point off the straight
	 * line between its ends, but the entrance edge a stand is drawn against is itself
	 * straight, so a taxiway curving away from the chord toward the stand would have to
	 * cross the chord first - the straight-line test is conservative in the direction that
	 * matters (it never MISSES a road that actually reaches the interior along a nearly
	 * straight run, which is every taxiway this gesture draws against).
	 */
	bool SegmentEntersInterior(const FVector2D& A, const FVector2D& B,
		TArrayView<const FVector2D> Outline)
	{
		if (RoadGeom::PointInPolygon(Outline, A) || RoadGeom::PointInPolygon(Outline, B))
		{
			return true;
		}
		const int32 Num = Outline.Num();
		for (int32 Index = 0; Index < Num; ++Index)
		{
			if (RoadGeom::SegmentsCross(A, B, Outline[Index], Outline[(Index + 1) % Num]))
			{
				return true;
			}
		}
		return false;
	}
}

FBuildQuote URoadEditFacade::QuoteStand(const UEntityDefinition& Definition,
	TArrayView<const FVector2D> Outline) const
{
	// THE ONE PLACE THIS QUOTE IS BUILT - fix round 1 on this task's own review: WhyStandRefused's
	// afford gate and PlaceStandInPlot's charge used to each compute the entity-plus-pad total
	// by hand, and had already drifted (WhyStandRefused summed BaseAmount but never combined
	// the "{0} + {1}" What text PlaceStandInPlot's own copy carried) - the same "two solvers,
	// free to disagree" shape issue #182 closed for the plot reservation itself, reopened here
	// in a smaller way. Mirrors PlaceEntityInPlot's own two-quote sum exactly.
	FBuildQuote Quote = BuildCost::ForEntity(Definition);
	const FBuildQuote ApronQuote = QuoteForApron(Outline);
	Quote.BaseAmount += ApronQuote.BaseAmount;
	Quote.What = FText::Format(NSLOCTEXT("BuildCost", "StandPlusPad", "{0} + {1}"),
		Quote.What, ApronQuote.What);
	return Quote;
}

FString URoadEditFacade::WhyStandRefused(TArrayView<const FVector2D> Outline) const
{
	// SELF-CROSSING FIRST, AND GUARDS THE MALFORMED CASE TOO - StandBox::WidthOf/DepthOf
	// below do not check Outline.Num() themselves (Solve/StandBox.h's own note), and every
	// real candidate is StandBox::BoxAt's own four corners, so fewer than four reads the
	// same as a rectangle that folds through itself: not a shape to measure.
	if (Outline.Num() < 4 || !RoadGeom::IsSimplePolygon(Outline))
	{
		return TEXT("the outline crosses itself");
	}

	// SIZE, MEASURED AGAINST CODE A'S OWN FLOOR - not the rectangle's own resolved letter,
	// because there is no resolved letter yet for anything smaller than A's floor: that is
	// exactly the condition StandBox::LetterOf is unset under (IcaoCode::LetterForStandSize's
	// "too small for any letter"). A's floor is the smallest of the six, so this is the one
	// gate that ever refuses for size, and it must run before LetterOf is asked to read a box
	// too small for it to name.
	const double Width = StandBox::WidthOf(Outline);
	const double Depth = StandBox::DepthOf(Outline);
	const double FloorWidth = IcaoCode::StandWidthForLetter(EIcaoCode::A);
	const double FloorDepth = IcaoCode::StandDepthForLetter(EIcaoCode::A);
	if (Width < FloorWidth || Depth < FloorDepth)
	{
		// TWO INDEPENDENT DEFICITS, NAMED SEPARATELY, so a drag that is both narrow and
		// shallow does not make the player guess which dimension to fix first.
		TArray<FString> Deficits;
		if (Width < FloorWidth)
		{
			Deficits.Add(FString::Printf(TEXT("needs %.0f m more width"),
				(FloorWidth - Width) / 100.0));
		}
		if (Depth < FloorDepth)
		{
			Deficits.Add(FString::Printf(TEXT("needs %.0f m more depth"),
				(FloorDepth - Depth) / 100.0));
		}
		return FString::Join(Deficits, TEXT(" and "));
	}

	// THE LETTER THE DRAWN BOX READS AS. Guaranteed set past the size gate above (the same
	// "smaller than Code A's floor" condition both check), but read back through the
	// TOptional rather than GetValue()'d blind, so a future change to either rule fails loud
	// here instead of asserting on a box neither gate actually refused.
	const TOptional<EIcaoCode> Letter = StandBox::LetterOf(Outline);
	if (!Letter.IsSet())
	{
		return TEXT("needs more width and depth");
	}

	// THE LETTER'S OWN TEMPLATE, RESOLVED AND MEASURED - Task 1's finding, live: A and B do
	// not fit their own floor today, C through F do. ResolveStandDefinitionFor is the one
	// place that answer is cached and logged; this asks it rather than re-deriving FitsItsLetter.
	if (Actor().ResolveStandDefinitionFor(*Letter) == nullptr)
	{
		return FString::Printf(TEXT("Code %s stands cannot be built yet"),
			IcaoCode::ToLetter(*Letter));
	}

	// OVERLAP WITH ANY OTHER PLOTTED ENTITY - a stand or a depot, named differently because a
	// player fixes the two by different gestures (move the new stand, or delete someone
	// else's depot). EVERY STAND IS PLOTTED, since Task 6 (URoadNetwork::PlaceEntity and
	// EnsureStandOutlines both give a point-placed or legacy-loaded stand the Code C box its
	// pose implies), so this test no longer special-cases one - the IsPlotted() guard below
	// exists only for a not-yet-alive slot, same as it always did for a depot's own check.
	// ENFORCED BY: Airside.Model.StandOutline.PointPlacedStandGetsOutline, ...LegacyGetsCodeCBox,
	// and AirportOps.Present.RuntimeLoad.LegacyStandGetsOutline (a save-game load, too).
	const URoadNetwork* Network = GetNetwork();
	if (Network != nullptr)
	{
		const TArray<FEntityInstance>& Entities = Network->GetEntities();
		for (int32 Index = 0; Index < Entities.Num(); ++Index)
		{
			const FEntityInstance& Entity = Entities[Index];
			if (!Entity.bAlive || !Entity.IsPlotted()) { continue; }
			if (OutlinesOverlap(Outline, Entity.Outline))
			{
				return Entity.IsStand()
					? FString::Printf(TEXT("overlaps stand %d"), Index)
					: FString(TEXT("overlaps a fuel depot"));
			}
		}
	}

	// A TAXIWAY THROUGH THE INTERIOR. SERVICE ROADS ARE ALLOWED - the template's own GSE road
	// crosses the box's back strip on purpose (UEntityDefinition::BuildStandTemplate), and the
	// entrance taxiway this stand is drawn off necessarily runs along (never through) the
	// entrance edge, which RoadGeom::SegmentsCross does not count as entering (collinear
	// overlap is not a crossing - see its own header).
	//
	// "SERVICE ROAD" IS PlotGesture::IsServiceRoad, the question the gesture's own anchor
	// search asks. This file kept a private copy (IsServiceRoadSegment) until 2026-09-23 on
	// the belief that Present/ could not reach Tool/ - but only the reverse is forbidden
	// (Check-Architecture rule 1: Tool/ never includes Present/), and a second copy of "a
	// truck may drive here" is a stand refused over a road the tool would not anchor on.
	if (Network != nullptr)
	{
		const TArray<FRoadSegment>& Segments = Network->GetSegments();
		for (int32 SegmentIndex = 0; SegmentIndex < Segments.Num(); ++SegmentIndex)
		{
			const FRoadSegment& Segment = Segments[SegmentIndex];
			if (!Segment.bAlive
				|| PlotGesture::IsServiceRoad(*Network, Network->SegmentIdAt(SegmentIndex))) { continue; }
			const FRoadNode* NodeA = Network->GetNode(Segment.A);
			const FRoadNode* NodeB = Network->GetNode(Segment.B);
			if (NodeA == nullptr || NodeB == nullptr) { continue; }
			if (SegmentEntersInterior(NodeA->Position, NodeB->Position, Outline))
			{
				return TEXT("a taxiway crosses the stand");
			}
		}
	}

	// AFFORD, LAST - the same "priced and refused before the scope opens" ordering
	// PlaceEntityInPlot uses (issue #193). QuoteStand is the ONE place the entity-plus-pad
	// quote is built - see its own comment for why this used to be a second, drifted copy of
	// PlaceStandInPlot's own pricing. Winding does not change the quote (BuildCost::ForApron
	// reasons about area magnitude), so this may be asked of Outline exactly as given.
	const UEntityDefinition* Definition = Actor().ResolveStandDefinitionFor(*Letter);
	const FBuildQuote Quote = QuoteStand(*Definition, Outline);
	if (!CanAfford(Quote))
	{
		return FString::Printf(TEXT("cannot afford %s"), *Quote.What.ToString());
	}

	return FString();
}

int32 URoadEditFacade::PlaceStandInPlot(const TArray<FVector2D>& Outline,
	FVector2D EntranceA, FVector2D EntranceB)
{
	ARoadNetworkActor& Owner = Actor();

	// ONE EVALUATOR - the #182 lesson applied to stands. WhyStandRefused is exactly what a
	// tool's own readout would ask, so a commit here can never approve what the readout just
	// refused, or the reverse. Asked of Outline BEFORE the CCW correction below, because
	// every check inside it is winding-independent - see WhyStandRefused's own header.
	const FString Refusal = WhyStandRefused(Outline);
	if (!Refusal.IsEmpty())
	{
		UE_LOG(LogRoadMesh, Warning, TEXT("PlaceStandInPlot refused: %s"), *Refusal);
		return INDEX_NONE;
	}

	URoadNetwork& Net = EnsureNetwork();

	// COUNTER-CLOCKWISE, exactly the correction PlaceEntityInPlot makes and for the same
	// reason - kept even though the gesture already hands in a CCW rectangle, because the
	// alternative is a facade correct only for the one caller that happens to get the
	// winding right. The entrance points travel with the reversal: StandBox::PoseFor and
	// PlotYard::InwardOf both read the interior side from the ORIGINAL winding direction, so
	// leaving them unswapped after a reversal would derive an Inward pointing the wrong way.
	TArray<FVector2D> Wound = Outline;
	FVector2D A = EntranceA;
	FVector2D B = EntranceB;
	if (RoadGeom::PolygonArea(Wound) < 0.0)
	{
		Algo::Reverse(Wound);
		Swap(A, B);
	}

	const TOptional<EIcaoCode> Letter = StandBox::LetterOf(Wound);
	// GUARANTEED SET: WhyStandRefused's size gate above already refused anything smaller
	// than Code A's floor, which is the same condition LetterOf is unset under, and
	// reversing a rectangle's winding does not change the lengths LetterOf measures. NOT
	// because opposite edges of a rectangle are equal - in floats they are not, and the
	// reversed outline's edge 0->1 is the drawn rectangle's far edge - but because
	// StandBox::WidthOf/DepthOf round to a whole uu, which both edges agree on.
	if (!Letter.IsSet())
	{
		UE_LOG(LogRoadMesh, Warning,
			TEXT("PlaceStandInPlot refused: the wound outline has no letter, though "
				 "WhyStandRefused passed it - report this as a bug."));
		return INDEX_NONE;
	}

	UEntityDefinition* Definition = Owner.ResolveStandDefinitionFor(*Letter);
	if (Definition == nullptr)
	{
		// SAME REASON: WhyStandRefused already checks this before returning empty. Guarded
		// rather than assumed, for the same "report this as a bug" reason as the letter above.
		UE_LOG(LogRoadMesh, Warning,
			TEXT("PlaceStandInPlot refused: Code %s has no definition, though WhyStandRefused "
				 "passed it - report this as a bug."), IcaoCode::ToLetter(*Letter));
		return INDEX_NONE;
	}

	const FVector2D Inward = PlotYard::InwardOf(Wound, A, B);
	// THE COMMITTED FIGURE (#292): what the stand actually gets built at, unlike the ghost
	// preview (FStandPlotTool::DescribeLetter), which cannot reach Content/ at all and uses
	// the floor instead - see that function's own comment for the accepted gap between them.
	const StandBox::FStandPose Pose =
		StandBox::PoseFor(A, B, Inward, *Letter, UAirsideSettings::ResolveLetterEnvelope(*Letter));

	// PRICED AND REFUSED BEFORE THE SCOPE OPENS - issue #193, the same ordering
	// PlaceEntityInPlot uses. WhyStandRefused already ran this exact afford check (through the
	// SAME QuoteStand this calls) as its own last gate, but its answer is not carried forward
	// (it returns a string, not a quote), so the charge itself is computed fresh here, on the
	// wound outline this call actually places.
	const FBuildQuote Quote = QuoteStand(*Definition, Wound);
	if (!CanAfford(Quote))
	{
		UE_LOG(LogRoadMesh, Log, TEXT("PlaceStandInPlot refused: cannot afford %s"),
			*Quote.What.ToString());
		return INDEX_NONE;
	}

	FRoadEditScope Edit(HistoryForEdit(), &Net, TEXT("place stand"));

	FEntityPlacement Placement;
	Placement.Definition = Definition;
	Placement.Anchors = Definition->Anchors;
	Placement.Position = Pose.Position;
	Placement.Heading = RoadGeom::Bearing(Pose.Facing);
	Placement.PoseRole = Definition->PoseRole;
	Placement.Outline = Wound;

	// THE WIDEST SPAN THAT STILL READS BACK AS THIS LETTER - see
	// IcaoCode::DesignSpanForLetter's own header for why MaxWingspanForLetter's ceiling
	// itself cannot be used here: a drawn stand's captured DesignWingspan feeds
	// IcaoCode::StandAdmits/StandRank the same way a legacy stand's does
	// (ArrivalPlanner::ChooseStand, UStandAllocator::Reserve), so the letter it reads back as
	// must be the letter it was actually drawn to.
	Placement.DesignWingspan = IcaoCode::DesignSpanForLetter(*Letter);

	const FEntityInstanceId Placed = Net.PlaceEntity(Placement);
	if (!Placed.IsSet())
	{
		// NOT SILENT: every other refusal in this function logs on the way out, and an
		// accepted placement that URoadNetwork::PlaceEntity itself then refuses (a dead
		// Definition slipping past ResolveStandDefinitionFor, say) is exactly the kind of
		// "the tool does nothing" report this project's CLAUDE.md warns is expensive to debug
		// blind.
		UE_LOG(LogRoadMesh, Warning,
			TEXT("PlaceStandInPlot refused: URoadNetwork::PlaceEntity refused an accepted "
				 "Code %s stand - report this as a bug."), IcaoCode::ToLetter(*Letter));
		return INDEX_NONE;
	}

	CommitPurchase(Edit, Quote);

	UE_LOG(LogRoadMesh, Log, TEXT("PlaceStandInPlot: Code %s stand, %.0f x %.0f m"),
		IcaoCode::ToLetter(*Letter),
		StandBox::WidthOf(Wound) / 100.0, StandBox::DepthOf(Wound) / 100.0);

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
	const TCHAR* Label = (Entity != nullptr && Entity->IsDepot())
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

	const TArray<FEntityInstance>& Entities = Network->GetEntities();

	// A STAND BY ITS STOP MARK, within the pick radius - its own position rather than any
	// anchor: an anchor is where a vehicle parks, the stand is the thing being pointed at.
	// FIRST, because it is the smaller target, the precedent FSelectTool sets for aircraft
	// over stands.
	//
	// A PLOT NEVER BY ITS POSITION. That is the gate midpoint, and until 2026-09-22 it was the
	// only way to select or remove a depot: a small click at the gate, with the whole plot and
	// its buildings dead to the cursor. Mapped out of reach here rather than filtered after.
	//
	// SINCE DRAWN STANDS, EVERY STAND IS PLOTTED TOO (placement and EnsureStandOutlines both
	// give one its box), so this radius pick now finds only a stand WITHOUT an outline - the
	// ground pick below takes every plotted one, stands and depots alike, by the ground the
	// player can see. Kept, not deleted, for the window a legacy stand has none: between a
	// load that has not migrated it yet and the rebind that does (the "gate cannot win the
	// radius pick over a stand" this used to justify it by no longer arises - no plotted
	// entity is in this pick at all).
	const int32 Stand = RoadSlot::NearestAlive<FEntityInstance>(Entities, Where, Radius,
		[](const FEntityInstance& Entity)
		{
			return Entity.IsPlotted() ? FVector2D(TNumericLimits<double>::Max()) : Entity.Position;
		});
	if (Stand != INDEX_NONE)
	{
		return Stand;
	}

	// A PLOT BY ITS GROUND: anywhere inside the outline the player drew, which is also
	// everywhere its buildings stand - PlotYard keeps every module inside the plot.
	for (int32 Index = 0; Index < Entities.Num(); ++Index)
	{
		const FEntityInstance& Entity = Entities[Index];
		if (Entity.bAlive && Entity.IsPlotted() && RoadGeom::PointInPolygon(Entity.Outline, Where))
		{
			return Index;
		}
	}
	return INDEX_NONE;
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
	// AdoptNetwork (#299) is the same swap-and-notify tail Travel and the two RevertEdit
	// sites use; HideGhost above already ran once, and AdoptNetwork's own call is harmless
	// repeated.
	AdoptNetwork(*NewObject<URoadNetwork>(&Owner));
}

FRoutePlan URoadEditFacade::FindRoute(
	FGuidelineNodeId Start, FGuidelineNodeId Goal, ETraversalClass Class, double Wingspan,
	ERouteErrand Errand) const
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
	Query.Errand = Errand;
	Query.Policy = FRoutePolicy::For(Errand);
	Query.AvoidRunways = Query.Policy.Avoidance;

	// THE ERRAND DECIDES, NOT THE CLASS. This read "vehicles always route with the table;
	// aircraft never" and branched on ETraversalClass - the ground-traffic spec's §4 rule,
	// implemented locally here and at three other sites. It was never quite true: a fuel
	// truck choosing a depot must NOT route with the table, or the winner flickers between
	// ticks (FuelService.cpp's own comment). The axis that survives every site is whether
	// the route may still change, and that is what the errand names.
	//
	// An aircraft's route is still fixed at clearance and stays fixed: PlayerIssued's row is
	// EOccupancyUse::Never, and the only thing that may change an aircraft's route afterwards
	// is UGroundTraffic::ReplanAt - a deadlock, or the graph itself changing.
	//
	// QueryingAgent stays 0: nothing has been dispatched yet, so there is no agent whose own
	// claims should be discounted from the cost.
	// TrafficModelProvider, not Actor().GetTraffic()->GetModel() directly: this class must
	// not reach past Network/History for anything else (#104, and see the class comment) -
	// ARoadNetworkActor wires the provider once, right after it creates Traffic, the same
	// way it wires OnChanged right after creating Facade.
	if (Query.Policy.Occupancy == EOccupancyUse::Required && TrafficModelProvider)
	{
		if (const UGroundTraffic* Model = TrafficModelProvider())
		{
			Query.Occupancy = &Model->GetOccupancy();
			Query.CongestionWeight = Model->Rules.CongestionWeight;
			Query.RunwayPenalty = Model->Rules.RunwayPenalty;
		}
	}

	return RouteSearch::Find(*Network, Query);
}
