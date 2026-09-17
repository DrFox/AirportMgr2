#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/AirlineDefinition.h"
#include "Model/Ledger.h"
#include "Model/OfferGenerator.h"
#include "Model/Pricing.h"
#include "Model/SimClock.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/**
	 * One run of the money: a clock, a ledger, and a scripted sequence of things that cost or
	 * earn. Returns the ledger so two runs can be compared entry for entry.
	 *
	 * SCRIPTED RATHER THAN SIMULATED. The point is not that the airport behaves the same - the
	 * traffic model has its own tests for that - but that nothing in the MONEY reads wall time.
	 * Two runs of this happen at different real instants, so anything keyed on FPlatformTime
	 * rather than USimClock shows up as a difference.
	 */
	ULedger* RunTheMoney(int32 Seed)
	{
		USimClock* Clock = NewObject<USimClock>();
		UPricing* Pricing = NewObject<UPricing>();
		ULedger* Ledger = NewObject<ULedger>();

		Clock->RealSecondsPerGameDay = 1200.0;
		Clock->StartAtHour(9.0);
		Ledger->Pricing = Pricing;
		Ledger->Clock = Clock;
		Ledger->Open(500000.0);

		FBuildQuote Taxiway;
		Taxiway.BaseAmount = 30000.0;
		Taxiway.What = FText::FromString(TEXT("Taxiway"));

		// A build, a day passing, an aeroplane paying, and a demolition - four of the six
		// categories, each dated by the clock rather than by when the test happened to run.
		const int32 Built = Ledger->Charge(Taxiway);
		Clock->Advance(60.0);
		Ledger->Post(Clock->Now(), ELedgerCategory::LandingFee, 1200.0,
			FText::FromString(TEXT("Landing")));
		Clock->Advance(60.0);
		Ledger->Post(Clock->Now(), ELedgerCategory::Upkeep, -150.0,
			FText::FromString(TEXT("Upkeep")));
		Ledger->Credit(Taxiway);
		Ledger->Reverse(Built);

		// The offer generator's own stream, seeded, is what makes the aeroplane CHOSEN
		// repeatable - see UOfferGenerator::Stream. Drawn here so a change to the global RNG
		// cannot creep back in unnoticed.
		UOfferGenerator* Generator = NewObject<UOfferGenerator>();
		Generator->Stream.Initialize(Seed);
		for (int32 Draw = 0; Draw < 8; ++Draw)
		{
			Ledger->Post(Clock->Now(), ELedgerCategory::ServiceFee,
				Generator->Stream.RandHelper(100), FText::FromString(TEXT("Service")));
		}

		return Ledger;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLedgerDeterminismTest,
	"AirportOps.Model.LedgerDeterminism",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FLedgerDeterminismTest::RunTest(const FString& Parameters)
{
	// THE TEST M1 DEFERRED, for want of a ledger to compare. Its plan named it and said it
	// would arrive with M3: "same seed, same inputs, same ledger". This is it.
	//
	// IT IS THE TEST THAT KEEPS WALL TIME OUT OF THE SIM. If it ever fails, something in the
	// money is reading the real clock rather than USimClock - and the symptom in a game would
	// be a save that reloads into a different financial position depending on when it was
	// loaded, which is close to impossible to diagnose from the outside.
	const ULedger* First = RunTheMoney(1234);
	const ULedger* Second = RunTheMoney(1234);

	if (!TestEqual(TEXT("the same inputs produce the same number of entries"),
		Second->Entries().Num(), First->Entries().Num()))
	{
		return false;
	}

	for (int32 Index = 0; Index < First->Entries().Num(); ++Index)
	{
		const FLedgerEntry& A = First->Entries()[Index];
		const FLedgerEntry& B = Second->Entries()[Index];

		TestEqual(*FString::Printf(TEXT("entry %d is for the same amount in both runs"), Index),
			B.Amount, A.Amount, 1e-9);
		TestEqual(*FString::Printf(TEXT("entry %d is dated the same GAME time in both runs, "
			"which is what fails if anything reads wall time"), Index), B.At, A.At, 1e-9);
		TestEqual(*FString::Printf(TEXT("entry %d is in the same category in both runs"), Index),
			B.Category, A.Category);
	}

	TestEqual(TEXT("and the balances agree, which is the line the systems map actually asks for"),
		Second->Balance(), First->Balance(), 1e-9);

	// A DIFFERENT SEED MUST DIVERGE, or the test above would pass on a generator that had
	// stopped drawing at all - the way a determinism test quietly becomes a tautology.
	const ULedger* Other = RunTheMoney(9999);
	TestNotEqual(TEXT("a different seed gives a different run, so the comparison above is "
		"actually comparing something"), Other->Balance(), First->Balance());
	return true;
}

#endif
