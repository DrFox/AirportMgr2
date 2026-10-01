#include "Tool/SelectTool.h"

#include "AirsideLog.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadAgent.h"
#include "Model/RoadNetwork.h"
#include "Model/RunwayQuery.h"
#include "Model/TaxiwayStrip.h"
#include "Solve/RoadGeom.h"

#define LOCTEXT_NAMESPACE "Airside"

namespace
{
	/**
	 * One entity drawn in a selection style: a plot as its whole outline, a stand as a ring
	 * at its stop mark.
	 *
	 * THE OUTLINE, NOT A RING AT THE GATE, since 2026-09-22: a plot is picked by its ground
	 * (URoadEditFacade::FindEntityAt), so the highlight has to be that ground, or the player
	 * clicks a building and sees a dot light up metres away at the road.
	 */
	void DrawEntity(const URoadNetwork& Network, int32 Index, EPreviewStyle Style,
		IToolPreviewSink& Sink)
	{
		if (!Network.GetEntities().IsValidIndex(Index))
		{
			return;
		}
		const FEntityInstance& Entity = Network.GetEntities()[Index];
		if (Entity.IsPlotted())
		{
			Sink.Polygon(Entity.Outline, Style);
		}
		else
		{
			Sink.Marker(Entity.Position, Style);
		}
	}

	/**
	 * A runway drawn as its whole centreline, threshold IN USE to far end - the strip, not the
	 * one segment clicked, because the facts (and so the card) are the strip's.
	 */
	void DrawRunway(const URoadNetwork& Network, int32 SegmentIndex, EPreviewStyle Style, IToolPreviewSink& Sink)
	{
		const FRoadSegment* Segment = Network.GetSegment(Network.SegmentIdAt(SegmentIndex));
		const FRoadNode* A = Segment != nullptr ? Network.GetNode(Segment->A) : nullptr;
		FRunwayEnd End;
		if (A != nullptr && Network.InUseRunwayAt(A->Position, End))
		{
			Sink.Polyline({ End.Threshold, End.FarEnd() }, Style);
		}
	}
}

namespace
{
	/**
	 * The taxiway whose PAVEMENT holds Point (TaxiwayStrip::FootprintOf, the one description of
	 * a segment's ground), or unset. Taxiways only - HasStrip: a road or a runway has no taxiway
	 * card. Every live segment, linearly (N was 34 on M_Test, 2026-09-28), on a click or a hover.
	 */
	FRoadSegmentId TaxiwaySegmentAt(const URoadNetwork& Network, const FVector2D& Point)
	{
		for (int32 Index = 0; Index < Network.GetSegments().Num(); ++Index)
		{
			const FRoadSegmentId Id = Network.SegmentIdAt(Index);
			TaxiwayStrip::FSegmentShape Shape;
			if (Id.IsSet() && TaxiwayStrip::HasStrip(Network, Id) && TaxiwayStrip::ShapeOf(Network, Id, Shape)
				&& RoadGeom::PointInPolygon(TaxiwayStrip::FootprintOf(Shape), Point))
			{
				return Id;
			}
		}
		return FRoadSegmentId();
	}

	/** A taxiway drawn as its pavement's outline - the ground the card describes. */
	void DrawTaxiway(const URoadNetwork& Network, int32 SegmentIndex, EPreviewStyle Style, IToolPreviewSink& Sink)
	{
		TaxiwayStrip::FSegmentShape Shape;
		if (TaxiwayStrip::ShapeOf(Network, Network.SegmentIdAt(SegmentIndex), Shape))
		{
			Sink.Polygon(TaxiwayStrip::FootprintOf(Shape), Style);
		}
	}
}

FText FSelectTool::GetDisplayName() const
{
	return LOCTEXT("SelectTool", "Select");
}

bool FSelectTool::PositionOf(const FToolContext& Context, ESelectionKind Kind, int32 Id, FVector2D& Out)
{
	if (Context.Target == nullptr)
	{
		return false;
	}
	switch (Kind)
	{
	case ESelectionKind::Aircraft:
	{
		const UGroundTraffic* Traffic = Context.Target->GetGroundTraffic();
		const FRoadAgent* Agent = Traffic != nullptr ? Traffic->FindAgent(Id) : nullptr;
		if (Agent == nullptr)
		{
			return false;
		}
		Out = Agent->GroundPosition();
		return true;
	}
	case ESelectionKind::Stand:
	{
		const URoadNetwork* Network = Context.Target->GetNetwork();
		if (Network == nullptr || !Network->GetEntities().IsValidIndex(Id) || !Network->GetEntities()[Id].bAlive)
		{
			return false;
		}
		Out = Network->GetEntities()[Id].Position;
		return true;
	}
	case ESelectionKind::Runway:
	{
		const URoadNetwork* Network = Context.Target->GetNetwork();
		const FRoadSegmentId Segment = Network != nullptr ? Network->SegmentIdAt(Id) : FRoadSegmentId();
		const FRoadSegment* Found = Segment.IsSet() ? Network->GetSegment(Segment) : nullptr;
		if (Found == nullptr || !Network->IsRunwaySegment(Segment))
		{
			return false;
		}
		Out = (Network->GetNode(Found->A)->Position + Network->GetNode(Found->B)->Position) * 0.5;
		return true;
	}
	case ESelectionKind::Taxiway:
	{
		// Runway's shape, for a live taxiway piece - a split kills it, and Tick clears the card.
		const URoadNetwork* Network = Context.Target->GetNetwork();
		const FRoadSegmentId Segment = Network != nullptr ? Network->SegmentIdAt(Id) : FRoadSegmentId();
		const FRoadSegment* Found = Segment.IsSet() ? Network->GetSegment(Segment) : nullptr;
		if (Found == nullptr || !TaxiwayStrip::HasStrip(*Network, Segment))
		{
			return false;
		}
		Out = (Network->GetNode(Found->A)->Position + Network->GetNode(Found->B)->Position) * 0.5;
		return true;
	}
	default:
		return false;
	}
}

void FSelectTool::OnClick(const FToolContext& Context)
{
	SelectionRef = Context.CurrentSelection();
	if (Context.CurrentSelection() == nullptr)
	{
		UE_LOG(LogAirside, Warning, TEXT("Select: click with no selection to write to - the driver built a context without one."));
		return;
	}
	// WRITTEN THROUGH THE CONTEXT'S DOOR (#446), once, with the slot's generation recorded by MakeSelection: a click that lands on what is
	// already selected is not a change and announces nothing.
	const URoadNetwork* Network = Context.Network();

	if (Context.HoverAgent != 0)
	{
		Context.SetSelection(MakeSelection(Network, ESelectionKind::Aircraft, Context.HoverAgent));
	}
	else if (Context.Target != nullptr)
	{
		const int32 Stand = Context.Target->FindEntityAt(Context.Cursor, Context.SnapRadius);
		if (Stand != INDEX_NONE)
		{
			Context.SetSelection(MakeSelection(Network, ESelectionKind::Stand, Stand));
		}
		else if (const FRoadSegmentId Runway = Context.Network() != nullptr
			? RunwayQuery::RunwaySegmentAt(*Context.Network(), Context.Cursor) : FRoadSegmentId(); Runway.IsSet())
		{
			// LAST, after aircraft and stand: a runway is under most of the airport's clicks
			// that matter, and an aircraft rolling on it must still be the thing picked.
			Context.SetSelection(MakeSelection(Network, ESelectionKind::Runway, Runway.Index));
		}
		else if (const FRoadSegmentId Taxiway = Context.Network() != nullptr
			? TaxiwaySegmentAt(*Context.Network(), Context.Cursor) : FRoadSegmentId(); Taxiway.IsSet())
		{
			// LAST OF ALL (strip stage 6): a taxiway is under most of an airport's clicks, and a
			// stand, a runway or an aircraft on it must still be the thing picked.
			Context.SetSelection(MakeSelection(Network, ESelectionKind::Taxiway, Taxiway.Index));
		}
		else
		{
			// A click on nothing deselects, as it does in every Cities-style game: the
			// panel closing is how the player knows the click registered.
			Context.ClearSelection();
		}
	}
	const FSelection& Sel = *Context.CurrentSelection();
	UE_LOG(LogAirside, Log, TEXT("Select: %s %d"),
		Sel.Kind == ESelectionKind::Aircraft ? TEXT("aircraft") : Sel.Kind == ESelectionKind::Stand ? TEXT("stand")
			: Sel.Kind == ESelectionKind::Runway ? TEXT("runway segment")
			: Sel.Kind == ESelectionKind::Taxiway ? TEXT("taxiway segment") : TEXT("nothing"),
		Sel.Id);
}

void FSelectTool::OnCancel(const FToolContext& Context)
{
	SelectionRef = Context.CurrentSelection();
	Context.ClearSelection();
}

void FSelectTool::Tick(const FToolContext& Context)
{
	SelectionRef = Context.CurrentSelection();
	if (Context.CurrentSelection() == nullptr || !Context.CurrentSelection()->IsSet())
	{
		return;
	}
	// Polled, not subscribed: a tool has no delegate lifetime to manage, and this runs
	// every frame anyway for the preview. A selected aircraft that has flown away or a
	// stand that was deleted under another tool must not leave the panel showing a ghost.
	// STALE BEFORE GONE (#446): a slot whose item was removed and whose index was reused is ALIVE, so PositionOf finds it and the
	// selection would quietly retarget; the generation recorded at the pick is what tells the two items apart.
	FVector2D Unused;
	if (IsStale(Context.Network(), *Context.CurrentSelection()) || !PositionOf(Context, Context.CurrentSelection()->Kind, Context.CurrentSelection()->Id, Unused))
	{
		UE_LOG(LogAirside, Log, TEXT("Select: selection %d no longer exists; cleared."), Context.CurrentSelection()->Id);
		Context.ClearSelection();
	}
}

FSelection FSelectTool::MakeSelection(const URoadNetwork* Network, ESelectionKind Kind, int32 Id)
{
	FSelection Out;
	Out.Kind = Kind;
	Out.Id = Kind == ESelectionKind::None ? 0 : Id;
	if (Network == nullptr)
	{
		return Out;
	}
	switch (Kind)
	{
	case ESelectionKind::Stand:
		Out.Generation = Network->EntityIdAt(Id).Generation;
		break;
	case ESelectionKind::Runway:
	case ESelectionKind::Taxiway:
		Out.Generation = Network->SegmentIdAt(Id).Generation;
		break;
	default:
		break;
	}
	return Out;
}

bool FSelectTool::IsStale(const URoadNetwork* Network, const FSelection& Selection)
{
	if (Network == nullptr)
	{
		return false;
	}
	int32 LiveGeneration = 0;
	bool bLive = false;
	switch (Selection.Kind)
	{
	case ESelectionKind::Stand:
	{
		const FEntityInstanceId Id = Network->EntityIdAt(Selection.Id);
		bLive = Id.IsSet();
		LiveGeneration = Id.Generation;
		break;
	}
	case ESelectionKind::Runway:
	case ESelectionKind::Taxiway:
	{
		const FRoadSegmentId Id = Network->SegmentIdAt(Selection.Id);
		bLive = Id.IsSet();
		LiveGeneration = Id.Generation;
		break;
	}
	default:
		return false;
	}
	return !bLive || (Selection.Generation != 0 && Selection.Generation != LiveGeneration);
}

void FSelectTool::BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const
{
	FVector2D At;
	if (Context.HoverAgent != 0 && PositionOf(Context, ESelectionKind::Aircraft, Context.HoverAgent, At))
	{
		Sink.Marker(At, EPreviewStyle::Hover);
	}
	else if (Context.Target != nullptr)
	{
		const int32 Stand = Context.Target->FindEntityAt(Context.Cursor, Context.SnapRadius);
		if (Stand != INDEX_NONE && PositionOf(Context, ESelectionKind::Stand, Stand, At)
			&& Context.Network() != nullptr)
		{
			DrawEntity(*Context.Network(), Stand, EPreviewStyle::Hover, Sink);
		}
		else if (Stand == INDEX_NONE && Context.Network() != nullptr)
		{
			const FRoadSegmentId Runway = RunwayQuery::RunwaySegmentAt(*Context.Network(), Context.Cursor);
			if (Runway.IsSet())
			{
				DrawRunway(*Context.Network(), Runway.Index, EPreviewStyle::Hover, Sink);
			}
			else if (const FRoadSegmentId Taxiway = TaxiwaySegmentAt(*Context.Network(), Context.Cursor); Taxiway.IsSet())
			{
				DrawTaxiway(*Context.Network(), Taxiway.Index, EPreviewStyle::Hover, Sink);
			}
		}
	}

	if (Context.CurrentSelection() != nullptr && Context.CurrentSelection()->IsSet()
		&& PositionOf(Context, Context.CurrentSelection()->Kind, Context.CurrentSelection()->Id, At))
	{
		if (Context.CurrentSelection()->Kind == ESelectionKind::Stand && Context.Network() != nullptr)
		{
			DrawEntity(*Context.Network(), Context.CurrentSelection()->Id, EPreviewStyle::Selected, Sink);
		}
		else if (Context.CurrentSelection()->Kind == ESelectionKind::Runway && Context.Network() != nullptr)
		{
			DrawRunway(*Context.Network(), Context.CurrentSelection()->Id, EPreviewStyle::Selected, Sink);
		}
		else if (Context.CurrentSelection()->Kind == ESelectionKind::Taxiway && Context.Network() != nullptr)
		{
			DrawTaxiway(*Context.Network(), Context.CurrentSelection()->Id, EPreviewStyle::Selected, Sink);
		}
		else
		{
			Sink.Marker(At, EPreviewStyle::Selected);
		}

		// The selected aircraft's remaining route, in the style the Route tool used: the one
		// useful picture that tool drew, kept.
		if (Context.CurrentSelection()->Kind == ESelectionKind::Aircraft)
		{
			const UGroundTraffic* Traffic = Context.Target->GetGroundTraffic();
			if (Traffic != nullptr)
			{
				// In RUNS, each in its meaning's style: cyan forward, amber where it backs up
				// (spec 2026-09-26 §5).
				for (const FRouteRun& Run : Traffic->RemainingRouteRuns(Context.CurrentSelection()->Id))
				{
					Sink.Polyline(Run.Points, Run.bReverse ? EPreviewStyle::ReverseRoute : EPreviewStyle::Route);
				}
			}
		}
	}
}

#undef LOCTEXT_NAMESPACE
