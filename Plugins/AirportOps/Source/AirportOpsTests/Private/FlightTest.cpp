#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/Flight.h"
#include "Model/RoadAgent.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlightPhaseMappingTest,
	"AirportOps.Model.Flight.PhaseFromAgent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFlightPhaseMappingTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("an arriving agent is a landing flight"),
		FlightPhaseFromAgent(EAgentPhase::Arriving, EFlightPhase::Inbound), EFlightPhase::Landing);

	// THE POINT OF THE FUNCTION: one agent phase, two answers. Taxiing happens on both sides
	// of the stand and the agent cannot tell them apart, so the flight's own phase does.
	TestEqual(TEXT("taxiing before the stand is TaxiIn"),
		FlightPhaseFromAgent(EAgentPhase::Taxiing, EFlightPhase::Landing), EFlightPhase::TaxiIn);
	TestEqual(TEXT("taxiing after the turnaround is TaxiOut"),
		FlightPhaseFromAgent(EAgentPhase::Taxiing, EFlightPhase::Turnaround), EFlightPhase::TaxiOut);

	TestEqual(TEXT("parked is the turnaround"),
		FlightPhaseFromAgent(EAgentPhase::Parked, EFlightPhase::TaxiIn), EFlightPhase::Turnaround);
	TestEqual(TEXT("departing is departing"),
		FlightPhaseFromAgent(EAgentPhase::Departing, EFlightPhase::TaxiOut), EFlightPhase::Departing);
	TestEqual(TEXT("gone is departed"),
		FlightPhaseFromAgent(EAgentPhase::Gone, EFlightPhase::Departing), EFlightPhase::Departed);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlightHolderIdTest,
	"AirportOps.Model.Flight.HolderIdCannotCollideWithAnAgent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFlightHolderIdTest::RunTest(const FString& Parameters)
{
	// UGroundTraffic allocates agent ids NextAgentId++ from 1. A holder id sharing that space
	// would mean a flight releasing an aeroplane's stand, or worse, holding one it does not
	// own - and the occupancy table cannot tell the difference, by design.
	UFlight* First = NewObject<UFlight>();
	First->Id = 1;
	TestTrue(TEXT("a holder id is negative"), First->HolderId() < 0);
	TestEqual(TEXT("and is the negative of the flight id"), First->HolderId(), -1);

	UFlight* Later = NewObject<UFlight>();
	Later->Id = 4096;
	TestTrue(TEXT("still negative however many flights have been"), Later->HolderId() < 0);
	TestNotEqual(TEXT("and distinct per flight"), Later->HolderId(), First->HolderId());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlightDefaultsTest,
	"AirportOps.Model.Flight.StartsOfferedWithNoAgent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFlightDefaultsTest::RunTest(const FString& Parameters)
{
	UFlight* Flight = NewObject<UFlight>();
	TestEqual(TEXT("a new flight is an offer"), Flight->Phase, EFlightPhase::Offered);
	TestEqual(TEXT("with no agent, so nothing maps a phase onto it"), Flight->AgentId, INDEX_NONE);
	TestFalse(TEXT("and no stand held"), Flight->Stand.IsSet());
	return true;
}

#endif
