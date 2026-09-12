#include "Model/StandAllocator.h"

#include "AirportOpsLog.h"
#include "Model/Flight.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"

bool UStandAllocator::Reserve(UGroundTraffic& Traffic, const URoadNetwork& Network, UFlight& Flight)
{
	const double Wingspan = Flight.Airframe.Wingspan;
	if (Wingspan <= 0.0)
	{
		// A flight with no airframe is a fixture nobody filled in. Refuse rather than hold
		// the smallest stand on the field for it.
		return false;
	}

	FEntityInstanceId Best;
	double BestWingspan = TNumericLimits<double>::Max();

	const TArray<FEntityInstance>& Entities = Network.GetEntities();
	for (int32 Index = 0; Index < Entities.Num(); ++Index)
	{
		const FEntityInstance& Stand = Entities[Index];
		if (!Stand.bAlive || !Stand.PoseNode.IsSet() || Stand.DesignWingspan < Wingspan)
		{
			continue;
		}

		// HELD IS ASKED OF THE TABLE, not of any book-keeping here: a stand with an aeroplane
		// ON it is held by that agent, and a stand promised to another flight is held by that
		// flight's holder id. One question covers both, which is the point of one table.
		if (Traffic.IsStandHeld(Stand.PoseNode, Flight.HolderId()))
		{
			continue;
		}

		if (Stand.DesignWingspan < BestWingspan)
		{
			BestWingspan = Stand.DesignWingspan;
			Best = Network.EntityIdAt(Index);
		}
	}

	if (!Best.IsSet())
	{
		return false;
	}

	const FEntityInstance* Chosen = Network.GetEntity(Best);
	if (Chosen == nullptr || !Traffic.HoldStand(Flight.HolderId(), Chosen->PoseNode))
	{
		return false;
	}

	Flight.Stand = Best;
	UE_LOG(LogAirportOps, Log,
		TEXT("Flight %d holds stand %d: a %.0f uu stand for a %.0f uu span"),
		Flight.Id, Best.Index, BestWingspan, Wingspan);
	return true;
}

void UStandAllocator::Release(UGroundTraffic& Traffic, UFlight& Flight)
{
	Traffic.ReleaseHold(Flight.HolderId());
}

void UStandAllocator::Reapply(UGroundTraffic& Traffic, const URoadNetwork& Network,
	const TArray<UFlight*>& Held)
{
	for (UFlight* Flight : Held)
	{
		if (Flight == nullptr || !Flight->Stand.IsSet())
		{
			continue;
		}

		const FEntityInstance* Stand = Network.GetEntity(Flight->Stand);
		if (Stand == nullptr || !Stand->PoseNode.IsSet())
		{
			// The stand was deleted under an accepted flight. Finding it another one belongs
			// to the sequencer's divert path, which does not exist yet - but the silence is
			// what must not happen: an aeroplane is coming for a stand nobody is holding.
			UE_LOG(LogAirportOps, Warning,
				TEXT("Flight %d held stand %d, which is gone from the graph"),
				Flight->Id, Flight->Stand.Index);
			continue;
		}

		Traffic.HoldStand(Flight->HolderId(), Stand->PoseNode);
	}
}
