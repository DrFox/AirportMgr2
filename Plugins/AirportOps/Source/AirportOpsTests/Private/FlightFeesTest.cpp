#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/AirlineDefinition.h"
#include "Model/Flight.h"
#include "Model/FlightBoard.h"
#include "Model/JobBoard.h"
#include "Model/GroundTraffic.h"
#include "Model/Ledger.h"
#include "Model/OfferGenerator.h"
#include "Model/Pricing.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Model/SimClock.h"
#include "Model/StandAllocator.h"
#include "OpsTransitionTestHelpers.h"
#include "Testing/AirsideTestGraph.h"

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
	// THE RATE THE OFFER FIXED (#442) - 120 an hour is a code C's at the lever's start; UOfferGenerator::MakeOffer writes it and
	// PostParkingFee reads it, so a hand-made flight carries it by hand, as FFlightPaysToLandTest sets LandingFee.
	Flight->ParkingRatePerHour = 120.0;

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

	// NEVER PARKED - ParkedAt is still zero. An aeroplane departed from the fallback junction it
	// waited on (#405), or one put on the field by the debug land key, reaches TaxiOut without
	// having parked at a stand; billing it from the epoch would hand the player a fee larger than
	// the airport. (A flight restored mid-air no longer can - a load re-queues it, #404.)
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
	UJobBoard* Fuel = NewObject<UJobBoard>();
	Ledger->Open(1000.0);
	Fuel->Ledger = Ledger;
	Fuel->Pricing = Pricing;

	// BY THE LITRE since 2026-09-28 (spec fuel-litres): 400 L at the default rate.
	Fuel->PostServiceFee(0.0, 400.0);
	TestEqual(TEXT("a completed fuelling earns the litres it sold"), Ledger->Balance(),
		1000.0 + 400.0 * Pricing->FuelPricePerLitre, 1e-6);
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FParkingAtTheOffersRateTest,
	"AirportOps.Model.FlightFees.ParkingIsBilledAtTheOffersRate",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FParkingAtTheOffersRateTest::RunTest(const FString& Parameters)
{
	// #442's PIN, THE WHOLE CHAIN: an offer made by the generator, accepted by the board, the fee lever stepped, the aeroplane
	// pushed off and taxiing out - parking is billed at the rate the OFFER was made at, not at the lever as it stands at departure.
	// It asked UPricing::ParkingFeePerHour then, which applies the CURRENT landing-fee multiplier: the trade the landing fee rules out
	// ("a fee computed on landing would let them accept cheaply and put the price up afterwards").
	FAirframe Airframe;
	Airframe.Wingspan = 3400.0;   // 34 m: Code C, 120 an hour at the lever's start
	Airframe.TurnaroundSeconds = 1800.0;
	const FTestAirport Field = FTestAirport::Build(Airframe);

	UPricing* Pricing = NewObject<UPricing>(GetTransientPackage());
	UOfferGenerator* Generator = NewObject<UOfferGenerator>(GetTransientPackage());
	Generator->Stream.Initialize(7);
	Generator->Pricing = Pricing;
	Generator->MaxPendingOffers = 1000;
	UAirlineDefinition* Airline = NewObject<UAirlineDefinition>(GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), UAirlineDefinition::StaticClass(), TEXT("FeesTestAirline")));
	Airline->DisplayName = FText::FromString(TEXT("Test Air"));
	Airline->PeakOffersPerHour = 600.0;
	FAirlineOffers Offering;
	Offering.Airline = Airline;
	FOfferCandidate Candidate;
	Candidate.Airframe = Airframe;
	Candidate.AirlineName = FText::FromString(TEXT("Test Air"));
	Candidate.TypeName = FText::FromString(TEXT("A320"));
	Offering.Fleet.Add(Candidate);

	USimClock* Clock = NewObject<USimClock>(GetTransientPackage());
	Clock->SetUniformDay(USimClock::SecondsPerDay);
	Clock->StartAtHour(9.0);
	TArray<UFlight*> Made;
	int32 NextId = 1;
	for (int32 Minute = 0; Minute < 30 && Made.IsEmpty(); ++Minute)
	{
		Made = Generator->TickMinute(*Field.Net, Field.Threshold, MakeArrayView(&Offering, 1), *Clock, 0,
			[&NextId]() { return NextId++; });
		Clock->Advance(UOfferGenerator::TickSeconds);
	}
	if (!TestTrue(TEXT("the generator made an offer"), !Made.IsEmpty())) { return false; }
	UFlight* Flight = Made[0];
	TestEqual(TEXT("the offer carries the parking rate of the lever it was made at - a tenth of a Code C's 1200 landing fee"),
		Flight->ParkingRatePerHour, 120.0, 1e-6);

	// ACCEPTED, through the board, with an aeroplane already its own (the fixture route FFlightBoardFollowsTheAgentTest uses).
	ULedger* Ledger = NewObject<ULedger>(GetTransientPackage());
	Ledger->Open(0.0);
	UFlightBoard* Board = NewObject<UFlightBoard>(GetTransientPackage());
	Board->Ledger = Ledger;
	// THE BOARD HAS THE LEVER WIRED, as UOpsRuntime's constructor wires it: what the old PostParkingFee asked at departure.
	Board->Pricing = Pricing;
	Board->Allocator = NewObject<UStandAllocator>(GetTransientPackage());
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	Flight->AgentId = 5;
	Board->AddOffer(*Clock, Flight);
	if (!TestTrue(TEXT("accepted"), Board->Accept(*Traffic, *Field.Net, *Clock, *Flight))) { return false; }

	// THE PLAYER PUTS THE PRICE UP before the aeroplane leaves - five steps, +50%.
	for (int32 Step = 0; Step < 5; ++Step) { Pricing->StepLandingFee(+1); }
	TestTrue(TEXT("CONTROL: the lever moved the LIVE rate - asking UPricing at departure would now bill more"),
		Pricing->ParkingFeePerHour(Airframe) > 150.0);

	// DEPARTS: two game hours on the stand, then the push ends and the taxi out begins - the TaxiOut row is where parking posts.
	Flight->SetPhaseForTest(EFlightPhase::Manoeuvring);
	Flight->ParkedAt = 1000.0;
	Clock->StartAtHour((1000.0 + 7200.0) / 3600.0);
	Board->OnAgentPhase(*Field.Net, *Clock, OpsTestTransition(5, EAgentPhase::Manoeuvring, EAgentPhase::Taxiing, EAgentEvent::PushedBack));
	if (!TestEqual(TEXT("taxiing out"), Flight->GetPhase(), EFlightPhase::TaxiOut)) { return false; }
	TestEqual(TEXT("parking is billed at the OFFER's rate: 2 h at 120, not at the lever's 180"), Ledger->Balance(), 240.0, 1e-3);
	TestEqual(TEXT("and recorded on the flight"), Flight->ParkingFee, 240.0, 1e-3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FParkingBilledOnceAcrossARedirectTest,
	"AirportOps.Model.FlightFees.ParkingIsBilledOnceAcrossARedirect",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FParkingBilledOnceAcrossARedirectTest::RunTest(const FString& Parameters)
{
	// THE PARKING FEE WAS POSTED AT "TaxiOut" ON EVERY AGENT EVENT THE FLIGHT HEARD WHILE IN IT, not on entering it (found while
	// diffing each writer's effects against its row, #442): a redirect of an aeroplane already taxiing out - an edit re-routed it,
	// a stranding was rescued - left the flight in TaxiOut and posted the fee AGAIN, for the longer stay, as a second ledger row.
	ULedger* Ledger = NewObject<ULedger>(GetTransientPackage());
	Ledger->Open(0.0);
	UFlightBoard* Board = NewObject<UFlightBoard>(GetTransientPackage());
	Board->Ledger = Ledger;
	USimClock* Clock = NewObject<USimClock>(GetTransientPackage());
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());

	UFlight* Flight = NewObject<UFlight>(GetTransientPackage());
	Flight->Id = 1;
	Flight->AirlineName = FText::FromString(TEXT("Test Air"));
	Flight->ParkingRatePerHour = 120.0;
	Flight->AgentId = 5;
	Flight->SetPhaseForTest(EFlightPhase::Manoeuvring);
	Flight->ParkedAt = 1000.0;
	Board->AddOffer(*Clock, Flight);

	Clock->StartAtHour((1000.0 + 7200.0) / 3600.0);
	Board->OnAgentPhase(*Net, *Clock, OpsTestTransition(5, EAgentPhase::Manoeuvring, EAgentPhase::Taxiing, EAgentEvent::PushedBack));
	if (!TestEqual(TEXT("taxiing out"), Flight->GetPhase(), EFlightPhase::TaxiOut)) { return false; }
	if (!TestEqual(TEXT("parking posted on entering TaxiOut: 2 h at 120"), Ledger->Balance(), 240.0, 1e-3)) { return false; }

	// A REDIRECT WHILE TAXIING OUT, an hour later: the flight stays TaxiOut, and nothing more is owed.
	Clock->StartAtHour((1000.0 + 10800.0) / 3600.0);
	Board->OnAgentPhase(*Net, *Clock, OpsTestTransition(5, EAgentPhase::Taxiing, EAgentPhase::Taxiing, EAgentEvent::Redirected));
	TestEqual(TEXT("still taxiing out"), Flight->GetPhase(), EFlightPhase::TaxiOut);
	TestEqual(TEXT("and the parking fee was not posted a second time"), Ledger->Balance(), 240.0, 1e-3);
	int32 ParkingRows = 0;
	for (const FLedgerEntry& Row : Ledger->Entries())
	{
		ParkingRows += Row.Category == ELedgerCategory::ParkingFee ? 1 : 0;
	}
	TestEqual(TEXT("one parking row in the ledger"), ParkingRows, 1);
	return true;
}

#endif
