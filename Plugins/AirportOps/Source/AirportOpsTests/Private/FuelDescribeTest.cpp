#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/Airframe.h"
#include "Model/FuelService.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The aircraft card's fuel line (2026-09-28): the load, what is left - counting down WHILE the
 * pump runs, not only at the end of a trip - and where the job has got to.
 */
namespace
{
	FFuelDemand& Demand(UFuelService& Service, EFuelDemandState State, double Owed, double Delivered, int32 Trips)
	{
		FFuelDemand& D = Service.AddDemandForTest(1, State, EFuelRefusal::None, 0);
		D.LitresOwed = Owed;
		D.LitresDelivered = Delivered;
		D.Trips = Trips;
		D.TankLitres = 1000.0;
		return D;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFuelDescribePumpingTest, "AirportOps.Fuel.Describe.CountsDownWhilePumping",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFuelDescribePumpingTest::RunTest(const FString& Parameters)
{
	UFuelService* Service = NewObject<UFuelService>();
	FFuelDemand& D = Demand(*Service, EFuelDemandState::Fuelling, 2900.0, 0.0, 0);
	D.LoadThisTrip = 1000.0;
	D.PumpStartedAt = 0.0;
	D.DwellEndsAt = 800.0;
	TestEqual(TEXT("halfway through the first trip, 500 L are in"),
		Service->DescribeAgent(1, 400.0), FString(TEXT("Fuel 2,900 L · 2,400 L left · fuelling (trip 1 of 3)")));
	TestEqual(TEXT("a pump that has not moved has not delivered"),
		Service->DescribeAgent(1, 0.0), FString(TEXT("Fuel 2,900 L · 2,900 L left · fuelling (trip 1 of 3)")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFuelDescribeStatesTest, "AirportOps.Fuel.Describe.EveryState",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFuelDescribeStatesTest::RunTest(const FString& Parameters)
{
	{
		UFuelService* Service = NewObject<UFuelService>();
		Demand(*Service, EFuelDemandState::TruckEnRoute, 1900.0, 1000.0, 1);
		TestEqual(TEXT("second trip on its way"), Service->DescribeAgent(1, 0.0),
			FString(TEXT("Fuel 2,900 L · 1,900 L left · truck en route (trip 2 of 3)")));
	}
	{
		UFuelService* Service = NewObject<UFuelService>();
		Demand(*Service, EFuelDemandState::Needed, 1900.0, 1000.0, 1);
		TestEqual(TEXT("between trips it waits for a truck"), Service->DescribeAgent(1, 0.0),
			FString(TEXT("Fuel 2,900 L · 1,900 L left · waiting for a truck (trip 2 of 3)")));
	}
	{
		UFuelService* Service = NewObject<UFuelService>();
		Demand(*Service, EFuelDemandState::Needed, 300.0, 0.0, 0);
		TestEqual(TEXT("a one-trip job names no trips"), Service->DescribeAgent(1, 0.0),
			FString(TEXT("Fuel 300 L · 300 L left · waiting for a truck")));
	}
	{
		UFuelService* Service = NewObject<UFuelService>();
		Demand(*Service, EFuelDemandState::Done, 0.0, 2900.0, 3);
		TestEqual(TEXT("done, in how many trips"), Service->DescribeAgent(1, 0.0),
			FString(TEXT("Fuel 2,900 L · done in 3 trips")));
	}
	{
		UFuelService* Service = NewObject<UFuelService>();
		Demand(*Service, EFuelDemandState::Done, 0.0, 0.0, 0);
		TestEqual(TEXT("a type with no tank"), Service->DescribeAgent(1, 0.0), FString(TEXT("Fuel · none needed")));
	}
	{
		UFuelService* Service = NewObject<UFuelService>();
		FFuelDemand& D = Demand(*Service, EFuelDemandState::Unserviceable, 2900.0, 0.0, 0);
		D.Why = EFuelRefusal::NoDepot;
		TestEqual(TEXT("the refusal, as before"), Service->DescribeAgent(1, 0.0),
			FString(TEXT("Fuel 2,900 L · no fuel depot")));
	}
	{
		UFuelService* Service = NewObject<UFuelService>();
		TestEqual(TEXT("no demand, no line"), Service->DescribeAgent(1, 0.0), FString());
	}
	return true;
}

#endif
