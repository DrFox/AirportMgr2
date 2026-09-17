#include "Tool/SnapGuideChain.h"

#include "Solve/RoadGeom.h"

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
	Parallel.ReferenceAt = Anchor.ReferenceAt;
	Parallel.Description = FString::Printf(TEXT("along %s"), *Anchor.ReferenceName);
	Parallel.Source = SnapGuide::ESource::Extending;
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

		// THE LINE GOES BACK TO THE POINT IT SWINGS AROUND. The world grid is not a thing on
		// the map to point at, and a dashed line shot off to nowhere would say less than one
		// that says "this is the corner you are square from". The label carries the rest.
		Candidate.ReferenceAt = Anchor.Origin;
		Candidate.Description = FString::Printf(TEXT("%d degrees"), Degrees);
		Candidate.Source = SnapGuide::ESource::World;
		Out.Add(Candidate);
	}
}

FSnapGuideChain::FSnapGuideChain()
{
	AddSource(MakeUnique<FExtendingGuideSource>());
	AddSource(MakeUnique<FWorldGuideSource>());
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
	const SnapGuide::FResult& Previous, const SnapGuide::FTuning& Tuning) const
{
	TArray<SnapGuide::FCandidate> Candidates;

	// Stage 1 gathers at most six. Reserved anyway because stage 2's Parallel and Collinear
	// propose per segment, and the array is rebuilt on every context - about three times a
	// frame, per FBuildSession::MakeContext.
	Candidates.Reserve(16);

	for (const TUniquePtr<IGuideSource>& Source : Sources)
	{
		Source->Propose(Network, Anchor, Candidates);
	}

	return SnapGuide::Arbitrate(Candidates, Anchor.Origin, Cursor, Previous, Tuning);
}
