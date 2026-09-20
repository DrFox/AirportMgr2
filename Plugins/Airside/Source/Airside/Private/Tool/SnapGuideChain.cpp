#include "Tool/SnapGuideChain.h"

#include "Entities/EntityDefinition.h"
#include "Model/RoadEntity.h"
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
	 * Which column a segment belongs to. A runway is not a type in the model - it is any
	 * segment whose profile is continuous through junctions - so the ONE test that decides it
	 * lives in URoadNetwork and is asked here rather than re-derived.
	 *
	 * TAGGING RATHER THAN SKIPPING, at this stage: the three segment sources still propose for
	 * runways exactly as they did before, so this change is behaviour-preserving. The gating in
	 * FSnapGuideChain::Resolve acts on the tag, and the partition follows it.
	 */
	SnapGuide::EReference ReferenceFor(const URoadNetwork& Network, FRoadSegmentId Segment)
	{
		return Network.IsRunwaySegment(Segment)
			? SnapGuide::EReference::Runway : SnapGuide::EReference::Road;
	}
}

void FExtendingGuideSource::Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
	TArray<SnapGuide::FCandidate>& Out) const
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
	TArray<SnapGuide::FCandidate>& Out) const
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
	TArray<SnapGuide::FCandidate>& Out) const
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
		Level.Reference = SnapGuide::EReference::ThisGesture;
		Out.Add(Level);

		SnapGuide::FCandidate Square = Level;
		Square.Direction = Across;
		Square.Description = FString::Printf(TEXT("square to %s"), *Point.Name);
		Out.Add(Square);
	}
}

void FParallelGuideSource::Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
	TArray<SnapGuide::FCandidate>& Out) const
{
	const double Reach = SnapGuide::FTuning().SearchRadiusUu;

	FRoadSegmentId Nearest;
	FVector2D NearestAt = FVector2D::ZeroVector;
	FVector2D NearestDir = FVector2D::ZeroVector;
	double BestSquared = Reach * Reach;

	const TArray<FRoadSegment>& Segments = Network.GetSegments();
	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		const FRoadSegmentId Id = Network.SegmentIdAt(Index);
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
	Along.Reference = ReferenceFor(Network, Nearest);
	Along.Description = FString::Printf(TEXT("parallel to %s"), *Name);
	Out.Add(Along);

	SnapGuide::FCandidate Square = Along;
	Square.Direction = RoadGeom::PerpCCW(NearestDir);
	Square.Description = FString::Printf(TEXT("square to %s"), *Name);
	Out.Add(Square);
}

void FCollinearGuideSource::Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
	TArray<SnapGuide::FCandidate>& Out) const
{
	const double Reach = SnapGuide::FTuning().SearchRadiusUu;

	const TArray<FRoadSegment>& Segments = Network.GetSegments();
	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		const FRoadSegmentId Id = Network.SegmentIdAt(Index);
		FVector2D A = FVector2D::ZeroVector;
		FVector2D B = FVector2D::ZeroVector;
		if (!GuideSegmentEnds(Network, Id, A, B))
		{
			continue;
		}

		const FVector2D Span = B - A;
		const FVector2D On = ClosestOn(A, B, Anchor.Origin);
		if (Span.IsNearlyZero()
			|| FVector2D::DistSquared(On, Anchor.Origin) > Reach * Reach)
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
		InLine.Reference = ReferenceFor(Network, Id);
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
	TArray<SnapGuide::FCandidate>& Out) const
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
	TArray<SnapGuide::FCandidate>& Out) const
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
	TArray<SnapGuide::FCandidate>& Out) const
{
	const double Reach = SnapGuide::FTuning().SearchRadiusUu;

	// THE SAME REFERENCE FParallelGuideSource PICKS - the nearest road to the drag - so
	// "parallel to the taxiway" and "the same gap as its neighbour" describe one road between
	// them, and the two guides compose into an answer rather than two unrelated ones.
	FRoadSegmentId Reference;
	FVector2D ReferenceAt = FVector2D::ZeroVector;
	FVector2D ReferenceDir = FVector2D::ZeroVector;
	double BestSquared = Reach * Reach;

	const TArray<FRoadSegment>& Segments = Network.GetSegments();
	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		const FRoadSegmentId Id = Network.SegmentIdAt(Index);
		FVector2D A = FVector2D::ZeroVector;
		FVector2D B = FVector2D::ZeroVector;
		if (!GuideSegmentEnds(Network, Id, A, B))
		{
			continue;
		}

		const FVector2D Span = B - A;
		const FVector2D On = ClosestOn(A, B, Anchor.Origin);
		const double Squared = FVector2D::DistSquared(On, Anchor.Origin);
		if (Span.IsNearlyZero() || Squared > BestSquared)
		{
			continue;
		}

		BestSquared = Squared;
		Reference = Id;
		ReferenceAt = On;
		ReferenceDir = Span.GetSafeNormal();
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
			|| FVector2D::DistSquared(ClosestOn(A, B, Anchor.Origin), Anchor.Origin) > Reach * Reach)
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
		Match.Reference = ReferenceFor(Network, Reference);

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

SnapGuide::FResult FSnapGuideChain::Resolve(const URoadNetwork& Network,
	const FGuideAnchor& Anchor, const FVector2D& Cursor,
	const SnapGuide::FResult& Previous, const FSnapGuideSettings& Enabled,
	const SnapGuide::FTuning& Tuning) const
{
	TArray<SnapGuide::FCandidate> Candidates;

	// Stage 1 gathered at most six. Reserved for more because Parallel and Collinear propose
	// per segment, and the array is rebuilt on every context - about three times a frame, per
	// FBuildSession::MakeContext.
	Candidates.Reserve(16);

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

		const int32 Before = Candidates.Num();
		Source->Propose(Network, Anchor, Candidates);

		// THE REFERENCE IS FILTERED AFTER, and only over what this source just added. A source
		// may span columns - the segment walkers tag Road or Runway per segment, which is not
		// known until the segment is in hand - so there is no single column to skip up front.
		// Walking only the newly-added range keeps this linear however many sources answered.
		//
		// BACKWARDS, because RemoveAtSwap moves the last element into the hole: forwards, the
		// element swapped in would never be examined.
		for (int32 Index = Candidates.Num() - 1; Index >= Before; --Index)
		{
			if (!Enabled.IsEnabled(Candidates[Index].Relation, Candidates[Index].Reference))
			{
				Candidates.RemoveAtSwap(Index, 1, EAllowShrinking::No);
			}
		}
	}

	return SnapGuide::Arbitrate(Candidates, Anchor.Origin, Cursor, Previous, Tuning);
}
