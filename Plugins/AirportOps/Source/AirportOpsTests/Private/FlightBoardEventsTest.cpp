#include "CoreMinimal.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/AirlineDefinition.h"
#include "Model/Flight.h"
#include "Model/FlightBoard.h"
#include "Model/GroundTraffic.h"
#include "Model/OfferGenerator.h"
#include "Model/OpsEventBus.h"
#include "Model/RoadAgent.h"
#include "Model/RoadNetwork.h"
#include "Model/SimClock.h"
#include "Model/StandAllocator.h"

#if WITH_DEV_AUTOMATION_TESTS

// WHAT THE FLIGHT BOARD PUBLISHES (spec 2026-09-29 §3): the three facts airline satisfaction is built
// on. World-free, with the board fixtures FlightBoardTest uses, copied rather than shared for that
// file's own reason.

namespace
{
	/** Everything a board test needs, with a bus the board publishes onto and recorders on it. */
	struct FFbEventsFixture
	{
		URoadNetwork* Net = nullptr;
		UGroundTraffic* Traffic = nullptr;
		USimClock* Clock = nullptr;
		UFlightBoard* Board = nullptr;
		FOpsEventBus Bus;
		TArray<FOfferExpiredEvent> Expired;
		TArray<FOfferDeclinedEvent> Declined;
		TArray<FFlightAirborneEvent> Airborne;
		TArray<FOfferAcceptedEvent> Accepted;

		FFbEventsFixture()
		{
			Net = NewObject<URoadNetwork>(GetTransientPackage());
			UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
			Net->PlaceEntity(Stand, Stand->Anchors, FVector2D(0.0, 0.0), 0.0, 3600.0, Stand->PoseRole, Stand->Trucks);
			Traffic = NewObject<UGroundTraffic>();
			Clock = NewObject<USimClock>();
			Board = NewObject<UFlightBoard>(GetTransientPackage());
			Board->Allocator = NewObject<UStandAllocator>(GetTransientPackage());
			Board->Bus = &Bus;
			Bus.BeginWiring();
			Bus.Subscribe<FOfferExpiredEvent>(EOpsTier::Sim, TEXT("test"), [this](const FOfferExpiredEvent& E) { Expired.Add(E); });
			Bus.Subscribe<FOfferDeclinedEvent>(EOpsTier::Sim, TEXT("test"), [this](const FOfferDeclinedEvent& E) { Declined.Add(E); });
			Bus.Subscribe<FFlightAirborneEvent>(EOpsTier::Sim, TEXT("test"), [this](const FFlightAirborneEvent& E) { Airborne.Add(E); });
			Bus.Subscribe<FOfferAcceptedEvent>(EOpsTier::Sim, TEXT("test"), [this](const FOfferAcceptedEvent& E) { Accepted.Add(E); });
			Bus.EndWiring();
		}

		UFlight* Offer(FName Airline)
		{
			UFlight* Flight = NewObject<UFlight>(GetTransientPackage());
			Flight->Airframe.Wingspan = 3400.0;
			Flight->AirlineId = Airline;
			Board->AddOffer(*Clock, Flight);
			return Flight;
		}

		/** A flight already on the ground as agent 5, driven through the phase sequence to Departing. Agent 5 is no
		 *  agent of this traffic model, so its Parked is at no stand and moves the flight nowhere (#405); the
		 *  manoeuvre and the taxi out after it are what reach Departing. */
		UFlight* DepartAt(double Now, double AcceptedAt, double ContractSeconds)
		{
			UFlight* Flight = NewObject<UFlight>(GetTransientPackage());
			Flight->Airframe.Wingspan = 3400.0;
			Flight->AirlineId = TEXT("Cumbria");
			Flight->AgentId = 5;
			Flight->Phase = EFlightPhase::Landing;
			Flight->AcceptedAt = AcceptedAt;
			Flight->ContractSeconds = ContractSeconds;
			Board->AddOffer(*Clock, Flight);
			Board->OnAgentPhase(*Traffic, *Net, *Clock, 5, EAgentPhase::Arriving, EAgentPhase::Taxiing);
			Board->OnAgentPhase(*Traffic, *Net, *Clock, 5, EAgentPhase::Taxiing, EAgentPhase::Parked);
			Board->OnAgentPhase(*Traffic, *Net, *Clock, 5, EAgentPhase::Parked, EAgentPhase::Manoeuvring);
			Board->OnAgentPhase(*Traffic, *Net, *Clock, 5, EAgentPhase::Manoeuvring, EAgentPhase::Taxiing);
			// THE CLOCK AT TAKE-OFF, set only now: AirborneAt is taken on the Departing change.
			Clock->StartAtHour(Now / 3600.0);
			Board->OnAgentPhase(*Traffic, *Net, *Clock, 5, EAgentPhase::Taxiing, EAgentPhase::Departing);
			return Flight;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlightBoardEventsDeclinedTest, "AirportOps.Model.FlightBoard.Events.Declined",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFlightBoardEventsDeclinedTest::RunTest(const FString&)
{
	FFbEventsFixture F;
	UFlight* Flight = F.Offer(TEXT("Cumbria"));
	F.Board->Decline(*F.Clock, *Flight);
	F.Bus.Drain();
	if (!TestEqual(TEXT("a decline is published once"), F.Declined.Num(), 1)) { return false; }
	TestEqual(TEXT("naming the flight"), F.Declined[0].FlightId, Flight->Id);
	TestEqual(TEXT("and its airline, so the roster can find it"), F.Declined[0].AirlineId, FName(TEXT("Cumbria")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlightBoardEventsAcceptedTest, "AirportOps.Model.FlightBoard.AcceptPublishesOfferAccepted",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFlightBoardEventsAcceptedTest::RunTest(const FString&)
{
	// SPEC 2026-09-29-ops-batch3 §2: the one offer decision with no event. Accept is a player command the
	// game module calls on the board directly, so without this nothing downstream (the alerts pass today,
	// the arrival queue in PR D) hears that a stand was just promised.
	FFbEventsFixture F;
	UFlight* Flight = F.Offer(TEXT("Cumbria"));
	if (!TestTrue(TEXT("the offer is accepted - the fixture's stand admits it"), F.Board->Accept(*F.Traffic, *F.Net, *F.Clock, *Flight))) { return false; }
	F.Bus.Drain();
	if (!TestEqual(TEXT("an accept is published once"), F.Accepted.Num(), 1)) { return false; }
	TestEqual(TEXT("naming the flight"), F.Accepted[0].FlightId, Flight->Id);
	TestEqual(TEXT("its airline"), F.Accepted[0].AirlineId, FName(TEXT("Cumbria")));
	TestTrue(TEXT("and the stand Accept held for it - the one the queue will wait on"), F.Accepted[0].Stand == Flight->Stand && Flight->Stand.IsSet());

	// A REFUSED ACCEPT IS NOT AN ACCEPT: the flight is no longer Offered, so Accept returns false and says nothing.
	TestFalse(TEXT("a second accept of the same flight is refused"), F.Board->Accept(*F.Traffic, *F.Net, *F.Clock, *Flight));
	F.Bus.Drain();
	TestEqual(TEXT("and publishes nothing more"), F.Accepted.Num(), 1);

	// NO STAND FREE (review M6): the fixture's one stand is held by the flight above, so Reserve refuses the
	// next offer - the refusal the inbox greys out, and not an accept.
	UFlight* Second = F.Offer(TEXT("Cumbria"));
	TestFalse(TEXT("an offer with no stand free is refused"), F.Board->Accept(*F.Traffic, *F.Net, *F.Clock, *Second));
	F.Bus.Drain();
	TestEqual(TEXT("and a refusal by the allocator publishes nothing"), F.Accepted.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlightBoardEventsExpiredTest, "AirportOps.Model.FlightBoard.Events.Expired",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFlightBoardEventsExpiredTest::RunTest(const FString&)
{
	FFbEventsFixture F;
	UFlight* Flight = F.Offer(TEXT("Cumbria"));
	Flight->OfferSecondsLeft = 1.0;
	F.Board->TickOffers(*F.Traffic, *F.Net, *F.Clock, 2.0);
	F.Bus.Drain();
	if (!TestEqual(TEXT("a lapse is published once"), F.Expired.Num(), 1)) { return false; }
	TestEqual(TEXT("with the flight's own lapse reason, which decides the airline's penalty"),
		F.Expired[0].Reason, Flight->LapseReason);
	TestEqual(TEXT("and its airline"), F.Expired[0].AirlineId, FName(TEXT("Cumbria")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlightBoardEventsAirborneTest, "AirportOps.Model.FlightBoard.Events.AirborneLateness",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFlightBoardEventsAirborneTest::RunTest(const FString&)
{
	{
		FFbEventsFixture F;
		// Accepted at 100 with a 600 s contract: due airborne by 700. Up at 1300 is 600 s late.
		F.DepartAt(1300.0, 100.0, 600.0);
		F.Bus.Drain();
		if (!TestEqual(TEXT("take-off is published once"), F.Airborne.Num(), 1)) { return false; }
		TestEqual(TEXT("lateness is measured against the contract the row showed"), F.Airborne[0].LateBySeconds, 600.0, 1e-6);
	}
	{
		FFbEventsFixture F;
		F.DepartAt(500.0, 100.0, 600.0);
		F.Bus.Drain();
		if (!TestEqual(TEXT("take-off is published once"), F.Airborne.Num(), 1)) { return false; }
		TestEqual(TEXT("early is negative, not clamped - what early is worth is the listener's call"),
			F.Airborne[0].LateBySeconds, -200.0, 1e-6);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOfferCarriesAirlineTest, "AirportOps.Model.Offers.MakeOfferCarriesAirline",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOfferCarriesAirlineTest::RunTest(const FString&)
{
	UAirlineDefinition* Airline = NewObject<UAirlineDefinition>(GetTransientPackage(), TEXT("TestAir"));
	UOfferGenerator* Generator = NewObject<UOfferGenerator>(GetTransientPackage());
	FOfferCandidate Candidate;
	Candidate.Airframe.Wingspan = 3400.0;
	UFlight* Flight = Generator->MakeOffer(FVector2D::ZeroVector, *Airline, Candidate, 0.0, 1);
	if (!TestNotNull(TEXT("an offer is made"), Flight)) { return false; }
	TestEqual(TEXT("the flight carries its airline's KEY - the definition's name, not its display text"),
		Flight->AirlineId, FName(TEXT("TestAir")));
	return true;
}

#endif
