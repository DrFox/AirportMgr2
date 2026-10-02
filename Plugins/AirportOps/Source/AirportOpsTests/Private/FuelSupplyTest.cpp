#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/FuelSupply.h"
#include "Model/ServiceRolePolicy.h"

#if WITH_DEV_AUTOMATION_TESTS

// FUEL SUPPLY (spec 2026-10-02-progression-and-fuel-supply §7): world-free, on a NewObject supply whose capacity is a
// number the test sets - the tanks' arithmetic is UJobBoard's and is tested there (Task 3).

namespace
{
	UFuelSupply* SupplyHolding(double Stock, double Capacity)
	{
		UFuelSupply* Supply = NewObject<UFuelSupply>(GetTransientPackage());
		Supply->CapacityOf = [Capacity]() { return Capacity; };
		Supply->StockLitres = Stock;
		return Supply;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFuelSupplyDrawTest, "AirportOps.Model.FuelSupply.DrawGrantsWhatIsHeld",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFuelSupplyDrawTest::RunTest(const FString&)
{
	UFuelSupply* Supply = SupplyHolding(1000.0, 30000.0);
	TestEqual(TEXT("a draw within the stock is granted whole"), Supply->Draw(600.0), 600.0, 1e-9);
	TestEqual(TEXT("and leaves the rest"), Supply->StockLitres, 400.0, 1e-9);
	TestEqual(TEXT("a draw beyond it is granted only what is there"), Supply->Draw(600.0), 400.0, 1e-9);
	TestEqual(TEXT("leaving nothing - never a negative stock"), Supply->StockLitres, 0.0, 1e-9);
	TestTrue(TEXT("and the supply is dry"), Supply->IsDry());
	TestEqual(TEXT("a negative draw grants nothing and adds nothing"), Supply->Draw(-50.0), 0.0, 1e-9);
	TestEqual(TEXT("CONTROL: the stock did not move"), Supply->StockLitres, 0.0, 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFuelSupplyReceiveTest, "AirportOps.Model.FuelSupply.ReceiveStopsAtCapacity",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFuelSupplyReceiveTest::RunTest(const FString&)
{
	UFuelSupply* Supply = SupplyHolding(25000.0, 30000.0);
	TestEqual(TEXT("TAKE-OR-PAY: only what fits is added"), Supply->Receive(10000.0), 5000.0, 1e-9);
	TestEqual(TEXT("and the tanks are full"), Supply->StockLitres, 30000.0, 1e-9);
	TestEqual(TEXT("a full supply has no free space"), Supply->FreeSpace(), 0.0, 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFuelSupplyOverCapacityTest, "AirportOps.Model.FuelSupply.StockAboveCapacityIsKeptNotTopped",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFuelSupplyOverCapacityTest::RunTest(const FString&)
{
	// A TANK SOLD UNDER THE STOCK (review focus 4): the fuel is not destroyed and not refunded - it is held, and nothing
	// more is taken until it is drawn below the new capacity.
	UFuelSupply* Supply = SupplyHolding(50000.0, 30000.0);
	TestEqual(TEXT("free space is never negative"), Supply->FreeSpace(), 0.0, 1e-9);
	TestEqual(TEXT("a delivery adds nothing"), Supply->Receive(5000.0), 0.0, 1e-9);
	TestEqual(TEXT("and the held stock is untouched"), Supply->StockLitres, 50000.0, 1e-9);
	TestEqual(TEXT("it can still all be drawn"), Supply->Draw(50000.0), 50000.0, 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFuelSupplyUnwiredTest, "AirportOps.Model.FuelSupply.UnwiredCapacityIsUnbounded",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFuelSupplyUnwiredTest::RunTest(const FString&)
{
	// A BARE NewObject (every pre-existing fuel test's board) must behave as fuel did before stock existed only when
	// the board has NO supply at all - see UJobBoard::FuelSupply. A supply with no capacity reader holds anything.
	UFuelSupply* Supply = NewObject<UFuelSupply>(GetTransientPackage());
	TestTrue(TEXT("no capacity reader: capacity unbounded"), Supply->Capacity() > 1e12);
	TestEqual(TEXT("so a delivery is taken whole"), Supply->Receive(1e6), 1e6, 1e-6);
	return true;
}

#endif
