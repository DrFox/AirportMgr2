#include "Tool/PlotPlaceTool.h"

#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"
#include "Build/DepotKit.h"
#include "Build/PlotLayoutStrategy.h"
#include "Tool/PlotGesture.h"
#include "Solve/PlotYard.h"
#include "Solve/RoadGeom.h"

#define LOCTEXT_NAMESPACE "Airside"

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

bool FPlotPlaceTool::DescribeGuideAnchor(const URoadNetwork* Network, IRoadEditTarget* Target,
	FGuideAnchor& Out) const
{
	// THE NETWORK IS UNUSED HERE, deliberately: this gesture's reference is its own frontage
	// and its points are its own pinned corners, both of which live on the tool. FRoadDrawTool
	// is the implementor that needs the graph - see IBuildTool::DescribeGuideAnchor.

	// ONLY THE TWO BACK CORNERS. The anchor click is a search for a service road and the
	// frontage runs ALONG one in quantised 5 m steps - both are already constrained, and an
	// angular guide over them would be a second opinion about where they may go, which is how
	// two rules about one number come to disagree (see QuantisedFrontage's own comment).
	if (Stage != EPlotStage::CornerA && Stage != EPlotStage::CornerB)
	{
		return false;
	}

	const FVector2D Frontage = Corners[1] - Corners[0];
	if (Frontage.IsNearlyZero())
	{
		return false;
	}

	// THE CORNER THE MOVING EDGE GROWS FROM: the far end of the frontage while corner 2 is
	// being placed, the anchor while corner 3 is. Both measured against the SAME frontage
	// direction, which is what makes the pair of clicks a rectangle rather than two unrelated
	// right angles.
	const bool bFarEnd = Stage == EPlotStage::CornerA;
	Out.Origin      = bFarEnd ? Corners[1] : Corners[0];
	Out.ReferenceAt = bFarEnd ? Corners[0] : Corners[1];
	Out.Reference   = Frontage.GetSafeNormal();

	// THE TOOL NAMES ITS OWN REFERENCE, so the source can say "square to the frontage"
	// without knowing what a frontage is - the same split that keeps EPreviewStyle a meaning
	// rather than a colour.
	Out.ReferenceName = TEXT("the frontage");

	// A PLOT DRAGS A CORNER OF THE SHAPE ITSELF, which is a BOUNDARY, and the half-widths stay
	// zero because there is no pavement either side of a corner. The zero is the answer, not a
	// gap: see EDragPoint on why a centreline against a boundary is the case that displaces.
	Out.Point = EDragPoint::Boundary;

	// THE CORNERS ALREADY PINNED, so the moving one can line up with them - "0 degrees to
	// corner 3" (the 2026-09-17 request). ONLY AS FAR AS PinnedCount: entries past it are
	// stale, and offering one would align the player against the PREVIOUS gesture's geometry,
	// which is the bug Quad's own comment records having shipped once.
	//
	// THE CORNER BEING DRAGGED IS NOT IN THE LIST. A point cannot line up with itself: its
	// own two lines pass through wherever the cursor is, so both would always be in tolerance
	// and the guide would say "you are level with yourself" on every frame.
	const int32 Pinned = PinnedCount();
	for (int32 Index = 0; Index < Pinned && Index < 4; ++Index)
	{
		// NUMBERED AS THE PLAYER COUNTS THEM - the readout says "Plot Points: 2/4", so corner
		// 0 is "corner 1" on screen. A label naming a corner the bar does not is worse than
		// no label.
		//
		// SPELT OUT, not braced, as RoadGuideAnchor::AddNodeCandidates is (and the deleted
		// FStandPlaceTool's DescribeGuideAnchor was): a third member arrived on FGuidePoint
		// in 2026-09-20 and a braced initialiser would have taken the default for it in
		// silence. Reference is
		// left at its ThisGesture default here DELIBERATELY - these are the gesture's own
		// pinned corners, the one column that needs no button - but that is a decision this
		// line states rather than one a brace would have made by omission.
		FGuidePoint Point;
		Point.At = Corners[Index];
		Point.Name = FString::Printf(TEXT("corner %d"), Index + 1);
		Out.AlignTo.Add(Point);
	}
	return true;
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
		Far = Corners[0] + Along * (Sign * PlotGesture::QuantisedFrontage(FMath::Abs(Reach)));
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

	// THE GUIDED CURSOR, not the raw one. The driver resolved it (FToolContext::Guide) and
	// InFront still has the last word: a corner guided square to the frontage but dragged
	// behind it slides back onto the frontage line, because concrete on the carriageway is a
	// harder rule than an alignment aid. That is also why BuildPreview draws its guide line
	// from the corner SHOWN here rather than from Guide.Point.
	const FVector2D Back = Pinned == 2 ? InFront(Context.GuidedCursor()) : Corners[2];
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
		// Guided like the corner before it, and against the same frontage - see Back above.
		Near = InFront(Context.GuidedCursor());
	}
	else if (Pinned >= 4)
	{
		Near = Corners[3];
	}
	OutQuad.Add(Near);
}

PlotYard::FReservation FPlotPlaceTool::ReservationFor(
	const FToolContext& Context, TArrayView<const FVector2D> Outline) const
{
	// FROM THE DEFINITION THIS TOOL IS ABOUT TO PLACE, which is the same object the presenter
	// reads off the built entity.
	//
	// IT USED TO MAP ITS OWN Kind, and that second source of truth shipped: DA_FuelDepot was
	// authored before EPlotLayout existed, so it carried the Scatter default while this line
	// said FuelYardBands. The player dragged out a banded ghost and got a scattered depot -
	// the exact preview-versus-built split the reservation design exists to prevent.
	//
	// A DEFINITION THE TARGET CANNOT RESOLVE falls back to the scatter, which is what an
	// unauthored plot type would have drawn anyway.
	//
	// RESOLVED BEFORE THE MEMO IS CONSULTED, because it is part of the memo's own key - see
	// FReservationMemo's comment.
	const UEntityDefinition* Definition =
		Context.Target != nullptr ? Context.Target->GetEntityDefinition(Kind) : nullptr;
	const EPlotLayout Layout =
		Definition != nullptr ? Definition->Layout : EPlotLayout::Scatter;

	// WHAT THE PLOT WOULD HOLD, rather than where this tool's module list would stand.
	// Capacity is a property of the ground being dragged out, decided once - so the ghost is
	// the depot the player gets, and the counts beside it are that same solve counted.
	//
	// RESOLVED ONCE, LAZILY, REGARDLESS OF WHETHER THE OUTLINE IS COMPLETE: DepotKitSpecs
	// walks EDepotModule, not the quad, so it owes nothing to Outline - and BuildReadout lists
	// every kit at zero from the Frontage stage on, before a solve has ever run, which means
	// the specs must exist even on the early return below.
	//
	// THROUGH THE TARGET, NOT DepotKitSpecs(UAirsideSettings::GetContent()) HERE (issue #181):
	// that line used to live in this file, the #78 pattern (RunwayTool reaching UAirsideContent
	// itself) shipping again in a new tool - and UPlotPresenter was already resolving the same
	// table on its own, so a level's content could disagree with itself between the ghost and
	// the built depot. IRoadEditTarget::ResolveDepotKits is the one place both now read it. A
	// null Target (see the Context.Target guard on Definition above) leaves the specs empty
	// rather than crashing - the same "no target, no content" answer GetEntityDefinition gives.
	if (!Memo.bSpecsResolved)
	{
		Memo.Specs = Context.Target != nullptr ? Context.Target->ResolveDepotKits() : TArray<PlotYard::FKitSpec>();
		Memo.bSpecsResolved = true;
	}

	if (Outline.Num() < 4)
	{
		return PlotYard::FReservation();
	}

	// THE MEMO HIT: the same four corners and the same layout as last time, so the packer
	// already ran for this shape and running it again would answer a question already asked.
	// EXACT EQUALITY, not a tolerance - Quad() either reproduces a pinned corner bit for bit
	// or derives the moving one from the same cursor value MakeToolContext resolved, so two
	// calls describing the same frame's shape compare equal without help.
	if (Memo.bValid && Memo.Layout == Layout
		&& Memo.Outline[0] == Outline[0] && Memo.Outline[1] == Outline[1]
		&& Memo.Outline[2] == Outline[2] && Memo.Outline[3] == Outline[3])
	{
		return Memo.Reservation;
	}

	// The pose the facade will store for this depot, so DepotYardSeed gives the yard that
	// gets BUILT rather than one that merely resembles it - see Build/DepotKit.h.
	const FVector2D Pose = (Outline[0] + Outline[1]) * 0.5;

	FPlotSite Site;
	Site.Outline = Outline;
	Site.FrontageA = Outline[0];
	Site.FrontageB = Outline[1];
	Site.Gate = Pose;
	Site.Seed = DepotYardSeed(Pose);

	Memo.Reservation = PlotLayoutFor(Layout)->Solve(Site, Memo.Specs);
	Memo.Outline[0] = Outline[0];
	Memo.Outline[1] = Outline[1];
	Memo.Outline[2] = Outline[2];
	Memo.Outline[3] = Outline[3];
	Memo.Layout = Layout;
	Memo.bValid = true;

	// FOR TESTS ONLY, and only on the path that actually paid for a solve - see
	// GetSolveCountForTest.
	++SolveCountForTest;

	return Memo.Reservation;
}

int32 FPlotPlaceTool::PlotUnder(const FToolContext& Context)
{
	const URoadNetwork* Network = Context.Network();
	if (Context.Target == nullptr || Network == nullptr)
	{
		return INDEX_NONE;
	}
	const int32 Under = Context.Target->FindEntityAt(Context.Cursor, Context.SnapRadius);
	if (!Network->GetEntities().IsValidIndex(Under))
	{
		return INDEX_NONE;
	}
	// A PRE-PLOT DEPOT HAS NO GROUND to click, so FindEntityAt finds it the way it finds a
	// stand - by its pose within the pick radius - and its fuel role is what says it is ours.
	// KIND, NOT OUTLINE: IsDepot() alone is enough now that a stand can be plotted too - the
	// old `IsPlotted() || PoseRole == Fuel` would have picked up a drawn stand here as well.
	const FEntityInstance& Entity = Network->GetEntities()[Under];
	return Entity.IsDepot() ? Under : INDEX_NONE;
}

void FPlotPlaceTool::OnClick(const FToolContext& Context)
{
	const URoadNetwork* Network = Context.Network();
	if (Context.Target == nullptr || Network == nullptr)
	{
		return;
	}

	// REMOVE: the depot whose ground was clicked, and nothing else - no gesture is started or
	// advanced. Until 2026-09-22 this tool ignored Remove entirely, so the bar's Remove button
	// lit on the depot tool did nothing and a depot could only go by undo. DeleteEntity
	// refunds through the purse and is one undo step, as a stand's removal is.
	if (Context.bRemoveModifier)
	{
		const int32 Doomed = PlotUnder(Context);
		if (Doomed != INDEX_NONE)
		{
			Context.Target->DeleteEntity(Doomed);
		}
		return;
	}

	switch (Stage)
	{
	case EPlotStage::Idle:
	{
		// THE SHARED FIRST CLICK - the grid, the side and the kerb offset all live in
		// PlotGesture::AnchorAt, which the stand tool asks of a taxiway the same way this asks
		// of a service road. See its own comments for each of the three.
		PlotGesture::FAnchor Anchor;
		if (!PlotGesture::AnchorAt(*Network, Context.Cursor, PlotGesture::IsServiceRoad, Anchor))
		{
			return;
		}
		Corners[0] = Anchor.Corner;
		Along = Anchor.Along;
		Inward = Anchor.Inward;

		// A FRESH GESTURE, so any refusal the LAST one earned at commit is no longer about
		// anything on screen - see bLastCommitRefused's own comment.
		bLastCommitRefused = false;

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
	// STEPPING BACK RE-OPENS THE GESTURE, so a refusal earned by the shape being left behind
	// no longer describes anything the player can still commit.
	bLastCommitRefused = false;

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
	const int32 Placed =
		Context.Target->PlaceEntityInPlot(Outline, Outline[0], Outline[1], Modules, Kind);

	// HONOUR THE RETURN - issue #182. This used to fall through to Idle whatever
	// PlaceEntityInPlot answered, so a plot the facade refused (its own reservation solve
	// found nothing to place, or found no definition to place it from) vanished from the
	// tool with only a log line the player never sees. INDEX_NONE now KEEPS the gesture in
	// Confirm - the same shape stays on screen, and BuildReadout's next call warns through
	// bLastCommitRefused. See that field's own comment on why this branch is a safety net
	// rather than the ordinary path: Committable is judged by the SAME evaluator.
	if (Placed == INDEX_NONE)
	{
		bLastCommitRefused = true;
		return;
	}

	// BACK TO IDLE, ready for the next one. A tool that stayed in Confirm would let the
	// player press Build twice and get two depots stacked on one plot.
	Stage = EPlotStage::Idle;
}

void FPlotPlaceTool::OnDeactivate(const FToolContext& Context)
{
	// Discarded outright, like the outline tool's part-drawn shape: it exists only on this
	// object, so nothing in the model has to be cleaned up.
	Stage = EPlotStage::Idle;
	bLastCommitRefused = false;
}

void FPlotPlaceTool::BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const
{
	const URoadNetwork* Network = Context.Network();
	if (Network == nullptr)
	{
		return;
	}

	// REMOVE shows what a click would take - the whole plot, outlined doomed - and none of
	// the placement ghost, which would be offering to build while the click deletes.
	if (Context.bRemoveModifier)
	{
		const int32 Doomed = PlotUnder(Context);
		if (Doomed != INDEX_NONE)
		{
			const FEntityInstance& Entity = Network->GetEntities()[Doomed];
			// Entity is already a depot here - PlotUnder only ever returns one - but the
			// kind is named on this line too, not left implicit in the caller, so a reader
			// (and Check-Architecture's IsPlotted rule) does not have to trust that at a
			// distance: IsPlotted() alone decides plotted vs pre-plot, never depot vs stand.
			if (Entity.IsDepot() && Entity.IsPlotted())
			{
				Sink.Polygon(Entity.Outline, EPreviewStyle::Doomed);
			}
			else
			{
				Sink.Marker(Entity.Position, EPreviewStyle::Doomed);
			}
			Sink.Label(Context.Cursor, TEXT("remove fuel depot"), EPreviewStyle::Doomed);
		}
		return;
	}

	if (Stage == EPlotStage::Idle)
	{
		// THE SAME GRID THE CLICK ANCHORS ON - PlotGesture::DescribeAnchors and AnchorAt are
		// one rule written once, so the heavier dot is the anchor a click takes.
		if (!PlotGesture::DescribeAnchors(*Network, Context.Cursor, PlotGesture::IsServiceRoad, Sink))
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

	// THE DASHED LINE TO WHAT IT IS LINED UP WITH, and the label saying which - snap-guides
	// design section 6, and the whole of what the request asked for: a ray along the guide
	// direction would say "you are at 90 degrees", and this says WHICH edge you are square to.
	//
	// DRAWN FROM THE CORNER THE QUAD SHOWS, not from Guide.Point, because the InFront clamp
	// in Quad may have moved it - a guide line that did not touch the shape would be pointing
	// at nothing.
	//
	// GATED ON THE MOVING CORNER, because Guide is only ever active while one of the two back
	// corners is under the cursor (see DescribeGuideAnchor), and Shown[Pinned] IS that corner.
	if (Context.Guide.bActive && (Pinned == 2 || Pinned == 3) && Shown.IsValidIndex(Pinned))
	{
		Sink.Guides(Context.Guide, Shown[Pinned]);
	}

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
	// ReservationFor RESOLVES Memo.Specs AS A SIDE EFFECT (issue #180) - reading it back here
	// rather than calling DepotKitSpecs again is the second half of "once per solve, not per
	// caller"; the first half is the memo hit above the case that made this call cheap.
	const PlotYard::FReservation Reservation = ReservationFor(Context, Shown);
	const TArray<PlotYard::FKitSpec>& Specs = Memo.Specs;

	TArray<FVector2D> StandOutline;
	for (const PlotYard::FReservedStand& Stand : Reservation.Stands)
	{
		if (!Specs.IsValidIndex(Stand.KitIndex))
		{
			continue;
		}

		// THE GROUND THE STAND CLAIMS, which is the run's footprint PLUS its apron ONLY WHEN
		// THE RESERVATION ACTUALLY CLAIMED IT (Reservation.bStandsIncludeApron - issue #193).
		// Outlining a single bay would promise the player two bays that are already spoken
		// for, and outlining the buildings alone would promise the apron as somewhere to
		// build; but PlotYard::Reserve (Scatter) never claims an apron in the first place, so
		// drawing footprint-plus-apron there outlines ground the sampler never fenced off, and
		// Stand.Centre is not even the centre of that rectangle - see FReservation's comment.
		const PlotYard::FKitSpec& Kit = Specs[Stand.KitIndex];
		PlotYard::FFootprint Run;
		if (Reservation.bStandsIncludeApron)
		{
			Run.LengthUu = Kit.Footprint.LengthUu + Kit.ApronUu.X;
			Run.WidthUu = Kit.RunWidthUu(Stand.RunLength) + Kit.ApronUu.Y * 2.0;
		}
		else
		{
			Run.LengthUu = Kit.Footprint.LengthUu;
			Run.WidthUu = Kit.RunWidthUu(Stand.RunLength);
		}

		PlotYard::StandCorners(Stand, Run, StandOutline);
		Sink.Polygon(StandOutline, EPreviewStyle::Pending);
	}
}

void FPlotPlaceTool::BuildReadout(const FToolContext& Context, IToolReadoutSink& Sink) const
{
	const URoadNetwork* Network = Context.Network();

	// REMOVE ASKS A DIFFERENT QUESTION from placement, so it gets its own answer: the Idle
	// readout's "move near a service road" would be advice about a gesture Remove never makes.
	if (Context.bRemoveModifier)
	{
		if (PlotUnder(Context) == INDEX_NONE)
		{
			Sink.Warning(TEXT("Click a fuel depot to remove it"));
		}
		Sink.Committable(false);
		return;
	}

	if (Stage == EPlotStage::Idle)
	{
		PlotGesture::FAnchor Unused;
		if (Network == nullptr
			|| !PlotGesture::AnchorAt(*Network, Context.Cursor, PlotGesture::IsServiceRoad, Unused))
		{
			// THE SAME QUESTION THE CLICK ASKS, so the warning cannot say "move near a
			// service road" while a click would have anchored perfectly well.
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
	// LITERALLY ONE COMPUTATION as of #180: this and BuildPreview's own call both read
	// Memo through ReservationFor, so a hover frame calling both pays for the packer once.
	const PlotYard::FReservation Reservation = ReservationFor(Context, Shown);
	const TArray<PlotYard::FKitSpec>& Specs = Memo.Specs;

	// A LINE PER KIT, because "Room for 4" could only ever mean "4 of the sample footprint" -
	// a number about a phantom tank rather than about anything the player can buy. These are
	// CEILINGS: what the plot will hold once they have paid for it, in whatever order they
	// like. Nothing here is a refusal.
	int32 Total = 0;
	for (int32 Kit = 0; Kit < Specs.Num(); ++Kit)
	{
		const int32 Ceiling = Reservation.CeilingFor(Kit);
		Total += Ceiling;
		Sink.Fact(DepotKitLabel(static_cast<EDepotModule>(Kit)), FString::FromInt(Ceiling));
	}

	// THE SAME TWO QUESTIONS URoadEditFacade::PlaceEntityInPlot ASKS - issue #182. Definition
	// is resolved through the target the same way ReservationFor resolves it for Layout, and
	// Total comes from the SAME evaluator call (PlotLayoutFor(Layout)->Solve, memoized above)
	// the facade runs again at commit - so Committable cannot light the Build button over a
	// plot the facade is about to refuse, which is what `Committable(Stage == Confirm)` did
	// regardless of whether the reservation placed anything.
	const bool bHasDefinition =
		Context.Target != nullptr && Context.Target->GetEntityDefinition(Kind) != nullptr;

	if (Stage == EPlotStage::Confirm)
	{
		if (!bHasDefinition)
		{
			// THE SAME FACT the facade's own log line names - see PlaceEntityInPlot's refusal
			// message - said here because the player, unlike the log, is looking at the bar.
			Sink.Warning(TEXT("No fuel depot is authored to build here"));
		}
		else if (Total == 0)
		{
			// WAS "No room to grow", fired when a full depot had no spare bay. Under
			// reservation a full plot is the normal end state and warning about it would cry
			// wolf on every well-drawn depot; a plot that holds NOTHING is the case worth
			// naming. The direct analogue of Manor Lords' "Plots without Extension Space" is
			// now the ghost itself.
			Sink.Warning(TEXT("This plot holds nothing"));
		}
		else if (bLastCommitRefused)
		{
			// See bLastCommitRefused's own comment on why this should be unreachable and is
			// kept anyway.
			Sink.Warning(TEXT("Build failed: the plot was refused"));
		}
	}

	Sink.Committable(Stage == EPlotStage::Confirm && bHasDefinition && Total > 0);
}

#undef LOCTEXT_NAMESPACE
