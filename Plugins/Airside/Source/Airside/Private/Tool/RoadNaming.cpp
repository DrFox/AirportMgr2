#include "Tool/RoadNaming.h"

#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Model/RoadTraffic.h"
#include "Profiles/RoadProfile.h"
#include "Solve/RunwayDesignator.h"

FString RoadNaming::Describe(const URoadNetwork& Network, FRoadSegmentId Segment)
{
	const FRoadSegment* Road = Network.GetSegment(Segment);
	if (Road == nullptr || !Road->bAlive)
	{
		return FString();
	}

	// THE RUNWAY QUESTION FIRST, because a runway's cross-section may well admit a vehicle and
	// the service-road test below would then claim it.
	if (Network.IsRunwaySegment(Segment))
	{
		const FRoadNode* A = Network.GetNode(Road->A);
		const FRoadNode* B = Network.GetNode(Road->B);
		if (A != nullptr && B != nullptr)
		{
			const FString Pair = RunwayDesignator::ToPairText(B->Position - A->Position);
			if (!Pair.IsEmpty())
			{
				return FString::Printf(TEXT("runway %s"), *Pair);
			}
		}
		return TEXT("the runway");
	}

	// ASKED OF THE GUIDELINES THE PROFILE DECLARES, which is what FPlotPlaceTool's own
	// IsServiceRoad asks and for the reason its comment gives: the segment carries no
	// ERoadKind, only a profile, and "a truck may drive here" is exactly what a GroundVehicle
	// guideline means.
	if (Road->Profile != nullptr)
	{
		for (const FProfileGuideline& Guideline : Road->Profile->Guidelines)
		{
			if (Guideline.Class == ETraversalClass::GroundVehicle)
			{
				return TEXT("the service road");
			}
		}
	}

	return TEXT("the taxiway");
}
