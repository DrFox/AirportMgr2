#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/Flight.h"
#include "Model/RoadAgent.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace FlightTest
{
	/** A transition as UGroundTraffic would announce it - the cause is the point, the goal does not matter here. */
	FAgentTransition Made(EAgentEvent Cause, EAgentPhase From, EAgentPhase To)
	{
		FAgentTransition T;
		T.AgentId = 5;
		T.From = From;
		T.To = To;
		T.Cause = Cause;
		return T;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlightPhaseMappingTest,
	"AirportOps.Model.Flight.PhaseFromTransition",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFlightPhaseMappingTest::RunTest(const FString& Parameters)
{
	using FlightTest::Made;
	TestEqual(TEXT("an arrival born is a landing flight"),
		FlightPhaseFromTransition(Made(EAgentEvent::Dispatched, EAgentPhase::Gone, EAgentPhase::Arriving), EFlightPhase::Inbound, false),
		EFlightPhase::Landing);
	TestEqual(TEXT("vacating is the taxi in"),
		FlightPhaseFromTransition(Made(EAgentEvent::Vacated, EAgentPhase::Arriving, EAgentPhase::Taxiing), EFlightPhase::Landing, false),
		EFlightPhase::TaxiIn);

	// A TAXI THAT GOES ON goes the way it was going: one cause, two answers, told apart by the flight's own phase.
	TestEqual(TEXT("a rescue before the stand resumes the taxi in"),
		FlightPhaseFromTransition(Made(EAgentEvent::Rescued, EAgentPhase::Stranded, EAgentPhase::Taxiing), EFlightPhase::TaxiIn, false),
		EFlightPhase::TaxiIn);
	TestEqual(TEXT("a rescue after the turnaround resumes the taxi out"),
		FlightPhaseFromTransition(Made(EAgentEvent::Rescued, EAgentPhase::Stranded, EAgentPhase::Taxiing), EFlightPhase::TaxiOut, false),
		EFlightPhase::TaxiOut);

	// #405, by the event's own goal: parked at a stand is the turnaround; parked anywhere else is still the taxi in.
	TestEqual(TEXT("parked at a stand is the turnaround"),
		FlightPhaseFromTransition(Made(EAgentEvent::Parked, EAgentPhase::Taxiing, EAgentPhase::Parked), EFlightPhase::TaxiIn, true),
		EFlightPhase::Turnaround);
	TestEqual(TEXT("parked short of a stand is still the taxi in"),
		FlightPhaseFromTransition(Made(EAgentEvent::Parked, EAgentPhase::Taxiing, EAgentPhase::Parked), EFlightPhase::TaxiIn, false),
		EFlightPhase::TaxiIn);

	// THE CAUSE TELLS THE TWO Parked -> Taxiing APART (review M1, M4) - the pair they share could not, so the board used
	// to ask the live agent whether a departure was armed, a drain after the fact.
	TestEqual(TEXT("sent off the fallback junction for a runway is the taxi OUT"),
		FlightPhaseFromTransition(Made(EAgentEvent::DepartOrdered, EAgentPhase::Parked, EAgentPhase::Taxiing), EFlightPhase::TaxiIn, false),
		EFlightPhase::TaxiOut);
	TestEqual(TEXT("sent off the fallback junction to a stand is still the taxi IN"),
		FlightPhaseFromTransition(Made(EAgentEvent::ReOffered, EAgentPhase::Parked, EAgentPhase::Taxiing), EFlightPhase::TaxiIn, false),
		EFlightPhase::TaxiIn);

	TestEqual(TEXT("lined up is departing"),
		FlightPhaseFromTransition(Made(EAgentEvent::LinedUp, EAgentPhase::Taxiing, EAgentPhase::Departing), EFlightPhase::TaxiOut, false),
		EFlightPhase::Departing);
	TestEqual(TEXT("gone off the climb is departed"),
		FlightPhaseFromTransition(Made(EAgentEvent::Gone, EAgentPhase::Departing, EAgentPhase::Gone), EFlightPhase::Departing, false),
		EFlightPhase::Departed);
	TestEqual(TEXT("stranded moves it nowhere"),
		FlightPhaseFromTransition(Made(EAgentEvent::Stranded, EAgentPhase::Taxiing, EAgentPhase::Stranded), EFlightPhase::TaxiIn, false),
		EFlightPhase::TaxiIn);
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
	// THE DECLARATION ORDER IS LOAD-BEARING and this is what pins it. FlightPhaseFromTransition
	// decides taxi-in from taxi-out by asking whether the flight has reached Turnaround, so
	// Manoeuvring must sit AFTER Turnaround - inserting it before would read every taxi OUT as
	// a taxi in, with no compiler complaint at all.
	TestTrue(TEXT("Turnaround comes before Manoeuvring"),
		EFlightPhase::Turnaround < EFlightPhase::Manoeuvring);
	TestTrue(TEXT("and Manoeuvring before TaxiOut"),
		EFlightPhase::Manoeuvring < EFlightPhase::TaxiOut);

	TestEqual(TEXT("a push off the stand is a manoeuvring flight"),
		FlightPhaseFromTransition(FlightTest::Made(EAgentEvent::DepartOrdered, EAgentPhase::Parked, EAgentPhase::Manoeuvring),
			EFlightPhase::Turnaround, true),
		EFlightPhase::Manoeuvring);

	// AND A TAXI THAT GOES ON AFTER IT IS STILL A TAXI OUT - which is what the ordering above buys, and
	// what would break silently if Manoeuvring were declared in the wrong place.
	TestEqual(TEXT("a rescue of a stranded push is the taxi out"),
		FlightPhaseFromTransition(FlightTest::Made(EAgentEvent::Rescued, EAgentPhase::Stranded, EAgentPhase::Taxiing),
			EFlightPhase::Manoeuvring, false),
		EFlightPhase::TaxiOut);
	TestEqual(TEXT("the push's end is the taxi out"),
		FlightPhaseFromTransition(FlightTest::Made(EAgentEvent::PushedBack, EAgentPhase::Manoeuvring, EAgentPhase::Taxiing),
			EFlightPhase::Manoeuvring, false),
		EFlightPhase::TaxiOut);

	return true;
}

#endif
