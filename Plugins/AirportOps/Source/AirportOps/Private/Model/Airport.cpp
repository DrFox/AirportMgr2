#include "Model/Airport.h"
#include "AirportOpsLog.h"
#include "Model/AirsideCapability.h"
#include "Model/OpsEventBus.h"
#include "Model/RoadNetwork.h"

EAirportStatus UAirport::Derive(bool bInClosedByPlayer, bool bHasRunway)
{
	// INTENT FIRST: only the player reopens an airport they closed, so deleting and rebuilding a runway
	// must not open it behind their back.
	if (bInClosedByPlayer)
	{
		return EAirportStatus::ClosedByPlayer;
	}
	return bHasRunway ? EAirportStatus::Open : EAirportStatus::NoRunway;
}

bool UAirport::HasRunway(const URoadNetwork& Network)
{
	// THE WALK DefaultApproachFocus AND THE BAR'S HasRunway ALREADY MAKE (AirsideCapability), so "a runway"
	// means one thing. Asked on a network change or a command, not per frame.
	return AirsideCapability::SummariseRunways(Network).Num() > 0;
}

bool UAirport::Refresh(const URoadNetwork& Network)
{
	const EAirportStatus Old = Current;
	Current = Derive(bClosedByPlayer, HasRunway(Network));
	if (Current == Old)
	{
		return false;
	}
	// SAID, every change: "why are there no offers?" is answered by this line or by its absence.
	UE_LOG(LogAirportOps, Log, TEXT("Airport: %s -> %s"), *UEnum::GetValueAsString(Old), *UEnum::GetValueAsString(Current));
	if (Bus != nullptr)
	{
		Bus->Publish(FAirportStatusChangedEvent{ Old, Current });
	}
	return true;
}

void UAirport::Reseat(const URoadNetwork& Network)
{
	// NO EVENT - see the class comment. Logged, so a load that came back closed says so.
	Current = Derive(bClosedByPlayer, HasRunway(Network));
	UE_LOG(LogAirportOps, Log, TEXT("Airport: %s (re-derived, no change announced)"), *UEnum::GetValueAsString(Current));
}

bool UAirport::SetClosedByPlayer(bool bClosed, const URoadNetwork& Network)
{
	bClosedByPlayer = bClosed;
	return Refresh(Network);
}
