#include "Tool/PlotGesture.h"

#include "Model/RoadNetwork.h"
#include "Model/TaxiwayStrip.h"
#include "Profiles/RoadProfile.h"
#include "Solve/GridSnap.h"
#include "Solve/RoadGeom.h"
#include "Tool/RoadBuildTool.h"

namespace PlotGesture
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
		// THROUGH ProfileFor (#459): a road with no profile of its own is the network's default, not "no road".
		const FRoadSegment* Segment = Network.GetSegment(Id);
		const URoadProfile* Profile = Segment != nullptr ? Network.ProfileFor(*Segment) : nullptr;
		if (Profile == nullptr)
		{
			return false;
		}

		// ASKED OF THE GUIDELINES THE PROFILE DECLARES, because "a truck may drive here" is
		// exactly what a GroundVehicle guideline means - and it is the same question
		// FAnchorLink asks of the graph when it joins a depot's pose. A cross-section with
		// no such line is a taxiway however it is labelled.
		for (const FProfileGuideline& Guideline : Profile->Guidelines)
		{
			if (Guideline.Class == ETraversalClass::GroundVehicle)
			{
				return true;
			}
		}
		return false;
	}

	bool IsTaxiway(const URoadNetwork& Network, FRoadSegmentId Id)
	{
		// THE RULE LIVES IN Model/TaxiwayStrip (IsAircraftOnly) since 2026-09-28, so the strip
		// query - which Model/ owns and cannot reach Tool/ for - asks the same question the
		// stand tool anchors by. Its "BOTH HALVES" comment moved with it.
		return TaxiwayStrip::IsAircraftOnly(Network, Id);
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
		// THROUGH ProfileFor (#459): read raw, a road with no profile of its own had no kerb, and the plot
		// was built over half the carriageway - the very thing this function exists to stop.
		const FRoadSegment* Segment = Network.GetSegment(Id);
		const URoadProfile* Profile = Segment != nullptr ? Network.ProfileFor(*Segment) : nullptr;
		if (Profile == nullptr)
		{
			return 0.0;
		}
		return bLeftOfSegment ? Profile->GetHalfWidthLeft() : Profile->GetHalfWidthRight();
	}

	/**
	 * A frontage length quantised to the plot's own steps.
	 *
	 * ROUNDED, NOT FLOORED, which is the difference between a grid that feels magnetic and
	 * one that feels grudging: floored, the cursor must travel a whole further step before
	 * the plot grows, so it always lags behind the hand.
	 *
	 * NOT the shed's own 4 m width (PlotFit::BayWidthUu, retired by issue #182). That number
	 * used to mean the plot's step as well - one number doing two jobs, which is how a plot
	 * could be drawn narrower than anything that could stand in it.
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

	FVector2D FrontageEnd(const FVector2D& Anchor, const FVector2D& Along,
		const FVector2D& Cursor, const GridSnap::FGridFrame& Grid)
	{
		const double Reach = FVector2D::DotProduct(Cursor - Anchor, Along);
		const double Sign = Reach < 0.0 ? -1.0 : 1.0;
		if (Grid.IsOn())
		{
			// NO SHORTER THAN MinFrontageUu, the floor QuantisedFrontage keeps - a crossing
			// within half a step of the anchor used to pin a zero-length entrance, after which
			// the stand had no entrance direction to guide or grid its depth by (review,
			// 2026-09-27). Either way along, as the stepped frontage runs.
			constexpr double Unbounded = 1e12;
			const double TMin = Sign > 0.0 ? MinFrontageUu : -Unbounded;
			const double TMax = Sign > 0.0 ? Unbounded : -MinFrontageUu;
			FVector2D OnGrid = Anchor;
			if (GridSnap::NearestCrossingInRange(Anchor, Along, Cursor, Grid, TMin, TMax, OnGrid))
			{
				return OnGrid;
			}
		}
		return Anchor + Along * (Sign * QuantisedFrontage(FMath::Abs(Reach)));
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
		// FrontageStepUu, not the shed's 4 m width: an anchor on a 4 m grid under a frontage
		// growing in 5 m steps would let two plots drawn side by side never sit flush, which
		// is the entire reason the frontage has a quantum.
		const double AlongRoad = FMath::Clamp(SegmentT, 0.0, 1.0) * Length;
		const int32 Count = FMath::FloorToInt(Length / FrontageStepUu);
		return FMath::Clamp(FMath::RoundToInt(AlongRoad / FrontageStepUu), 0, Count);
	}

	/**
	 * How far along the road anchor N stands, uu.
	 *
	 * THE MULTIPLICATION LIVES HERE, once. Both callers used to do it themselves against the
	 * shed's own 4 m width (PlotFit::BayWidthUu, retired by issue #182), so moving the index
	 * to the 5 m step left them multiplying by 4 m - the anchor landed 2 km from the cursor
	 * and three tests failed for reasons that looked nothing like the cause. An index and its
	 * stride are one fact.
	 */
	double AnchorOffset(int32 Index)
	{
		return Index * FrontageStepUu;
	}

	/**
	 * The accepted road nearest the cursor - a service road, for the depot - and where along
	 * it the cursor falls.
	 *
	 * "SERVICE ROAD" IS NOW THE CALLER'S Accept, not a rule written in here: the depot tool
	 * passes IsServiceRoad and the stand tool IsTaxiway. The search is the same question
	 * either way - only which carriageway counts differs - and two copies of the loop would
	 * be two reaches that could drift apart.
	 *
	 * ASKED OF THE NETWORK, not read off FToolContext::Snap. That answers "what did the
	 * cursor HIT", which is only ever a segment while the cursor is over the carriageway -
	 * and this gesture is used from where the plot will stand, off the road entirely. A
	 * different question deserves its own search rather than a widened snap radius, which
	 * would change what every other tool's click means.
	 *
	 * NOT FAnchorLink, which was deleted for a related reason: it searched the LIVE graph to
	 * recover a placed depot's frontage, so a road laid later could move it. Searching at
	 * GESTURE time is the safe half - the answer is frozen into the outline at commit.
	 */
	bool NearestRoad(const URoadNetwork& Network, const FVector2D& Cursor,
		FRoadFilter Accept, FRoadSetback Setback, FRoadSegmentId& OutSegment, double& OutT)
	{
		// REACH IS MEASURED FROM THE FRONTAGE, not the centreline, since the clearance strip
		// (2026-09-28): a stand's anchor dots stand a strip beyond the kerb - 28 m on the
		// standard taxiway - and a reach counted from the road left the dots the player aims
		// at outside it, so a click on them anchored nothing
		// (Airside.Tool.StandPlot.FlushNeighboursBothPlace went red on exactly that). Roads
		// also RANK by that distance, so the road whose frontage is nearer wins.
		//
		// NOT CLAMPED AT ZERO: a cursor inside two taxiways' strips (a junction) is "negative"
		// from both frontages, and ranks by how far in. Clamped, both read 0 and array order
		// chose - the farther taxiway could win (review, 2026-09-28).
		// ENFORCED BY: Airside.Tool.StandGrid.NearerTaxiwayWinsInsideTwoStrips
		double Best = AnchorReachUu;
		bool bFound = false;

		const TArray<FRoadSegment>& Segments = Network.GetSegments();
		for (int32 Index = 0; Index < Segments.Num(); ++Index)
		{
			const FRoadSegment& Segment = Segments[Index];
			const FRoadSegmentId Id = Network.SegmentIdAt(Index);
			if (!Segment.bAlive || !Accept(Network, Id))
			{
				continue;
			}

			const FRoadNode* A = Network.GetNode(Segment.A);
			const FRoadNode* B = Network.GetNode(Segment.B);
			if (A == nullptr || B == nullptr)
			{
				continue;
			}

			const double T = RoadGeom::ClosestPointOnSegment(A->Position, B->Position, Cursor);
			const double FromFrontage =
				FVector2D::Distance(FMath::Lerp(A->Position, B->Position, T), Cursor) - Setback(Network, Id);
			if (FromFrontage > Best)
			{
				continue;
			}

			Best = FromFrontage;
			OutSegment = Id;
			OutT = T;
			bFound = true;
		}
		return bFound;
	}

	bool AnchorAt(const URoadNetwork& Network, const FVector2D& Cursor, FRoadFilter Accept, FRoadSetback Setback,
		FAnchor& Out, const GridSnap::FGridFrame& Grid)
	{
		// MOVED FROM FPlotPlaceTool::OnClick's Idle case when the stand tool needed the same
		// first click against a taxiway. ONE RULE, EVERY PLOT TOOL: a second copy of the grid
		// and the kerb offset is two plots, drawn side by side off two kinds of road, that
		// cannot sit flush - the reason the frontage has a quantum at all.
		//
		// FROM WHERE THE PLOT GOES, not from the carriageway. See NearestRoad for why this
		// searches rather than reading the driver's snap.
		FRoadSegmentId Road;
		double AlongT = 0.0;
		if (!NearestRoad(Network, Cursor, Accept, Setback, Road, AlongT))
		{
			return false;
		}

		FVector2D RoadA = FVector2D::ZeroVector;
		FVector2D RoadB = FVector2D::ZeroVector;
		if (!Network.SegmentEnds(Road, RoadA, RoadB))
		{
			return false;
		}

		const FVector2D Span = RoadB - RoadA;
		const double Length = Span.Size();
		if (Length <= 0.0)
		{
			return false;
		}

		FAnchor Anchor;
		Anchor.Road = Road;
		Anchor.AlongT = AlongT;
		Anchor.RoadA = RoadA;
		Anchor.RoadB = RoadB;
		Anchor.Along = Span / Length;

		// ANCHORED ON THE ROAD'S OWN BAY GRID, measured from the segment's A end. Quantising
		// per SEGMENT rather than globally means two plots on one segment sit flush and a
		// plot never straddles a junction - see the design doc's open question 1.
		Anchor.Corner = RoadA + Anchor.Along * AnchorOffset(AnchorIndexAt(AlongT, Length));

		// WHICH SIDE THE CURSOR IS ON, not a rule. A depot goes on the side of the road the
		// player is pointing at; the alternative is a fixed side that is wrong half the time
		// and cannot be argued with.
		const FVector2D Left = RoadGeom::PerpCCW(Anchor.Along);
		const double Side = FVector2D::DotProduct(Cursor - Anchor.Corner, Left);
		Anchor.Inward = Side >= 0.0 ? Left : -Left;

		// OFF THE CARRIAGEWAY, and only now that the side is known. Measured BEFORE this
		// step, because the side has to be read against the centreline the cursor was
		// judged from - offsetting first would tilt that test by half a road width.
		// ON THE KERB, NOT PAST THE STRIP (user, 2026-09-28: a first click out in the grass was
		// "slightly unintuitive"). The corner is where the player reaches for the road; a tool
		// with a clearance strip (the stand tool) opens its box a strip beyond it itself - see
		// FStagedPlotTool::FrontGap. Setback still widens the REACH, in NearestRoad.
		// ENFORCED BY: Airside.Tool.StandPlot.StartsAtTheKerb
		Anchor.Corner += Anchor.Inward * KerbOffset(Network, Road, Side >= 0.0);

		// THE GRID - WORLD OR TURNED TO THIS ROAD - REPLACES THE BAY GRID WHEN IT IS ON. The bay grid is phased from each
		// segment's A end, so two plots off two segments could not share a line (the 2026-09-27
		// report that started the world grid); a world crossing along the SAME kerb line keeps
		// the side and the kerb offset above and changes only where along it the corner sits.
		// Measured from the kerb line's own start so the segment bound below is a plain [0, L].
		if (Grid.IsOn())
		{
			const FVector2D KerbStart = Anchor.Corner - Anchor.Along * FVector2D::DotProduct(
				Anchor.Corner - RoadA, Anchor.Along);
			// THE NEAREST CROSSING ON THE SEGMENT, not the nearest anywhere: a cursor beside a
			// segment's end whose nearest line falls just past it takes the last line on the
			// road instead of refusing (review, 2026-09-27). Refused only when the segment is
			// shorter than a step and holds no line at all.
			FVector2D OnGrid = Anchor.Corner;
			if (!GridSnap::NearestCrossingInRange(KerbStart, Anchor.Along, Cursor, Grid, 0.0, Length, OnGrid))
			{
				return false;
			}
			Anchor.Corner = OnGrid;
		}

		// WRITTEN ONLY ON SUCCESS, so a caller that ignores the return still holds the
		// anchor it had - CLAUDE.md's out-parameter rule.
		Out = Anchor;
		return true;
	}

	bool DescribeAnchors(const URoadNetwork& Network, const FVector2D& Cursor, FRoadFilter Accept, FRoadSetback Setback,
		IToolPreviewSink& Sink, const GridSnap::FGridFrame& Grid)
	{
		// The anchors the player could take, so the grid is visible before it is committed
		// to. Snap style: these are what the gesture would attach to.
		//
		// AnchorAt'S QUESTION FIRST, so "is there an anchor here" has one answer: the tools'
		// readouts ask AnchorAt, and this used to answer yes on its own for a zero-length
		// segment or one whose ends could not be read - drawing a dot the click then refused,
		// while the bar said "move near a road" (review round 1).
		//
		// ONE SEARCH, NOT TWO - issue #302. This used to call NearestRoad a second time here
		// for the segment and T that AnchorAt had just resolved, an O(segments) search paid for
		// twice on every Idle-stage hover frame; Anchor.Road/AlongT/RoadA/RoadB are that same
		// search's own answer, carried out on FAnchor rather than thrown away.
		FAnchor Anchor;
		if (!AnchorAt(Network, Cursor, Accept, Setback, Anchor, Grid))
		{
			return false;
		}

		// ON THE GRID THE ONE ANCHOR IS DRAWN, not the segment's row of bay dots: those
		// are the bay grid's slots, and drawing them while the click takes a world crossing is
		// the preview disagreeing with the click. The overlay already draws the lines.
		if (Grid.IsOn())
		{
			Sink.Marker(Anchor.Corner, EPreviewStyle::Pending);
			return true;
		}

		const FVector2D Span = Anchor.RoadB - Anchor.RoadA;
		const double Length = Span.Size();
		const FVector2D Unit = Span.GetSafeNormal();

		// THE DOTS STAND WHERE THE CORNER WILL, off the kerb on the side the cursor
		// is on - not on the centreline they are derived from. A dot you aim at and
		// a corner that lands half a road away is the preview disagreeing with the
		// click, which is the one thing this codebase will not have.
		const FVector2D Left = RoadGeom::PerpCCW(Unit);
		const bool bLeft = FVector2D::DotProduct(Cursor - Anchor.RoadA, Left) >= 0.0;
		const FVector2D Offset = (bLeft ? Left : -Left) * KerbOffset(Network, Anchor.Road, bLeft);

		// THE ONE A CLICK WOULD TAKE IS DRAWN DIFFERENTLY. A row of identical dots
		// says where anchors exist; it does not say which one the cursor has. Pending
		// is the style every other tool uses for "this is what the click does", and
		// it double-rings, so the chosen point reads at a glance.
		const int32 Chosen = AnchorIndexAt(Anchor.AlongT, Length);

		const int32 Count = FMath::FloorToInt(Length / FrontageStepUu);
		for (int32 I = 0; I <= Count; ++I)
		{
			Sink.Marker(Anchor.RoadA + Unit * AnchorOffset(I) + Offset,
				I == Chosen ? EPreviewStyle::Pending : EPreviewStyle::Snap);
		}
		return true;
	}
}
