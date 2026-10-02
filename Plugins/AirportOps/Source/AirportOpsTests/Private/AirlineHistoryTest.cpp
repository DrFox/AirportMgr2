#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/AirlineHistory.h"
#include "Model/AirlineRoster.h"
#include "Model/OpsEventBus.h"
#include "Model/OpsSave.h"
#include "Model/RoadNetwork.h"

#if WITH_DEV_AUTOMATION_TESTS

// THE AIRLINE HISTORY (spec 2026-10-02-airlines-panel section 1.2), world-free. Prefixed names: the test module is a unity build.

namespace
{
	using ECause = EAirlineSatisfactionCause;

	const FAirlineCauseTally* HistTestTally(const FAirlineDay& Day, ECause Kind)
	{
		return Day.Tallies.FindByPredicate([Kind](const FAirlineCauseTally& T) { return T.Kind == Kind; });
	}

	/** A roster wired to a history and a bus the way UOpsRuntime wires them, with one airline "A" at 0.5. */
	struct FHistoryFixture
	{
		UAirlineRoster* Roster = nullptr;
		UAirlineHistory* History = nullptr;
		FOpsEventBus Bus;

		FHistoryFixture()
		{
			Roster = NewObject<UAirlineRoster>(GetTransientPackage());
			History = NewObject<UAirlineHistory>(GetTransientPackage());
			Roster->History = History;
			Roster->Bus = &Bus;
			Roster->Ensure(TEXT("A"));
			Bus.BeginWiring();
			Bus.Subscribe<FFlightOffBlocksEvent>(EOpsTier::Reaction, TEXT("Airlines"), [this](const FFlightOffBlocksEvent& E) { Roster->OnFlightOffBlocks(E); });
			Bus.Subscribe<FDayEndedEvent>(EOpsTier::Reaction, TEXT("Airlines"), [this](const FDayEndedEvent& E) { Roster->OnDayEnded(E); });
			Bus.EndWiring();
		}

		void OffBlocks(double LateBy)
		{
			Bus.Publish(FFlightOffBlocksEvent{ 1, TEXT("A"), LateBy });
			Bus.Drain();
		}

		void EndDay(int32 Day = 1)
		{
			Bus.Publish(FDayEndedEvent{ Day });
			Bus.Drain();
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirlineHistoryTalliesTest, "AirportOps.Model.AirlineHistory.RecordTallies",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAirlineHistoryTalliesTest::RunTest(const FString&)
{
	UAirlineHistory* H = NewObject<UAirlineHistory>(GetTransientPackage());
	H->Record(TEXT("A"), ECause::OnTime, 0.03, 0.53);
	H->Record(TEXT("A"), ECause::OnTime, 0.03, 0.56);
	H->Record(TEXT("A"), ECause::LateOffStand, -0.04, 0.52);
	const FAirlineDays* Row = H->Find(TEXT("A"));
	if (!TestNotNull(TEXT("the airline has a row"), Row) || !TestEqual(TEXT("one open day"), Row->Days.Num(), 1)) { return false; }
	const FAirlineDay& Today = Row->Days[0];
	const FAirlineCauseTally* OnTime = HistTestTally(Today, ECause::OnTime);
	const FAirlineCauseTally* Late = HistTestTally(Today, ECause::LateOffStand);
	if (!TestNotNull(TEXT("on-time tally"), OnTime) || !TestNotNull(TEXT("late tally"), Late)) { return false; }
	TestEqual(TEXT("two on time"), OnTime->Count, 2);
	TestEqual(TEXT("worth +0.06"), OnTime->SumDelta, 0.06, 1e-9);
	TestEqual(TEXT("one late"), Late->Count, 1);
	TestEqual(TEXT("worth -0.04"), Late->SumDelta, -0.04, 1e-9);
	TestEqual(TEXT("the close is the last NewSatisfaction"), Today.CloseSatisfaction, 0.52, 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirlineHistoryKeepsSevenTest, "AirportOps.Model.AirlineHistory.CloseDayKeepsSeven",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAirlineHistoryKeepsSevenTest::RunTest(const FString&)
{
	UAirlineHistory* H = NewObject<UAirlineHistory>(GetTransientPackage());
	H->ResetForNewGame(0);
	FAirlineStanding Standing;
	Standing.AirlineId = TEXT("A");
	Standing.Satisfaction = 0.6;
	H->Record(TEXT("A"), ECause::OnTime, 0.03, 0.6);
	for (int32 Index = 0; Index < 9; ++Index)
	{
		H->CloseDay(MakeArrayView(&Standing, 1));
	}
	const FAirlineDays* Row = H->Find(TEXT("A"));
	if (!TestNotNull(TEXT("the row"), Row)) { return false; }
	TestEqual(TEXT("seven days kept, today included"), Row->Days.Num(), UAirlineHistory::DaysKept);
	TestEqual(TEXT("the oldest is day 3 (days 0..9 existed)"), Row->Days[0].Day, 3);
	TestEqual(TEXT("the last is day 9"), Row->Days.Last().Day, 9);
	TestEqual(TEXT("and it is open: nothing tallied"), Row->Days.Last().Tallies.Num(), 0);
	TestEqual(TEXT("carrying the previous close"), Row->Days.Last().CloseSatisfaction, 0.6, 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirlineHistoryDriftDayTest, "AirportOps.Model.AirlineHistory.DriftBelongsToTheDayItCloses",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAirlineHistoryDriftDayTest::RunTest(const FString&)
{
	FHistoryFixture F;
	F.OffBlocks(1500.0);   // late: moves A off its start, so the drift has something to forgive
	F.EndDay();
	const FAirlineDays* Row = F.History->Find(TEXT("A"));
	if (!TestNotNull(TEXT("the row"), Row) || !TestEqual(TEXT("the closed day and the new one"), Row->Days.Num(), 2)) { return false; }
	const FAirlineCauseTally* ClosedDrift = HistTestTally(Row->Days[0], ECause::DailyDrift);
	TestNotNull(TEXT("the drift is tallied in the CLOSED day - the roster records before it closes"), ClosedDrift);
	TestNull(TEXT("and not in the new day"), HistTestTally(Row->Days[1], ECause::DailyDrift));
	TestTrue(TEXT("the new day is open and empty"), Row->Days[1].Tallies.Num() == 0);
	TestEqual(TEXT("the closed day's close is the post-drift satisfaction"), Row->Days[0].CloseSatisfaction, F.Roster->Find(TEXT("A"))->Satisfaction, 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirlineHistoryRolloverTest, "AirportOps.Model.AirlineHistory.RolloverGainsAPointAndRestartsToday",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAirlineHistoryRolloverTest::RunTest(const FString&)
{
	FHistoryFixture F;
	F.OffBlocks(-30.0);
	const FAirlineDays* Row = F.History->Find(TEXT("A"));
	if (!TestNotNull(TEXT("the row"), Row)) { return false; }
	TestEqual(TEXT("one point before the rollover"), Row->Days.Num(), 1);
	F.EndDay();
	TestEqual(TEXT("two after - the trend gained a point"), Row->Days.Num(), 2);
	TestEqual(TEXT("today's tally restarted"), Row->Days.Last().Tallies.Num(), 0);
	for (int32 Index = 0; Index < 8; ++Index) { F.EndDay(); }
	TestEqual(TEXT("never more than seven"), Row->Days.Num(), UAirlineHistory::DaysKept);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirlineHistoryClampTest, "AirportOps.Model.AirlineHistory.ClampedChangeRecordsRealDelta",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAirlineHistoryClampTest::RunTest(const FString&)
{
	FHistoryFixture F;
	F.Roster->Tuning.OnTimeBonus = 0.03;
	F.Roster->Tuning.Start = 0.5;
	// Drive A to 0.99 through the public handlers: a long run of on-time flights clamps at 1.0 on its own.
	for (int32 Index = 0; Index < 40; ++Index) { F.OffBlocks(-30.0); }
	const FAirlineStanding* Standing = F.Roster->Find(TEXT("A"));
	if (!TestNotNull(TEXT("the standing"), Standing)) { return false; }
	TestEqual(TEXT("A is at the ceiling"), Standing->Satisfaction, 1.0, 1e-9);
	const FAirlineDay& Today = F.History->Find(TEXT("A"))->Days.Last();
	const FAirlineCauseTally* OnTime = HistTestTally(Today, ECause::OnTime);
	if (!TestNotNull(TEXT("an on-time tally"), OnTime)) { return false; }
	// 0.5 -> 1.0 is exactly 0.5 of movement, however many +0.03 steps it took; the last step moved only what was left.
	TestEqual(TEXT("the summed delta is what really moved, not 40 x 0.03"), OnTime->SumDelta, 0.5, 1e-9);
	const int32 CountAtCeiling = OnTime->Count;
	F.OffBlocks(-30.0);
	TestEqual(TEXT("at 1.0 another on-time records nothing"), HistTestTally(F.History->Find(TEXT("A"))->Days.Last(), ECause::OnTime)->Count, CountAtCeiling);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirlineHistoryClampedDeltaTest, "AirportOps.Model.AirlineHistory.ClampedFirstStepRecordsTheRemainder",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAirlineHistoryClampedDeltaTest::RunTest(const FString&)
{
	FHistoryFixture F;
	F.Roster->Tuning.OnTimeBonus = 0.03;
	F.Roster->Tuning.Start = 0.99;
	F.Roster->Ensure(TEXT("B"));   // seeded at Start 0.99
	F.Bus.Publish(FFlightOffBlocksEvent{ 1, TEXT("B"), -30.0 });
	F.Bus.Drain();
	const FAirlineCauseTally* OnTime = HistTestTally(F.History->Find(TEXT("B"))->Days.Last(), ECause::OnTime);
	if (!TestNotNull(TEXT("an on-time tally"), OnTime)) { return false; }
	TestEqual(TEXT("0.99 + 0.03 clamps: the recorded delta is 0.01"), OnTime->SumDelta, 0.01, 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirlineHistorySaveTest, "AirportOps.Model.AirlineHistory.SaveLoadRoundTrip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAirlineHistorySaveTest::RunTest(const FString&)
{
	FHistoryFixture F;
	F.OffBlocks(1500.0);
	F.EndDay();
	F.OffBlocks(-30.0);   // mid-day: a partial tally in the open day
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	FOpsSnapshot Snapshot;
	TArray<IOpsPersistent*> Saved = { F.History };
	OpsSave::Capture(Saved, *Net, Snapshot);

	UAirlineHistory* Loaded = NewObject<UAirlineHistory>(GetTransientPackage());
	Loaded->Record(TEXT("Stale"), ECause::OnTime, 0.01, 0.51);   // a load replaces, it does not merge
	TArray<IOpsPersistent*> Into = { Loaded };
	if (!TestTrue(TEXT("restore succeeds"), OpsSave::Restore(Snapshot, Into, *Net))) { return false; }
	TestNull(TEXT("nothing the loading history held before"), Loaded->Find(TEXT("Stale")));
	const FAirlineDays* Was = F.History->Find(TEXT("A"));
	const FAirlineDays* Now = Loaded->Find(TEXT("A"));
	if (!TestNotNull(TEXT("the row comes back"), Now)) { return false; }
	TestEqual(TEXT("the same days"), Now->Days.Num(), Was->Days.Num());
	TestEqual(TEXT("today's day number"), Loaded->GetCurrentDay(), F.History->GetCurrentDay());
	for (int32 Index = 0; Index < Was->Days.Num() && Index < Now->Days.Num(); ++Index)
	{
		TestEqual(TEXT("day number"), Now->Days[Index].Day, Was->Days[Index].Day);
		TestEqual(TEXT("close"), Now->Days[Index].CloseSatisfaction, Was->Days[Index].CloseSatisfaction, 1e-9);
		TestEqual(TEXT("tally kinds"), Now->Days[Index].Tallies.Num(), Was->Days[Index].Tallies.Num());
	}
	const FAirlineCauseTally* Partial = HistTestTally(Now->Days.Last(), ECause::OnTime);
	TestTrue(TEXT("today's partial tally survived"), Partial != nullptr && Partial->Count == 1);

	// A SAVE WITH NO "AirlineHistory" BLOB loads empty.
	FOpsSnapshot Bare;
	TArray<IOpsPersistent*> None;
	OpsSave::Capture(None, *Net, Bare);
	UAirlineHistory* Empty = NewObject<UAirlineHistory>(GetTransientPackage());
	Empty->Record(TEXT("Stale"), ECause::OnTime, 0.01, 0.51);
	TArray<IOpsPersistent*> IntoEmpty = { Empty };
	OpsSave::Restore(Bare, IntoEmpty, *Net);
	TestNull(TEXT("no blob, no rows"), Empty->Find(TEXT("Stale")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirlineHistoryNewGameTest, "AirportOps.Model.AirlineHistory.NewGameResets",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAirlineHistoryNewGameTest::RunTest(const FString&)
{
	UAirlineHistory* H = NewObject<UAirlineHistory>(GetTransientPackage());
	H->Record(TEXT("A"), ECause::OnTime, 0.03, 0.53);
	H->ResetForNewGame(5);
	TestNull(TEXT("every row gone"), H->Find(TEXT("A")));
	TestEqual(TEXT("today is the day it was told"), H->GetCurrentDay(), 5);
	H->Record(TEXT("A"), ECause::OnTime, 0.03, 0.53);
	TestEqual(TEXT("and a new row opens under it"), H->Find(TEXT("A"))->Days[0].Day, 5);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirlineHistoryUnknownTest, "AirportOps.Model.AirlineHistory.UnknownAirlineGrowsARowOnlyFromTheRoster",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAirlineHistoryUnknownTest::RunTest(const FString&)
{
	// Record ADDS a row for an id it has not seen: its one caller, the roster, only calls for seeded rows. The roster is what
	// keeps a debug flight (NAME_None) or a removed airline out - and that half is what this test measures.
	FHistoryFixture F;
	F.Bus.Publish(FFlightOffBlocksEvent{ 1, TEXT("Nobody"), -30.0 });
	F.Bus.Publish(FFlightOffBlocksEvent{ 2, NAME_None, -30.0 });
	F.Bus.Drain();
	TestNull(TEXT("an airline the roster was never seeded with grows no history row"), F.History->Find(TEXT("Nobody")));
	TestNull(TEXT("nor does the debug flight"), F.History->Find(NAME_None));
	UAirlineHistory* Bare = NewObject<UAirlineHistory>(GetTransientPackage());
	Bare->Record(TEXT("Fresh"), ECause::OnTime, 0.01, 0.51);
	TestNotNull(TEXT("called directly, the history adds the row"), Bare->Find(TEXT("Fresh")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
