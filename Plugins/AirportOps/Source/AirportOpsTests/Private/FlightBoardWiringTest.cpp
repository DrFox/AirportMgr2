#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/FlightBoard.h"
#include "Model/JobBoard.h"
#include "Model/Ledger.h"
#include "Model/OfferGenerator.h"
#include "Model/Pricing.h"
#include "Model/SimClock.h"
#include "Model/StandAllocator.h"
#include "Present/OpsRuntime.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlightBoardIsComposedByTheRuntimeTest,
	"AirportOps.Present.FlightBoardIsComposedByTheRuntime",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFlightBoardIsComposedByTheRuntimeTest::RunTest(const FString& Parameters)
{
	// THE SEAM, not the logic. Every one of these is a line in the constructor that somebody
	// could forget while the game still ran: an unallocated board accepts nothing, and a
	// board with no generator has an inbox that never fills.
	UOpsRuntime* Runtime = NewObject<UOpsRuntime>();

	UFlightBoard* Board = Runtime->GetFlightBoard();
	TestNotNull(TEXT("the runtime composes a flight board"), Board);
	TestNotNull(TEXT("and an offer generator"), Runtime->GetOfferGenerator());
	if (Board == nullptr)
	{
		return false;
	}

	TestNotNull(TEXT("the board has an allocator, or it can never hold a stand"),
		Board->Allocator.Get());
	TestEqual(TEXT("the board's generator is the runtime's, not a second one"),
		Board->Generator.Get(), Runtime->GetOfferGenerator());

	// THE MONEY, WIRED HERE TOO (#425), not first at Attach: each of these is Transient now - a save must not carry a
	// path to another session's subobject - so nothing but the constructor ever sets them, and a runtime that has
	// never attached must already have them.
	// NO LEDGER ON THE BOARD since #506's review: billing is handed the runtime's own in WireBus - AirportOps.Present.Bus.BillingIsWired
	// charges a landing through it.
	//
	// THE RUNTIME'S OWN TWO SUBOBJECTS, ASKED FIRST (moved here from AirportOps.Runtime.LedgerIsComposedByTheRuntime, #462 M8, which held
	// the same unattached runtime): the runtime grew a ledger and a pricing resolver, and a constructor that silently failed to make one
	// would leave every fee posting to nothing at all, with no error anywhere - the money would simply never move. They are asked BEFORE
	// the equalities below, which pass with both sides null (a null Pricing on the board equals a null GetPricing()).
	if (!TestNotNull(TEXT("the runtime composes a ledger, as it does every other subobject"), Runtime->GetLedger())) { return false; }
	if (!TestNotNull(TEXT("and a pricing resolver"), Runtime->GetPricing())) { return false; }
	TestTrue(TEXT("the board prices at the runtime's prices"), Board->Pricing.Get() == Runtime->GetPricing());
	TestTrue(TEXT("and asks the runtime's job board about fuel"), Board->Fuel.Get() == Runtime->GetJobBoard());
	TestTrue(TEXT("the generator prices offers at the runtime's prices"),
		Runtime->GetOfferGenerator() != nullptr && Runtime->GetOfferGenerator()->Pricing.Get() == Runtime->GetPricing());
	TestTrue(TEXT("the ledger dates entries by the runtime's clock at its prices"),
		Runtime->GetLedger()->Clock.Get() == Runtime->GetClock() && Runtime->GetLedger()->Pricing.Get() == Runtime->GetPricing());
	TestTrue(TEXT("the job board banks fuel into the runtime's ledger at its prices"),
		Runtime->GetJobBoard()->Ledger.Get() == Runtime->GetLedger() && Runtime->GetJobBoard()->Pricing.Get() == Runtime->GetPricing());

	// Unattached, there is no actor to dispatch into, and the board must not pretend there
	// is: a flight coming due here logs that the board is not attached rather than vanishing.
	TestFalse(TEXT("an unattached runtime leaves the dispatcher unset"),
		static_cast<bool>(Board->Dispatcher));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlightBoardIdsAreUniqueTest,
	"AirportOps.Present.FlightBoardIdsAreUnique",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFlightBoardIdsAreUniqueTest::RunTest(const FString& Parameters)
{
	// Ids are what stand holds are keyed on, negated. Two flights sharing one would share a
	// hold, and releasing either would free the other's stand.
	UOpsRuntime* Runtime = NewObject<UOpsRuntime>();
	UFlightBoard* Board = Runtime->GetFlightBoard();
	if (!TestNotNull(TEXT("a board"), Board)) { return false; }

	const int32 First = Board->TakeNextId();
	const int32 Second = Board->TakeNextId();
	TestTrue(TEXT("ids start at 1, because HolderId negates them"), First >= 1);
	TestNotEqual(TEXT("and no two flights share one"), First, Second);
	return true;
}

#endif
