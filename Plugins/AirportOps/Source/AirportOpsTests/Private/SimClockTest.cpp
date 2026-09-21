#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/SimClock.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSimClockSpeedTest,
	"AirportOps.Model.SimClock.Speed",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSimClockSpeedTest::RunTest(const FString& Parameters)
{
	USimClock* Clock = NewObject<USimClock>();
	Clock->RealSecondsPerGameDay = 1200.0;  // 20 real minutes per day -> 72 game s per real s

	Clock->Advance(1.0);
	TestEqual(TEXT("x1 advances by the day compression alone"), Clock->Now(), 72.0, 1e-9);

	Clock->SetSpeed(ESimSpeed::X2);
	Clock->Advance(1.0);
	TestEqual(TEXT("x2 doubles the compressed rate"), Clock->Now(), 72.0 + 144.0, 1e-9);

	Clock->SetSpeed(ESimSpeed::Paused);
	Clock->Advance(10.0);
	TestEqual(TEXT("paused advances nothing however long the real step"), Clock->Now(), 216.0, 1e-9);

	TestEqual(TEXT("Multiplier is the speed table, not the compression"),
		USimClock::Multiplier(ESimSpeed::X8), 8.0, 1e-12);
	TestEqual(TEXT("paused multiplier is zero"), USimClock::Multiplier(ESimSpeed::Paused), 0.0, 1e-12);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSimClockDayTest,
	"AirportOps.Model.SimClock.Day",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSimClockDayTest::RunTest(const FString& Parameters)
{
	USimClock* Clock = NewObject<USimClock>();
	Clock->RealSecondsPerGameDay = 1.0;  // one real second is one game day: makes the arithmetic readable

	Clock->Advance(1.5);
	TestEqual(TEXT("a day and a half is day 1"), Clock->Day(), 1);
	TestEqual(TEXT("time of day wraps to half a day"), Clock->TimeOfDay(), USimClock::SecondsPerDay * 0.5, 1e-6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSimClockSchedulerTest,
	"AirportOps.Model.SimClock.Scheduler",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSimClockSchedulerTest::RunTest(const FString& Parameters)
{
	USimClock* Clock = NewObject<USimClock>();
	Clock->RealSecondsPerGameDay = USimClock::SecondsPerDay;  // 1 real s == 1 game s, so steps read plainly

	TArray<FString> Fired;
	Clock->At(5.0, [&Fired]() { Fired.Add(TEXT("at5")); });
	Clock->Every(2.0, [&Fired]() { Fired.Add(TEXT("every2")); });
	const int32 Cancelled = Clock->At(3.0, [&Fired]() { Fired.Add(TEXT("cancelled")); });
	TestTrue(TEXT("a pending entry can be cancelled"), Clock->Cancel(Cancelled));
	TestFalse(TEXT("cancelling twice reports nothing to cancel"), Clock->Cancel(Cancelled));

	Clock->Advance(1.0);
	TestEqual(TEXT("nothing is due at t=1"), Fired.Num(), 0);

	// ONE big step. The point of the scheduler is that a large step drains every due entry
	// in TIME order, not that it fires whatever happens to be due at the end - a x8 frame
	// crossing several deliveries must post them all, oldest first.
	Clock->Advance(5.0);  // now t=6: every2 at 2,4,6; at5 at 5
	const TArray<FString> Expected = { TEXT("every2"), TEXT("every2"), TEXT("at5"), TEXT("every2") };
	TestEqual(TEXT("due entries fire in time order across one large step"), Fired, Expected);

	// An entry scheduled in the past fires on the next Advance rather than being lost.
	Clock->At(1.0, [&Fired]() { Fired.Add(TEXT("late")); });
	Clock->Advance(0.0);
	TestEqual(TEXT("a past-due entry fires on the next advance"), Fired.Last(), FString(TEXT("late")));

	TestEqual(TEXT("Every rejects a non-positive interval"), Clock->Every(0.0, []() {}), static_cast<int32>(INDEX_NONE));
	return true;
}

/**
 * THE LISTS-THAT-MUST-AGREE TEST. ESimSpeed and USimClock::SpeedLadder are two lists of
 * the same thing, and CLAUDE.md's rule is to check where the list is CONSUMED. A speed
 * added to the enum but missed off the ladder compiles and runs and is simply unreachable:
 * the player presses "faster" at the top rung and nothing happens, with no error anywhere.
 *
 * It walks StaticEnum rather than a hand-written list, so the enum is the single source of
 * truth and this cannot rot in the same direction as the bug it catches.
 *
 * BESIDE ITS SUBJECT since issue #191: the ladder moved off UOpsRuntime onto this class,
 * along with ResumeSpeed and StepSpeed/TogglePause - see USimClock::ResumeSpeed's own
 * comment for the save bug that made the move worth doing.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSimClockSpeedLadderTest,
	"AirportOps.Model.SimClock.SpeedLadderCoversEveryRung",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSimClockSpeedLadderTest::RunTest(const FString& Parameters)
{
	const TArrayView<const ESimSpeed> Ladder = USimClock::SpeedLadder();
	const UEnum* Enum = StaticEnum<ESimSpeed>();
	if (Enum == nullptr)
	{
		AddError(TEXT("StaticEnum<ESimSpeed>() is null"));
		return false;
	}

	for (int32 I = 0; I < Enum->NumEnums() - 1; ++I)   // -1 skips the generated _MAX
	{
		const ESimSpeed Value = static_cast<ESimSpeed>(Enum->GetValueByIndex(I));
		const FString Name = Enum->GetNameStringByIndex(I);

		// Paused is deliberately not a rung: it is TogglePause's business, and stepping
		// faster from paused means resume, not unpause into the slowest speed.
		if (Value == ESimSpeed::Paused)
		{
			TestFalse(TEXT("Paused is not a rung on the speed ladder"), Ladder.Contains(Value));
			continue;
		}

		TestTrue(*FString::Printf(TEXT("ESimSpeed::%s appears on the speed ladder, so the "
			"player can actually reach it"), *Name), Ladder.Contains(Value));

		TestTrue(*FString::Printf(TEXT("ESimSpeed::%s has a non-zero multiplier"), *Name),
			USimClock::Multiplier(Value) > 0.0);
	}

	// Ordered, fastest last, because StepSpeed indexes it and a shuffled ladder would make
	// "faster" sometimes slower.
	for (int32 I = 1; I < Ladder.Num(); ++I)
	{
		TestTrue(*FString::Printf(TEXT("rung %d is faster than rung %d"), I, I - 1),
			USimClock::Multiplier(Ladder[I]) > USimClock::Multiplier(Ladder[I - 1]));
	}

	TestEqual(TEXT("x16 multiplies by 16"), USimClock::Multiplier(ESimSpeed::X16), 16.0, 1e-12);
	TestEqual(TEXT("x32 multiplies by 32"), USimClock::Multiplier(ESimSpeed::X32), 32.0, 1e-12);
	return true;
}

/**
 * STEP AND PAUSE, WORLD-FREE (issue #191, #98 partial). StepSpeed, TogglePause and ResumeSpeed
 * moved here from UOpsRuntime so the SAVED object and the logic that computes what it holds
 * are the same one - see ResumeSpeed's own comment on this class for the save bug that made
 * the move worth doing, not just tidier. UOpsRuntime keeps methods of the same name, but they
 * are now a two-line forward to this class plus the push into the actor and the event bus,
 * which is Present/'s job and needs a world (AirportOps.Present.Runtime covers that half).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSimClockStepAndPauseTest,
	"AirportOps.Model.SimClock.StepAndPause",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSimClockStepAndPauseTest::RunTest(const FString& Parameters)
{
	USimClock* Clock = NewObject<USimClock>();
	TestEqual(TEXT("a bare clock starts at x1"), Clock->GetSpeed(), ESimSpeed::X1);

	Clock->StepSpeed(+1);
	TestEqual(TEXT("stepping climbs one rung"), Clock->GetSpeed(), ESimSpeed::X2);

	Clock->TogglePause();
	TestEqual(TEXT("pausing sets Paused"), Clock->GetSpeed(), ESimSpeed::Paused);
	Clock->TogglePause();
	TestEqual(TEXT("unpausing restores the speed it was paused from"), Clock->GetSpeed(), ESimSpeed::X2);

	// Asked of the LADDER rather than a literal - see SpeedLadderCoversEveryRung's own
	// comment for the maintenance cost of a hard-coded top rung.
	const TArrayView<const ESimSpeed> Ladder = USimClock::SpeedLadder();
	Clock->StepSpeed(Ladder.Num() + 2);
	TestEqual(TEXT("stepping past the top clamps at the fastest rung"),
		Clock->GetSpeed(), Ladder.Last());
	Clock->StepSpeed(-Ladder.Num() - 4);
	TestEqual(TEXT("stepping past the bottom clamps at the slowest rung, never Paused"),
		Clock->GetSpeed(), Ladder[0]);

	// Stepping WHILE paused steps from ResumeSpeed, not from Paused itself - Paused has no
	// rung index, and "faster" from a stop means resume, one notch up from where the player
	// left it, not unpause into the slowest rung.
	Clock->SetSpeed(ESimSpeed::X4);
	Clock->TogglePause();
	Clock->StepSpeed(+1);
	TestEqual(TEXT("stepping while paused resumes one rung above where the pause was from"),
		Clock->GetSpeed(), ESimSpeed::X8);
	return true;
}

/**
 * A new game opens in the morning, not at midnight. Starting at 00:00 put the sun at its
 * dusk floor with nothing due for six game hours, which is a poor first thing to see.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSimClockStartHourTest,
	"AirportOps.Model.SimClock.StartsAtAnHourOfDayOne",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSimClockStartHourTest::RunTest(const FString& Parameters)
{
	USimClock* Clock = NewObject<USimClock>(GetTransientPackage());

	Clock->StartAtHour(9.0);
	TestEqual(TEXT("09:00 is 32400 game seconds in"), Clock->Now(), 32400.0, 1e-9);
	TestEqual(TEXT("and it is still day one"), Clock->Day(), 0);
	TestEqual(TEXT("time of day agrees"), Clock->TimeOfDay(), 32400.0, 1e-9);

	// The scheduler must book relative to the NEW time. Every() fires at Now() + Interval,
	// which is why StartAtHour has to be called before anything is scheduled - this pins
	// the half of that contract the clock itself owns.
	int32 Fired = 0;
	Clock->RealSecondsPerGameDay = USimClock::SecondsPerDay;   // TimeScale 1: advance 1:1
	Clock->Every(3600.0, [&Fired]() { ++Fired; });
	Clock->Advance(3599.0);
	TestEqual(TEXT("nothing is due just short of the first hour"), Fired, 0);
	Clock->Advance(2.0);
	TestEqual(TEXT("the first firing is an hour after the start time"), Fired, 1);

	Clock->StartAtHour(24.0);
	TestEqual(TEXT("24:00 wraps to midnight rather than overflowing the day"), Clock->Now(), 0.0, 1e-9);
	return true;
}

#endif
