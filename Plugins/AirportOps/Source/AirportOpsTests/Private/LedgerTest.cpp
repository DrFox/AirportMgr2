#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/Ledger.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLedgerFoldTest,
	"AirportOps.Model.LedgerFold",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FLedgerFoldTest::RunTest(const FString& Parameters)
{
	ULedger* Ledger = NewObject<ULedger>();
	Ledger->Open(500000.0);

	Ledger->Post(0.0, ELedgerCategory::LandingFee, 1200.0, FText::FromString(TEXT("G-ABCD")));
	Ledger->Post(10.0, ELedgerCategory::Placement, -150000.0, FText::FromString(TEXT("Taxiway")));

	TestEqual(TEXT("the balance is the opening balance plus every entry"),
		Ledger->Balance(), 351200.0, 1e-6);
	TestEqual(TEXT("the cached total agrees with a fresh fold, which is the invariant that "
		"makes caching it safe at all"), Ledger->Balance(), Ledger->FoldBalanceForTest(), 1e-6);
	TestEqual(TEXT("both entries were kept: the entries are the record, the total is a "
		"convenience"), Ledger->Entries().Num(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLedgerReverseTest,
	"AirportOps.Model.LedgerReverse",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FLedgerReverseTest::RunTest(const FString& Parameters)
{
	ULedger* Ledger = NewObject<ULedger>();
	Ledger->Open(1000.0);

	const int32 Id = Ledger->Post(0.0, ELedgerCategory::Placement, -400.0,
		FText::FromString(TEXT("Taxiway")));
	TestTrue(TEXT("a posted entry hands back an id to reverse it by"), Id != INDEX_NONE);

	TestTrue(TEXT("reversing a real id succeeds"), Ledger->Reverse(5.0, Id));
	TestEqual(TEXT("a reversal cancels its charge to the penny, which is what undo means"),
		Ledger->Balance(), 1000.0, 1e-9);
	TestEqual(TEXT("and it is a NEW entry - the ledger is append-only, never edited"),
		Ledger->Entries().Num(), 2);

	TestFalse(TEXT("the same charge cannot be reversed twice, or a double undo would pay the "
		"player twice for one build"), Ledger->Reverse(6.0, Id));
	TestFalse(TEXT("an unknown id reverses nothing"), Ledger->Reverse(7.0, 9999));
	TestFalse(TEXT("and INDEX_NONE, which is what an unpriced edit carries, reverses nothing"),
		Ledger->Reverse(8.0, INDEX_NONE));
	TestEqual(TEXT("none of the three refusals moved the balance"),
		Ledger->Balance(), 1000.0, 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLedgerRollUpTest,
	"AirportOps.Model.LedgerRollUp",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FLedgerRollUpTest::RunTest(const FString& Parameters)
{
	ULedger* Ledger = NewObject<ULedger>();
	Ledger->Open(0.0);
	Ledger->MaxDays = 30;

	const double Day = 86400.0;
	Ledger->Post(1.0 * Day, ELedgerCategory::Upkeep, -100.0, FText::FromString(TEXT("old")));
	Ledger->Post(2.0 * Day, ELedgerCategory::Upkeep, -100.0, FText::FromString(TEXT("old")));
	Ledger->Post(40.0 * Day, ELedgerCategory::LandingFee, 500.0, FText::FromString(TEXT("new")));

	const double Before = Ledger->Balance();
	Ledger->RollUp(40.0 * Day);

	TestEqual(TEXT("roll-up preserves the balance EXACTLY - it summarises history, it never "
		"rewrites it"), Ledger->Balance(), Before, 1e-9);
	TestEqual(TEXT("the two entries older than MaxDays became one brought-forward entry, "
		"leaving it and the recent one"), Ledger->Entries().Num(), 2);
	TestEqual(TEXT("and the summary carries their sum, so nothing was lost in the fold"),
		Ledger->Entries()[0].Amount, -200.0, 1e-9);
	TestEqual(TEXT("the summary is marked as one, so a finance screen can tell a fold from a "
		"real movement of money"),
		Ledger->Entries()[0].Category, ELedgerCategory::BroughtForward);

	// NOTHING OLD ENOUGH TO FOLD must leave the ledger alone. Without this, a roll-up on a
	// young game would insert an empty brought-forward row every day it ran.
	const int32 Rows = Ledger->Entries().Num();
	Ledger->RollUp(40.0 * Day);
	TestEqual(TEXT("a roll-up with nothing old enough to fold adds no entry"),
		Ledger->Entries().Num(), Rows);
	return true;
}

/**
 * THE ROLL-UP DECISION (issue #191): PostDailyUpkeep's skip-if-zero used to guard RollUp too,
 * because both lived behind one early return in UOpsRuntime - so a day with nothing standing
 * (Base <= 0) silently deferred folding old entries as well as skipping the charge. Folding
 * is housekeeping unrelated to whether today happened to cost anything, so this pins that a
 * zero-Base day still rolls up. Ledger->Post itself is checked separately in the assertions
 * on the charge below.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLedgerPostDailyUpkeepTest,
	"AirportOps.Model.LedgerPostDailyUpkeep",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FLedgerPostDailyUpkeepTest::RunTest(const FString& Parameters)
{
	const double Day = 86400.0;

	// Base > 0: an upkeep entry is posted, and it is a charge (negative).
	{
		ULedger* Ledger = NewObject<ULedger>();
		Ledger->Open(1000.0);
		Ledger->PostDailyUpkeep(50.0, 1.0 * Day);
		TestEqual(TEXT("a positive base posts one upkeep entry"), Ledger->Entries().Num(), 1);
		TestEqual(TEXT("charged, not credited"), Ledger->Entries()[0].Amount, -50.0, 1e-9);
		TestEqual(TEXT("filed as Upkeep"), Ledger->Entries()[0].Category, ELedgerCategory::Upkeep);
	}

	// Base <= 0: no entry - "an airport with nothing standing on it costs nothing to own" -
	// but RollUp still runs and still folds anything old enough to fold. THE BUG THIS PINS:
	// before issue #191, the runtime's skip-if-zero guard returned before EITHER RollUp ran.
	{
		ULedger* Ledger = NewObject<ULedger>();
		Ledger->Open(0.0);
		Ledger->MaxDays = 30;
		Ledger->Post(1.0 * Day, ELedgerCategory::Upkeep, -10.0, FText::FromString(TEXT("old")));
		Ledger->Post(2.0 * Day, ELedgerCategory::Upkeep, -10.0, FText::FromString(TEXT("old")));

		const double Before = Ledger->Balance();
		Ledger->PostDailyUpkeep(0.0, 40.0 * Day);

		TestEqual(TEXT("a zero base posts no charge"), Ledger->Entries().Num(), 1);
		TestEqual(TEXT("but the two old entries still folded into a brought-forward one - "
			"RollUp is not conditional on today's charge"),
			Ledger->Entries()[0].Category, ELedgerCategory::BroughtForward);
		TestEqual(TEXT("folding never moves the balance"), Ledger->Balance(), Before, 1e-9);
	}
	return true;
}

#endif
