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

void FStandPlotTool::Shape(const FToolContext& Context, TArray<FVector2D>& OutShape) const
{
	OutShape.Reset();

	const int32 PinnedNow = PinnedCount();
	if (PinnedNow < 1)
	{
		return;
	}

	// Corner 0: the anchor, pinned by the first click and never moving after.
	const FVector2D Anchor = Corners[0];

	// Corner 1: the entrance edge's far end - along the taxiway, quantised on the depot's own
	// steps, and EITHER WAY along it, exactly as FPlotPlaceTool::Shape derives its frontage. The
	// RAW cursor, as there: the first two clicks are already snapped to the grid and a guide
	// over them would be a second opinion about where they may go.
	FVector2D Far = Corners[1];
	if (PinnedNow == 1)
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
	if (PinnedNow == 2)
	{
		const double Raw = FMath::Max(0.0,
			FVector2D::DotProduct(Context.GuidedCursor() - Anchor, Inward));
		const double Depth =
			FMath::RoundToDouble(Raw / StandPlotRules::DepthStepUu) * StandPlotRules::DepthStepUu;
		Back = Far + Inward * Depth;
	}
	else if (PinnedNow >= 3)
	{
		Back = Corners[2];
	}

	OutShape.Add(Anchor);
	OutShape.Add(Far);
	OutShape.Add(Back);
	OutShape.Add(Anchor + (Back - Far));
}

bool FStandPlotTool::CanCloseShape(TConstArrayView<FVector2D> Shown) const
{
	// ASKED AS A DEPTH, NOT ONLY AS IsSimplePolygon: a zero-depth rectangle is two edges
	// laid back over the other two, and RoadGeom::SegmentsCross does not count a collinear
	// overlap as a crossing, so IsSimplePolygon passed it and the click locked a stand of
	// no depth (Airside.Tool.StandPlot.ZeroDepthClickStays, review round 1). The polygon
	// test stays for anything else that could fold - it is WhyStandRefused's own first
	// question, asked earlier, and matching that function's wording instead would let a
	// reworded refusal silently disarm it. Every OTHER refusal - too small, an unfit
	// letter, an overlap - still locks: the rectangle is a real shape, and the readout
	// names the lever while the player looks at it.
	return StandBox::DepthOf(Shown) > 0.0 && RoadGeom::IsSimplePolygon(Shown);
}

int32 FStandPlotTool::Place(const FToolContext& Context, const TArray<FVector2D>& Outline) const
{
	// THE SAME RECTANGLE THE GHOST DREW, entrance edge 0->1 as StandBox reads it. The facade
	// asks WhyStandRefused again and derives the pose from the letter the box reads as - the
	// tool states the ground, never the pose.
	return Context.Target->PlaceStandInPlot(Outline, Outline[0], Outline[1]);
}

FString FStandPlotTool::RefusalFor(const FToolContext& Context, TConstArrayView<FVector2D> Shown) const
{
	if (RefusalMemo.Matches(Shown))
	{
		return RefusalMemo.Payload;
	}

	const FString Why = Context.Target != nullptr ? Context.Target->WhyStandRefused(Shown) : FString();
	RefusalMemo.Store(Shown, Why);

	// FOR TESTS ONLY, and only on the path that actually paid for the ask - see
	// GetRefusalCountForTest.
	++RefusalCountForTest;
	return Why;
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

void FStandPlotTool::DescribeRemoveExtra(const FToolContext& Context, int32 Doomed,
	const FEntityInstance& Entity, IToolPreviewSink& Sink) const
{
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
}

void FStandPlotTool::Describe(const FToolContext& Context, TConstArrayView<FVector2D> Shown,
	IToolPreviewSink& Sink) const
{
	// The other three edges, provisional while the depth still follows the cursor. UNLIKE THE
	// DEPOT'S THREE EDGES, one style covers all of them: a rectangle's fourth corner is
	// MECHANICALLY DERIVED from the other three (Shape() above), never its own click, so it is
	// exactly as settled as they are the moment the depth is pinned - it has no independent
	// freedom the way the depot's own near corner does before its own fourth click.
	const int32 PinnedNow = PinnedCount();
	const EPreviewStyle Rest = PinnedNow >= 3 ? EPreviewStyle::Pinned : EPreviewStyle::Provisional;
	Sink.Line(Shown[1], Shown[2], Rest);
	Sink.Line(Shown[2], Shown[3], Rest);
	Sink.Line(Shown[3], Shown[0], Rest);

	DescribeLetter(Context, Shown, Sink);

	// THE LETTER, OR WHY NOT, on the ground at the stand's centre - the same sentence the
	// readout warns with, from the facade's one evaluator, so the ghost and the bar agree.
	//
	// ASKED HERE UNCONDITIONALLY, even at Entrance with a zero-depth rectangle - unlike
	// DescribeReadout's own guard, which was already deliberately withheld until the depth is
	// dragged (see that method). Both read the SAME memo, so asking here first at Entrance
	// costs nothing extra once DescribeReadout goes on to ask again from Depth on.
	const FVector2D Centre = (Shown[0] + Shown[2]) * 0.5;
	const FString Why = RefusalFor(Context, Shown);
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

void FStandPlotTool::DescribeReadout(const FToolContext& Context, TConstArrayView<FVector2D> Shown,
	IToolReadoutSink& Sink) const
{
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
	// useless to a player who has not dragged the depth yet. THE SAME MEMO DescribeLetter's own
	// preview already asked (issue #302) - PinnedCount() >= 2 is "Depth or Confirm", the stages
	// that stage's own Entrance-only guard used to name by its old EStandStage.
	FString Why;
	if (PinnedCount() >= 2 && Context.Target != nullptr)
	{
		Why = RefusalFor(Context, Shown);
	}
	if (!Why.IsEmpty())
	{
		Sink.Warning(Why);
	}
	else if (IsConfirmed() && bLastCommitRefused)
	{
		// See bLastCommitRefused's own comment on why this should be unreachable.
		Sink.Warning(TEXT("Build failed: the stand was refused"));
	}

	Sink.Committable(IsConfirmed() && Context.Target != nullptr && Why.IsEmpty());
}

#undef LOCTEXT_NAMESPACE
