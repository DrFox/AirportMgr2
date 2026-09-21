#include "Tool/SnapGuideChain.h"

#include "Entities/EntityDefinition.h"
#include "Model/RoadEntity.h"
#include "Model/RoadApron.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Solve/RoadGeom.h"
#include "Tool/RoadNaming.h"
#include "Tool/SnapGuideLabel.h"

namespace
{
	// STAGE 3 HAS HAPPENED (#192): the SegmentEnds this file and FPlotPlaceTool.cpp each kept a
	// PREFIXED copy of - the unity build made a plain name collide the moment both landed in one
	// blob - now has one home, URoadNetwork::SegmentEnds. Both call sites use it.

	/** The point on segment A-B nearest P. Clamped to the segment, not to its infinite line. */
	FVector2D ClosestOn(const FVector2D& A, const FVector2D& B, const FVector2D& P)
	{
		return FMath::Lerp(A, B, RoadGeom::ClosestPointOnSegment(A, B, P));
	}

	/**
	 * The three spokes a reference's END throws off: 45, 90 and 135 degrees to it.
	 *
	 * NOT 0, which is the line the reference itself lies on - that is Collinear, and offering it
	 * here as well would be two relations naming one line. NOT the reflections either: a guide is
	 * a LINE and SnapGuide::Arbitrate measures the ACUTE angle, so 225 degrees is the same line
	 * as 45 - the same reason FWorldGuideSource proposes four directions rather than eight.
	 *
	 * LABEL, NOT NAME - #183. This used to take the already-resolved FString and Printf it three
	 * times per call; now it takes the un-formatted RECIPE (which segment, or the apron) and
	 * copies it into each spoke's Label, filling in only what varies (Kind, Degrees). Nothing
	 * here allocates - see FGuideLabel.
	 */
	void AddSpokes(const FVector2D& End, const FVector2D& Along, SnapGuide::EReference Reference,
		const SnapGuide::FGuideLabel& Subject, TArray<SnapGuide::FCandidate>& Out)
	{
		for (const int32 Degrees : { 45, 90, 135 })
		{
			const double Radians = FMath::DegreesToRadians(static_cast<double>(Degrees));
			const double Cos = FMath::Cos(Radians);
			const double Sin = FMath::Sin(Radians);

			SnapGuide::FCandidate Spoke;

			// ROTATED FROM THE REFERENCE'S OWN DIRECTION, not from the world: "45 degrees to the
			// taxiway" is a fact about that taxiway, and a world-relative angle would read the
			// same on screen while pointing somewhere else on any road that is not axis-aligned.
			Spoke.Direction = FVector2D(Along.X * Cos - Along.Y * Sin, Along.X * Sin + Along.Y * Cos);

			// THROUGH THE END ITSELF, which is the whole of what this relation means. The drag's
			// own origin is nowhere on it, so the fit is Perpendicular: it answers where the
			// cursor ENDED UP, not which way it set off - the same split PointAlign made.
			Spoke.Through = End;

			// AND THE DASHED LINE GOES TO THAT SAME END, so the player can see WHICH one it
			// radiates from. Spokes off the two ends are parallel lines a segment apart and the
			// label cannot tell them apart; only the drawn line can.
			Spoke.ReferenceAt = End;
			Spoke.Fit = SnapGuide::EFit::Perpendicular;
			Spoke.Relation = SnapGuide::ERelation::AngledFrom;
			Spoke.Reference = Reference;
			// "FROM THE END OF", not "to", and the wording is load-bearing since 2026-09-20.
			// The Direction row gained its own 45 and 135 that day, so "45 degrees to the
			// taxiway" would have named two different guides - and BOTH can hold at once,
			// because they are different fit kinds and the arbiter takes one winner of each.
			// Two identical labels on two lines pointing at two places is the mark whose
			// meaning has gone. This one says the thing that makes it itself: it comes out of
			// the reference's END.
			Spoke.Label = Subject;
			Spoke.Label.Kind = SnapGuide::ELabelKind::AngledFromEnd;
			Spoke.Label.Degrees = Degrees;
			Out.Add(Spoke);
		}
	}

	/**
	 * The four directions a reference offers: along it, square to it, and the two diagonals.
	 *
	 * 45 DEGREE INCREMENTS, LIKE THE WORLD GRID - added 2026-09-20 from PIE: "it should have
	 * more options than square and parallel... the 45 degree increments should be consistent
	 * for direction." They were not. World offered 0/45/90/135 while every other column in
	 * the row offered 0 and 90 only, so one button meant two different things depending on
	 * which column it was crossed with. ONE HELPER now, so a fifth reference cannot quietly
	 * be given two of the four.
	 *
	 * THROUGH THE DRAG'S OWN ORIGIN, AND ANGULAR, which is what makes this the Direction row
	 * and not AngledFrom. That distinction is why both exist: this answers "which way am I
	 * heading", judged on the direction the drag set off; AngledFrom answers "am I standing on
	 * the line out of THAT road's end", judged on where the cursor landed.
	 *
	 * ALONGVERB, because a stand does not read like a road: "aligned with stand 3" and
	 * "parallel to the taxiway" are one relation in two sets of words, and the caller is the
	 * only thing that knows which its own reference wants.
	 *
	 * LABEL, NOT NAME - #183, the same change AddSpokes got: Subject is the un-formatted recipe,
	 * copied into each of the four candidates with only Kind (and Degrees, on the diagonals) set
	 * per iteration. AlongVerb is still passed as a raw literal pointer, never copied into a
	 * heap string - FGuideLabel::Verb is exactly that pointer.
	 */
	void AddDirections(const FVector2D& Along, const FVector2D& Origin,
		const FVector2D& ReferenceAt, SnapGuide::EReference Reference, const TCHAR* AlongVerb,
		const SnapGuide::FGuideLabel& Subject, TArray<SnapGuide::FCandidate>& Out)
	{
		for (const int32 Degrees : { 0, 45, 90, 135 })
		{
			const double Radians = FMath::DegreesToRadians(static_cast<double>(Degrees));
			const double Cos = FMath::Cos(Radians);
			const double Sin = FMath::Sin(Radians);

			SnapGuide::FCandidate Candidate;

			// ROTATED FROM THE REFERENCE'S OWN DIRECTION, not from the world - the same
			// arithmetic AddSpokes uses and for the same reason: "45 degrees to the taxiway"
			// is a fact about that taxiway, and a world-relative angle would read identically
			// on screen while pointing somewhere else on any road that is not axis-aligned.
			//
			// AT 90 THIS IS EXACTLY RoadGeom::PerpCCW, which is what each caller used before
			// the four were folded into one loop - so the square they already offered is the
			// same line, not a second way of computing it.
			Candidate.Direction = FVector2D(Along.X * Cos - Along.Y * Sin, Along.X * Sin + Along.Y * Cos);
			Candidate.Through = Origin;
			Candidate.Fit = SnapGuide::EFit::Angular;
			Candidate.ReferenceAt = ReferenceAt;
			Candidate.Relation = SnapGuide::ERelation::Parallel;
			Candidate.Reference = Reference;

			// THE TWO NAMED ANGLES KEEP THEIR WORDS. "parallel to the taxiway" and "square to
			// the taxiway" are what a player has read since stage 1, and are plainer than "0
			// degrees" and "90 degrees" would be. Only the diagonals, which have no such word,
			// fall back to a number.
			Candidate.Label = Subject;
			if (Degrees == 0)
			{
				Candidate.Label.Kind = SnapGuide::ELabelKind::Along;
				Candidate.Label.Verb = AlongVerb;
			}
			else if (Degrees == 90)
			{
				Candidate.Label.Kind = SnapGuide::ELabelKind::SquareTo;
			}
			else
			{
				Candidate.Label.Kind = SnapGuide::ELabelKind::DegreesTo;
				Candidate.Label.Degrees = Degrees;
			}
			Out.Add(Candidate);
		}
	}

	/**
	 * Every live apron edge in reach of the drag, as (A, B, Along).
	 *
	 * ONE WALK, FOUR SOURCES. The apron column needs a source per relation - see
	 * FApronGuideSource - and four copies of this loop is four places for the wrap-around from
	 * the last corner back to the first to be got wrong.
	 */
	void ForEachApronEdge(const URoadNetwork& Network, const FVector2D& Origin,
		const SnapGuide::FTuning& Tuning,
		TFunctionRef<void(const FVector2D&, const FVector2D&, const FVector2D&)> Visit)
	{
		const double Reach = Tuning.SearchRadiusUu;

		for (const FApronSurface& Apron : Network.GetAprons())
		{
			if (!Apron.bAlive || Apron.Outline.Num() < 3)
			{
				continue;
			}

			for (int32 Index = 0; Index < Apron.Outline.Num(); ++Index)
			{
				// WRAPPING, so the last corner joins the first: an outline is a closed polygon,
				// and the edge that closes it is as real as any other.
				const FVector2D& A = Apron.Outline[Index];
				const FVector2D& B = Apron.Outline[(Index + 1) % Apron.Outline.Num()];

				const FVector2D Span = B - A;
				if (Span.IsNearlyZero()
					|| FVector2D::DistSquared(ClosestOn(A, B, Origin), Origin) > Reach * Reach)
				{
					continue;
				}
				Visit(A, B, Span.GetSafeNormal());
			}
		}
	}

	/**
	 * Which end of the gesture a source's reach is measured from.
	 *
	 * NAMED RATHER THAN INLINE so each call site SAYS which it wants: passing Anchor.Origin or
	 * Cursor directly reads identically at a glance, and the difference is the whole of the
	 * 2026-09-20 matching-gap report. See IGuideSource::Propose.
	 */
	const FVector2D& WhichWayFromHere(const FGuideAnchor& Anchor, const FVector2D&)
	{
		return Anchor.Origin;
	}

	const FVector2D& WhereTheFarEndLanded(const FGuideAnchor&, const FVector2D& Cursor)
	{
		return Cursor;
	}

	/**
	 * One of the four world axes: the compass bearing it lies on, and what a player reads.
	 *
	 * ONE TABLE, NOT A LOOP OVER ANGLES BESIDE A SWITCH THAT NAMES THEM. The number and the
	 * word are one fact about one axis, and two lists are two things to keep in step - see
	 * CLAUDE.md on lists that must agree being one list.
	 *
	 * NAMED AS AN AXIS, BOTH ENDS. A guide is a LINE: SnapGuide::Arbitrate measures the ACUTE
	 * angle, so a candidate and its opposite are one guide, and "north" would name a ray the
	 * player may equally well drag the other way along. "north-south" names what is actually
	 * being offered.
	 *
	 * WORDS RATHER THAN THE NUMBER, changed 2026-09-20 on the question "should angled from on
	 * world give a cardinal direction". The numbers were ALREADY cardinal - "0 degrees" was
	 * north and "90" was east, agreeing with the runway designators - but nothing said so, and
	 * they sat on screen beside "45 degrees to the taxiway", which is an angle measured from
	 * THAT ROAD. One frame absolute, one relative, in the same words. The bearing stays here
	 * because the direction is computed from it, and because it is the tie to RunwayDesignator.
	 */
	struct FWorldAxis
	{
		/** Compass: clockwise from north, which is +X. See FWorldGuideSource::Propose. */
		int32 Bearing;
		const TCHAR* Name;
	};

	const FWorldAxis WorldAxes[] = {
		{ 0,   TEXT("north-south") },
		{ 45,  TEXT("northeast-southwest") },
		{ 90,  TEXT("east-west") },
		{ 135, TEXT("northwest-southeast") },
	};

	/**
	 * Which ROAD column a segment belongs to - Taxiway or ServiceRoad. False for a runway, and
	 * false for a segment that is not live.
	 *
	 * ONE CALL ANSWERS BOTH QUESTIONS the road sources used to ask separately: "is this a
	 * runway, which another source owns" and "what is it, for the label". They were
	 * IsRunwaySegment at the top of each loop and RoadNaming::Describe at the bottom, and the
	 * column between them was a hard-coded EReference::Road that no longer exists.
	 *
	 * THE RUNWAY ANSWER IS A REFUSAL HERE, NOT A VALUE, because that is what every caller does
	 * with it: `continue`. FRunwayGuideSource and FRunwayLineGuideSource own runways and own
	 * them with a different reach - unbounded, since an airport squares to its runways from
	 * anywhere on it - so a road source that walked one would put two near-identical candidates
	 * in the same race and let a runway answer while the Runway column was switched off. That
	 * is the 2026-09-20 report, and this is where it stays fixed.
	 *
	 * PREFIXED: the tests module and this one are unity builds, and "RoadColumnOf" is a name a
	 * second file would also pick.
	 */
	bool GuideRoadColumn(const URoadNetwork& Network, FRoadSegmentId Id,
		SnapGuide::EReference& Out)
	{
		return RoadNaming::ReferenceOf(Network, Id, Out)
			&& Out != SnapGuide::EReference::Runway;
	}

	/**
	 * Walks URoadNetwork::GetSegments() once, gated to the Road columns (Taxiway/ServiceRoad,
	 * bounded by Tuning.SearchRadiusUu) or the Runway column (unbounded), and calls Visit for
	 * every segment that clears the column and reach gates - #192. This IS the prologue
	 * FSegmentGuideSource::Propose runs for its five children, and FParallelGuideSource calls
	 * it directly for the same walk with a different reduction on top - see both headers.
	 *
	 * WhichWayFromHere/WhereTheFarEndLanded, not a raw ternary: they were written the day the
	 * Origin/Cursor split shipped and never wired to a shared walker until this one existed -
	 * naming the choice at the call site is the whole of what IGuideSource::Propose argues for.
	 */
	void WalkGuideSegments(const URoadNetwork& Network, const FGuideAnchor& Anchor,
		const FVector2D& Cursor, bool bRunwayColumn, EGuideReachFrom ReachFrom,
		const SnapGuide::FTuning& Tuning,
		TFunctionRef<void(FRoadSegmentId, const FVector2D&, const FVector2D&, SnapGuide::EReference)> Visit)
	{
		const double Reach = Tuning.SearchRadiusUu;

		const TArray<FRoadSegment>& Segments = Network.GetSegments();
		for (int32 Index = 0; Index < Segments.Num(); ++Index)
		{
			const FRoadSegmentId Id = Network.SegmentIdAt(Index);

			// WHICH COLUMN, AND (for the Road walk) RUNWAYS REFUSED, in one question - see
			// GuideRoadColumn. The Runway walk asks Network.IsRunwaySegment directly instead:
			// GuideRoadColumn's whole job is refusing runways, so it has nothing to say to a
			// caller that wants only them.
			SnapGuide::EReference Column = SnapGuide::EReference::Runway;
			if (bRunwayColumn)
			{
				if (!Network.IsRunwaySegment(Id))
				{
					continue;
				}
			}
			else if (!GuideRoadColumn(Network, Id, Column))
			{
				continue;
			}

			FVector2D A = FVector2D::ZeroVector;
			FVector2D B = FVector2D::ZeroVector;
			if (!Network.SegmentEnds(Id, A, B))
			{
				continue;
			}

			const FVector2D Span = B - A;
			if (Span.IsNearlyZero())
			{
				continue;
			}

			if (ReachFrom != EGuideReachFrom::None)
			{
				const FVector2D& From = (ReachFrom == EGuideReachFrom::Origin)
					? WhichWayFromHere(Anchor, Cursor) : WhereTheFarEndLanded(Anchor, Cursor);
				if (FVector2D::DistSquared(ClosestOn(A, B, From), From) > Reach * Reach)
				{
					continue;
				}
			}

			Visit(Id, A, B, Column);
		}
	}

}

void FExtendingGuideSource::Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
	const FVector2D& Cursor, TArray<SnapGuide::FCandidate>& Out,
	const SnapGuide::FTuning& Tuning) const
{
	if (Anchor.Reference.IsNearlyZero())
	{
		return;
	}

	const FVector2D Along = Anchor.Reference.GetSafeNormal();

	SnapGuide::FCandidate Parallel;
	Parallel.Direction = Along;

	// THROUGH THE DRAG'S OWN ORIGIN - this source answers "which way from here", so the line
	// it means is the one out of the corner being dragged. Filled explicitly since 2026-09-17:
	// it used to be implicit in the arbiter, until PointAlign arrived with lines that pass
	// nowhere near the origin.
	Parallel.Through = Anchor.Origin;
	Parallel.Fit = SnapGuide::EFit::Angular;
	Parallel.ReferenceAt = Anchor.ReferenceAt;
	// GestureReference NEEDS NO INDEX - the anchor carries exactly one ReferenceName, unlike
	// AlignTo's several points. See ELabelSubject.
	Parallel.Label.Kind = SnapGuide::ELabelKind::Along;
	Parallel.Label.Subject = SnapGuide::ELabelSubject::GestureReference;
	Parallel.Label.Verb = TEXT("along");
	Parallel.Relation = SnapGuide::ERelation::Extending;
	Parallel.Reference = SnapGuide::EReference::ThisGesture;
	Out.Add(Parallel);

	// THE PERPENDICULAR IS THE ONE THAT SQUARES A PLOT, and it is proposed from the same
	// reference rather than by a second source, because it is the same fact about the same
	// edge - see design section 3's "the incoming segment's direction, and its perpendicular".
	// RoadGeom::PerpCCW rather than a hand-written (-y, x): the sign convention is stated
	// once in this codebase and this is not the place to restate it.
	SnapGuide::FCandidate Square = Parallel;
	Square.Direction = RoadGeom::PerpCCW(Along);
	Square.Label.Kind = SnapGuide::ELabelKind::SquareTo;
	Out.Add(Square);
}

void FWorldGuideSource::Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
	const FVector2D& Cursor, TArray<SnapGuide::FCandidate>& Out,
	const SnapGuide::FTuning& Tuning) const
{
	for (const FWorldAxis& Axis : WorldAxes)
	{
		SnapGuide::FCandidate Candidate;
		const double Radians = FMath::DegreesToRadians(static_cast<double>(Axis.Bearing));

		// (NORTH, EAST), NOT (X-FROM-A-MATHS-ANGLE, Y). RunwayDesignator declares north to be
		// +X and east +Y, so cos of a COMPASS bearing is its northing and sin its easting -
		// which is why Bearing below is the same number a runway is named from, and why the
		// two namespaces cannot drift apart. Airside.Tool.WorldAxesAreNamedByTheCompass holds
		// them together by measuring one against the other.
		Candidate.Direction = FVector2D(FMath::Cos(Radians), FMath::Sin(Radians));

		// Through the origin, like Extending: a world axis is still "which way from here".
		Candidate.Through = Anchor.Origin;
		Candidate.Fit = SnapGuide::EFit::Angular;

		// THE LINE GOES BACK TO THE POINT IT SWINGS AROUND. The world grid is not a thing on
		// the map to point at, and a dashed line shot off to nowhere would say less than one
		// that says "this is the corner you are square from". The label carries the rest.
		Candidate.ReferenceAt = Anchor.Origin;
		Candidate.Label.Kind = SnapGuide::ELabelKind::Literal;
		Candidate.Label.Text = Axis.Name;
		Candidate.Relation = SnapGuide::ERelation::Parallel;
		Candidate.Reference = SnapGuide::EReference::World;
		Out.Add(Candidate);
	}
}

void FPointAlignGuideSource::Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
	const FVector2D& Cursor, TArray<SnapGuide::FCandidate>& Out,
	const SnapGuide::FTuning& Tuning) const
{
	if (Anchor.Reference.IsNearlyZero())
	{
		return;
	}

	const FVector2D Along = Anchor.Reference.GetSafeNormal();
	const FVector2D Across = RoadGeom::PerpCCW(Along);

	// INDEXED, NOT A RANGE-FOR - #183. A GesturePoint label carries an INDEX into AlignTo rather
	// than a copy of Point.Name, so Describe can look the name up again later for the (at most
	// two) winners instead of every point-times-two candidate paying for it now.
	for (int32 Index = 0; Index < Anchor.AlignTo.Num(); ++Index)
	{
		const FGuidePoint& Point = Anchor.AlignTo[Index];

		// TWO LINES PER POINT, along the reference and across it. "0 degrees to corner 3" is
		// the one that makes a rectangle out of a plot; the perpendicular is what says the two
		// back corners sit above one another.
		//
		// THE LINE GOES THROUGH THE POINT, and the dashed line is drawn TO it - which is the
		// same value here, unlike Extending where the reference edge has two different ends.
		SnapGuide::FCandidate Level;
		Level.Direction = Along;
		Level.Through = Point.At;
		Level.Fit = SnapGuide::EFit::Perpendicular;
		Level.ReferenceAt = Point.At;
		Level.Label.Kind = SnapGuide::ELabelKind::DegreesTo;
		Level.Label.Degrees = 0;
		Level.Label.Subject = SnapGuide::ELabelSubject::GesturePoint;
		Level.Label.SubjectIndex = Index;
		Level.Relation = SnapGuide::ERelation::LevelWith;

		// THE POINT'S OWN COLUMN, not this source's. One source serves both the gesture's
		// corners and the network's nodes, and the tool is the only thing that knows which is
		// which - see FGuidePoint::Reference.
		Level.Reference = Point.Reference;
		Out.Add(Level);

		SnapGuide::FCandidate Square = Level;
		Square.Direction = Across;
		Square.Label.Kind = SnapGuide::ELabelKind::SquareTo;
		Out.Add(Square);
	}
}

void FParallelGuideSource::Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
	const FVector2D& Cursor, TArray<SnapGuide::FCandidate>& Out,
	const SnapGuide::FTuning& Tuning) const
{
	// NOT ONE OF FSegmentGuideSource's CHILDREN - see that struct's header. This reduces
	// WalkGuideSegments' walk to a single NEAREST match rather than emitting per segment: every
	// road proposing would put the whole field in the race, and the winner would be decided by
	// a road the player cannot see - see FTuning::SearchRadiusUu.
	FRoadSegmentId Nearest;
	FVector2D NearestAt = FVector2D::ZeroVector;
	FVector2D NearestDir = FVector2D::ZeroVector;
	SnapGuide::EReference NearestColumn = SnapGuide::EReference::Taxiway;
	double BestSquared = TNumericLimits<double>::Max();

	// MEASURED FROM THE ORIGIN, not from the cursor: the road the gesture STARTED beside is the
	// one it is being drawn parallel to, and a search keyed to the cursor would hand the guide
	// to a different road halfway through the drag. WalkGuideSegments' own reach test already
	// excludes anything beyond Tuning.SearchRadiusUu of the origin, so every segment this
	// visitor sees is already in reach and "smallest Squared wins" is the whole of "nearest".
	WalkGuideSegments(Network, Anchor, Cursor, /*bRunwayColumn=*/false, EGuideReachFrom::Origin,
		Tuning,
		[&Anchor, &Nearest, &NearestAt, &NearestDir, &NearestColumn, &BestSquared](
			FRoadSegmentId Id, const FVector2D& A, const FVector2D& B, SnapGuide::EReference Column)
		{
			const FVector2D On = ClosestOn(A, B, Anchor.Origin);
			const double Squared = FVector2D::DistSquared(On, Anchor.Origin);
			if (Squared > BestSquared)
			{
				return;
			}

			BestSquared = Squared;
			Nearest = Id;
			NearestAt = On;
			NearestDir = (B - A).GetSafeNormal();

			// CARRIED FROM THE WINNER, not asked again at the bottom. The nearest road is picked
			// once and its column is a fact about THAT segment; a second classification call
			// after the loop would be a second chance to pick a different one.
			NearestColumn = Column;
		});

	if (NearestDir.IsNearlyZero())
	{
		return;
	}

	// THE COLUMN AND THE LABEL COME FROM ONE CLASSIFICATION, which is the whole of the
	// 2026-09-20 split: this used to hard-code EReference::Road while Describe said "the
	// service road", so the line appeared under a button marked Road. RoadNaming answers both.
	//
	// THE NAME ITSELF IS NOT RESOLVED HERE - #183. RoadNaming::Describe used to run once per
	// Propose call regardless of whether this source's four candidates went on to win; now the
	// segment's array index travels in the label and SnapGuide::Describe resolves it only if one
	// wins - see FGuideLabel::SegmentIndex on why an index, never the handle itself.
	SnapGuide::FGuideLabel Subject;
	Subject.Subject = SnapGuide::ELabelSubject::Segment;
	Subject.SegmentIndex = Nearest.Index;
	AddDirections(NearestDir, Anchor.Origin, NearestAt, NearestColumn, TEXT("parallel to"),
		Subject, Out);
}

void FSegmentGuideSource::Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
	const FVector2D& Cursor, TArray<SnapGuide::FCandidate>& Out,
	const SnapGuide::FTuning& Tuning) const
{
	WalkGuideSegments(Network, Anchor, Cursor, bRunwayColumn, ReachFromPoint, Tuning,
		[this, &Anchor, &Cursor, &Out](FRoadSegmentId Id, const FVector2D& A, const FVector2D& B,
			SnapGuide::EReference Column)
		{
			EmitForSegment(Id, A, B, Column, Anchor, Cursor, Out);
		});
}

void FCollinearGuideSource::EmitForSegment(FRoadSegmentId Id, const FVector2D& A,
	const FVector2D& B, SnapGuide::EReference Column, const FGuideAnchor& /*Anchor*/,
	const FVector2D& Cursor, TArray<SnapGuide::FCandidate>& Out) const
{
	// THROUGH THE SEGMENT'S OWN END, which is what makes this the line the road LIES ON rather
	// than one through the drag. The arbiter measures the cursor's distance from that line, so
	// the candidate is eligible exactly when the cursor is on the road's extension - however
	// far along it the drag has gone.
	SnapGuide::FCandidate InLine;
	InLine.Direction = (B - A).GetSafeNormal();
	InLine.Through = A;
	InLine.Fit = SnapGuide::EFit::Perpendicular;
	InLine.Relation = SnapGuide::ERelation::Collinear;

	// PER SEGMENT, not per source. One walk of the graph passes a taxiway and a service road in
	// the same pass, and the two answer to different buttons since 2026-09-20.
	InLine.Reference = Column;
	InLine.Label.Kind = SnapGuide::ELabelKind::InLineWith;
	InLine.Label.Subject = SnapGuide::ELabelSubject::Segment;
	InLine.Label.SegmentIndex = Id.Index;

	// THE DASHED LINE GOES TO THE ROAD ITSELF, not to the point on its extension where the
	// cursor happens to be: the player needs to see WHICH road they are in line with, and the
	// near end of it is the part they can recognise.
	InLine.ReferenceAt = ClosestOn(A, B, Cursor);
	Out.Add(InLine);
}

void FRunwayGuideSource::EmitForSegment(FRoadSegmentId Id, const FVector2D& A,
	const FVector2D& B, SnapGuide::EReference Column, const FGuideAnchor& Anchor,
	const FVector2D& /*Cursor*/, TArray<SnapGuide::FCandidate>& Out) const
{
	// NO REACH TEST, and that one absence is the only thing separating this source from
	// Parallel - see the declaration for why it is deliberate. EVERY MATCHING SEGMENT EMITS,
	// unlike Parallel: Design section 3 says "every runway's heading" and means it, which only
	// reads as one number when Parallel's OWN reach keeps the taxiway race small enough that
	// "nearest" is the right question to ask instead.
	SnapGuide::FGuideLabel Subject;
	Subject.Subject = SnapGuide::ELabelSubject::Segment;
	Subject.SegmentIndex = Id.Index;
	AddDirections((B - A).GetSafeNormal(), Anchor.Origin, ClosestOn(A, B, Anchor.Origin),
		Column, TEXT("parallel to"), Subject, Out);
}

void FRunwayLineGuideSource::EmitForSegment(FRoadSegmentId Id, const FVector2D& A,
	const FVector2D& B, SnapGuide::EReference Column, const FGuideAnchor& /*Anchor*/,
	const FVector2D& Cursor, TArray<SnapGuide::FCandidate>& Out) const
{
	// NO REACH TEST, like FRunwayGuideSource and unlike FCollinearGuideSource: a runway's
	// extended centreline is the approach path, and it is meaningful from anywhere.
	//
	// THROUGH THE RUNWAY'S OWN END, not through the drag - that is what makes this the line the
	// runway LIES ON rather than one out of the cursor, and why it is Perpendicular where
	// FRunwayGuideSource's two are Angular.
	SnapGuide::FCandidate InLine;
	InLine.Direction = (B - A).GetSafeNormal();
	InLine.Through = A;
	InLine.Fit = SnapGuide::EFit::Perpendicular;

	// THE DASHED LINE GOES TO THE RUNWAY ITSELF, not to the point on its extension where the
	// cursor happens to be: the player needs to see WHICH runway they are in line with.
	InLine.ReferenceAt = ClosestOn(A, B, Cursor);
	InLine.Relation = SnapGuide::ERelation::Collinear;
	InLine.Reference = Column;
	InLine.Label.Kind = SnapGuide::ELabelKind::InLineWith;
	InLine.Label.Subject = SnapGuide::ELabelSubject::Segment;
	InLine.Label.SegmentIndex = Id.Index;
	Out.Add(InLine);
}

void FAngledRoadGuideSource::EmitForSegment(FRoadSegmentId Id, const FVector2D& A,
	const FVector2D& B, SnapGuide::EReference Column, const FGuideAnchor& /*Anchor*/,
	const FVector2D& /*Cursor*/, TArray<SnapGuide::FCandidate>& Out) const
{
	const FVector2D Along = (B - A).GetSafeNormal();
	SnapGuide::FGuideLabel Subject;
	Subject.Subject = SnapGuide::ELabelSubject::Segment;
	Subject.SegmentIndex = Id.Index;

	// BOTH ENDS - see this source's own header for why neither may be picked for the player -
	// and both under the SEGMENT'S OWN column, so "45 degrees to the service road" answers to
	// the ServiceRoad button and not to the Taxiway one.
	AddSpokes(A, Along, Column, Subject, Out);
	AddSpokes(B, Along, Column, Subject, Out);
}

void FAngledRunwayGuideSource::EmitForSegment(FRoadSegmentId Id, const FVector2D& A,
	const FVector2D& B, SnapGuide::EReference Column, const FGuideAnchor& /*Anchor*/,
	const FVector2D& /*Cursor*/, TArray<SnapGuide::FCandidate>& Out) const
{
	// NO REACH TEST, like every other source in the Runway column: a rapid-exit taxiway is laid
	// from wherever the player is standing, not only from beside the threshold.
	const FVector2D Along = (B - A).GetSafeNormal();
	SnapGuide::FGuideLabel Subject;
	Subject.Subject = SnapGuide::ELabelSubject::Segment;
	Subject.SegmentIndex = Id.Index;
	AddSpokes(A, Along, Column, Subject, Out);
	AddSpokes(B, Along, Column, Subject, Out);
}

void FApronGuideSource::Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
	const FVector2D& Cursor, TArray<SnapGuide::FCandidate>& Out,
	const SnapGuide::FTuning& Tuning) const
{
	SnapGuide::FGuideLabel Subject;
	Subject.Subject = SnapGuide::ELabelSubject::ApronEdge;
	ForEachApronEdge(Network, Anchor.Origin, Tuning,
		[&Anchor, &Subject, &Out](const FVector2D& A, const FVector2D& B, const FVector2D& Along)
		{
			// ANGULAR, THROUGH THE DRAG'S OWN ORIGIN - this answers "which way from here", so
			// there is no position to be flush with and the half-width never applies.
			AddDirections(Along, Anchor.Origin, ClosestOn(A, B, Anchor.Origin),
				SnapGuide::EReference::Apron, TEXT("parallel to"), Subject, Out);
		});
}

void FApronLineGuideSource::Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
	const FVector2D& Cursor, TArray<SnapGuide::FCandidate>& Out,
	const SnapGuide::FTuning& Tuning) const
{
	ForEachApronEdge(Network, Cursor, Tuning,
		[&Anchor, &Cursor, &Out](const FVector2D& A, const FVector2D& B, const FVector2D& Along)
		{
			SnapGuide::FCandidate InLine;
			InLine.Direction = Along;
			InLine.Fit = SnapGuide::EFit::Perpendicular;
			InLine.ReferenceAt = ClosestOn(A, B, Cursor);
			InLine.Relation = SnapGuide::ERelation::Collinear;
			InLine.Reference = SnapGuide::EReference::Apron;
			InLine.Label.Subject = SnapGuide::ELabelSubject::ApronEdge;

			// FLUSH, NOT CENTRED. An apron edge is a BOUNDARY and a road's cursor is its
			// CENTRELINE, so lining the two up directly would put half the road's pavement over
			// the apron. Displacing by the half-width puts the road's EDGE on it, which is what
			// "in line with the apron" means to a player. Design section 6.
			//
			// A BOUNDARY DRAG IS NOT DISPLACED: an apron corner against another apron's edge is
			// boundary against boundary, and those already mean the same thing.
			const FVector2D Across = RoadGeom::PerpCCW(Along);
			const bool bCentreline = Anchor.Point == EDragPoint::Centreline;
			const double Left = bCentreline ? Anchor.HalfWidthLeft : 0.0;
			const double Right = bCentreline ? Anchor.HalfWidthRight : 0.0;

			if (FMath::IsNearlyZero(Left) && FMath::IsNearlyZero(Right))
			{
				// ONE LINE, NOT TWO COINCIDENT ONES. Identical candidates would tie and make the
				// source-order rule arbitrate a choice that does not exist.
				InLine.Through = A;
				InLine.Label.Kind = SnapGuide::ELabelKind::InLineWith;
				Out.Add(InLine);
				return;
			}

			// BOTH SIDES, and NOT a mirrored pair: GetHalfWidthLeft and GetHalfWidthRight are
			// separate because a cross-section may be off-centre. Flush-inside and flush-outside
			// are both real intents - a taxiway running along the apron, or one abutting it - so
			// neither may be chosen for the player.
			SnapGuide::FCandidate Near = InLine;
			Near.Through = A + Across * Left;
			Near.Label.Kind = SnapGuide::ELabelKind::EdgeFlushWith;
			Out.Add(Near);

			SnapGuide::FCandidate Far = InLine;
			Far.Through = A - Across * Right;
			Far.Label = Near.Label;
			Out.Add(Far);
		});
}

void FApronAngledGuideSource::Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
	const FVector2D& Cursor, TArray<SnapGuide::FCandidate>& Out,
	const SnapGuide::FTuning& Tuning) const
{
	// ONE END PER EDGE, not both: an outline is closed, so every corner is the A end of exactly
	// one edge. Visiting B as well would propose each corner's spokes twice - once per edge
	// meeting there - and a duplicate candidate is a tie the source order then has to break.
	SnapGuide::FGuideLabel Subject;
	Subject.Subject = SnapGuide::ELabelSubject::ApronEdge;
	ForEachApronEdge(Network, Cursor, Tuning,
		[&Subject, &Out](const FVector2D& A, const FVector2D& B, const FVector2D& Along)
		{
			AddSpokes(A, Along, SnapGuide::EReference::Apron, Subject, Out);
		});
}

void FApronCornerGuideSource::Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
	const FVector2D& Cursor, TArray<SnapGuide::FCandidate>& Out,
	const SnapGuide::FTuning& Tuning) const
{
	// NEEDS THE GESTURE'S OWN AXES, like FPointAlignGuideSource: "level with that corner" means
	// level ALONG the edge you are extending, so with no reference there is no axis to measure
	// against and nothing to propose rather than an invented one.
	if (Anchor.Reference.IsNearlyZero())
	{
		return;
	}

	const FVector2D Along = Anchor.Reference.GetSafeNormal();
	const FVector2D Across = RoadGeom::PerpCCW(Along);

	ForEachApronEdge(Network, Cursor, Tuning,
		[&Along, &Across, &Out](const FVector2D& A, const FVector2D& B, const FVector2D&)
		{
			// THE CORNER IS A POINT, so it is not displaced by the drag's half-width - there is
			// no extended edge for a road's flank to run flush along. See this source's header.
			SnapGuide::FCandidate Level;
			Level.Direction = Along;
			Level.Through = A;
			Level.Fit = SnapGuide::EFit::Perpendicular;
			Level.ReferenceAt = A;
			Level.Relation = SnapGuide::ERelation::LevelWith;
			Level.Reference = SnapGuide::EReference::Apron;
			// LITERAL, NOT DegreesTo/SquareTo + ApronEdge: an apron CORNER has no "the apron
			// edge" to be level or square WITH, so the whole string is fixed rather than built
			// from a subject - see ELabelKind::Literal.
			Level.Label.Kind = SnapGuide::ELabelKind::Literal;
			Level.Label.Text = TEXT("0 degrees to the apron corner");
			Out.Add(Level);

			SnapGuide::FCandidate Square = Level;
			Square.Direction = Across;
			Square.Label.Text = TEXT("square to the apron corner");
			Out.Add(Square);
		});
}

FString EntityNaming::Describe(const FEntityInstance& Entity)
{
	if (Entity.Definition == nullptr)
	{
		return TEXT("the installation");
	}

	// THE AUTHORED NAME WHEN THERE IS ONE, the asset's own when there is not. An unset
	// DisplayName is a content task rather than a bug, so this must not read as one on screen.
	const FString Authored = Entity.Definition->DisplayName.ToString();
	return Authored.IsEmpty() ? Entity.Definition->GetName() : Authored;
}

void FAlignedGuideSource::Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
	const FVector2D& Cursor, TArray<SnapGuide::FCandidate>& Out,
	const SnapGuide::FTuning& Tuning) const
{
	const double Reach = Tuning.SearchRadiusUu;

	// INDEXED, NOT A RANGE-FOR - #183, the same change PointAlign got: the label carries an
	// INDEX into Network.GetEntities() so EntityNaming::Describe runs again only for a winner,
	// in SnapGuide::Describe, rather than once per entity in reach whether it wins or not.
	const TArray<FEntityInstance>& Entities = Network.GetEntities();
	for (int32 Index = 0; Index < Entities.Num(); ++Index)
	{
		const FEntityInstance& Entity = Entities[Index];
		if (!Entity.bAlive
			|| FVector2D::DistSquared(Entity.Position, Anchor.Origin) > Reach * Reach)
		{
			continue;
		}

		// HEADING IS RADIANS - see FEntityInstance::Heading. A degrees/radians slip here would
		// point the guide somewhere plausible and wrong, which is the worst kind.
		const FVector2D Facing(FMath::Cos(Entity.Heading), FMath::Sin(Entity.Heading));

		// "ALIGNED WITH", not "parallel to": a stand is a thing that FACES, and a road is a
		// thing that runs. The dashed line goes to the thing itself, which for an entity is
		// simply its pose.
		SnapGuide::FGuideLabel Subject;
		Subject.Subject = SnapGuide::ELabelSubject::Entity;
		Subject.SubjectIndex = Index;
		AddDirections(Facing, Anchor.Origin, Entity.Position, SnapGuide::EReference::Stand,
			TEXT("aligned with"), Subject, Out);
	}
}

void FOffsetGuideSource::Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
	const FVector2D& Cursor, TArray<SnapGuide::FCandidate>& Out,
	const SnapGuide::FTuning& Tuning) const
{
	const double Reach = Tuning.SearchRadiusUu;

	// THE SAME REFERENCE FParallelGuideSource PICKS - the nearest road to the drag - so
	// "parallel to the taxiway" and "the same gap as its neighbour" describe one road between
	// them, and the two guides compose into an answer rather than two unrelated ones.
	FRoadSegmentId Reference;
	FVector2D ReferenceAt = FVector2D::ZeroVector;
	FVector2D ReferenceDir = FVector2D::ZeroVector;
	SnapGuide::EReference ReferenceColumn = SnapGuide::EReference::Taxiway;
	double BestSquared = Reach * Reach;

	const TArray<FRoadSegment>& Segments = Network.GetSegments();
	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		const FRoadSegmentId Id = Network.SegmentIdAt(Index);

		// RUNWAYS REFUSED, because the reference must be the one FParallelGuideSource picks and
		// that source excludes them. A reference the two disagree about breaks the composition
		// this source's own header promises: "parallel to the taxiway" and "the same gap as its
		// neighbour" describing ONE road between them. See GuideRoadColumn.
		SnapGuide::EReference Column = SnapGuide::EReference::Taxiway;
		if (!GuideRoadColumn(Network, Id, Column))
		{
			continue;
		}

		FVector2D A = FVector2D::ZeroVector;
		FVector2D B = FVector2D::ZeroVector;
		if (!Network.SegmentEnds(Id, A, B))
		{
			continue;
		}

		const FVector2D Span = B - A;
		// FROM THE CURSOR, and this is the 2026-09-20 report: a long road starts far from the
		// pair it is being matched against, so an origin-keyed search found nothing and the
		// matching-gap guide never appeared. Reported as "the next road has to have a shorter
		// segment"; length was the symptom. See Airside.Tool.OffsetGuideReachesWhatTheCursorIsNear.
		const FVector2D On = ClosestOn(A, B, Cursor);
		const double Squared = FVector2D::DistSquared(On, Cursor);
		if (Span.IsNearlyZero() || Squared > BestSquared)
		{
			continue;
		}

		BestSquared = Squared;
		Reference = Id;
		ReferenceAt = On;
		ReferenceDir = Span.GetSafeNormal();
		ReferenceColumn = Column;
	}

	if (ReferenceDir.IsNearlyZero())
	{
		return;
	}

	const FVector2D Across = RoadGeom::PerpCCW(ReferenceDir);

	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		const FRoadSegmentId Id = Network.SegmentIdAt(Index);

		SnapGuide::EReference Column = SnapGuide::EReference::Taxiway;
		if (!GuideRoadColumn(Network, Id, Column))
		{
			continue;
		}

		// THE NEIGHBOUR MUST BE THE SAME KIND AS THE REFERENCE - new with the 2026-09-20 split,
		// and a narrowing rather than a relabelling. ICAO separates taxiways by the wingspan
		// admitted; what a service road keeps from the next one is a question of what has to
		// drive between them. A pair of one of each keeps a gap that is neither standard, so
		// offering it as "40 m, matching the taxiway" would put a number under a button that
		// does not govern where it came from. See SnapGuide::IsLegalCell's MatchingGap row.
		if (Column != ReferenceColumn)
		{
			continue;
		}

		if (Id == Reference)
		{
			continue;
		}

		FVector2D A = FVector2D::ZeroVector;
		FVector2D B = FVector2D::ZeroVector;
		if (!Network.SegmentEnds(Id, A, B))
		{
			continue;
		}

		const FVector2D Span = B - A;
		if (Span.IsNearlyZero()
			|| FVector2D::DistSquared(ClosestOn(A, B, Cursor), Cursor) > Reach * Reach)
		{
			continue;
		}

		// A NEIGHBOUR IS A ROAD PARALLEL TO THE REFERENCE. One that crosses it has no single
		// gap to copy - the distance between them depends where you measure, so there is no
		// number to offer.
		const FVector2D Dir = Span.GetSafeNormal();
		if (!FMath::IsNearlyZero(Dir.X * ReferenceDir.Y - Dir.Y * ReferenceDir.X, 1.0e-3))
		{
			continue;
		}

		// THE GAP, measured perpendicular from the reference's line to the neighbour's near
		// end. Near-zero means the two are the same road drawn twice, or a continuation of it:
		// offering a zero gap would propose drawing on top of the reference.
		const FVector2D ToNeighbour = ClosestOn(A, B, ReferenceAt) - ReferenceAt;
		const double Signed = FVector2D::DotProduct(ToNeighbour, Across);
		const double Gap = FMath::Abs(Signed);
		if (Gap < 1.0)
		{
			continue;
		}

		SnapGuide::FCandidate Match;
		Match.Direction = ReferenceDir;

		// AWAY FROM THE NEIGHBOUR, never toward it: the whole offer is "another road, one gap
		// further on", and a line laid on the neighbour's own side would propose drawing on top
		// of the very road that suggested the number. Keyed to the neighbour rather than to the
		// cursor because the two agree everywhere the guide can actually be seen - a drag
		// BETWEEN the pair is nearer the reference than the neighbour, so the cursor's side is
		// the neighbour's side, and "the cursor's side" would hand back the neighbour's line.
		Match.Through = ReferenceAt - Across * FMath::Sign(Signed) * Gap;
		Match.Fit = SnapGuide::EFit::Perpendicular;
		Match.ReferenceAt = ReferenceAt;
		Match.Relation = SnapGuide::ERelation::MatchingGap;

		// THE REFERENCE'S COLUMN, which the filter above has just made the neighbour's too, so
		// there is one answer rather than a choice between two. That is what "taxiway
		// separation and service-road separation are different standards" comes to in code.
		Match.Reference = ReferenceColumn;

		// THE NUMBER IS IN THE LABEL. "matching the taxiway" alone would leave the player
		// unable to tell 40 m from 45 m, which is the one thing they are trying to control.
		// GapUu TRAVELS RAW, /100 DEFERRED TO Describe: the conversion is one multiply, and
		// doing it here would not save an allocation - the point is not calling
		// RoadNaming::Describe(Network, Reference) for every neighbour whether it wins or not.
		Match.Label.Kind = SnapGuide::ELabelKind::MatchingGap;
		Match.Label.Subject = SnapGuide::ELabelSubject::Segment;
		Match.Label.SegmentIndex = Reference.Index;
		Match.Label.GapUu = Gap;
		Out.Add(Match);
	}
}

FSnapGuideChain::FSnapGuideChain()
{
	AddSource(MakeUnique<FExtendingGuideSource>());
	AddSource(MakeUnique<FPointAlignGuideSource>());
	AddSource(MakeUnique<FAlignedGuideSource>());
	AddSource(MakeUnique<FCollinearGuideSource>());
	AddSource(MakeUnique<FParallelGuideSource>());
	AddSource(MakeUnique<FRunwayGuideSource>());
	AddSource(MakeUnique<FRunwayLineGuideSource>());
	AddSource(MakeUnique<FAngledRoadGuideSource>());
	AddSource(MakeUnique<FAngledRunwayGuideSource>());
	AddSource(MakeUnique<FApronGuideSource>());
	AddSource(MakeUnique<FApronLineGuideSource>());
	AddSource(MakeUnique<FApronAngledGuideSource>());
	AddSource(MakeUnique<FApronCornerGuideSource>());
	AddSource(MakeUnique<FWorldGuideSource>());
	AddSource(MakeUnique<FOffsetGuideSource>());
}

void FSnapGuideChain::AddSource(TUniquePtr<IGuideSource> Source)
{
	if (Source.IsValid())
	{
		Sources.Add(MoveTemp(Source));
	}
}

void FSnapGuideChain::ProposeAll(const URoadNetwork& Network, const FGuideAnchor& Anchor,
	const FVector2D& Cursor, const FSnapGuideSettings& Enabled,
	TArray<SnapGuide::FCandidate>& Out, const SnapGuide::FTuning& Tuning) const
{
	// Stage 1 gathered at most six. Reserved for more because Parallel and Collinear propose
	// per segment, and the array is rebuilt on every context - about three times a frame, per
	// FBuildSession::MakeContext.
	Out.Reserve(16);

	// THE PARAMETER IS `Enabled`, NOT `Sources`: the member holding the links is already called
	// Sources, and a parameter of that name would shadow it - the loop below would then be
	// iterating the settings struct.
	for (const TUniquePtr<IGuideSource>& Source : Sources)
	{
		// THE RELATION IS SKIPPED BEFORE IT WORKS, not filtered after. Collinear walks every
		// segment in reach; doing that and discarding the result is waste.
		if (!Enabled.IsRelationOn(Source->Relation()))
		{
			continue;
		}

		const int32 Before = Out.Num();
		Source->Propose(Network, Anchor, Cursor, Out, Tuning);

		// THE REFERENCE IS FILTERED AFTER, and only over what this source just added. A source
		// may span columns - FRunwayGuideSource is Runway while FParallelGuideSource is Road,
		// and a source is not obliged to declare one - so there is no single column to skip up
		// front the way the relation is.
		// Walking only the newly-added range keeps this linear however many sources answered.
		//
		// BACKWARDS, because RemoveAtSwap moves the last element into the hole: forwards, the
		// element swapped in would never be examined.
		for (int32 Index = Out.Num() - 1; Index >= Before; --Index)
		{
			// THE TWO FLAGS ONLY, NOT IsEnabled. IsEnabled also asks IsLegalCell, and filtering
			// on that here would SILENTLY DISCARD a source proposing into a hole - hiding the
			// one fault Airside.Tool.GuideGridHasNoCellOutsideTheList exists to find. That test
			// was written against IsEnabled first and passed with a source deliberately pointed
			// at Collinear x World, which is how this was found: the gate was covering for it.
			//
			// A hole is unreachable because no source proposes into one, and the test is what
			// holds that true - not a filter that quietly tidies it away.
			if (!Enabled.IsRelationOn(Out[Index].Relation)
				|| !Enabled.IsReferenceOn(Out[Index].Reference))
			{
				Out.RemoveAtSwap(Index, 1, EAllowShrinking::No);
			}
		}
	}

}

SnapGuide::FResult FSnapGuideChain::Resolve(const URoadNetwork& Network,
	const FGuideAnchor& Anchor, const FVector2D& Cursor,
	const SnapGuide::FResult& Previous, const FSnapGuideSettings& Enabled,
	const SnapGuide::FTuning& Tuning) const
{
	TArray<SnapGuide::FCandidate> Candidates;
	ProposeAll(Network, Anchor, Cursor, Enabled, Candidates, Tuning);
	SnapGuide::FResult Result = SnapGuide::Arbitrate(Candidates, Anchor.Origin, Cursor, Previous, Tuning);

	// THE ONLY PLACE Description IS BUILT - #183. Arbitrate throws away every candidate but the
	// (at most two) winners; this is the one point in the whole call that still has both Network
	// and Anchor in scope AND knows which candidates survived, so it is where SnapGuide::Describe
	// gets called - never inside Propose, which is what used to pay for every candidate discarded
	// a moment later.
	for (SnapGuide::FCandidate& Winner : Result.Winners)
	{
		Winner.Description = SnapGuide::Describe(Network, Anchor, Winner.Label);
	}

	return Result;
}
