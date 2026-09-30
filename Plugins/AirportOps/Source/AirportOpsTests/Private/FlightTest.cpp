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
	TestEqual(TEXT("a new flight is an offer"), Flight->GetPhase(), EFlightPhase::Offered);
	TestEqual(TEXT("with no agent, so nothing maps a phase onto it"), Flight->AgentId, INDEX_NONE);
	TestFalse(TEXT("and no stand held"), Flight->Stand.IsSet());
	// THE DECLARATION ORDER IS NOT LOAD-BEARING ANY MORE (#442): FlightPhaseFromTransition decides taxi-in from taxi-out by asking
	// FlightPhase::HasReachedStand, which is a switch on the phase and not a comparison of two. What pins it is that Manoeuvring -
	// the push, which the taxi out follows - and TaxiOut both count as reaching the stand, with no reading of where they sit.
	TestTrue(TEXT("a flight coming off its stand has reached it"), FlightPhase::HasReachedStand(EFlightPhase::Manoeuvring));
	TestTrue(TEXT("and so has the taxi out"), FlightPhase::HasReachedStand(EFlightPhase::TaxiOut));
	TestFalse(TEXT("the taxi in has not"), FlightPhase::HasReachedStand(EFlightPhase::TaxiIn));

	TestEqual(TEXT("a push off the stand is a manoeuvring flight"),
		FlightPhaseFromTransition(FlightTest::Made(EAgentEvent::DepartOrdered, EAgentPhase::Parked, EAgentPhase::Manoeuvring),
			EFlightPhase::Turnaround, true),
		EFlightPhase::Manoeuvring);

	// AND A TAXI THAT GOES ON AFTER IT IS STILL A TAXI OUT - which is what HasReachedStand above buys, and
	// what would break if Manoeuvring stopped counting as having reached the stand.
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlightEveryPhaseHasOneStageTest,
	"AirportOps.Model.Flight.EveryPhaseHasOneStage",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFlightEveryPhaseHasOneStageTest::RunTest(const FString& Parameters)
{
	// THE PREDICATES ARE ONE TABLE, stated by name here so a phase added to the enum fails THIS test as well as the build at
	// StageOf: each phase lands in exactly one stage, and every predicate is that stage and nothing else. The three the issue
	// named (IsUnarrived, IsOnGround, IsTerminal) and the two derived from them.
	struct FExpectation
	{
		EFlightPhase Phase;
		EFlightStage Stage;
	};
	const FExpectation Expected[] = {
		{ EFlightPhase::Offered,     EFlightStage::Offer },
		{ EFlightPhase::Accepted,    EFlightStage::Unarrived },
		{ EFlightPhase::Inbound,     EFlightStage::Unarrived },
		{ EFlightPhase::Landing,     EFlightStage::OnTheWayIn },
		{ EFlightPhase::TaxiIn,      EFlightStage::OnTheWayIn },
		{ EFlightPhase::Turnaround,  EFlightStage::AtStandOrLeaving },
		{ EFlightPhase::Manoeuvring, EFlightStage::AtStandOrLeaving },
		{ EFlightPhase::TaxiOut,     EFlightStage::AtStandOrLeaving },
		{ EFlightPhase::Departing,   EFlightStage::AtStandOrLeaving },
		{ EFlightPhase::Departed,    EFlightStage::Terminal },
		{ EFlightPhase::Declined,    EFlightStage::Terminal },
		{ EFlightPhase::Expired,     EFlightStage::Terminal },
		{ EFlightPhase::Cancelled,   EFlightStage::Terminal },
		{ EFlightPhase::Withdrawn,   EFlightStage::Terminal },
	};

	// EVERY ENUMERATOR THE ENUM HAS, asked of reflection: a phase nobody added a row for is a count mismatch here.
	const UEnum* Enum = StaticEnum<EFlightPhase>();
	if (!TestNotNull(TEXT("the enum reflects"), Enum)) { return false; }
	TestEqual(TEXT("every phase has an expectation - a new phase must be placed in a stage"),
		Enum->NumEnums() - 1 /* the _MAX entry */, static_cast<int32>(UE_ARRAY_COUNT(Expected)));

	for (const FExpectation& Each : Expected)
	{
		const FString Name = Enum->GetNameStringByValue(static_cast<int64>(Each.Phase));
		TestTrue(*(Name + TEXT(" is a phase the enum names")), Enum->IsValidEnumValue(static_cast<int64>(Each.Phase)));
		TestTrue(*(Name + TEXT(": its stage")), FlightPhase::StageOf(Each.Phase) == Each.Stage);
		const bool bUnarrived = Each.Stage == EFlightStage::Unarrived;
		const bool bOnGround = Each.Stage == EFlightStage::OnTheWayIn || Each.Stage == EFlightStage::AtStandOrLeaving;
		const bool bTerminal = Each.Stage == EFlightStage::Terminal;
		TestEqual(*(Name + TEXT(": IsUnarrived")), FlightPhase::IsUnarrived(Each.Phase), bUnarrived);
		TestEqual(*(Name + TEXT(": IsOnGround")), FlightPhase::IsOnGround(Each.Phase), bOnGround);
		TestEqual(*(Name + TEXT(": IsTerminal")), FlightPhase::IsTerminal(Each.Phase), bTerminal);
		TestEqual(*(Name + TEXT(": IsLive is unarrived or on the ground")), FlightPhase::IsLive(Each.Phase), bUnarrived || bOnGround);
		TestEqual(*(Name + TEXT(": HasReachedStand")), FlightPhase::HasReachedStand(Each.Phase), Each.Stage == EFlightStage::AtStandOrLeaving);
		TestEqual(*(Name + TEXT(": IsArriving")), FlightPhase::IsArriving(Each.Phase), Each.Stage == EFlightStage::OnTheWayIn);
	}
	return true;
}

#endif
