#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/Flight.h"
#include "Model/Ledger.h"

#if WITH_DEV_AUTOMATION_TESTS

// THE MODEL'S PREDICATES, ASKED OF THE MODEL (#447): the UI re-derived "overdrawn" (`Balance() < 0`, four copies) and "late" (`AirborneBy() - Now`,
// three) itself. Each is one function on the object that owns the fact, and the views present its answer.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLedgerOverdrawnTest, "AirportOps.Model.Ledger.IsOverdrawnIsBelowZero",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FLedgerOverdrawnTest::RunTest(const FString&)
{
	ULedger* Ledger = NewObject<ULedger>(GetTransientPackage());
	Ledger->Open(100.0);
	TestFalse(TEXT("positive money is not overdrawn"), Ledger->IsOverdrawn());
	Ledger->Post(0.0, ELedgerCategory::Placement, -100.0, FText::FromString(TEXT("to the penny")));
	TestEqual(TEXT("setup: exactly zero"), Ledger->Balance(), 0.0);
	TestFalse(TEXT("ZERO IS NOT OVERDRAWN - the boundary the bar's red, the ledger's flag and the alert must agree on"), Ledger->IsOverdrawn());
	Ledger->Post(1.0, ELedgerCategory::Upkeep, -1.0, FText::FromString(TEXT("one under")));
	TestTrue(TEXT("a penny under is"), Ledger->IsOverdrawn());
	Ledger->Post(2.0, ELedgerCategory::LandingFee, 50.0, FText::FromString(TEXT("a fee")));
	TestFalse(TEXT("and money in clears it"), Ledger->IsOverdrawn());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlightContractLeftTest, "AirportOps.Model.Flight.ContractLeftAndLate",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFlightContractLeftTest::RunTest(const FString&)
{
	UFlight* Flight = NewObject<UFlight>(GetTransientPackage());
	Flight->AcceptedAt = 1000.0;
	Flight->ContractSeconds = 3600.0;
	TestEqual(TEXT("at the accept, the whole contract is left"), Flight->ContractSecondsLeft(1000.0), 3600.0);
	TestEqual(TEXT("halfway, half"), Flight->ContractSecondsLeft(2800.0), 1800.0);
	TestFalse(TEXT("not late while time is left"), Flight->IsLate(2800.0));
	TestEqual(TEXT("at the deadline none is left"), Flight->ContractSecondsLeft(4600.0), 0.0);
	TestFalse(TEXT("ON THE DEADLINE IS NOT LATE - the boundary every view must draw alike"), Flight->IsLate(4600.0));
	TestTrue(TEXT("a second past it is late"), Flight->IsLate(4601.0));
	TestEqual(TEXT("and the left is negative by how late"), Flight->ContractSecondsLeft(4700.0), -100.0);
	TestEqual(TEXT("the same figure AirborneBy gives - one subtraction"), Flight->ContractSecondsLeft(123.0), Flight->AirborneBy() - 123.0);
	return true;
}

#endif
