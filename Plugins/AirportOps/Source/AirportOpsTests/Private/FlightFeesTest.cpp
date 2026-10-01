#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/AirlineDefinition.h"
#include "Model/Flight.h"
#include "Model/FlightBilling.h"
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
	/** A board and the money beside it - the ledger is billing's, handed in (#506 review), not the board's. */
	UFlightBoard* PaidBoard(ULedger*& OutLedger, UPricing*& OutPricing, double Opening = 0.0)
	{
		OutLedger = NewObject<ULedger>();
		OutPricing = NewObject<UPricing>();
		OutLedger->Open(Opening);

		UFlightBoard* Board = NewObject<UFlightBoard>();
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

	FlightBilling::PostLandingFee(Ledger, 0.0, *Flight);

	TestEqual(TEXT("landing credits the fee the OFFER quoted, not one recomputed at touchdown - "
		"the number the player accepted must not have been a lie"),
		Ledger->Balance(), 1200.0, 1e-6);
	TestEqual(TEXT("and it is booked as a landing fee"),
		Ledger->Entries()[0].Category, ELedgerCategory::LandingFee);

	FlightBilling::PostLandingFee(Ledger, 0.0, *Flight);
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
	FlightBilling::PostParkingFee(Ledger, 1000.0 + 7200.0, *Flight);

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
	FlightBilling::PostParkingFee(Ledger, 50000.0, *Flight);

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
	UFlight* Flight = CodeCFlight();
	Flight->LandingFee = 1200.0;
	Flight->ParkedAt = 1000.0;

	// NO LEDGER HANDED IN - billing's own null since #506's review (the board holds none to forget).
	FlightBilling::PostLandingFee(/*Ledger*/ nullptr, 0.0, *Flight);
	FlightBilling::PostParkingFee(/*Ledger*/ nullptr, 8200.0, *Flight);

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

	// THE FORFEIT IS NOT HERE (#463): it was two assertions comparing the balance and the row count with values captured one line
	// earlier, which cannot fail. "An Unserviceable demand never reaches PostServiceFee, so the ledger of an aircraft nothing could
	// serve is exactly as it opened" is measured where it can go red - AirportOps.Model.UnserviceableStillDeparts runs a real
	// turnaround with no depot against a real ledger and asserts no ServiceFee row and no balance change (spec D7: a forfeit is an
	// entry that does not happen, never a negative one).
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
	// THE BOARD HAS THE LEVER WIRED, as UOpsRuntime's constructor wires it: what the old PostParkingFee asked at departure.
	Board->Pricing = Pricing;
	Board->Allocator = NewObject<UStandAllocator>(GetTransientPackage());
	// BILLING IS A REACTION TO THE PHASE (#442 item 4): the runtime's "Billing" subscription on this test's own bus, drained after
	// each change as the runtime's Tick drains it.
	FOpsTestBilling Billing(*Board, Ledger);
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
	Billing.Bus.Drain();
	if (!TestEqual(TEXT("taxiing out"), Flight->GetPhase(), EFlightPhase::TaxiOut)) { return false; }
	TestEqual(TEXT("parking is billed at the OFFER's rate: 2 h at 120, not at the lever's 180"), Ledger->Balance(), 240.0, 1e-3);
	TestEqual(TEXT("and recorded on the flight"), Flight->ParkingFee, 240.0, 1e-3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FParkingBilledOncePerFlightTest,
	"AirportOps.Model.FlightFees.ParkingIsBilledOncePerFlight",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FParkingBilledOncePerFlightTest::RunTest(const FString& Parameters)
{
	// AN AEROPLANE THAT PARKS AGAIN after its taxi out began enters TaxiOut a second time (#442 review): the once-per-entry guard lets that
	// through, and PostParkingFee billed the overlapping hours again from the original ParkedAt. One flight, one parking fee - the landing
	// fee's rule, and its bLandingFeePaid.
	ULedger* Ledger = NewObject<ULedger>(GetTransientPackage());
	Ledger->Open(0.0);
	UFlightBoard* Board = NewObject<UFlightBoard>(GetTransientPackage());
	FOpsTestBilling Billing(*Board, Ledger);   // the reaction that posts the fee (#442 item 4), drained after each change
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
	Billing.Bus.Drain();
	if (!TestEqual(TEXT("taxiing out"), Flight->GetPhase(), EFlightPhase::TaxiOut)) { return false; }
	if (!TestEqual(TEXT("parking posted on entering TaxiOut: 2 h at 120"), Ledger->Balance(), 240.0, 1e-3)) { return false; }
	TestTrue(TEXT("and the flight knows it was billed"), Flight->bParkingFeePaid);

	// IT PARKS AGAIN (a Parked event reaches Turnaround from any phase that has reached the stand), an hour later, and pushes back out.
	Clock->StartAtHour((1000.0 + 10800.0) / 3600.0);
	Board->OnAgentPhase(*Net, *Clock, OpsTestTransition(5, EAgentPhase::Taxiing, EAgentPhase::Parked, EAgentEvent::Parked));
	Billing.Bus.Drain();
	if (!TestEqual(TEXT("PRECONDITION: back on the stand"), Flight->GetPhase(), EFlightPhase::Turnaround)) { return false; }
	Clock->StartAtHour((1000.0 + 14400.0) / 3600.0);
	Board->OnAgentPhase(*Net, *Clock, OpsTestTransition(5, EAgentPhase::Parked, EAgentPhase::Taxiing, EAgentEvent::DepartOrdered));
	Billing.Bus.Drain();
	if (!TestEqual(TEXT("PRECONDITION: taxiing out again - a second ENTRY"), Flight->GetPhase(), EFlightPhase::TaxiOut)) { return false; }
	TestEqual(TEXT("and the flight is billed ONCE: the balance is unchanged"), Ledger->Balance(), 240.0, 1e-3);
	int32 ParkingRows = 0;
	for (const FLedgerEntry& Row : Ledger->Entries())
	{
		ParkingRows += Row.Category == ELedgerCategory::ParkingFee ? 1 : 0;
	}
	TestEqual(TEXT("one parking row in the ledger"), ParkingRows, 1);
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
	FOpsTestBilling Billing(*Board, Ledger);   // the reaction that posts the fee (#442 item 4), drained after each change
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
	Billing.Bus.Drain();
	if (!TestEqual(TEXT("taxiing out"), Flight->GetPhase(), EFlightPhase::TaxiOut)) { return false; }
	if (!TestEqual(TEXT("parking posted on entering TaxiOut: 2 h at 120"), Ledger->Balance(), 240.0, 1e-3)) { return false; }

	// A REDIRECT WHILE TAXIING OUT, an hour later: the flight stays TaxiOut, and nothing more is owed.
	Clock->StartAtHour((1000.0 + 10800.0) / 3600.0);
	Board->OnAgentPhase(*Net, *Clock, OpsTestTransition(5, EAgentPhase::Taxiing, EAgentPhase::Taxiing, EAgentEvent::Redirected));
	Billing.Bus.Drain();
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBilledOnTheBusARoundLaterTest,
	"AirportOps.Model.FlightFees.BilledOnTheBusARoundLater",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBilledOnTheBusARoundLaterTest::RunTest(const FString& Parameters)
{
	// #442 ITEM 4 MOVED WHEN MONEY POSTS: billing is a Sim-tier reaction to FFlightPhaseChangedEvent, so a fee is posted when the
	// change is HEARD - a round later, in the drain that made it - not inside the agent event that made it, as UFlightBoard::
	// OnAgentPhase posted it. Pinned both ways: the change alone posts nothing, and the drain posts the fee priced and dated AS OF THE
	// CHANGE even with the clock moved on before it (the event carries At). The runtime's Tick drains inside the frame, so a reader
	// later in the frame still sees it: AirportOps.Present.Bus.BillingIsWired is that half.
	ULedger* Ledger = NewObject<ULedger>(GetTransientPackage());
	Ledger->Open(0.0);
	UFlightBoard* Board = NewObject<UFlightBoard>(GetTransientPackage());
	FOpsTestBilling Billing(*Board, Ledger);
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
	const int32 RowsBefore = Ledger->Entries().Num();
	Board->OnAgentPhase(*Net, *Clock, OpsTestTransition(5, EAgentPhase::Manoeuvring, EAgentPhase::Taxiing, EAgentEvent::PushedBack));
	if (!TestEqual(TEXT("PRECONDITION: the flight entered TaxiOut"), Flight->GetPhase(), EFlightPhase::TaxiOut)) { return false; }
	TestEqual(TEXT("the change itself posts nothing - billing is the bus's reaction, not the board's"), Ledger->Entries().Num(), RowsBefore);
	TestFalse(TEXT("and the flight is not billed yet"), Flight->bParkingFeePaid);

	// AN HOUR PASSES BEFORE THE DRAIN - and the stay must not grow with it.
	Clock->StartAtHour((1000.0 + 10800.0) / 3600.0);
	Billing.Bus.Drain();
	TestEqual(TEXT("the drain posts the parking fee: 2 h at 120, priced as of the change, not the hour since"), Ledger->Balance(), 240.0, 1e-3);
	if (TestEqual(TEXT("one parking row"), Ledger->Entries().Num(), RowsBefore + 1))
	{
		TestEqual(TEXT("dated the change, not the drain"), Ledger->Entries().Last().At, 1000.0 + 7200.0, 1e-3);
	}
	return true;
}

#endif
