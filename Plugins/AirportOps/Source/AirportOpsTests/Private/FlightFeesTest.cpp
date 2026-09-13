#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/Flight.h"
#include "Model/FlightBoard.h"
#include "Model/FuelService.h"
#include "Model/Ledger.h"
#include "Model/Pricing.h"
#include "Model/RoadEntity.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** A board with the money wired in, which UOpsRuntime::Attach is the production equivalent of. */
	UFlightBoard* PaidBoard(ULedger*& OutLedger, UPricing*& OutPricing, double Opening = 0.0)
	{
		OutLedger = NewObject<ULedger>();
		OutPricing = NewObject<UPricing>();
		OutLedger->Open(Opening);

		UFlightBoard* Board = NewObject<UFlightBoard>();
		Board->Ledger = OutLedger;
		Board->Pricing = OutPricing;
		return Board;
	}

	UFlight* CodeCFlight()
	{
		UFlight* Flight = NewObject<UFlight>();
		Flight->Id = 1;
		Flight->Airframe.Wingspan = 2800.0;   // 28 m, code C
		Flight->AirlineName = FText::FromString(TEXT("Test Air"));
		return Flight;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlightPaysToLandTest,
	"AirportOps.Model.FlightPaysToLand",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFlightPaysToLandTest::RunTest(const FString& Parameters)
{
	ULedger* Ledger = nullptr;
	UPricing* Pricing = nullptr;
	UFlightBoard* Board = PaidBoard(Ledger, Pricing);

	UFlight* Flight = CodeCFlight();
	Flight->LandingFee = 1200.0;

	Board->PostLandingFee(0.0, *Flight);

	TestEqual(TEXT("landing credits the fee the OFFER quoted, not one recomputed at touchdown - "
		"the number the player accepted must not have been a lie"),
		Ledger->Balance(), 1200.0, 1e-6);
	TestEqual(TEXT("and it is booked as a landing fee"),
		Ledger->Entries()[0].Category, ELedgerCategory::LandingFee);

	Board->PostLandingFee(0.0, *Flight);
	TestEqual(TEXT("a second call banks nothing: a flight lands once, and OnAgentPhase fires "
		"more than once per flight"), Ledger->Balance(), 1200.0, 1e-6);
	TestEqual(TEXT("and writes no second entry either"), Ledger->Entries().Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlightPaysToParkTest,
	"AirportOps.Model.FlightPaysToPark",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFlightPaysToParkTest::RunTest(const FString& Parameters)
{
	ULedger* Ledger = nullptr;
	UPricing* Pricing = nullptr;
	UFlightBoard* Board = PaidBoard(Ledger, Pricing);

	UFlight* Flight = CodeCFlight();
	Flight->ParkedAt = 1000.0;

	// Two game hours on the stand, at a code C rate of 120 an hour.
	Board->PostParkingFee(1000.0 + 7200.0, *Flight);

	TestEqual(TEXT("parking is charged for the hours actually occupied"),
		Ledger->Balance(), 240.0, 1e-6);
	TestEqual(TEXT("and recorded on the flight, so a row can show what it earned"),
		Flight->ParkingFee, 240.0, 1e-6);
	TestEqual(TEXT("booked as parking, not folded into the landing"),
		Ledger->Entries()[0].Category, ELedgerCategory::ParkingFee);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlightNeverParkedPaysNothingTest,
	"AirportOps.Model.FlightNeverParkedPaysNothing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFlightNeverParkedPaysNothingTest::RunTest(const FString& Parameters)
{
	ULedger* Ledger = nullptr;
	UPricing* Pricing = nullptr;
	UFlightBoard* Board = PaidBoard(Ledger, Pricing);

	UFlight* Flight = CodeCFlight();

	// NEVER PARKED - ParkedAt is still zero. A flight restored mid-air, or one put on the field
	// by the debug land key, reaches TaxiOut without having parked; billing it from the epoch
	// would hand the player a fee larger than the airport.
	Board->PostParkingFee(50000.0, *Flight);

	TestEqual(TEXT("a flight that never parked pays no parking"), Ledger->Balance(), 0.0, 1e-9);
	TestEqual(TEXT("and leaves no entry to explain a charge that did not happen"),
		Ledger->Entries().Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeesWithoutALedgerAreHarmlessTest,
	"AirportOps.Model.FeesWithoutALedgerAreHarmless",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFeesWithoutALedgerAreHarmlessTest::RunTest(const FString& Parameters)
{
	// NULL IS A WORKING STATE. Dozens of existing board tests drive a flight through its whole
	// lifecycle with no ledger at all, and a fee posting that dereferenced one would turn every
	// one of them into a crash rather than a failure.
	UFlightBoard* Board = NewObject<UFlightBoard>();
	UFlight* Flight = CodeCFlight();
	Flight->LandingFee = 1200.0;
	Flight->ParkedAt = 1000.0;

	Board->PostLandingFee(0.0, *Flight);
	Board->PostParkingFee(8200.0, *Flight);

	TestFalse(TEXT("with no ledger the landing fee is not marked paid, so wiring one up later "
		"does not find the flight already settled"), Flight->bLandingFeePaid);
	TestEqual(TEXT("and no parking fee is recorded on the flight either"),
		Flight->ParkingFee, 0.0, 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFuelServiceEarnsItsFeeTest,
	"AirportOps.Model.FuelServiceEarnsItsFee",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFuelServiceEarnsItsFeeTest::RunTest(const FString& Parameters)
{
	ULedger* Ledger = NewObject<ULedger>();
	UPricing* Pricing = NewObject<UPricing>();
	UFuelService* Fuel = NewObject<UFuelService>();
	Ledger->Open(1000.0);
	Fuel->Ledger = Ledger;
	Fuel->Pricing = Pricing;

	FAirframe Airframe;
	Airframe.Wingspan = 2800.0;   // code C

	Fuel->PostServiceFee(0.0, Airframe);
	TestEqual(TEXT("a completed fuelling earns the service fee"), Ledger->Balance(), 1600.0, 1e-6);
	TestEqual(TEXT("booked as a service fee, so a finance screen can tell it from a landing"),
		Ledger->Entries()[0].Category, ELedgerCategory::ServiceFee);

	// THE FORFEIT IS AN ENTRY THAT DOES NOT HAPPEN, never a negative one (spec D7). An
	// Unserviceable demand never reaches PostServiceFee at all, so the shape of the rule is
	// that nothing is called - and this is what that looks like from the ledger's side.
	const double AfterOneFuelling = Ledger->Balance();
	const int32 Rows = Ledger->Entries().Num();
	TestEqual(TEXT("an aircraft nothing could serve leaves the balance exactly as it was"),
		Ledger->Balance(), AfterOneFuelling, 1e-9);
	TestEqual(TEXT("and writes no row: a forfeit is not a fine, and there is nothing in this "
		"build a flight can be late against"), Ledger->Entries().Num(), Rows);
	return true;
}

#endif
