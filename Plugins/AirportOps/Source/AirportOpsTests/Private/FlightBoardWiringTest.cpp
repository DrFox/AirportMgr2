#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/FlightBoard.h"
#include "Model/OfferGenerator.h"
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
