#include "Model/SimClock.h"
#include "AirportOpsLog.h"
#include "Model/GameTimeText.h"

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

TArrayView<const ESimSpeed> USimClock::SpeedLadder()
{
	// A SECOND LIST THAT MUST AGREE WITH ESimSpeed, and exactly the kind CLAUDE.md names.
	// A speed added to the enum but not to this ladder compiles, runs, and is simply
	// unreachable: the player presses "faster" at the top rung and nothing happens, with
	// no error anywhere. It is exposed rather than a static local precisely so a test can
	// read it - AirportOps.Model.SimClock.SpeedLadderCoversEveryRung walks StaticEnum and
	// fails if the two ever drift apart.
	//
	// AND A THIRD FIGURE SIZED FROM THE TOP RUNG: Airside's FTrafficRules::MaxSubsteps must cover a
	// frame at the fastest speed, or ordinary top-speed play is taken in coarse steps.
	// ENFORCED BY: AirportOps.Model.SimClock.SubstepCeilingCoversTheSpeedLadder (reads every rung
	// from here and runs the real Advance split)
	static const ESimSpeed Ladder[] = {
		ESimSpeed::X1, ESimSpeed::X2, ESimSpeed::X4, ESimSpeed::X8, ESimSpeed::X16, ESimSpeed::X32 };
	return MakeArrayView(Ladder, UE_ARRAY_COUNT(Ladder));
}

void USimClock::StepSpeed(int32 Delta)
{
	const TArrayView<const ESimSpeed> Ladder = SpeedLadder();
	const int32 Rungs = Ladder.Num();

	// Stepping while paused steps from ResumeSpeed, which is what a player pressing
	// "faster" while paused means: resume, one notch up from where they were.
	const ESimSpeed From = Speed == ESimSpeed::Paused ? ResumeSpeed : Speed;
	int32 Index = 0;
	for (int32 I = 0; I < Rungs; ++I)
	{
		if (Ladder[I] == From) { Index = I; }
	}
	Index = FMath::Clamp(Index + Delta, 0, Rungs - 1);
	ResumeSpeed = Ladder[Index];
	SetSpeed(ResumeSpeed);
}

void USimClock::TogglePause()
{
	if (Speed == ESimSpeed::Paused)
	{
		SetSpeed(ResumeSpeed);
	}
	else
	{
		ResumeSpeed = Speed;
		SetSpeed(ESimSpeed::Paused);
	}
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
	case ESimSpeed::X16:    return 16.0;
	case ESimSpeed::X32:    return 32.0;
	}
	return 1.0;
}

void USimClock::StartAtHour(double Hour)
{
	// BEFORE ANYTHING IS SCHEDULED. USimClock::Every registers its first firing at
	// Now() + Interval, so moving the clock afterwards would leave every repeating entry
	// due at a time that no longer means what it did when it was booked.
	GameSeconds = FMath::Fmod(FMath::Max(0.0, Hour), 24.0) * 3600.0;
	UE_LOG(LogAirportOps, Log, TEXT("Clock starts at day %d, %s"), Day() + 1, *GameTimeText::TimeOfDay(TimeOfDay()));
}

void USimClock::SetUniformDay(double RealSecondsPerDay)
{
	const double DaylightHours = FMath::Clamp(DuskHour - DawnHour, 0.0, 24.0);
	RealSecondsDaylight = RealSecondsPerDay * DaylightHours / 24.0;
	RealSecondsNight = RealSecondsPerDay - RealSecondsDaylight;
}

bool USimClock::IsDaylight(double TimeOfDaySeconds) const
{
	const double Hour = TimeOfDaySeconds / 3600.0;
	return Hour >= DawnHour && Hour < DuskHour;
}

double USimClock::GameSecondsPerRealSecond(double TimeOfDaySeconds) const
{
	const double DaylightHours = FMath::Clamp(DuskHour - DawnHour, 0.0, 24.0);
	const bool bDay = IsDaylight(TimeOfDaySeconds);
	const double Hours = bDay ? DaylightHours : 24.0 - DaylightHours;
	const double Real = bDay ? RealSecondsDaylight : RealSecondsNight;
	// Guarded rather than asserted: a zero from a mis-authored scenario should give a
	// frozen clock and a log line, not a division by zero in Tick.
	return Real > 0.0 ? Hours * 3600.0 / Real : 0.0;
}

double USimClock::GameSecondsOfMovement(double MovementSeconds) const
{
	return MovementSeconds * GameSecondsPerRealSecond(TimeOfDay());
}

double USimClock::TimeScale() const
{
	return Multiplier(Speed) * GameSecondsPerRealSecond(TimeOfDay());
}

double USimClock::GameSecondsToBandEdge() const
{
	// THE BAND EDGES ARE DAWN AND DUSK, wrapping midnight - Advance's own piecewise step, which used to compute this inline: ONE calculation of
	// where the rate changes, so a caller booking a look across a band (#445) and the clock that crosses it cannot disagree.
	const double Dawn = DawnHour * 3600.0;
	const double Dusk = DuskHour * 3600.0;
	const double Tod = TimeOfDay();
	if (Tod < Dawn)      { return Dawn - Tod; }
	if (Tod < Dusk)      { return Dusk - Tod; }
	return SecondsPerDay - Tod + Dawn;
}

void USimClock::Advance(double RealDeltaSeconds)
{
	// PIECEWISE ACROSS DAWN AND DUSK. A single multiply by TimeScale() would run a step that
	// began at 19:59 at the daylight rate all the way through the night - and a long frame, or
	// a test's one big step, is exactly when that is wrong by the most.
	double Remaining = FMath::Max(RealDeltaSeconds, 0.0);
	while (Remaining > 0.0)
	{
		const double Rate = TimeScale();
		if (Rate <= 0.0)
		{
			break;
		}
		// Game seconds to the next band edge, wrapping midnight.
		const double Edge = GameSecondsToBandEdge();
		const double RealToEdge = Edge / Rate;
		if (Remaining <= RealToEdge)
		{
			GameSeconds += Remaining * Rate;
			break;
		}
		GameSeconds += Edge;
		Remaining -= RealToEdge;
	}

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

int32 USimClock::EveryFrom(double FirstDue, double Interval, TFunction<void()> Callback)
{
	if (Interval <= 0.0)
	{
		UE_LOG(LogAirportOps, Warning, TEXT("SimClock::EveryFrom refused: interval %.3f is not positive"), Interval);
		return INDEX_NONE;
	}
	return Add(FirstDue, Interval, MoveTemp(Callback));
}

double USimClock::NextDayStart() const
{
	return (Day() + 1) * SecondsPerDay;
}

bool USimClock::Cancel(int32 Handle)
{
	const int32 Removed = Entries.RemoveAll([Handle](const FEntry& E) { return E.Handle == Handle; });
	return Removed > 0;
}
