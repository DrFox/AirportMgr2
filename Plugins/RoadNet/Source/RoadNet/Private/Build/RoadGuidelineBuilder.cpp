#include "Build/RoadGuidelineBuilder.h"

#include "Build/RoadMeshBuilder.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"

namespace
{
	/**
	 * Where a guideline crosses a cut line, as a lerp parameter from the right cut to the
	 * left. Mirrors FRoadProfileBands' convention so the two never disagree about which
	 * way "right to left" runs.
	 */
	double AlphaForOffset(const URoadProfile* Profile, double CentreOffset)
	{
		const double HalfLeft  = Profile ? FMath::Max(Profile->GetHalfWidthLeft(),  0.0) : 0.0;
		const double HalfRight = Profile ? FMath::Max(Profile->GetHalfWidthRight(), 0.0) : 0.0;
		const double Total = HalfLeft + HalfRight;
		if (Total <= 0.0)
		{
			return 0.5;
		}
		return FMath::Clamp((CentreOffset + HalfRight) / Total, 0.0, 1.0);
	}
}

void FRoadGuidelineBuilder::Build(URoadNetwork& Network, const FRoadSolveResult& Solved)
{
	const TArray<FRoadSegment>& Segments = Network.GetSegments();

	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		const FRoadSegment& Segment = Segments[Index];
		if (!Segment.bAlive || !Segment.bSolvedA || !Segment.bSolvedB)
		{
			continue;
		}

		FRoadSegmentId SegmentId;
		SegmentId.Index = Index;
		SegmentId.Generation = Segment.Generation;

		const URoadProfile* Profile = Segment.Profile.Get();
		if (Profile == nullptr)
		{
			continue;
		}

		for (const FProfileGuideline& Declared : Profile->Guidelines)
		{
			const double Alpha = AlphaForOffset(Profile, Declared.CentreOffset);

			// End B's cut line is authored from B's point of view, so its left is this
			// segment's right walking A to B - swapped here exactly as AddSegment swaps it.
			const FVector2D AtA =
				FRoadMeshBuilder::CutLinePoint(Segment.RightCutA, Segment.LeftCutA, Alpha);
			const FVector2D AtB =
				FRoadMeshBuilder::CutLinePoint(Segment.LeftCutB, Segment.RightCutB, Alpha);

			FGuidelineEdge Edge;
			Edge.A = Network.AddGuidelineNode(AtA);
			Edge.B = Network.AddGuidelineNode(AtB);
			Edge.Control = (AtA + AtB) * 0.5;   // straight until curves are derived
			Edge.AllowedTraffic = FTrafficMask::Only(Declared.Class);
			Edge.AllowedTraffic.Add(ETraversalClass::Emergency);
			Edge.Direction = Declared.Direction;
			Edge.Width = Declared.Width;
			Edge.MaxWingspan = Declared.MaxWingspan;
			Edge.DerivedFrom = SegmentId;
			Edge.bDerived = true;

			Network.AddGuidelineEdge(MoveTemp(Edge));
		}
	}
}
