#include "Model/OpsSafetyNet.h"
#include "AirportOpsLog.h"
#include "Model/OpsEventBus.h"
#include "Model/SimClock.h"

void FOpsSafetyNet::Bind(FOpsEventBus& InBus, USimClock& InClock, double InPeriodSeconds)
{
	Bus = &InBus;
	Clock = &InClock;
	PeriodSeconds = InPeriodSeconds;
}

void FOpsSafetyNet::Want(FName Pass, bool bWanted)
{
	if (bWanted)
	{
		Wanting.AddUnique(Pass);
	}
	else
	{
		Wanting.Remove(Pass);
	}
	Rearm();
}

void FOpsSafetyNet::CancelAll()
{
	Wanting.Reset();
	Rearm();
}

void FOpsSafetyNet::Rearm()
{
	if (Clock == nullptr || Bus == nullptr)
	{
		return;
	}
	const bool bWanted = Wanting.Num() > 0;
	if (bWanted && Handle == INDEX_NONE)
	{
		Handle = Clock->Every(PeriodSeconds, [this]() { Fire(); });
	}
	else if (!bWanted && Handle != INDEX_NONE)
	{
		Clock->Cancel(Handle);
		Handle = INDEX_NONE;
	}
}

void FOpsSafetyNet::Fire()
{
	// EACH WANTING PASS RUNS ITSELF, and only while it wants to - another pass's waiting is no reason to run this one. MARKED AS THE
	// NET'S: the one cause a run is suspect for (EPassCause). Copied first: a marked pass cannot run inside this call, but the list is
	// the net's own and a future Mark that reached back into Want would otherwise edit it mid-walk.
	const TArray<FName> Marked = Wanting;
	for (const FName Pass : Marked)
	{
		Bus->MarkDirty(Pass, EPassCause::SafetyNet);
	}
}
