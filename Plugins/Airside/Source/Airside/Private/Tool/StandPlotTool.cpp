#include "Tool/StandPlotTool.h"

#include "Model/GroundTraffic.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Solve/IcaoCode.h"
#include "Solve/RoadGeom.h"
#include "Solve/StandBox.h"

#define LOCTEXT_NAMESPACE "Airside"

namespace StandPlotRules
{
	/**
	 * The depth quantum, uu - one metre.
	 *
	 * A METRE, NOT THE ENTRANCE'S 5 m STEP, because depth is what separates one letter from the
	 * next and the table's depth floors sit on whole metres (IcaoCode's StandDepth column) - a
	 * 5 m step would put some floors between two reachable depths, and the player could never
	 * draw the smallest stand of that letter. Still quantised, so the readout's "Size" and the
	 * committed outline agree to the metre with what the player read.
	 */
	constexpr double DepthStepUu = 100.0;
}

FText FStandPlotTool::GetDisplayName() const
{
	// Must match the registry's own Name for key 3 - Airside.Tool.BuildSession asserts the
	// two cannot drift.
	return LOCTEXT("StandTool", "Stand");
}

int32 FStandPlotTool::PinnedCount() const
{
	// READ OFF THE STAGE so the two cannot disagree - FPlotPlaceTool::PinnedCount's reason.
	switch (Stage)
	{
	case EStandStage::Idle:     return 0;
	case EStandStage::Entrance: return 1;
	case EStandStage::Depth:    return 2;
	case EStandStage::Confirm:  return 3;
	}
	return 0;
}

void FStandPlotTool::Rect(const FToolContext& Context, TArray<FVector2D>& Out) const
{
	Out.Reset();

	const int32 Pinned = PinnedCount();
	if (Pinned < 1)
	{
		return;
	}

	// Corner 0: the anchor, pinned by the first click and never moving after.
	const FVector2D Anchor = Corners[0];

	// Corner 1: the entrance edge's far end - along the taxiway, quantised on the depot's own
	// steps, and EITHER WAY along it, exactly as FPlotPlaceTool::Quad derives its frontage. The
	// RAW cursor, as there: the first two clicks are already snapped to the grid and a guide
	// over them would be a second opinion about where they may go.
	FVector2D Far = Corners[1];
	if (Pinned == 1)
	{
		const double Reach = FVector2D::DotProduct(Context.Cursor - Anchor, Along);
		const double Sign = Reach < 0.0 ? -1.0 : 1.0;
		Far = Anchor + Along * (Sign * PlotGesture::QuantisedFrontage(FMath::Abs(Reach)));
	}

	// Corner 2 is the far end carried inward by the depth.
	//
	// The depth: how far inward of the ENTRANCE LINE the cursor is, never negative - a stand
	// behind its own entrance would be built on the taxiway it opens off. Measured along
	// Inward alone, so the rectangle stays a rectangle wherever the cursor wanders sideways:
	// the template the letter carries is one, and a skewed quad would need its letter read off
	// an inscribed rectangle the player cannot see (drawn-stands spec).
	//
	// ZERO UNTIL THE DEPTH IS BEING DRAGGED, rather than no corners at all, so every caller from
	// Entrance on gets the same four-corner shape and none of them special-cases a "line".
	//
	// ONCE PINNED, Corners[2] AS STORED rather than a depth re-derived from it: a dot product
	// and a multiply back would move the point by an ulp, and a stand dragged exactly to a
	// letter's depth floor can fall an ulp short of it - see StandBox::LetterOf.
	FVector2D Back = Far;
	if (Pinned == 2)
	{
		const double Raw = FMath::Max(0.0,
			FVector2D::DotProduct(Context.GuidedCursor() - Anchor, Inward));
		const double Depth =
			FMath::RoundToDouble(Raw / StandPlotRules::DepthStepUu) * StandPlotRules::DepthStepUu;
		Back = Far + Inward * Depth;
	}
	else if (Pinned >= 3)
	{
		Back = Corners[2];
	}

	Out.Add(Anchor);
	Out.Add(Far);
	Out.Add(Back);
	Out.Add(Anchor + (Back - Far));
}

int32 FStandPlotTool::StandUnder(const FToolContext& Context)
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
	// KIND, NOT OUTLINE: every stand and every drawn depot has an outline now, so IsStand() is
	// the only question that tells them apart - see FEntityInstance::IsStand. A depot under
	// the cursor is the depot tool's to remove, where the player can see it is one.
	return Network->GetEntities()[Under].IsStand() ? Under : INDEX_NONE;
}

void FStandPlotTool::OnClick(const FToolContext& Context)
{
	const URoadNetwork* Network = Context.Network();
	if (Context.Target == nullptr || Network == nullptr)
	{
		return;
	}

	// REMOVE: the stand whose ground was clicked, and nothing else - no gesture is started or
	// advanced. DeleteEntity refunds through the purse and is one undo step, and takes the
	// stand's anchor nodes with it.
	if (Context.bRemoveModifier)
	{
		const int32 Doomed = StandUnder(Context);
		if (Doomed != INDEX_NONE)
		{
			Context.Target->DeleteEntity(Doomed);
		}
		return;
	}

	switch (Stage)
	{
	case EStandStage::Idle:
	{
		// THE DEPOT'S FIRST CLICK, AGAINST A TAXIWAY: the grid, the side and the kerb offset
		// all come from PlotGesture::AnchorAt, so a stand and a depot drawn off one grid can
		// sit flush. IsTaxiway, never IsServiceRoad - a stand opening onto a road trucks use
		// would have its arrivals taxi on one.
		PlotGesture::FAnchor Anchor;
		if (!PlotGesture::AnchorAt(*Network, Context.Cursor, PlotGesture::IsTaxiway, Anchor))
		{
			return;
		}
		Corners[0] = Anchor.Corner;
		Along = Anchor.Along;
		Inward = Anchor.Inward;

		// A FRESH GESTURE - see bLastCommitRefused's own comment.
		bLastCommitRefused = false;
		Stage = EStandStage::Entrance;
		return;
	}

	case EStandStage::Entrance:
	{
		// PINNED FROM WHAT WAS ON SCREEN, not recomputed: Rect is what the ghost drew.
		TArray<FVector2D> Shown;
		Rect(Context, Shown);
		if (Shown.Num() < 4)
		{
			return;
		}
		Corners[1] = Shown[1];
		Stage = EStandStage::Depth;
		return;
	}

	case EStandStage::Depth:
	{
		TArray<FVector2D> Shown;
		Rect(Context, Shown);
		if (Shown.Num() < 4)
		{
			return;
		}

		// REFUSED AT THE CLICK THAT WOULD MAKE IT, not at commit, so the player is never left
		// holding a shape that can only be escaped by cancelling. In a rectangle the only way
		// to fail this is ZERO DEPTH - a cursor on or behind the entrance line - which folds
		// the rectangle onto its own entrance edge.
		//
		// ASKED AS A DEPTH, NOT ONLY AS IsSimplePolygon: a zero-depth rectangle is two edges
		// laid back over the other two, and RoadGeom::SegmentsCross does not count a collinear
		// overlap as a crossing, so IsSimplePolygon passed it and the click locked a stand of
		// no depth (Airside.Tool.StandPlot.ZeroDepthClickStays, review round 1). The polygon
		// test stays for anything else that could fold - it is WhyStandRefused's own first
		// question, asked earlier, and matching that function's wording instead would let a
		// reworded refusal silently disarm it. Every OTHER refusal - too small, an unfit
		// letter, an overlap - still locks: the rectangle is a real shape, and the readout
		// names the lever while the player looks at it.
		if (StandBox::DepthOf(Shown) <= 0.0 || !RoadGeom::IsSimplePolygon(Shown))
		{
			return;
		}
		Corners[2] = Shown[2];
		Stage = EStandStage::Confirm;
		return;
	}

	case EStandStage::Confirm:
		// NOTHING. The gesture is locked and Build is the only way on - a click that committed
		// here would delete the review beat the staging exists for.
		return;
	}
}

void FStandPlotTool::OnCancel(const FToolContext& Context)
{
	// STEPPING BACK RE-OPENS THE GESTURE, so a refusal earned by the shape left behind no
	// longer describes anything the player can still commit.
	bLastCommitRefused = false;

	// ONE STAGE AT A TIME, the depot's answer to a misclick: binning the whole gesture is a
	// harsher response than the mistake deserves.
	switch (Stage)
	{
	case EStandStage::Confirm:  Stage = EStandStage::Depth;    return;
	case EStandStage::Depth:    Stage = EStandStage::Entrance; return;
	case EStandStage::Entrance: Stage = EStandStage::Idle;     return;
	case EStandStage::Idle:     return;
	}
}

void FStandPlotTool::OnCommit(const FToolContext& Context)
{
	// EVERY STAGE BUT THE LAST IGNORES THIS. Build is a widget, reachable whenever the bar is
	// on screen - committing from Depth would build whatever depth the cursor happened to hold.
	if (Stage != EStandStage::Confirm || Context.Target == nullptr)
	{
		return;
	}

	TArray<FVector2D> Outline;
	Rect(Context, Outline);
	if (Outline.Num() < 4)
	{
		return;
	}

	// THE SAME RECTANGLE THE GHOST DREW, entrance edge 0->1 as StandBox reads it. The facade
	// asks WhyStandRefused again and derives the pose from the letter the box reads as - the
	// tool states the ground, never the pose.
	const int32 Placed = Context.Target->PlaceStandInPlot(Outline, Outline[0], Outline[1]);

	// HONOUR THE RETURN (issue #182's lesson): a refusal KEEPS the gesture in Confirm with the
	// same shape on screen, and the readout says so. Falling through to Idle would make the
	// stand vanish with only a log line the player never sees.
	if (Placed == INDEX_NONE)
	{
		bLastCommitRefused = true;
		return;
	}

	// BACK TO IDLE, ready for the next. Staying in Confirm would let a second Build stack two
	// stands on one rectangle.
	Stage = EStandStage::Idle;
}

void FStandPlotTool::OnDeactivate(const FToolContext& Context)
{
	// Discarded outright: the gesture exists only on this object, so nothing in the model has
	// to be cleaned up.
	Stage = EStandStage::Idle;
	bLastCommitRefused = false;
}

void FStandPlotTool::DescribeLetter(const FToolContext& Context, TConstArrayView<FVector2D> Shown,
	IToolPreviewSink& Sink) const
{
	// A RECTANGLE WITH NO LETTER HAS NO AIRCRAFT to draw a wing or a lead-in for; the refusal
	// label BuildPreview draws at the centre says why instead.
	const TOptional<EIcaoCode> Letter = StandBox::LetterOf(Shown);
	if (!Letter.IsSet())
	{
		return;
	}

	// THE POSE THE COMMIT WILL STORE - StandBox::PoseFor is what URoadEditFacade::
	// PlaceStandInPlot derives it with, so the keep-out and lead-in are drawn about the stop
	// mark an arrival will actually stop on, not an impression of it.
	const StandBox::FStandPose Pose = StandBox::PoseFor(Shown[0], Shown[1], Inward, *Letter);
	const FVector2D Left = RoadGeom::PerpCCW(Pose.Facing);
	auto ToWorld = [&Pose, &Left](double X, double Y)
	{
		return Pose.Position + Pose.Facing * X + Left * Y;
	};

	// THE WING KEEP-OUT, in the stand's own frame: X between the letter's aft-most trailing
	// edge and forward-most leading edge, Y out to half its widest span - exactly the box
	// IcaoCode::WingKeepOutContains tests, which is the ONE place that box is written. Drawn so
	// the player sees why nothing will be laid under the wing before they build.
	const double Fwd = IcaoCode::WingFwdForLetter(*Letter);
	const double Aft = IcaoCode::WingAftForLetter(*Letter);
	const double HalfSpan = 0.5 * IcaoCode::MaxWingspanForLetter(*Letter);
	const FVector2D KeepOut[4] = {
		ToWorld(Aft, -HalfSpan), ToWorld(Fwd, -HalfSpan), ToWorld(Fwd, HalfSpan), ToWorld(Aft, HalfSpan) };
	Sink.Polygon(KeepOut, EPreviewStyle::Pending);

	// THE LEAD-IN: entrance midpoint to the stop mark, the line an arrival taxis in along and
	// is pushed back out along.
	Sink.Line((Shown[0] + Shown[1]) * 0.5, Pose.Position, EPreviewStyle::Pending);
}

void FStandPlotTool::BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const
{
	const URoadNetwork* Network = Context.Network();
	if (Network == nullptr)
	{
		return;
	}

	// REMOVE shows what a click would take - the stand's ground, outlined doomed - and none of
	// the placement ghost, which would be offering to build while the click deletes.
	if (Context.bRemoveModifier)
	{
		const int32 Doomed = StandUnder(Context);
		if (Doomed == INDEX_NONE)
		{
			return;
		}
		const FEntityInstance& Entity = Network->GetEntities()[Doomed];
		// IsStand() is already true - StandUnder only returns a stand - but named on this line
		// so IsPlotted() here reads as "has ground to outline", never as a kind test.
		if (Entity.IsStand() && Entity.IsPlotted())
		{
			Sink.Polygon(Entity.Outline, EPreviewStyle::Doomed);
		}
		else
		{
			Sink.Marker(Entity.Position, EPreviewStyle::Doomed);
		}
		Sink.Label(Context.Cursor, TEXT("remove stand"), EPreviewStyle::Doomed);

		// IN USE. The claim holder, read from the traffic table through the edit target (Model/,
		// so a tool may see it). The click still deletes - the player owns the infrastructure -
		// but not without being told who is about to lose a stand (stand-occupancy spec §6).
		if (const UGroundTraffic* Traffic = Context.Target->GetGroundTraffic())
		{
			const int32 Holder = Entity.PoseNode.IsSet() ? Traffic->HolderOfNode(Entity.PoseNode) : 0;
			if (Holder != 0)
			{
				Sink.Label(Context.Cursor + FVector2D(0.0, 600.0),
					FString::Printf(TEXT("in use by aircraft %d"), Holder), EPreviewStyle::Refused);
			}
		}
		return;
	}

	if (Stage == EStandStage::Idle)
	{
		// THE GRID A CLICK ANCHORS ON, drawn by the same rule the click takes.
		if (!PlotGesture::DescribeAnchors(*Network, Context.Cursor, PlotGesture::IsTaxiway, Sink))
		{
			Sink.Label(Context.Cursor, TEXT("move near a taxiway"), EPreviewStyle::Refused);
		}
		return;
	}

	TArray<FVector2D> Shown;
	Rect(Context, Shown);
	if (Shown.Num() < 4)
	{
		return;
	}

	// A DOT PER POINT ALREADY PINNED, so "Stand Points: 2/3" has something on the ground to
	// count against. Corner 2 is the pinned depth point once there are three.
	const int32 Pinned = PinnedCount();
	for (int32 I = 0; I < Pinned && I < Shown.Num(); ++I)
	{
		Sink.Marker(Shown[I], EPreviewStyle::Pinned);
	}

	// THE ENTRANCE EDGE, pinned from the second click on. Alone while it is being dragged: the
	// zero-depth rectangle behind it is not a shape yet.
	Sink.Line(Shown[0], Shown[1], Pinned >= 2 ? EPreviewStyle::Pinned : EPreviewStyle::Provisional);
	if (Pinned < 2)
	{
		return;
	}

	// The other three edges, provisional while the depth still follows the cursor.
	const EPreviewStyle Rest = Pinned >= 3 ? EPreviewStyle::Pinned : EPreviewStyle::Provisional;
	Sink.Line(Shown[1], Shown[2], Rest);
	Sink.Line(Shown[2], Shown[3], Rest);
	Sink.Line(Shown[3], Shown[0], Rest);

	DescribeLetter(Context, Shown, Sink);

	// THE LETTER, OR WHY NOT, on the ground at the stand's centre - the same sentence the
	// readout warns with, from the facade's one evaluator, so the ghost and the bar agree.
	const FVector2D Centre = (Shown[0] + Shown[2]) * 0.5;
	const FString Why = Context.Target != nullptr ? Context.Target->WhyStandRefused(Shown) : FString();
	const TOptional<EIcaoCode> Letter = StandBox::LetterOf(Shown);
	if (!Why.IsEmpty())
	{
		Sink.Label(Centre, Why, EPreviewStyle::Refused);
	}
	else if (Letter.IsSet())
	{
		Sink.Label(Centre, FString::Printf(TEXT("Code %s"), IcaoCode::ToLetter(*Letter)),
			EPreviewStyle::Pending);
	}
}

void FStandPlotTool::BuildReadout(const FToolContext& Context, IToolReadoutSink& Sink) const
{
	const URoadNetwork* Network = Context.Network();

	// REMOVE ASKS A DIFFERENT QUESTION from placement, so it gets its own answer: "move near a
	// taxiway" would be advice about a gesture Remove never makes.
	if (Context.bRemoveModifier)
	{
		if (StandUnder(Context) == INDEX_NONE)
		{
			Sink.Warning(TEXT("Click a stand to remove it"));
		}
		Sink.Committable(false);
		return;
	}

	if (Stage == EStandStage::Idle)
	{
		// THE SAME QUESTION THE CLICK ASKS, so the warning cannot say "move near a taxiway"
		// while a click would have anchored perfectly well.
		PlotGesture::FAnchor Unused;
		if (Network == nullptr
			|| !PlotGesture::AnchorAt(*Network, Context.Cursor, PlotGesture::IsTaxiway, Unused))
		{
			Sink.Warning(TEXT("Move near a taxiway"));
		}
		Sink.Committable(false);
		return;
	}

	TArray<FVector2D> Shown;
	Rect(Context, Shown);
	if (Shown.Num() < 4)
	{
		Sink.Committable(false);
		return;
	}

	// FIRST, because it says where the player is in the gesture.
	Sink.Fact(TEXT("Stand Points"), FString::Printf(TEXT("%d/3"), PinnedCount()));

	const double Width = StandBox::WidthOf(Shown);
	const double Depth = StandBox::DepthOf(Shown);
	Sink.Fact(TEXT("Size"), FString::Printf(TEXT("%.0f x %.0f m"), Width / 100.0, Depth / 100.0));

	// THE LETTER THE GROUND READS AS - the game mechanic, live. "-" is a real answer: smaller
	// than any stand, and the warning below names how much more is needed.
	const TOptional<EIcaoCode> Letter = StandBox::LetterOf(Shown);
	Sink.Fact(TEXT("Stand"), Letter.IsSet()
		? FString::Printf(TEXT("Code %s"), IcaoCode::ToLetter(*Letter)) : FString(TEXT("-")));

	// THE NEXT LETTER'S LEVER, so the thresholds are learnt by dragging rather than looked up.
	// ROUNDED UP to whole metres: "3 m deeper" that turns out to need 3.4 is a promise broken.
	// A part already met is omitted - "0 m wider" is noise. The enum's ordinal is the table's
	// row order (IcaoCode.cpp's RowFor relies on the same), so +1 is the next letter.
	if (Letter.IsSet() && *Letter != EIcaoCode::F)
	{
		const EIcaoCode Next = static_cast<EIcaoCode>(static_cast<uint8>(*Letter) + 1);
		const int32 Wider =
			FMath::Max(0, FMath::CeilToInt((IcaoCode::StandWidthForLetter(Next) - Width) / 100.0));
		const int32 Deeper =
			FMath::Max(0, FMath::CeilToInt((IcaoCode::StandDepthForLetter(Next) - Depth) / 100.0));
		TArray<FString> Parts;
		if (Wider > 0) { Parts.Add(FString::Printf(TEXT("%d m wider"), Wider)); }
		if (Deeper > 0) { Parts.Add(FString::Printf(TEXT("%d m deeper"), Deeper)); }
		if (Parts.Num() > 0)
		{
			Sink.Fact(TEXT("Next"), FString::Printf(TEXT("Code %s: %s"),
				IcaoCode::ToLetter(Next), *FString::Join(Parts, TEXT(", "))));
		}
	}

	// THE FACADE'S ONE EVALUATOR, from the depth stage on - the same call PlaceStandInPlot
	// makes at commit, so the button cannot light over a stand the commit will refuse, and the
	// sentence is the facade's rather than a second wording of the same rule. Not asked while
	// the entrance is dragged: a zero-depth rectangle "crosses itself", which is true and
	// useless to a player who has not dragged the depth yet.
	FString Why;
	if (Stage != EStandStage::Entrance && Context.Target != nullptr)
	{
		Why = Context.Target->WhyStandRefused(Shown);
	}
	if (!Why.IsEmpty())
	{
		Sink.Warning(Why);
	}
	else if (Stage == EStandStage::Confirm && bLastCommitRefused)
	{
		// See bLastCommitRefused's own comment on why this should be unreachable.
		Sink.Warning(TEXT("Build failed: the stand was refused"));
	}

	Sink.Committable(Stage == EStandStage::Confirm && Context.Target != nullptr && Why.IsEmpty());
}

#undef LOCTEXT_NAMESPACE
