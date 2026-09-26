#include "Tool/SelectTool.h"

#include "AirsideLog.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadAgent.h"
#include "Model/RoadNetwork.h"

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
	default:
		return false;
	}
}

void FSelectTool::OnClick(const FToolContext& Context)
{
	SelectionRef = Context.Selection;
	if (Context.Selection == nullptr)
	{
		UE_LOG(LogAirside, Warning, TEXT("Select: click with no selection to write to - the driver built a context without one."));
		return;
	}
	FSelection& Sel = *Context.Selection;

	if (Context.HoverAgent != 0)
	{
		Sel.Kind = ESelectionKind::Aircraft;
		Sel.Id = Context.HoverAgent;
	}
	else if (Context.Target != nullptr)
	{
		const int32 Stand = Context.Target->FindEntityAt(Context.Cursor, Context.SnapRadius);
		if (Stand != INDEX_NONE)
		{
			Sel.Kind = ESelectionKind::Stand;
			Sel.Id = Stand;
		}
		else
		{
			// A click on nothing deselects, as it does in every Cities-style game: the
			// panel closing is how the player knows the click registered.
			Sel.Clear();
		}
	}
	UE_LOG(LogAirside, Log, TEXT("Select: %s %d"),
		Sel.Kind == ESelectionKind::Aircraft ? TEXT("aircraft") : Sel.Kind == ESelectionKind::Stand ? TEXT("stand") : TEXT("nothing"),
		Sel.Id);
}

void FSelectTool::OnCancel(const FToolContext& Context)
{
	SelectionRef = Context.Selection;
	if (Context.Selection != nullptr)
	{
		Context.Selection->Clear();
	}
}

void FSelectTool::Tick(const FToolContext& Context)
{
	SelectionRef = Context.Selection;
	if (Context.Selection == nullptr || !Context.Selection->IsSet())
	{
		return;
	}
	// Polled, not subscribed: a tool has no delegate lifetime to manage, and this runs
	// every frame anyway for the preview. A selected aircraft that has flown away or a
	// stand that was deleted under another tool must not leave the panel showing a ghost.
	FVector2D Unused;
	if (!PositionOf(Context, Context.Selection->Kind, Context.Selection->Id, Unused))
	{
		UE_LOG(LogAirside, Log, TEXT("Select: selection %d no longer exists; cleared."), Context.Selection->Id);
		Context.Selection->Clear();
	}
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
	}

	if (Context.Selection != nullptr && Context.Selection->IsSet()
		&& PositionOf(Context, Context.Selection->Kind, Context.Selection->Id, At))
	{
		if (Context.Selection->Kind == ESelectionKind::Stand && Context.Network() != nullptr)
		{
			DrawEntity(*Context.Network(), Context.Selection->Id, EPreviewStyle::Selected, Sink);
		}
		else
		{
			Sink.Marker(At, EPreviewStyle::Selected);
		}

		// The selected aircraft's remaining route, in the style the Route tool used: the one
		// useful picture that tool drew, kept.
		if (Context.Selection->Kind == ESelectionKind::Aircraft)
		{
			const UGroundTraffic* Traffic = Context.Target->GetGroundTraffic();
			if (Traffic != nullptr)
			{
				// In RUNS, each in its meaning's style: cyan forward, amber where it backs up
				// (spec 2026-09-26 §5).
				for (const FRouteRun& Run : Traffic->RemainingRouteRuns(Context.Selection->Id))
				{
					Sink.Polyline(Run.Points, Run.bReverse ? EPreviewStyle::ReverseRoute : EPreviewStyle::Route);
				}
			}
		}
	}
}

#undef LOCTEXT_NAMESPACE
