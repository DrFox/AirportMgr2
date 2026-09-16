#include "Tool/PlotPlaceTool.h"

#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"
#include "Solve/PlotFit.h"
#include "Solve/RoadGeom.h"

#define LOCTEXT_NAMESPACE "Airside"

namespace
{
	/**
	 * Is this segment a service road - something a truck may drive on?
	 *
	 * ASKED OF THE PROFILE'S TRAVERSAL CLASS, not of an ERoadKind on the segment, because
	 * the segment does not carry one: the kind is a CHOICE the facade resolves into a
	 * profile, and the profile is what survives onto the graph. Snapping to a taxiway would
	 * let the player build a depot that dispatches onto one.
	 */
	bool IsServiceRoad(const URoadNetwork& Network, FRoadSegmentId Id)
	{
		const FRoadSegment* Segment = Network.GetSegment(Id);
		if (Segment == nullptr || Segment->Profile == nullptr)
		{
			return false;
		}

		// ASKED OF THE GUIDELINES THE PROFILE DECLARES, because "a truck may drive here" is
		// exactly what a GroundVehicle guideline means - and it is the same question
		// FAnchorLink asks of the graph when it joins a depot's pose. A cross-section with
		// no such line is a taxiway however it is labelled.
		for (const FProfileGuideline& Guideline : Segment->Profile->Guidelines)
		{
			if (Guideline.Class == ETraversalClass::GroundVehicle)
			{
				return true;
			}
		}
		return false;
	}

	/** A segment's straight-line ends, or false if either is dead. */
	bool SegmentEnds(const URoadNetwork& Network, FRoadSegmentId Id,
		FVector2D& OutA, FVector2D& OutB)
	{
		const FRoadSegment* Segment = Network.GetSegment(Id);
		if (Segment == nullptr)
		{
			return false;
		}

		const FRoadNode* A = Network.GetNode(Segment->A);
		const FRoadNode* B = Network.GetNode(Segment->B);
		if (A == nullptr || B == nullptr)
		{
			return false;
		}

		OutA = A->Position;
		OutB = B->Position;
		return true;
	}
}

FText FPlotPlaceTool::GetDisplayName() const
{
	return LOCTEXT("FuelDepotTool", "Fuel depot");
}

void FPlotPlaceTool::Frontage(FVector2D& OutA, FVector2D& OutB) const
{
	const FVector2D Far = Anchor + Along * (static_cast<double>(Width) * PlotFit::BayWidthUu);

	// THE GRID WANTS THE INTERIOR ON THE LEFT of A->B, so the two ends are ordered to make
	// that true rather than assumed to be in the right order. Along already points whichever
	// way the player dragged, so half the time it is not.
	const FVector2D Unit = (Far - Anchor).GetSafeNormal();
	if (FVector2D::DotProduct(RoadGeom::PerpCCW(Unit), Inward) >= 0.0)
	{
		OutA = Anchor;
		OutB = Far;
	}
	else
	{
		OutA = Far;
		OutB = Anchor;
	}
}

int32 FPlotPlaceTool::WidthAt(const FToolContext& Context) const
{
	// ROUNDED, NOT FLOORED, and this is the difference between a grid that feels magnetic
	// and one that feels grudging: floored, the cursor must travel a whole further bay
	// before the plot grows, so it always lags behind the hand.
	const double Reach = FVector2D::DotProduct(Context.Cursor - Anchor, Along);
	return FMath::Max(1, FMath::RoundToInt(FMath::Abs(Reach) / PlotFit::BayWidthUu));
}

int32 FPlotPlaceTool::DepthAt(const FToolContext& Context) const
{
	const double Reach = FVector2D::DotProduct(Context.Cursor - Anchor, Inward);
	return FMath::Max(1, FMath::RoundToInt(Reach / PlotFit::BayDepthUu));
}

void FPlotPlaceTool::ShownSize(const FToolContext& Context, int32& OutWidth, int32& OutDepth) const
{
	// ONE ROW UNTIL DEPTH IS REACHED, and not the Depth member, which still holds whatever
	// the LAST gesture locked - Stage returning to Idle does not reset it. Drawing that stale
	// depth is what made the second plot of a session ghost three rows deep while the bar
	// beside it said one.
	OutWidth = Stage == EPlotStage::Width ? WidthAt(Context) : Width;
	OutDepth = Stage == EPlotStage::Depth ? DepthAt(Context)
		: (Stage == EPlotStage::Width ? 1 : Depth);
}

void FPlotPlaceTool::OnClick(const FToolContext& Context)
{
	const URoadNetwork* Network = Context.Network();
	if (Context.Target == nullptr || Network == nullptr)
	{
		return;
	}

	switch (Stage)
	{
	case EPlotStage::Idle:
	{
		// THE DRIVER HAS ALREADY SNAPPED. Both drivers resolve FToolContext::Snap from the
		// one per-airport FRoadSnapSettings before a tool sees it, so running a chain here
		// would make the same click behave differently in PIE and in the editor mode -
		// which is the bug that struct's own comment records being fixed.
		if (Context.Snap.Kind != ERoadSnapKind::Segment
			|| !IsServiceRoad(*Network, Context.Snap.Segment))
		{
			return;
		}

		FVector2D RoadA = FVector2D::ZeroVector;
		FVector2D RoadB = FVector2D::ZeroVector;
		if (!SegmentEnds(*Network, Context.Snap.Segment, RoadA, RoadB))
		{
			return;
		}

		const FVector2D Span = RoadB - RoadA;
		const double Length = Span.Size();
		if (Length <= 0.0)
		{
			return;
		}

		Along = Span / Length;

		// ANCHORED ON THE ROAD'S OWN BAY GRID, measured from the segment's A end. Quantising
		// per SEGMENT rather than globally means two plots on one segment sit flush and a
		// plot never straddles a junction - see the design doc's open question 1.
		const double AlongRoad = FMath::Clamp(Context.Snap.SegmentT, 0.0, 1.0) * Length;
		const double Snapped = FMath::RoundToDouble(AlongRoad / PlotFit::BayWidthUu)
			* PlotFit::BayWidthUu;
		Anchor = RoadA + Along * FMath::Clamp(Snapped, 0.0, Length);

		// WHICH SIDE THE CURSOR IS ON, not a rule. A depot goes on the side of the road the
		// player is pointing at; the alternative is a fixed side that is wrong half the time
		// and cannot be argued with.
		const FVector2D Left = RoadGeom::PerpCCW(Along);
		const double Side = FVector2D::DotProduct(Context.Cursor - Anchor, Left);
		Inward = Side >= 0.0 ? Left : -Left;

		Stage = EPlotStage::Width;
		return;
	}

	case EPlotStage::Width:
	{
		Width = WidthAt(Context);

		// DRAGGED BACK PAST THE ANCHOR RUNS THE PLOT THE OTHER WAY, rather than refusing or
		// collapsing to nothing. A player who anchors and then changes their mind about
		// which way to go should not have to cancel and start again.
		if (FVector2D::DotProduct(Context.Cursor - Anchor, Along) < 0.0)
		{
			Along = -Along;

			// INWARD IS NOT FLIPPED WITH IT. It is an absolute world direction - the side of
			// the road the cursor was on when the plot was anchored - and that side does not
			// change because the player dragged west instead of east. Flipping it here put
			// the whole plot across the road, which Airside.Tool.PlotWidthRunsBothWays
			// caught: the plot ran the right way and sat on the wrong side.
			//
			// Nothing downstream needs Inward to be Along's left normal. Frontage() orders
			// its two ends to put the interior on the left, which is where that contract is
			// actually met.
		}

		Stage = EPlotStage::Depth;
		return;
	}

	case EPlotStage::Depth:
		Depth = DepthAt(Context);
		Stage = EPlotStage::Confirm;
		return;

	case EPlotStage::Confirm:
		// NOTHING. The gesture is locked and the Build button is the only way on - a click
		// that committed here would delete the review beat the staging exists for.
		return;
	}
}

void FPlotPlaceTool::OnCancel(const FToolContext& Context)
{
	// ONE STAGE AT A TIME, the same answer the outline tool gives a misclick: binning the
	// whole gesture is a harsher response than the mistake deserves.
	switch (Stage)
	{
	case EPlotStage::Confirm: Stage = EPlotStage::Depth; return;
	case EPlotStage::Depth:   Stage = EPlotStage::Width; return;
	case EPlotStage::Width:   Stage = EPlotStage::Idle;  return;
	case EPlotStage::Idle:    return;
	}
}

void FPlotPlaceTool::OnCommit(const FToolContext& Context)
{
	// EVERY STAGE BUT THE LAST IGNORES THIS. Build is a widget, not a stage of the gesture,
	// so it is reachable whenever the bar is on screen - committing from Width would build a
	// plot with no depth at all.
	if (Stage != EPlotStage::Confirm || Context.Target == nullptr)
	{
		return;
	}

	FVector2D FrontA = FVector2D::ZeroVector;
	FVector2D FrontB = FVector2D::ZeroVector;
	Frontage(FrontA, FrontB);

	const TArray<FVector2D> Outline = PlotFit::GridOutline(FrontA, FrontB, Width, Depth);
	if (Outline.Num() < 3)
	{
		return;
	}

	Context.Target->PlaceEntityInPlot(Outline, FrontA, FrontB, Modules, Kind);

	// BACK TO IDLE, ready for the next one. A tool that stayed in Confirm would let the
	// player press Build twice and get two depots stacked on one plot.
	Stage = EPlotStage::Idle;
}

void FPlotPlaceTool::OnDeactivate(const FToolContext& Context)
{
	// Discarded outright, like the outline tool's part-drawn shape: it exists only on this
	// object, so nothing in the model has to be cleaned up.
	Stage = EPlotStage::Idle;
}

void FPlotPlaceTool::BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const
{
	const URoadNetwork* Network = Context.Network();
	if (Network == nullptr)
	{
		return;
	}

	if (Stage == EPlotStage::Idle)
	{
		// The anchors the player could take, so the grid is visible before it is committed
		// to. Snap style: these are what the gesture would attach to.
		if (Context.Snap.Kind == ERoadSnapKind::Segment
			&& IsServiceRoad(*Network, Context.Snap.Segment))
		{
			FVector2D RoadA = FVector2D::ZeroVector;
			FVector2D RoadB = FVector2D::ZeroVector;
			if (SegmentEnds(*Network, Context.Snap.Segment, RoadA, RoadB))
			{
				const FVector2D Span = RoadB - RoadA;
				const double Length = Span.Size();
				const FVector2D Unit = Span.GetSafeNormal();
				const int32 Count = FMath::FloorToInt(Length / PlotFit::BayWidthUu);
				for (int32 I = 0; I <= Count; ++I)
				{
					Sink.Marker(RoadA + Unit * (I * PlotFit::BayWidthUu), EPreviewStyle::Snap);
				}
			}
		}
		else
		{
			Sink.Label(Context.Cursor, TEXT("move near a service road"), EPreviewStyle::Refused);
		}
		return;
	}

	int32 ShownWidth = 0;
	int32 ShownDepth = 0;
	ShownSize(Context, ShownWidth, ShownDepth);

	FVector2D FrontA = FVector2D::ZeroVector;
	FVector2D FrontB = FVector2D::ZeroVector;
	{
		// Frontage() uses the LOCKED width; while dragging, the shown one is what matters.
		const FVector2D Far = Anchor + Along * (static_cast<double>(ShownWidth) * PlotFit::BayWidthUu);
		const FVector2D Unit = (Far - Anchor).GetSafeNormal();
		const bool bLeft = FVector2D::DotProduct(RoadGeom::PerpCCW(Unit), Inward) >= 0.0;
		FrontA = bLeft ? Anchor : Far;
		FrontB = bLeft ? Far : Anchor;
	}

	Sink.Polygon(PlotFit::GridOutline(FrontA, FrontB, ShownWidth, ShownDepth),
		EPreviewStyle::Pending);

	const PlotFit::FPlotGrid Grid = PlotFit::BuildGrid(FrontA, FrontB, ShownWidth, ShownDepth);

	// WHICH WAY EACH FILLED BAY FACES. Not decoration: +X faces AWAY from the road and the
	// truck leaves out of the back, so a depot built facing the wrong way looks perfectly
	// correct until something drives - the most expensive class of mistake this project has.
	const int32 Placed = FMath::Min(ShownWidth, Modules.Num());
	for (int32 I = 0; I < Placed && I < Grid.Slots.Num(); ++I)
	{
		const PlotFit::FPlotBay& Slot = Grid.Slots[I];
		Sink.CrossMark(Slot.Centre,
			FVector2D(FMath::Cos(Slot.Heading), FMath::Sin(Slot.Heading)),
			EPreviewStyle::Pending);
	}

	// The slots behind row 1, marked so the depth step's payoff is visible while it is being
	// chosen rather than only after Build.
	for (int32 I = ShownWidth; I < Grid.Slots.Num(); ++I)
	{
		Sink.Marker(Grid.Slots[I].Centre, EPreviewStyle::Heal);
	}
}

void FPlotPlaceTool::BuildReadout(const FToolContext& Context, IToolReadoutSink& Sink) const
{
	const URoadNetwork* Network = Context.Network();

	if (Stage == EPlotStage::Idle)
	{
		if (Network == nullptr || Context.Snap.Kind != ERoadSnapKind::Segment
			|| !IsServiceRoad(*Network, Context.Snap.Segment))
		{
			Sink.Warning(TEXT("Move near a service road"));
		}
		Sink.Committable(false);
		return;
	}

	int32 ShownWidth = 0;
	int32 ShownDepth = 0;
	ShownSize(Context, ShownWidth, ShownDepth);

	Sink.Fact(TEXT("Bays"), FString::FromInt(ShownWidth));
	Sink.Fact(TEXT("Rows"), FString::FromInt(ShownDepth));

	// WHAT YOU GET against what you asked for. A width of two silently dropping the pump is
	// exactly the kind of thing a player discovers after paying for it.
	const int32 Placed = FMath::Min(ShownWidth, Modules.Num());
	Sink.Fact(TEXT("Modules"), FString::Printf(TEXT("%d of %d"), Placed, Modules.Num()));
	Sink.Fact(TEXT("Expansion slots"),
		FString::FromInt(ShownWidth * ShownDepth - Placed));

	if (Stage == EPlotStage::Confirm && ShownDepth <= 1)
	{
		// The direct analogue of Manor Lords' "Plots without Extension Space". A warning and
		// never a refusal: a one-row depot works perfectly well and may be exactly what the
		// player wants.
		Sink.Warning(TEXT("No room to grow"));
	}

	Sink.Committable(Stage == EPlotStage::Confirm);
}

#undef LOCTEXT_NAMESPACE
