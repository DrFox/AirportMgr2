#include "Tool/SnapGuideChain.h"

#include "Entities/EntityDefinition.h"
#include "Model/RoadEntity.h"
#include "Model/RoadApron.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Solve/RoadGeom.h"
#include "Tool/RoadNaming.h"

namespace
{
	/**
	 * A live segment's two ends. False when the segment or either node has gone.
	 *
	 * PREFIXED because FPlotPlaceTool.cpp has a SegmentEnds of its own in ITS anonymous
	 * namespace, and this module is a UNITY build: two such helpers of one name compile
	 * perfectly alone and collide the moment they land in the same blob. That is not
	 * hypothetical - it is what this file did on the build that introduced it, and it is the
	 * same trap AirsideTestFixtures.h was written to close for the test module.
	 *
	 * THE DUPLICATION IS REAL and left deliberately: merging the two means a shared header and
	 * an edit to the plot tool, which is not this change's business. Noted for stage 3.
	 */
	bool GuideSegmentEnds(const URoadNetwork& Network, FRoadSegmentId Id,
		FVector2D& OutA, FVector2D& OutB)
	{
		const FRoadSegment* Segment = Network.GetSegment(Id);
		if (Segment == nullptr || !Segment->bAlive)
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
	 */
	void AddSpokes(const FVector2D& End, const FVector2D& Along, SnapGuide::EReference Reference,
		const FString& Name, TArray<SnapGuide::FCandidate>& Out)
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
			Spoke.Description = FString::Printf(TEXT("%d degrees to %s"), Degrees, *Name);
			Out.Add(Spoke);
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
		TFunctionRef<void(const FVector2D&, const FVector2D&, const FVector2D&)> Visit)
	{
		const double Reach = SnapGuide::FTuning().SearchRadiusUu;

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

	/** What a player reads for an apron. FApronSurface carries no name - see FApronGuideSource. */
	const TCHAR* ApronEdgeName() { return TEXT("the apron edge"); }

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
	 * PREFIXED like GuideSegmentEnds above: the tests module and this one are unity builds, and
	 * "RoadColumnOf" is a name a second file would also pick.
	 */
	bool GuideRoadColumn(const URoadNetwork& Network, FRoadSegmentId Id,
		SnapGuide::EReference& Out)
	{
		return RoadNaming::ReferenceOf(Network, Id, Out)
			&& Out != SnapGuide::EReference::Runway;
	}

}

void FExtendingGuideSource::Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
	const FVector2D& Cursor, TArray<SnapGuide::FCandidate>& Out) const
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
	Parallel.Description = FString::Printf(TEXT("along %s"), *Anchor.ReferenceName);
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
	Square.Description = FString::Printf(TEXT("square to %s"), *Anchor.ReferenceName);
	Out.Add(Square);
}

void FWorldGuideSource::Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
	const FVector2D& Cursor, TArray<SnapGuide::FCandidate>& Out) const
{
	for (const int32 Degrees : { 0, 45, 90, 135 })
	{
		SnapGuide::FCandidate Candidate;
		const double Radians = FMath::DegreesToRadians(static_cast<double>(Degrees));
		Candidate.Direction = FVector2D(FMath::Cos(Radians), FMath::Sin(Radians));

		// Through the origin, like Extending: a world axis is still "which way from here".
		Candidate.Through = Anchor.Origin;
		Candidate.Fit = SnapGuide::EFit::Angular;

		// THE LINE GOES BACK TO THE POINT IT SWINGS AROUND. The world grid is not a thing on
		// the map to point at, and a dashed line shot off to nowhere would say less than one
		// that says "this is the corner you are square from". The label carries the rest.
		Candidate.ReferenceAt = Anchor.Origin;
		Candidate.Description = FString::Printf(TEXT("%d degrees"), Degrees);
		Candidate.Relation = SnapGuide::ERelation::Parallel;
		Candidate.Reference = SnapGuide::EReference::World;
		Out.Add(Candidate);
	}
}

void FPointAlignGuideSource::Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
	const FVector2D& Cursor, TArray<SnapGuide::FCandidate>& Out) const
{
	if (Anchor.Reference.IsNearlyZero())
	{
		return;
	}

	const FVector2D Along = Anchor.Reference.GetSafeNormal();
	const FVector2D Across = RoadGeom::PerpCCW(Along);

	for (const FGuidePoint& Point : Anchor.AlignTo)
	{
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
		Level.Description = FString::Printf(TEXT("0 degrees to %s"), *Point.Name);
		Level.Relation = SnapGuide::ERelation::LevelWith;

		// THE POINT'S OWN COLUMN, not this source's. One source serves both the gesture's
		// corners and the network's nodes, and the tool is the only thing that knows which is
		// which - see FGuidePoint::Reference.
		Level.Reference = Point.Reference;
		Out.Add(Level);

		SnapGuide::FCandidate Square = Level;
		Square.Direction = Across;
		Square.Description = FString::Printf(TEXT("square to %s"), *Point.Name);
		Out.Add(Square);
	}
}

void FParallelGuideSource::Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
	const FVector2D& Cursor, TArray<SnapGuide::FCandidate>& Out) const
{
	const double Reach = SnapGuide::FTuning().SearchRadiusUu;

	FRoadSegmentId Nearest;
	FVector2D NearestAt = FVector2D::ZeroVector;
	FVector2D NearestDir = FVector2D::ZeroVector;
	SnapGuide::EReference NearestColumn = SnapGuide::EReference::Taxiway;
	double BestSquared = Reach * Reach;

	const TArray<FRoadSegment>& Segments = Network.GetSegments();
	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		const FRoadSegmentId Id = Network.SegmentIdAt(Index);

		// WHICH COLUMN, AND RUNWAYS REFUSED, in one question - see GuideRoadColumn.
		SnapGuide::EReference Column = SnapGuide::EReference::Taxiway;
		if (!GuideRoadColumn(Network, Id, Column))
		{
			continue;
		}

		FVector2D A = FVector2D::ZeroVector;
		FVector2D B = FVector2D::ZeroVector;
		if (!GuideSegmentEnds(Network, Id, A, B))
		{
			continue;
		}

		const FVector2D Span = B - A;
		if (Span.IsNearlyZero())
		{
			continue;
		}

		// MEASURED FROM THE ORIGIN, not from the cursor: the road the gesture STARTED beside
		// is the one it is being drawn parallel to, and a search keyed to the cursor would
		// hand the guide to a different road halfway through the drag.
		const FVector2D On = ClosestOn(A, B, Anchor.Origin);
		const double Squared = FVector2D::DistSquared(On, Anchor.Origin);
		if (Squared > BestSquared)
		{
			continue;
		}

		BestSquared = Squared;
		Nearest = Id;
		NearestAt = On;
		NearestDir = Span.GetSafeNormal();

		// CARRIED FROM THE WINNER, not asked again at the bottom. The nearest road is picked
		// once and its column is a fact about THAT segment; a second classification call after
		// the loop would be a second chance to pick a different one.
		NearestColumn = Column;
	}

	if (NearestDir.IsNearlyZero())
	{
		return;
	}

	const FString Name = RoadNaming::Describe(Network, Nearest);

	SnapGuide::FCandidate Along;
	Along.Direction = NearestDir;
	Along.Through = Anchor.Origin;
	Along.Fit = SnapGuide::EFit::Angular;
	Along.ReferenceAt = NearestAt;
	Along.Relation = SnapGuide::ERelation::Parallel;

	// THE COLUMN AND THE LABEL COME FROM ONE CLASSIFICATION, which is the whole of the
	// 2026-09-20 split: this used to hard-code EReference::Road while Describe said "the
	// service road", so the line appeared under a button marked Road. RoadNaming answers both.
	Along.Reference = NearestColumn;
	Along.Description = FString::Printf(TEXT("parallel to %s"), *Name);
	Out.Add(Along);

	SnapGuide::FCandidate Square = Along;
	Square.Direction = RoadGeom::PerpCCW(NearestDir);
	Square.Description = FString::Printf(TEXT("square to %s"), *Name);
	Out.Add(Square);
}

void FCollinearGuideSource::Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
	const FVector2D& Cursor, TArray<SnapGuide::FCandidate>& Out) const
{
	const double Reach = SnapGuide::FTuning().SearchRadiusUu;

	const TArray<FRoadSegment>& Segments = Network.GetSegments();
	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		const FRoadSegmentId Id = Network.SegmentIdAt(Index);

		// WHICH COLUMN, AND RUNWAYS REFUSED, in one question - see GuideRoadColumn.
		SnapGuide::EReference Column = SnapGuide::EReference::Taxiway;
		if (!GuideRoadColumn(Network, Id, Column))
		{
			continue;
		}

		FVector2D A = FVector2D::ZeroVector;
		FVector2D B = FVector2D::ZeroVector;
		if (!GuideSegmentEnds(Network, Id, A, B))
		{
			continue;
		}

		const FVector2D Span = B - A;
		// FROM THE CURSOR, not the origin. This source answers where the far end LANDED, and
		// the far end is under the cursor - a drag that began 300 m away is still being aimed
		// at the road beneath it. See IGuideSource::Propose.
		const FVector2D On = ClosestOn(A, B, Cursor);
		if (Span.IsNearlyZero()
			|| FVector2D::DistSquared(On, Cursor) > Reach * Reach)
		{
			continue;
		}

		// THROUGH THE SEGMENT'S OWN END, which is what makes this the line the road LIES ON
		// rather than one through the drag. The arbiter measures the cursor's distance from
		// that line, so the candidate is eligible exactly when the cursor is on the road's
		// extension - however far along it the drag has gone.
		SnapGuide::FCandidate InLine;
		InLine.Direction = Span.GetSafeNormal();
		InLine.Through = A;
		InLine.Fit = SnapGuide::EFit::Perpendicular;
		InLine.Relation = SnapGuide::ERelation::Collinear;

		// PER SEGMENT, not per source. One walk of the graph passes a taxiway and a service
		// road in the same pass, and the two answer to different buttons since 2026-09-20.
		InLine.Reference = Column;
		InLine.Description = FString::Printf(TEXT("in line with %s"),
			*RoadNaming::Describe(Network, Id));

		// THE DASHED LINE GOES TO THE ROAD ITSELF, not to the point on its extension where the
		// cursor happens to be: the player needs to see WHICH road they are in line with, and
		// the near end of it is the part they can recognise.
		InLine.ReferenceAt = On;
		Out.Add(InLine);
	}
}

void FRunwayGuideSource::Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
	const FVector2D& Cursor, TArray<SnapGuide::FCandidate>& Out) const
{
	const TArray<FRoadSegment>& Segments = Network.GetSegments();
	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		const FRoadSegmentId Id = Network.SegmentIdAt(Index);
		if (!Network.IsRunwaySegment(Id))
		{
			continue;
		}

		FVector2D A = FVector2D::ZeroVector;
		FVector2D B = FVector2D::ZeroVector;
		if (!GuideSegmentEnds(Network, Id, A, B))
		{
			continue;
		}

		const FVector2D Span = B - A;
		if (Span.IsNearlyZero())
		{
			continue;
		}

		// NO REACH TEST, and that one absence is the only thing separating this source from
		// Parallel - see the declaration for why it is deliberate.
		const FString Name = RoadNaming::Describe(Network, Id);

		SnapGuide::FCandidate Along;
		Along.Direction = Span.GetSafeNormal();
		Along.Through = Anchor.Origin;
		Along.Fit = SnapGuide::EFit::Angular;
		Along.ReferenceAt = ClosestOn(A, B, Anchor.Origin);
		Along.Relation = SnapGuide::ERelation::Parallel;
		Along.Reference = SnapGuide::EReference::Runway;
		Along.Description = FString::Printf(TEXT("parallel to %s"), *Name);
		Out.Add(Along);

		SnapGuide::FCandidate Square = Along;
		Square.Direction = RoadGeom::PerpCCW(Along.Direction);
		Square.Description = FString::Printf(TEXT("square to %s"), *Name);
		Out.Add(Square);
	}
}

void FRunwayLineGuideSource::Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
	const FVector2D& Cursor, TArray<SnapGuide::FCandidate>& Out) const
{
	const TArray<FRoadSegment>& Segments = Network.GetSegments();
	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		const FRoadSegmentId Id = Network.SegmentIdAt(Index);
		if (!Network.IsRunwaySegment(Id))
		{
			continue;
		}

		FVector2D A = FVector2D::ZeroVector;
		FVector2D B = FVector2D::ZeroVector;
		if (!GuideSegmentEnds(Network, Id, A, B))
		{
			continue;
		}

		const FVector2D Span = B - A;
		if (Span.IsNearlyZero())
		{
			continue;
		}

		// NO REACH TEST, like FRunwayGuideSource and unlike FCollinearGuideSource: a runway's
		// extended centreline is the approach path, and it is meaningful from anywhere.
		//
		// THROUGH THE RUNWAY'S OWN END, not through the drag - that is what makes this the line
		// the runway LIES ON rather than one out of the cursor, and why it is Perpendicular
		// where FRunwayGuideSource's two are Angular.
		SnapGuide::FCandidate InLine;
		InLine.Direction = Span.GetSafeNormal();
		InLine.Through = A;
		InLine.Fit = SnapGuide::EFit::Perpendicular;

		// THE DASHED LINE GOES TO THE RUNWAY ITSELF, not to the point on its extension where
		// the cursor happens to be: the player needs to see WHICH runway they are in line with.
		InLine.ReferenceAt = ClosestOn(A, B, Cursor);
		InLine.Relation = SnapGuide::ERelation::Collinear;
		InLine.Reference = SnapGuide::EReference::Runway;
		InLine.Description = FString::Printf(TEXT("in line with %s"),
			*RoadNaming::Describe(Network, Id));
		Out.Add(InLine);
	}
}

void FAngledRoadGuideSource::Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
	const FVector2D& Cursor, TArray<SnapGuide::FCandidate>& Out) const
{
	const double Reach = SnapGuide::FTuning().SearchRadiusUu;

	const TArray<FRoadSegment>& Segments = Network.GetSegments();
	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		const FRoadSegmentId Id = Network.SegmentIdAt(Index);

		// WHICH COLUMN, AND RUNWAYS REFUSED, in one question. They are owned by
		// FAngledRunwayGuideSource and owned WITHOUT a reach; walking one here would let it
		// answer while the Runway column was switched off - the 2026-09-20 report in a new
		// place. See GuideRoadColumn.
		SnapGuide::EReference Column = SnapGuide::EReference::Taxiway;
		if (!GuideRoadColumn(Network, Id, Column))
		{
			continue;
		}

		FVector2D A = FVector2D::ZeroVector;
		FVector2D B = FVector2D::ZeroVector;
		if (!GuideSegmentEnds(Network, Id, A, B))
		{
			continue;
		}

		const FVector2D Span = B - A;
		if (Span.IsNearlyZero()
			// FROM THE CURSOR - a spoke is a line to LAND on, so the reach follows the end
			// being placed rather than the one already pinned. See IGuideSource::Propose.
			|| FVector2D::DistSquared(ClosestOn(A, B, Cursor), Cursor) > Reach * Reach)
		{
			continue;
		}

		const FVector2D Along = Span.GetSafeNormal();
		const FString Name = RoadNaming::Describe(Network, Id);

		// BOTH ENDS - see this source's own header for why neither may be picked for the
		// player - and both under the SEGMENT'S OWN column, so "45 degrees to the service
		// road" answers to the ServiceRoad button and not to the Taxiway one.
		AddSpokes(A, Along, Column, Name, Out);
		AddSpokes(B, Along, Column, Name, Out);
	}
}

void FAngledRunwayGuideSource::Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
	const FVector2D& Cursor, TArray<SnapGuide::FCandidate>& Out) const
{
	const TArray<FRoadSegment>& Segments = Network.GetSegments();
	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		const FRoadSegmentId Id = Network.SegmentIdAt(Index);
		if (!Network.IsRunwaySegment(Id))
		{
			continue;
		}

		FVector2D A = FVector2D::ZeroVector;
		FVector2D B = FVector2D::ZeroVector;
		if (!GuideSegmentEnds(Network, Id, A, B))
		{
			continue;
		}

		const FVector2D Span = B - A;
		if (Span.IsNearlyZero())
		{
			continue;
		}

		// NO REACH TEST, like every other source in the Runway column: a rapid-exit taxiway is
		// laid from wherever the player is standing, not only from beside the threshold.
		const FVector2D Along = Span.GetSafeNormal();
		const FString Name = RoadNaming::Describe(Network, Id);
		AddSpokes(A, Along, SnapGuide::EReference::Runway, Name, Out);
		AddSpokes(B, Along, SnapGuide::EReference::Runway, Name, Out);
	}
}

void FApronGuideSource::Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
	const FVector2D& Cursor, TArray<SnapGuide::FCandidate>& Out) const
{
	ForEachApronEdge(Network, Anchor.Origin,
		[&Anchor, &Out](const FVector2D& A, const FVector2D& B, const FVector2D& Along)
		{
			// ANGULAR, THROUGH THE DRAG'S OWN ORIGIN - this answers "which way from here", so
			// there is no position to be flush with and the half-width never applies.
			SnapGuide::FCandidate Parallel;
			Parallel.Direction = Along;
			Parallel.Through = Anchor.Origin;
			Parallel.Fit = SnapGuide::EFit::Angular;
			Parallel.ReferenceAt = ClosestOn(A, B, Anchor.Origin);
			Parallel.Relation = SnapGuide::ERelation::Parallel;
			Parallel.Reference = SnapGuide::EReference::Apron;
			Parallel.Description = FString::Printf(TEXT("parallel to %s"), ApronEdgeName());
			Out.Add(Parallel);

			SnapGuide::FCandidate Square = Parallel;
			Square.Direction = RoadGeom::PerpCCW(Along);
			Square.Description = FString::Printf(TEXT("square to %s"), ApronEdgeName());
			Out.Add(Square);
		});
}

void FApronLineGuideSource::Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
	const FVector2D& Cursor, TArray<SnapGuide::FCandidate>& Out) const
{
	ForEachApronEdge(Network, Cursor,
		[&Anchor, &Cursor, &Out](const FVector2D& A, const FVector2D& B, const FVector2D& Along)
		{
			SnapGuide::FCandidate InLine;
			InLine.Direction = Along;
			InLine.Fit = SnapGuide::EFit::Perpendicular;
			InLine.ReferenceAt = ClosestOn(A, B, Cursor);
			InLine.Relation = SnapGuide::ERelation::Collinear;
			InLine.Reference = SnapGuide::EReference::Apron;

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
				InLine.Description = FString::Printf(TEXT("in line with %s"), ApronEdgeName());
				Out.Add(InLine);
				return;
			}

			// BOTH SIDES, and NOT a mirrored pair: GetHalfWidthLeft and GetHalfWidthRight are
			// separate because a cross-section may be off-centre. Flush-inside and flush-outside
			// are both real intents - a taxiway running along the apron, or one abutting it - so
			// neither may be chosen for the player.
			SnapGuide::FCandidate Near = InLine;
			Near.Through = A + Across * Left;
			Near.Description = FString::Printf(TEXT("edge flush with %s"), ApronEdgeName());
			Out.Add(Near);

			SnapGuide::FCandidate Far = InLine;
			Far.Through = A - Across * Right;
			Far.Description = Near.Description;
			Out.Add(Far);
		});
}

void FApronAngledGuideSource::Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
	const FVector2D& Cursor, TArray<SnapGuide::FCandidate>& Out) const
{
	// ONE END PER EDGE, not both: an outline is closed, so every corner is the A end of exactly
	// one edge. Visiting B as well would propose each corner's spokes twice - once per edge
	// meeting there - and a duplicate candidate is a tie the source order then has to break.
	ForEachApronEdge(Network, Cursor,
		[&Out](const FVector2D& A, const FVector2D& B, const FVector2D& Along)
		{
			AddSpokes(A, Along, SnapGuide::EReference::Apron, ApronEdgeName(), Out);
		});
}

void FApronCornerGuideSource::Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
	const FVector2D& Cursor, TArray<SnapGuide::FCandidate>& Out) const
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

	ForEachApronEdge(Network, Cursor,
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
			Level.Description = TEXT("0 degrees to the apron corner");
			Out.Add(Level);

			SnapGuide::FCandidate Square = Level;
			Square.Direction = Across;
			Square.Description = TEXT("square to the apron corner");
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
	const FVector2D& Cursor, TArray<SnapGuide::FCandidate>& Out) const
{
	const double Reach = SnapGuide::FTuning().SearchRadiusUu;

	for (const FEntityInstance& Entity : Network.GetEntities())
	{
		if (!Entity.bAlive
			|| FVector2D::DistSquared(Entity.Position, Anchor.Origin) > Reach * Reach)
		{
			continue;
		}

		// HEADING IS RADIANS - see FEntityInstance::Heading. A degrees/radians slip here would
		// point the guide somewhere plausible and wrong, which is the worst kind.
		const FVector2D Facing(FMath::Cos(Entity.Heading), FMath::Sin(Entity.Heading));
		const FString Name = EntityNaming::Describe(Entity);

		SnapGuide::FCandidate Along;
		Along.Direction = Facing;
		Along.Through = Anchor.Origin;
		Along.Fit = SnapGuide::EFit::Angular;

		// THE DASHED LINE GOES TO THE THING ITSELF, which for an entity is simply its pose.
		Along.ReferenceAt = Entity.Position;
		Along.Relation = SnapGuide::ERelation::Parallel;
		Along.Reference = SnapGuide::EReference::Stand;
		Along.Description = FString::Printf(TEXT("aligned with %s"), *Name);
		Out.Add(Along);

		SnapGuide::FCandidate Square = Along;
		Square.Direction = RoadGeom::PerpCCW(Facing);
		Square.Description = FString::Printf(TEXT("square to %s"), *Name);
		Out.Add(Square);
	}
}

void FOffsetGuideSource::Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
	const FVector2D& Cursor, TArray<SnapGuide::FCandidate>& Out) const
{
	const double Reach = SnapGuide::FTuning().SearchRadiusUu;

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
		if (!GuideSegmentEnds(Network, Id, A, B))
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
	const FString ReferenceName = RoadNaming::Describe(Network, Reference);

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
		if (!GuideSegmentEnds(Network, Id, A, B))
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
		Match.Description = FString::Printf(TEXT("%.0f m, matching %s"),
			Gap / 100.0, *ReferenceName);
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
	TArray<SnapGuide::FCandidate>& Out) const
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
		Source->Propose(Network, Anchor, Cursor, Out);

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
	ProposeAll(Network, Anchor, Cursor, Enabled, Candidates);
	return SnapGuide::Arbitrate(Candidates, Anchor.Origin, Cursor, Previous, Tuning);
}
