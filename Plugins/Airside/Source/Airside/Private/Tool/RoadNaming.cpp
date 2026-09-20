#include "Tool/RoadNaming.h"

#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Model/RoadTraffic.h"
#include "Profiles/RoadProfile.h"
#include "Solve/RunwayDesignator.h"

bool RoadNaming::ReferenceOf(const URoadNetwork& Network, FRoadSegmentId Segment,
	SnapGuide::EReference& Out)
{
	const FRoadSegment* Road = Network.GetSegment(Segment);
	if (Road == nullptr || !Road->bAlive)
	{
		return false;
	}

	// THE RUNWAY QUESTION FIRST, because a runway's cross-section may well admit a vehicle and
	// the service-road test below would then claim it.
	if (Network.IsRunwaySegment(Segment))
	{
		Out = SnapGuide::EReference::Runway;
		return true;
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
				Out = SnapGuide::EReference::ServiceRoad;
				return true;
			}
		}
	}

	Out = SnapGuide::EReference::Taxiway;
	return true;
}

FString RoadNaming::Describe(const URoadNetwork& Network, FRoadSegmentId Segment)
{
	// BUILT ON THE CLASSIFICATION ABOVE rather than repeating it. These two used to be one
	// function that returned only the string, and the guide sources then had to ask the
	// profile themselves to learn the column - a second definition of "service road" living a
	// few hundred lines from this one. See this namespace's own header.
	SnapGuide::EReference Reference = SnapGuide::EReference::Taxiway;
	if (!ReferenceOf(Network, Segment, Reference))
	{
		return FString();
	}

	switch (Reference)
	{
	case SnapGuide::EReference::Runway:
	{
		// HONOURED, NOT ASSUMED: a runway with an end the graph cannot see has no designator
		// to print, and "runway " with nothing after it would read as a bug on screen.
		const FRoadSegment* Road = Network.GetSegment(Segment);
		const FRoadNode* A = Road != nullptr ? Network.GetNode(Road->A) : nullptr;
		const FRoadNode* B = Road != nullptr ? Network.GetNode(Road->B) : nullptr;
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

	case SnapGuide::EReference::ServiceRoad:
		return TEXT("the service road");

	default:
		// TAXIWAY - and a `default` rather than a case of its own BECAUSE the other four
		// members of EReference cannot arrive here: ReferenceOf answers with three. Spelling
		// them out would be four live-looking branches that no call can reach.
		return TEXT("the taxiway");
	}
}
