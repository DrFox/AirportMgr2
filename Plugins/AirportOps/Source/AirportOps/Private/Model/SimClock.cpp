#include "Model/SimClock.h"
#include "AirportOpsLog.h"

int32 USimClock::Day() const
{
	return static_cast<int32>(FMath::FloorToDouble(GameSeconds / SecondsPerDay));
}

double USimClock::TimeOfDay() const
{
	return GameSeconds - Day() * SecondsPerDay;
}

void USimClock::SetSpeed(ESimSpeed NewSpeed)
{
	if (Speed == NewSpeed)
	{
		return;
	}
	Speed = NewSpeed;
	UE_LOG(LogAirportOps, Log, TEXT("Sim speed x%.0f"), Multiplier(Speed));
}

double USimClock::Multiplier(ESimSpeed InSpeed)
{
	switch (InSpeed)
	{
	case ESimSpeed::Paused: return 0.0;
	case ESimSpeed::X1:     return 1.0;
	case ESimSpeed::X2:     return 2.0;
	case ESimSpeed::X4:     return 4.0;
	case ESimSpeed::X8:     return 8.0;
	}
	return 1.0;
}

double USimClock::TimeScale() const
{
	// Guarded rather than asserted: a zero from a mis-authored scenario should give a
	// frozen clock and a log line, not a division by zero in Tick.
	if (RealSecondsPerGameDay <= 0.0)
	{
		return 0.0;
	}
	return Multiplier(Speed) * (SecondsPerDay / RealSecondsPerGameDay);
}

void USimClock::Advance(double RealDeltaSeconds)
{
	GameSeconds += RealDeltaSeconds * TimeScale();

	// Drain in due order, re-scanning after every callback: a callback may schedule
	// something that is ALREADY due (an At in the past), and it must fire in this same
	// drain rather than wait a frame. Bounded by the entries' own due times, since every
	// repeating entry advances past Now() on each fire.
	while (true)
	{
		int32 Earliest = INDEX_NONE;
		for (int32 Index = 0; Index < Entries.Num(); ++Index)
		{
			if (Entries[Index].Due <= GameSeconds
				&& (Earliest == INDEX_NONE || Entries[Index].Due < Entries[Earliest].Due))
			{
				Earliest = Index;
			}
		}
		if (Earliest == INDEX_NONE)
		{
			break;
		}

		// Copy the callback out before firing: the callback may Cancel or add entries,
		// which reallocates the array under a reference.
		TFunction<void()> Fire = Entries[Earliest].Callback;
		if (Entries[Earliest].Interval > 0.0)
		{
			Entries[Earliest].Due += Entries[Earliest].Interval;
		}
		else
		{
			Entries.RemoveAt(Earliest);
		}
		Fire();
	}
}

int32 USimClock::Add(double Due, double Interval, TFunction<void()>&& Callback)
{
	FEntry Entry;
	Entry.Handle = NextHandle++;
	Entry.Due = Due;
	Entry.Interval = Interval;
	Entry.Callback = MoveTemp(Callback);
	Entries.Add(MoveTemp(Entry));
	return Entries.Last().Handle;
}

int32 USimClock::At(double GameTime, TFunction<void()> Callback)
{
	return Add(GameTime, 0.0, MoveTemp(Callback));
}

int32 USimClock::Every(double Interval, TFunction<void()> Callback)
{
	if (Interval <= 0.0)
	{
		UE_LOG(LogAirportOps, Warning, TEXT("SimClock::Every refused: interval %.3f is not positive"), Interval);
		return INDEX_NONE;
	}
	return Add(GameSeconds + Interval, Interval, MoveTemp(Callback));
}

bool USimClock::Cancel(int32 Handle)
{
	const int32 Removed = Entries.RemoveAll([Handle](const FEntry& E) { return E.Handle == Handle; });
	return Removed > 0;
}
