#include "Tool/PlotPlaceTool.h"

#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"
#include "Build/DepotKit.h"
#include "Solve/PlotFit.h"
#include "Solve/PlotYard.h"
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

	/**
	 * How far off the centreline the plot's frontage sits, uu.
	 *
	 * A SEGMENT'S ENDS ARE NODE POSITIONS, so everything derived from them is on the road's
	 * CENTRELINE - and a plot anchored there is built over half the carriageway, which is
	 * what PIE showed on 2026-09-16. The frontage belongs against the kerb.
	 *
	 * THE SIDE'S OWN HALF WIDTH, not the larger of the two: a profile may be asymmetric
	 * (URoadProfile::CentrelineOffset), and taking the max would leave a strip of unbuilt
	 * ground on the narrow side that nothing explains.
	 */
	double KerbOffset(const URoadNetwork& Network, FRoadSegmentId Id, bool bLeftOfSegment)
	{
		const FRoadSegment* Segment = Network.GetSegment(Id);
		if (Segment == nullptr || Segment->Profile == nullptr)
		{
			return 0.0;
		}
		return bLeftOfSegment ? Segment->Profile->GetHalfWidthLeft()
			: Segment->Profile->GetHalfWidthRight();
	}

	using PlotGesture::MinFrontageUu;
	using PlotGesture::FrontageStepUu;

	/**
	 * A frontage length quantised to the plot's own steps.
	 *
	 * ROUNDED, NOT FLOORED, which is the difference between a grid that feels magnetic and
	 * one that feels grudging: floored, the cursor must travel a whole further step before
	 * the plot grows, so it always lags behind the hand.
	 *
	 * NOT PlotFit::BayWidthUu. That is 4 m because a SHED is 4 m, and it used to mean the
	 * plot's step as well - one number doing two jobs, which is how a plot could be drawn
	 * narrower than anything that could stand in it.
	 */
	double QuantisedFrontage(double Raw)
	{
		if (Raw <= MinFrontageUu)
		{
			return MinFrontageUu;
		}
		const double Steps = FMath::RoundToDouble((Raw - MinFrontageUu) / FrontageStepUu);
		return MinFrontageUu + Steps * FrontageStepUu;
	}

	/**
	 * Which anchor point a click would take on this road, and where it stands.
	 *
	 * ONE RULE, TWO CALLERS: OnClick takes it, and the Idle preview draws it heavier than its
	 * neighbours so the snap is VISIBLE before the click rather than discovered after. A
	 * second copy of this arithmetic is a dot that lights up in one place and anchors in
	 * another.
	 */
	int32 AnchorIndexAt(double SegmentT, double Length)
	{
		// FrontageStepUu, not BayWidthUu: an anchor on a 4 m grid under a frontage growing in
		// 5 m steps would let two plots drawn side by side never sit flush, which is the
		// entire reason the frontage has a quantum.
		const double AlongRoad = FMath::Clamp(SegmentT, 0.0, 1.0) * Length;
		const int32 Count = FMath::FloorToInt(Length / FrontageStepUu);
		return FMath::Clamp(FMath::RoundToInt(AlongRoad / FrontageStepUu), 0, Count);
	}

	/**
	 * How far along the road anchor N stands, uu.
	 *
	 * THE MULTIPLICATION LIVES HERE, once. Both callers used to do it themselves against
	 * PlotFit::BayWidthUu, so moving the index to the 5 m step left them multiplying by 4 m -
	 * the anchor landed 2 km from the cursor and three tests failed for reasons that looked
	 * nothing like the cause. An index and its stride are one fact.
	 */
	double AnchorOffset(int32 Index)
	{
		return Index * FrontageStepUu;
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

int32 FPlotPlaceTool::PinnedCount() const
{
	// READ OFF THE STAGE so the two cannot disagree. A separate counter would be a second
	// thing to keep in step with the stage machine, and the readout's "N/4" would be free to
	// drift from what the gesture is actually doing.
	switch (Stage)
	{
	case EPlotStage::Idle:     return 0;
	case EPlotStage::Frontage: return 1;
	case EPlotStage::CornerA:  return 2;
	case EPlotStage::CornerB:  return 3;
	case EPlotStage::Confirm:  return 4;
	}
	return 0;
}

void FPlotPlaceTool::Quad(const FToolContext& Context, TArray<FVector2D>& OutQuad) const
{
	OutQuad.Reset();

	const int32 Pinned = PinnedCount();
	if (Pinned < 1)
	{
		return;
	}

	// Corner 0 is the anchor, pinned by the first click and never moving after.
	OutQuad.Add(Corners[0]);

	// Corner 1: the far end of the frontage. Along the road, quantised, and it may run EITHER
	// WAY - a player who anchors and then changes their mind about direction should not have
	// to cancel and start again.
	FVector2D Far = Corners[1];
	if (Pinned == 1)
	{
		const double Reach = FVector2D::DotProduct(Context.Cursor - Corners[0], Along);
		const double Sign = Reach < 0.0 ? -1.0 : 1.0;
		Far = Corners[0] + Along * (Sign * QuantisedFrontage(FMath::Abs(Reach)));
	}
	OutQuad.Add(Far);

	if (Pinned < 2)
	{
		return;
	}

	// Corners 2 and 3: the back pair, FREE IN THE PLANE. They rode the frontage's own normal
	// until 2026-09-17, which made every plot a trapezoid with perpendicular sides - the
	// player could set each corner's depth and never its position along the road.
	//
	// ONE CONSTRAINT SURVIVES: a corner may not fall BEHIND the frontage. The plot goes on
	// the side of the road the player chose at the anchor, and a corner across that line
	// would lay concrete on the carriageway. A cursor dragged back there slides onto the
	// frontage line rather than being refused, because refusing a drag mid-gesture gives the
	// player nothing to correct.
	auto InFront = [&](const FVector2D& Point)
	{
		const double Depth = FVector2D::DotProduct(Point - Corners[0], Inward);
		return Depth >= 0.0 ? Point : Point - Inward * Depth;
	};

	const FVector2D Back = Pinned == 2 ? InFront(Context.Cursor) : Corners[2];
	OutQuad.Add(Back);

	// UNTIL IT IS REACHED, THE NEAR CORNER COMPLETES A PARALLELOGRAM. Two pinned corners then
	// read as a finished shape the player adjusts, rather than one trailing off - and a
	// parallelogram is the honest completion now that the corners are free, where mirroring
	// the DEPTH would quietly snap the plot square and hide the freedom just gained.
	//
	// IT MUST NOT READ Corners[3] HERE: that entry is stale until the fourth click writes it,
	// and drawing a stale corner is how a ghost shows the PREVIOUS gesture's geometry - the
	// exact bug the old Depth member caused, which took a deliberate re-break to prove.
	FVector2D Near = Corners[0] + (Back - Far);
	if (Pinned == 3)
	{
		Near = InFront(Context.Cursor);
	}
	else if (Pinned >= 4)
	{
		Near = Corners[3];
	}
	OutQuad.Add(Near);
}

PlotYard::FYard FPlotPlaceTool::YardFor(TArrayView<const FVector2D> Outline) const
{
	if (Outline.Num() < 4)
	{
		return PlotYard::FYard();
	}

	TArray<PlotYard::FFootprint> Footprints;
	Footprints.Reserve(Modules.Num());
	for (const EDepotModule Module : Modules)
	{
		Footprints.Add(DepotFootprint(Module));
	}

	// The pose the facade will store for this depot, so DepotYardSeed gives the yard that
	// gets BUILT rather than one that merely resembles it - see Build/DepotKit.h.
	const FVector2D Pose = (Outline[0] + Outline[1]) * 0.5;

	return PlotYard::LayOut(Outline, Outline[0], Outline[1], Pose, Footprints,
		DepotYardSeed(Pose), DepotFootprint(EDepotModule::Tank));
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
		Corners[0] = RoadA + Along
			* AnchorOffset(AnchorIndexAt(Context.Snap.SegmentT, Length));

		// WHICH SIDE THE CURSOR IS ON, not a rule. A depot goes on the side of the road the
		// player is pointing at; the alternative is a fixed side that is wrong half the time
		// and cannot be argued with.
		const FVector2D Left = RoadGeom::PerpCCW(Along);
		const double Side = FVector2D::DotProduct(Context.Cursor - Corners[0], Left);
		Inward = Side >= 0.0 ? Left : -Left;

		// OFF THE CARRIAGEWAY, and only now that the side is known. Measured BEFORE this
		// step, because the side has to be read against the centreline the cursor was
		// judged from - offsetting first would tilt that test by half a road width.
		Corners[0] += Inward * KerbOffset(*Network, Context.Snap.Segment, Side >= 0.0);

		Stage = EPlotStage::Frontage;
		return;
	}

	case EPlotStage::Frontage:
	{
		// PINNED FROM WHAT WAS ON SCREEN, not recomputed. Quad() is what the ghost drew this
		// frame, so a click can only ever pin the shape the player was looking at.
		TArray<FVector2D> Shown;
		Quad(Context, Shown);
		if (Shown.Num() < 2)
		{
			return;
		}
		Corners[1] = Shown[1];
		Stage = EPlotStage::CornerA;
		return;
	}

	case EPlotStage::CornerA:
	{
		TArray<FVector2D> Shown;
		Quad(Context, Shown);
		if (Shown.Num() < 3)
		{
			return;
		}
		Corners[2] = Shown[2];
		Stage = EPlotStage::CornerB;
		return;
	}

	case EPlotStage::CornerB:
	{
		TArray<FVector2D> Shown;
		Quad(Context, Shown);
		if (Shown.Num() < 4)
		{
			return;
		}

		// REFUSED AT THE CLICK THAT WOULD MAKE IT, not at commit, so the player is never left
		// holding a shape that cannot be built and can only escape by cancelling.
		//
		// LOAD-BEARING AGAIN as of 2026-09-17. While the back corners rode the frontage's
		// normal the quad could not fold and this could not fire - it was documented as
		// unreachable, and a test written for it pinned a legal plot instead. Freeing the
		// corners to move sideways brought the fold back: drag the last corner across the
		// one before it and edge 3->0 crosses edge 1->2.
		//
		// What it protects is the ear-clipper downstream, which produces overlapping faces
		// rather than an error when fed a crossed polygon. PlaceEntityInPlot asks the same
		// question and keeps asking it: this is earlier, not instead.
		if (!RoadGeom::IsSimplePolygon(Shown))
		{
			return;
		}
		Corners[3] = Shown[3];
		Stage = EPlotStage::Confirm;
		return;
	}

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
	case EPlotStage::Confirm:  Stage = EPlotStage::CornerB;  return;
	case EPlotStage::CornerB:  Stage = EPlotStage::CornerA;  return;
	case EPlotStage::CornerA:  Stage = EPlotStage::Frontage; return;
	case EPlotStage::Frontage: Stage = EPlotStage::Idle;     return;
	case EPlotStage::Idle:     return;
	}
}

void FPlotPlaceTool::OnCommit(const FToolContext& Context)
{
	// EVERY STAGE BUT THE LAST IGNORES THIS. Build is a widget, not a stage of the gesture,
	// so it is reachable whenever the bar is on screen - committing from Frontage would build a
	// plot with no depth at all.
	if (Stage != EPlotStage::Confirm || Context.Target == nullptr)
	{
		return;
	}

	TArray<FVector2D> Outline;
	Quad(Context, Outline);
	if (Outline.Num() < 4)
	{
		return;
	}

	// THE SAME QUAD THE GHOST DREW. Built from Quad() rather than rebuilt from a width and a
	// depth, so what is committed cannot differ from what was on screen when Build was hit.
	Context.Target->PlaceEntityInPlot(Outline, Outline[0], Outline[1], Modules, Kind);

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

				// THE DOTS STAND WHERE THE CORNER WILL, off the kerb on the side the cursor
				// is on - not on the centreline they are derived from. A dot you aim at and
				// a corner that lands half a road away is the preview disagreeing with the
				// click, which is the one thing this codebase will not have.
				const FVector2D Left = RoadGeom::PerpCCW(Unit);
				const bool bLeft = FVector2D::DotProduct(Context.Cursor - RoadA, Left) >= 0.0;
				const FVector2D Offset = (bLeft ? Left : -Left)
					* KerbOffset(*Network, Context.Snap.Segment, bLeft);

				// THE ONE A CLICK WOULD TAKE IS DRAWN DIFFERENTLY. A row of identical dots
				// says where anchors exist; it does not say which one the cursor has. Pending
				// is the style every other tool uses for "this is what the click does", and
				// it double-rings, so the chosen point reads at a glance.
				const int32 Chosen = AnchorIndexAt(Context.Snap.SegmentT, Length);

				const int32 Count = FMath::FloorToInt(Length / FrontageStepUu);
				for (int32 I = 0; I <= Count; ++I)
				{
					Sink.Marker(RoadA + Unit * AnchorOffset(I) + Offset,
						I == Chosen ? EPreviewStyle::Pending : EPreviewStyle::Snap);
				}
			}
		}
		else
		{
			Sink.Label(Context.Cursor, TEXT("move near a service road"), EPreviewStyle::Refused);
		}
		return;
	}

	TArray<FVector2D> Shown;
	Quad(Context, Shown);
	if (Shown.Num() < 2)
	{
		return;
	}

	// A DOT PER CORNER ALREADY PLACED, so "Plot Points: 2/4" has something on the ground to
	// count against rather than being a number the player has to take on trust.
	const int32 Pinned = PinnedCount();
	for (int32 I = 0; I < Pinned && I < Shown.Num(); ++I)
	{
		Sink.Marker(Shown[I], EPreviewStyle::Pinned);
	}

	// THE FRONTAGE IS PINNED FROM THE SECOND CLICK ON. At one corner it still follows the
	// cursor, so it is drawn solid only once it has stopped moving.
	Sink.Line(Shown[0], Shown[1],
		Pinned >= 2 ? EPreviewStyle::Pinned : EPreviewStyle::Provisional);

	if (Shown.Num() < 4)
	{
		return;
	}

	// The rest of the boundary. AN EDGE IS PINNED WHEN BOTH ITS ENDS ARE, and provisional
	// the moment either is still under the cursor.
	Sink.Line(Shown[1], Shown[2],
		Pinned >= 3 ? EPreviewStyle::Pinned : EPreviewStyle::Provisional);
	Sink.Line(Shown[2], Shown[3],
		Pinned >= 4 ? EPreviewStyle::Pinned : EPreviewStyle::Provisional);
	Sink.Line(Shown[3], Shown[0],
		Pinned >= 4 ? EPreviewStyle::Pinned : EPreviewStyle::Provisional);

	// CONTENTS AT THREE CORNERS, NOT TWO. With two pinned both back corners are unknown and
	// the plot has no settled depth anywhere, so anything drawn inside it is a promise the
	// next two clicks break. With three, only one corner moves - and that is a promise the
	// gesture can keep. Drawing them from the first click is what PIE called out on
	// 2026-09-16; see the four-point gesture design doc.
	if (Pinned < 3)
	{
		return;
	}

	// THE MODULES THEMSELVES, where they will actually stand.
	//
	// This drew cross-marks for "which way each bay faces" and rings for "slots behind row 1"
	// - both derived from a bay grid that NOTHING BUILT any more, and which has since been
	// deleted outright. They were describing a structure that no longer existed, which is
	// why they read as decoration: "I'm not actually sure what they are supposed to be
	// telling me" (PIE, 2026-09-16). A mark whose meaning has gone is worse than no mark.
	//
	// Drawing the real footprints is only honest because the seed matches: DepotYardSeed off
	// the frontage midpoint is the Position the facade will store, so these outlines are the
	// boxes Build puts down, not an impression of them.
	const PlotYard::FYard Yard = YardFor(Shown);

	TArray<FVector2D> StandOutline;
	for (int32 I = 0; I < Yard.Stands.Num() && I < Modules.Num(); ++I)
	{
		if (!Yard.Stands[I].bPlaced)
		{
			continue;
		}
		PlotYard::StandCorners(Yard.Stands[I], DepotFootprint(Modules[I]), StandOutline);
		Sink.Polygon(StandOutline, EPreviewStyle::Pending);
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

	TArray<FVector2D> Shown;
	Quad(Context, Shown);
	if (Shown.Num() < 2)
	{
		Sink.Committable(false);
		return;
	}

	// FIRST, because it is the line that says where the player is in the gesture; every other
	// fact is about a shape that may not be finished.
	Sink.Fact(TEXT("Plot Points"), FString::Printf(TEXT("%d/4"), PinnedCount()));

	// BAYS AND ROWS ARE GONE: a quadrilateral has neither, and a fact whose NAME survived its
	// meaning is worse than one that was removed - the player reads a number describing a
	// structure the plot does not have. Same reasoning that retired "Expansion slots".
	Sink.Fact(TEXT("Frontage"), FString::Printf(TEXT("%.0f m"),
		FVector2D::Distance(Shown[0], Shown[1]) / 100.0));

	// THE SAME SOLVER THE PRESENTER RUNS, and the same call the ghost above draws from - so
	// the boxes on screen and the counts on the bar are one computation, not two that agree.
	const PlotYard::FYard Yard = YardFor(Shown);

	// WHAT YOU GET against what you asked for. A plot too tight silently dropping the pump
	// is exactly the kind of thing a player discovers after paying for it.
	Sink.Fact(TEXT("Modules"), FString::Printf(TEXT("%d of %d"),
		Modules.Num() - Yard.DroppedCount(), Modules.Num()));
	Sink.Fact(TEXT("Room for"), FString::FromInt(Yard.RoomForMore));

	if (Stage == EPlotStage::Confirm && Yard.RoomForMore == 0)
	{
		// The direct analogue of Manor Lords' "Plots without Extension Space". A warning and
		// never a refusal: a one-row depot works perfectly well and may be exactly what the
		// player wants.
		Sink.Warning(TEXT("No room to grow"));
	}

	Sink.Committable(Stage == EPlotStage::Confirm);
}

#undef LOCTEXT_NAMESPACE
