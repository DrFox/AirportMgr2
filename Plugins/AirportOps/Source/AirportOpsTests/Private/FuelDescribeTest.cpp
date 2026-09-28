#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/Airframe.h"
#include "Model/JobBoard.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The aircraft card's fuel line (2026-09-28): the load, what is left - counting down WHILE the
 * pump runs, not only at the end of a trip - and where the job has got to.
 */
namespace
{
	FServiceJob& Demand(UJobBoard& Service, EServiceJobState State, double Owed, double Delivered, int32 Trips)
	{
		FServiceJob& D = Service.AddJobForTest(1, State, EServiceRefusal::None, 0);
		D.QuantityOwed = Owed;
		D.QuantityDelivered = Delivered;
		D.Trips = Trips;
		D.TankLitres = 1000.0;
		return D;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFuelDescribePumpingTest, "AirportOps.Fuel.Describe.CountsDownWhilePumping",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFuelDescribePumpingTest::RunTest(const FString& Parameters)
{
	UJobBoard* Service = NewObject<UJobBoard>();
	FServiceJob& D = Demand(*Service, EServiceJobState::Serving, 2900.0, 0.0, 0);
	D.TripQuantity = 1000.0;
	D.TripStartedAt = 0.0;
	D.TripEndsAt = 800.0;
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
		UJobBoard* Service = NewObject<UJobBoard>();
		Demand(*Service, EServiceJobState::Underway, 1900.0, 1000.0, 1);
		TestEqual(TEXT("second trip on its way"), Service->DescribeAgent(1, 0.0),
			FString(TEXT("Fuel 2,900 L · 1,900 L left · truck en route (trip 2 of 3)")));
	}
	{
		UJobBoard* Service = NewObject<UJobBoard>();
		Demand(*Service, EServiceJobState::Open, 1900.0, 1000.0, 1);
		TestEqual(TEXT("between trips it waits for a truck"), Service->DescribeAgent(1, 0.0),
			FString(TEXT("Fuel 2,900 L · 1,900 L left · waiting for a truck (trip 2 of 3)")));
	}
	{
		UJobBoard* Service = NewObject<UJobBoard>();
		Demand(*Service, EServiceJobState::Open, 300.0, 0.0, 0);
		TestEqual(TEXT("a one-trip job names no trips"), Service->DescribeAgent(1, 0.0),
			FString(TEXT("Fuel 300 L · 300 L left · waiting for a truck")));
	}
	{
		UJobBoard* Service = NewObject<UJobBoard>();
		Demand(*Service, EServiceJobState::Done, 0.0, 2900.0, 3);
		TestEqual(TEXT("done, in how many trips"), Service->DescribeAgent(1, 0.0),
			FString(TEXT("Fuel 2,900 L · done in 3 trips")));
	}
	{
		UJobBoard* Service = NewObject<UJobBoard>();
		Demand(*Service, EServiceJobState::Done, 0.0, 0.0, 0);
		TestEqual(TEXT("a type with no tank"), Service->DescribeAgent(1, 0.0), FString(TEXT("Fuel · none needed")));
	}
	{
		UJobBoard* Service = NewObject<UJobBoard>();
		FServiceJob& D = Demand(*Service, EServiceJobState::Unserviceable, 2900.0, 0.0, 0);
		D.Why = EServiceRefusal::NoDepot;
		TestEqual(TEXT("the refusal, as before"), Service->DescribeAgent(1, 0.0),
			FString(TEXT("Fuel 2,900 L · no fuel depot")));
	}
	{
		UJobBoard* Service = NewObject<UJobBoard>();
		TestEqual(TEXT("no demand, no line"), Service->DescribeAgent(1, 0.0), FString());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFuelDescribeVehicleTest, "AirportOps.Fuel.Describe.Vehicle",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFuelDescribeVehicleTest::RunTest(const FString& Parameters)
{
	// THE TRUCK'S OWN CARD (stage 3): selecting a vehicle's agent shows what the VEHICLE is doing -
	// the state it owns, what it carries, how much work is queued behind the current job. Through the
	// same DescribeAgent the aircraft card uses, so the inspector needs no second seam.
	UJobBoard* Service = NewObject<UJobBoard>();
	FEntityInstanceId Depot;
	Depot.Index = 4;
	FServiceJob& Job = Service->AddJobForTest(1, EServiceJobState::Underway, EServiceRefusal::None, 0);
	Job.Stand.Index = 2;
	const int32 JobId = Job.Id;
	FServiceVehicle& Vehicle = Service->AddVehicleForTest(TEXT("FUEL"), Depot, EServiceVehicleState::ToJob, 9700.0);
	Vehicle.AgentId = 7;
	Vehicle.CurrentJob = JobId;
	Vehicle.Queue = { 99 };
	TestEqual(TEXT("driving to a job"), Service->DescribeAgent(7, 0.0),
		FString(TEXT("FUEL · to stand 2 · 9,700 L · 1 queued")));

	Vehicle.State = EServiceVehicleState::Serving;
	Vehicle.Queue.Reset();
	TestEqual(TEXT("serving, nothing behind it"), Service->DescribeAgent(7, 0.0),
		FString(TEXT("FUEL · fuelling at stand 2 · 9,700 L")));

	Vehicle.State = EServiceVehicleState::ToFacility;
	Vehicle.CurrentJob = 0;
	TestEqual(TEXT("going home"), Service->DescribeAgent(7, 0.0),
		FString(TEXT("FUEL · to depot 4 · 9,700 L")));
	return true;
}

#endif
