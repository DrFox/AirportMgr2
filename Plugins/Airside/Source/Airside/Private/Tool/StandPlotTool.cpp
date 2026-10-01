#include "Tool/StandPlotTool.h"

#include "AirsideLog.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Model/TaxiwayStrip.h"
#include "Solve/GridSnap.h"
#include "Solve/IcaoCode.h"
#include "Solve/RoadGeom.h"
#include "Solve/StandBox.h"
#include "Tool/PavementAxis.h"
#include "Tool/SnapGuideChain.h"

#define LOCTEXT_NAMESPACE "Airside"

// StandPlotRules::DepthStepUu moved to StandPlotTool.h (2026-09-26, on its own
// review): a test enforcing every IcaoCode floor against the quantum needs to read the same
// declaration the tool quantises by, not a second literal.

namespace StandPlotGuide
{
	/**
	 * The winner the depth obeys: a POSITIONAL guide (EFit::Perpendicular) running PARALLEL to
	 * the entrance - a neighbouring stand's back edge. Null otherwise.
	 *
	 * BOTH TESTS, from review on 2026-09-27. Angular winners ("along the entrance", "north")
	 * fix a direction the depth does not have - one near the entrance line projected the depth
	 * to zero. A positional line ACROSS the entrance (a neighbour's side edge on the edge being
	 * dragged) fixes nothing about depth, and took it as a raw cursor projection with no step.
	 *
	 * NAMESPACED, not anonymous: a unity build, and "BackEdgeGuide" is a name a second file
	 * could choose.
	 */
	const SnapGuide::FCandidate* BackEdgeGuide(const FToolContext& Context, const FVector2D& Inward)
	{
		if (!Context.Guide.bActive)
		{
			return nullptr;
		}
		// Parallel to the entrance = square to Inward. 1e-3 is ~0.06 degrees: the neighbour's
		// edge came from the same kind of taxiway-aligned rectangle, not a hand-drawn line.
		constexpr double SquareEpsilon = 1e-3;
		for (const SnapGuide::FCandidate& Winner : Context.Guide.Winners)
		{
			if (Winner.Fit == SnapGuide::EFit::Perpendicular
				&& FMath::Abs(FVector2D::DotProduct(Winner.Direction.GetSafeNormal(), Inward)) < SquareEpsilon)
			{
				return &Winner;
			}
		}
		return nullptr;
	}
}

FText FStandPlotTool::GetDisplayName() const
{
	// Must match the registry's own Name for key 3 - Airside.Tool.BuildSession.RegistryAndSession asserts the
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

	// Corner 0: the anchor - pinned by the first click ON THE KERB, never moving after - carried
	// out by FrontGap, the taxiway's clearance strip (strip spec 2026-09-28). EVERY CORNER BELOW
	// IS THE BOX'S, not the kerb's: the base pins Shown[N] straight into Corners[N], so a shape
	// in two frames would be carried out twice. The strip is the ground between kerb and box.
	// ENFORCED BY: Airside.Tool.StandPlot.StartsAtTheKerb
	const FVector2D Kerb = Corners[0];
	const FVector2D Anchor = Kerb + Inward * FrontGap;

	// Corner 1: the entrance edge's far end - along the taxiway, quantised on the depot's own
	// steps, and EITHER WAY along it, exactly as FPlotPlaceTool::Shape derives its frontage. The
	// RAW cursor, as there: the first two clicks are already snapped to the grid and a guide
	// over them would be a second opinion about where they may go.
	FVector2D Far = Corners[1];
	if (PinnedNow == 1)
	{
		// ALONG THE KERB, where the anchor's own grid crossings were measured, then carried out by
		// the gap - square to Along, so the crossing the kerb found is the box's too.
		Far = PlotGesture::FrontageEnd(Kerb, Along, Context.Cursor, Context.GridFrame) + Inward * FrontGap;
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
		// THREE RULES, STRONGEST FIRST (world-grid-snap design section 2):
		//   1. A POSITIONAL guide - a neighbouring stand's back edge, via Collinear x Stand - sets
		//      the depth outright, unquantised: lining up with it is the point, and a 0.5 m step
		//      would put the edge beside the neighbour's instead of on it. Angular winners ("square
		//      to the entrance") do not count: they fix a direction, and the depth is already
		//      measured along one.
		//   2. The grid (Context.GridFrame - under Follow, laid along the taxiway, so the back edge
		//      lands a whole number of steps off its centreline): the nearest crossing along the
		//      inward edge from Far.
		//   3. Today's 0.5 m step.
		// The chain's own grid point (a no-winner Guide.Point) is NOT used for 2: it rounds both
		// axes, and on a diagonal taxiway its projection onto Inward is no grid crossing at all.
		// Rule 1 is StandPlotGuide::BackEdgeGuide - a positional winner PARALLEL to the entrance.
		//
		// RULES 2 AND 3 READ THE RAW CURSOR, exactly as the tool did before it had a guide
		// anchor: every other winner is angular or runs across the entrance, and following one
		// moves the depth for a reason that is not about depth (see BackEdgeGuide).
		double Depth = 0.0;
		if (StandPlotGuide::BackEdgeGuide(Context, Inward) != nullptr)
		{
			Depth = FMath::Max(0.0, FVector2D::DotProduct(Context.GuidedCursor() - Anchor, Inward));
		}
		else if (FVector2D OnGrid = Far; Context.GridFrame.IsOn()
			&& GridSnap::NearestCrossingAlong(Far, Inward, Context.Cursor, Context.GridFrame, OnGrid))
		{
			Depth = FMath::Max(0.0, FVector2D::DotProduct(OnGrid - Far, Inward));
		}
		else
		{
			const double Raw = FMath::Max(0.0,
				FVector2D::DotProduct(Context.Cursor - Anchor, Inward));
			Depth = FMath::RoundToDouble(Raw / StandPlotRules::DepthStepUu) * StandPlotRules::DepthStepUu;
		}
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

bool FStandPlotTool::DescribeGuideAnchor(const URoadNetwork* Network, IRoadEditTarget* Target,
	FGuideAnchor& Out) const
{
	// THE DEPTH DRAG ONLY. The anchor and the frontage search for a taxiway and run along it
	// (FStagedPlotTool::SnapsToGrid covers their grid); a guide over them would be a second
	// opinion about where they may go - FPlotPlaceTool::DescribeGuideAnchor's reason.
	//
	// ADDED 2026-09-27 for the report that started the world grid: this tool had no anchor, so
	// the Collinear x Stand source that already proposes every stand's edges never ran for it,
	// and two stands of one depth off two taxiways could not be lined up by any means.
	if (PinnedCount() != 2)
	{
		return false;
	}

	const FVector2D Entrance = Corners[1] - Corners[0];
	if (Entrance.IsNearlyZero())
	{
		return false;
	}

	// SWINGING ROUND THE ENTRANCE'S FAR END, the corner the side edge being dragged grows from -
	// the depot's own choice for its first back corner.
	Out.Origin = Corners[1];
	Out.ReferenceAt = Corners[0];
	Out.Reference = Entrance.GetSafeNormal();
	Out.ReferenceName = TEXT("the entrance");

	// A CORNER OF THE SHAPE, not a centreline - see FPlotPlaceTool::DescribeGuideAnchor.
	Out.Point = EDragPoint::Boundary;
	return true;
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
	// tool states the ground, never the pose. The PAVEMENT is the tool's to state - the row's
	// pick - and the facade prices and captures exactly that.
	return Context.Target->PlaceStandInPlot(Outline, Outline[0], Outline[1], Pavement);
}

void FStandPlotTool::GetVariantAxes(const FToolContext& Context, TArray<FToolVariantAxis>& Out) const
{
	// ALL FOUR (an empty list), as the runway's row - see this function's header.
	Pavement::AppendAxis(Out, Pavement, {});
}

bool FStandPlotTool::SelectVariant(const FToolContext& Context, int32 Axis, int32 Option)
{
	// AN INDEX INTO THE OFFERED LIST, never into the enum - the row AppendAxis built, the same
	// mapping FRunwayTool::SelectVariant makes.
	const TArray<EPavement> Offered = Pavement::Offered({});
	if (Axis != 0 || !Offered.IsValidIndex(Option))
	{
		return false;
	}
	Pavement = Offered[Option];
	UE_LOG(LogAirside, Log, TEXT("Stand surface -> %s"), Pavement::Name(Pavement));
	return true;
}

FString FStandPlotTool::RefusalFor(const FToolContext& Context, TConstArrayView<FVector2D> Shown) const
{
	// NO TARGET, NO OPINION - what the ternary this replaced answered, and nothing to key a memo on.
	if (Context.Target == nullptr)
	{
		return FString();
	}

	// THE SITE HALF, REMEMBERED. Its key is everything it reads: the outline (Matches) and the
	// target's edit epoch (moved by every edit of the model, entities included - a stand placed into
	// this outline is exactly such an edit - and by an undo's graph swap). The pavement is NOT in it -
	// see RefusalFor's own comment.
	const uint32 Epoch = Context.Target->GetEditEpoch();
	const bool bHit = SiteMemo.Matches(Shown) && SiteMemo.Payload.Epoch == Epoch;
	if (!bHit)
	{
		SiteMemo.Store(Shown, FSitePayload{ Epoch, Context.Target->WhyStandSiteRefused(Shown) });

		// FOR TESTS ONLY, and only on the path that actually paid for the ask - see
		// GetRefusalCountForTest.
		++RefusalCountForTest;
	}
	if (!SiteMemo.Payload.Why.IsEmpty())
	{
		return SiteMemo.Payload.Why;
	}

	// THE MONEY HALF, FRESH EVERY CALL - the purse moves with no edit for an epoch to count, so
	// this is the half no key on the model can cover.
	return Context.Target->WhyStandUnaffordable(Shown, Pavement);
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
	//
	// Context.Envelopes, NOT IcaoCode::FloorEnvelopeForLetter (#292 review finding): Tool/ may
	// not include Content/AirsideSettings itself (Check-Architecture's include-direction rule),
	// but the SAME resolved table PlaceStandInPlot reads is threaded onto the context by
	// URoadEditFacade::MakeTunables -> FBuildSession::MakeContext (see FToolContext::Envelopes'
	// own comment), so the ghost, the drawn-stand commit and the point-placement path all read
	// one figure - not three, agreeing only by coincidence while nothing has raised a letter yet.
	const StandBox::FStandPose Pose =
		StandBox::PoseFor(Shown[0], Shown[1], Inward, Shown, *Letter, Context.Envelopes[*Letter]);
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
	// THE ENTRANCE EDGE, pinned from the second click on, is drawn by the shared base
	// (FStagedPlotTool::BuildPreview) before this hook is even called. ALONE WHILE IT IS
	// BEING DRAGGED: at one pinned corner Shape() completes a zero-depth rectangle (Back =
	// Far until Pinned reaches 2), which is not a shape yet - nothing past the entrance edge
	// is drawn or asked about. Regression caught in PR #333 review: the base's own gate is
	// `Shown.Num() < 4`, and Shape() always returns four points from one pinned corner on, so
	// without this guard the degenerate rectangle reached DescribeLetter and WhyStandRefused a
	// frame early and drew a "needs N m more depth" refusal the old tool never showed.
	// THE CLEARANCE STRIP, from the entrance drag on: the ground between the kerb the player
	// clicked and the box, which carries no paint (strip spec 2026-09-28). DRAWN AND NAMED, so the
	// gap reads as a rule rather than a misplaced box - the user found it unexplained (2026-09-28).
	// ENFORCED BY: Airside.Tool.StandPlot.StartsAtTheKerb
	if (FrontGap > 0.0 && Shown.Num() >= 2)
	{
		const FVector2D K0 = Shown[0] - Inward * FrontGap;
		const FVector2D K1 = Shown[1] - Inward * FrontGap;
		Sink.Line(K0, K1, EPreviewStyle::Guide);
		Sink.Line(K0, Shown[0], EPreviewStyle::Guide);
		Sink.Line(K1, Shown[1], EPreviewStyle::Guide);
		Sink.Label((K0 + Shown[1]) * 0.5,
			FString::Printf(TEXT("clearance strip %.1f m"), FrontGap / 100.0), EPreviewStyle::Guide);
	}

	if (PinnedCount() < 2)
	{
		return;
	}

	// The two SIDE edges, provisional while the depth still follows the cursor. UNLIKE THE
	// DEPOT'S THREE EDGES, one style covers both: a rectangle's fourth corner is MECHANICALLY
	// DERIVED from the other three (Shape() above), never its own click, so it is exactly as
	// settled as they are the moment the depth is pinned - it has no independent freedom the
	// way the depot's own near corner does before its own fourth click.
	const int32 PinnedNow = PinnedCount();
	const EPreviewStyle Rest = PinnedNow >= 3 ? EPreviewStyle::Pinned : EPreviewStyle::Provisional;
	Sink.Line(Shown[1], Shown[2], Rest);

	// THE FAR EDGE (2->3): opposite the taxiway the entrance (0->1) opens off, where a service
	// vehicle now enters and leaves (far-side-entry spec §2). ServiceEdge, NOT Rest, because
	// this edge's meaning is service access whether the depth is still being dragged or locked
	// - unlike the two side edges, it is not ABOUT the gesture's own settledness.
	//
	// OFFSET OUT BY THE SERVICE ROAD'S HALF-WIDTH (user ruling 2026-09-27, "kerb on the edge"):
	// the line marks where the player draws the road's CENTRE, so its near kerb lies on the far
	// edge. Drawn on the edge itself, it invited a road whose near lane sat 150 uu INSIDE the
	// stand - short of the corner run every entry is inset by, so a C truck hard-joined the near
	// lane and the other entries crossed it to the far one (measured 2026-09-27). Outward is the
	// side edge's own direction, 1->2, which runs from the entrance to the far edge.
	const FVector2D Outward = (Shown[2] - Shown[1]).GetSafeNormal();
	const FVector2D Out = Outward * Context.ServiceRoadHalfWidth;
	Sink.Line(Shown[2] + Out, Shown[3] + Out, EPreviewStyle::ServiceEdge);
	Sink.Line(Shown[3], Shown[0], Rest);

	// WHAT THE DEPTH IS LINED UP WITH, while it is being dragged - the consumer of
	// DescribeGuideAnchor above. Without it the guide is computed and never seen, the shape of
	// the bug IBuildTool::WantsFreeStartGuides records. From the back corner the shape shows.
	//
	// THE BACK-EDGE WINNER ONLY: it is the one the depth obeys (BackEdgeGuide), and a dashed
	// "north" or "45 degrees to the taxiway" beside a shape it does not move is a label that lies.
	if (PinnedNow == 2)
	{
		if (const SnapGuide::FCandidate* BackEdge = StandPlotGuide::BackEdgeGuide(Context, Inward))
		{
			SnapGuide::FResult Obeyed;
			Obeyed.bActive = true;
			Obeyed.Winners.Add(*BackEdge);
			Obeyed.Point = Context.Guide.Point;
			Sink.Guides(Obeyed, Shown[2]);
		}
	}

	DescribeLetter(Context, Shown, Sink);

	// THE LETTER, OR WHY NOT, on the ground at the stand's centre - the same sentence the
	// readout warns with, from the facade's one evaluator, so the ghost and the bar agree.
	// Reached only from Depth on (the guard above), the same stage DescribeReadout's own guard
	// below withholds Why until - both read the SAME memo, so asking here first costs nothing
	// extra once DescribeReadout goes on to ask again.
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

	// THE STRIP, AND WHOSE IT IS: the taxiway's letter, not the stand's, sets it - a B stand off
	// an F taxiway pays F's 34.5 m, and the readout is where the player learns why.
	if (FrontGap > 0.0 && Context.Network() != nullptr)
	{
		// THROUGH ProfileFor (#459): read raw, a taxiway with no profile of its own showed no strip fact at all.
		const FRoadSegment* Road = Context.Network()->GetSegment(FrontRoad);
		if (const URoadProfile* Profile = Road != nullptr ? Context.Network()->ProfileFor(*Road) : nullptr)
		{
			Sink.Fact(TEXT("Strip"), FString::Printf(TEXT("%.1f m (Code %s taxiway)"), FrontGap / 100.0,
				IcaoCode::ToLetter(IcaoCode::TaxiwayLetterForWidth(Profile->GetTotalWidth()))));
		}
	}

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

double FStandPlotTool::FrontSetback(const URoadNetwork& Network, FRoadSegmentId Id) const
{
	// THE TAXIWAY'S STRIP, BY ITS OWN LETTER - not the stand's. The wing that overhangs the
	// stand edge belongs to what taxis past, so a Code B stand off a Code F taxiway pays F's
	// 34.5 m (strip spec 2026-09-28; the cost nudges the player to a B spur, as real aprons do).
	// ENFORCED BY: Airside.Tool.StandPlot.StartsAtTheKerb (its entrance is Kerb + the taxiway's StripWidthOf)
	return TaxiwayStrip::StripWidthOf(Network, Id);
}
