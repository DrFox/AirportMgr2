#include "Model/AirsideCapability.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Profiles/RoadProfile.h"

double FAirsideCapability::LongestRunway() const
{
	double Longest = 0.0;
	for (const FRunwaySummary& R : Runways)
	{
		Longest = FMath::Max(Longest, R.End.Length);
	}
	return Longest;
}

namespace
{
	bool SameStrip(const FRunwaySummary& A, const FVector2D& Threshold, const FVector2D& FarEnd)
	{
		// 1 uu: thresholds come from the same node positions, so anything looser would be
		// tolerating a disagreement that cannot happen.
		const double Tol = 1.0;
		const FVector2D AFar = A.End.FarEnd();
		return (FVector2D::Distance(A.End.Threshold, Threshold) < Tol && FVector2D::Distance(AFar, FarEnd) < Tol)
			|| (FVector2D::Distance(A.End.Threshold, FarEnd) < Tol && FVector2D::Distance(AFar, Threshold) < Tol);
	}
}

TArray<FRunwaySummary> AirsideCapability::SummariseRunways(const URoadNetwork& Network)
{
	TArray<FRunwaySummary> Runways;

	const TArray<FRoadSegment>& Segments = Network.GetSegments();
	for (const FRoadSegment& Segment : Segments)
	{
		if (!Segment.bAlive) { continue; }
		const URoadProfile* Profile = Network.ProfileFor(Segment);
		if (Profile == nullptr || !Profile->bContinuousThroughJunctions) { continue; }

		const FRoadNode* A = Network.GetNode(Segment.A);
		if (A == nullptr) { continue; }

		// Queried AT AN END, not the midpoint: RunwayExtentAt's proximity gate measures the
		// distance to the nearest segment END against the runway's width, so a long segment's
		// midpoint is "not on a runway" by that rule. Its end is on it by definition.
		FRunwayEnd End;
		if (!Network.RunwayExtentAt(A->Position, End)) { continue; }
		const FVector2D FarEnd = End.FarEnd();

		const bool bKnown = Runways.ContainsByPredicate(
			[&](const FRunwaySummary& R) { return SameStrip(R, End.Threshold, FarEnd); });
		if (bKnown) { continue; }

		FRunwaySummary R;
		R.End = End;
		R.Profile = Profile;
		Runways.Add(R);
	}
	return Runways;
}

FAirsideCapability AirsideCapability::Summarise(const URoadNetwork& Network)
{
	FAirsideCapability Out;
	// The runway walk is SummariseRunways, so CheckArrival and this agree on what one runway is.
	Out.Runways = SummariseRunways(Network);

	const TArray<FEntityInstance>& Entities = Network.GetEntities();
	for (int32 Index = 0; Index < Entities.Num(); ++Index)
	{
		const FEntityInstance& E = Entities[Index];
		if (!E.bAlive) { continue; }
		FStandSummary S;
		S.Entity = Network.EntityIdAt(Index);
		S.DesignWingspan = E.DesignWingspan;
		S.Pavement = E.Pavement;
		for (const FResolvedAnchor& Anchor : E.ResolvedAnchors)
		{
			S.AnchorRoles.AddUnique(Anchor.Role);
		}
		Out.Stands.Add(S);
	}
	return Out;
}
