#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/BuildPurse.h"
#include "Model/Ledger.h"
#include "Model/Pricing.h"
#include "Model/SimClock.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	ULedger* PurseWith(double Opening, UPricing*& OutPricing)
	{
		OutPricing = NewObject<UPricing>();
		ULedger* Ledger = NewObject<ULedger>();
		Ledger->Pricing = OutPricing;
		Ledger->Open(Opening);
		return Ledger;
	}

	FBuildQuote QuoteOf(double Amount)
	{
		FBuildQuote Quote;
		Quote.BaseAmount = Amount;
		Quote.What = FText::FromString(TEXT("Taxiway"));
		return Quote;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLedgerChargesAndReversesTest,
	"AirportOps.Model.LedgerChargesAndReverses",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FLedgerChargesAndReversesTest::RunTest(const FString& Parameters)
{
	UPricing* Pricing = nullptr;
	ULedger* Ledger = PurseWith(100000.0, Pricing);

	const int32 Id = Ledger->Charge(QuoteOf(30000.0));
	TestTrue(TEXT("a charge hands back an id"), Id != INDEX_NONE);
	TestEqual(TEXT("building takes the money"), Ledger->Balance(), 70000.0, 1e-6);
	TestEqual(TEXT("booked as placement"),
		Ledger->Entries()[0].Category, ELedgerCategory::Placement);

	Ledger->Reverse(Id);
	TestEqual(TEXT("undo puts back exactly what the build took"),
		Ledger->Balance(), 100000.0, 1e-6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLedgerCreditsScrapTest,
	"AirportOps.Model.LedgerCreditsScrap",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FLedgerCreditsScrapTest::RunTest(const FString& Parameters)
{
	UPricing* Pricing = nullptr;
	ULedger* Ledger = PurseWith(0.0, Pricing);
	Pricing->RefundFraction = 0.5;

	Ledger->Credit(QuoteOf(30000.0));

	TestEqual(TEXT("demolition gives back the refund fraction of today's price, not all of it - "
		"tearing out a taxiway is a decision with a cost, not a mistake being corrected"),
		Ledger->Balance(), 15000.0, 1e-6);
	TestEqual(TEXT("booked as a refund"), Ledger->Entries()[0].Category, ELedgerCategory::Refund);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLedgerNegativeBalanceLocksPlacementTest,
	"AirportOps.Model.LedgerNegativeBalanceLocksPlacement",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FLedgerNegativeBalanceLocksPlacementTest::RunTest(const FString& Parameters)
{
	UPricing* Pricing = nullptr;
	ULedger* Ledger = PurseWith(1000.0, Pricing);

	TestTrue(TEXT("what the balance covers is affordable"), Ledger->CanAfford(QuoteOf(1000.0)));
	TestFalse(TEXT("a penny more is not"), Ledger->CanAfford(QuoteOf(1000.01)));

	// THE GDD'S "A NEGATIVE BALANCE LOCKS PLACEMENT" IS THIS, and not a second rule written
	// somewhere else that could disagree with it. Upkeep is the only thing that can put the
	// player under, because a build is never authorised past the balance in the first place.
	Ledger->Post(0.0, ELedgerCategory::Upkeep, -2000.0, FText::FromString(TEXT("Upkeep")));
	TestTrue(TEXT("upkeep can take the balance under water"), Ledger->Balance() < 0.0);
	TestFalse(TEXT("and while it is under water nothing at all can be built"),
		Ledger->CanAfford(QuoteOf(1.0)));

	// A FREE QUOTE STAYS AFFORDABLE EVEN THEN. Splitting a segment or naming a runway costs
	// nothing, and a player who cannot pay their upkeep should still be able to rename a
	// runway - locking placement is not locking the editor.
	TestTrue(TEXT("a free edit is still allowed while negative"), Ledger->CanAfford(QuoteOf(0.0)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLedgerDatesItsEntriesTest,
	"AirportOps.Model.LedgerDatesItsEntries",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FLedgerDatesItsEntriesTest::RunTest(const FString& Parameters)
{
	UPricing* Pricing = nullptr;
	ULedger* Ledger = PurseWith(100000.0, Pricing);

	USimClock* Clock = NewObject<USimClock>();
	Clock->StartAtHour(9.0);
	Ledger->Clock = Clock;

	Ledger->Charge(QuoteOf(1000.0));

	// IBuildPurse HANDS NO TIME DOWN - Airside has no notion of game time - so the ledger has
	// to date its own entries. Without the clock every build would pile up at time zero and
	// the roll-up would fold the lot on the first day.
	TestEqual(TEXT("a build is dated by the game clock, not left at zero"),
		Ledger->Entries()[0].At, Clock->Now(), 1e-9);
	TestTrue(TEXT("and that is a real time of day, not the epoch"), Clock->Now() > 0.0);
	return true;
}

#endif
