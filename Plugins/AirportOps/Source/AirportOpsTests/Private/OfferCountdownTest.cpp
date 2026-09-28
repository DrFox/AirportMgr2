#include "CoreMinimal.h"
#include "Build/AnchorLink.h"
#include "Content/AirsideSettings.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/Flight.h"
#include "Model/FlightBoard.h"
#include "Model/GroundTraffic.h"
#include "Model/LandingRun.h"
#include "Model/RoadAgent.h"
#include "Model/RoadNetwork.h"
#include "Model/SimClock.h"
#include "Model/StandAllocator.h"
#include "Profiles/RoadProfile.h"
#include "Testing/AirsideTestGraph.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The offer countdown (spec 2026-09-28 section 3): REAL seconds, drained only while unpaused,
 * whatever the speed; a lapse says whether the player could ever have taken it.
 */
namespace
{
	FAirframe CountdownAirframe()
	{
		FAirframe Out;
		Out.Wingspan = 1200.0;
		Out.TurnaroundSeconds = 1800.0;
		return Out;
	}

	/**
	 * A runway, an exit, a taxiway and ONE stand - the smallest field ArrivalPlanner will say
	 * None for, so an offer here is genuinely acceptable. Copied from OfferGeneratorTest's
	 * FieldWith rather than shared: two small fixtures beat a header both files must agree on.
	 * bWithStand false gives the same field with nothing to park on.
	 */
	URoadNetwork* CountdownField(bool bWithStand = true)
	{
		const FAirframe For = CountdownAirframe();
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		const double Needed = FLandingRun::RequiredLandingDistance(
			For.Chassis.Ground, For.Climb, For.Approach) * FLandingRun::LandingMargin;
		const double Length = FMath::Max(Needed * 3.0, 60000.0);
		const FVector2D ExitAt(Length * 0.5, 0.0);

		URoadProfile* Runway = TestProfiles::Runway();
		const FRoadNodeId ThresholdNode = Net->AddNode(FVector2D::ZeroVector);
		const FRoadNodeId ExitNode = Net->AddNode(ExitAt);
		const FRoadNodeId FarNode = Net->AddNode(FVector2D(Length, 0.0));
		Net->AddStraightSegment(ThresholdNode, ExitNode, Runway);
		Net->AddStraightSegment(ExitNode, FarNode, Runway);
		const FRoadNodeId TaxiEnd = Net->AddNode(ExitAt + FVector2D(0.0, -20000.0));
		Net->AddStraightSegment(ExitNode, TaxiEnd, TestProfiles::Taxiway());
		TestGraph::Derive(*Net);

		if (bWithStand)
		{
			UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
			Net->PlaceEntity(Stand, Stand->Anchors, ExitAt + FVector2D(9000.0, -10000.0), 0.0,
				6000.0, Stand->PoseRole, Stand->Trucks);
		}
		FAnchorLink::Build(*Net, UAirsideSettings::ResolveLargestServiceVehicle());
		return Net;
	}

	struct FCountdownRig
	{
		URoadNetwork* Net = nullptr;
		UGroundTraffic* Traffic = nullptr;
		USimClock* Clock = nullptr;
		UFlightBoard* Board = nullptr;

		explicit FCountdownRig(bool bWithStand = true)
		{
			Net = CountdownField(bWithStand);
			Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
			Clock = NewObject<USimClock>(GetTransientPackage());
			Board = NewObject<UFlightBoard>(GetTransientPackage());
			Board->Allocator = NewObject<UStandAllocator>(GetTransientPackage());
			Board->Dispatcher = [](const FVector2D&, const FAirframe&) { return true; };
		}

		UFlight* Offer(double Window, double Lead = 900.0, double Contract = 4200.0)
		{
			UFlight* Flight = NewObject<UFlight>(GetTransientPackage());
			Flight->Airframe = CountdownAirframe();
			Flight->OfferWindowSeconds = Window;
			Flight->OfferSecondsLeft = Window;
			Flight->LeadTimeSeconds = Lead;
			Flight->ContractSeconds = Contract;
			Board->AddOffer(*Clock, Flight);
			return Flight;
		}

		void Tick(double RealSeconds) { Board->TickOffers(*Traffic, *Net, *Clock, RealSeconds); }
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOfferCountdownRealSecondsTest, "AirportOps.Model.Offers.Countdown.DrainsInRealSeconds",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOfferCountdownRealSecondsTest::RunTest(const FString& Parameters)
{
	// REAL, NOT GAME: at x4 the clock runs four times as fast, and an offer that lapsed four
	// times as fast would be the eight-second window this spec was written to get rid of.
	FCountdownRig AtOne;
	UFlight* A = AtOne.Offer(60.0);
	AtOne.Tick(10.0);
	TestEqual(TEXT("ten real seconds at x1 take ten off"), A->OfferSecondsLeft, 50.0, 1e-9);

	FCountdownRig AtFour;
	AtFour.Clock->SetSpeed(ESimSpeed::X4);
	UFlight* B = AtFour.Offer(60.0);
	AtFour.Tick(10.0);
	TestEqual(TEXT("and ten at x4 take ten too"), B->OfferSecondsLeft, 50.0, 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOfferCountdownPausedTest, "AirportOps.Model.Offers.Countdown.PausedDoesNotDrain",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOfferCountdownPausedTest::RunTest(const FString& Parameters)
{
	FCountdownRig Rig;
	UFlight* Flight = Rig.Offer(60.0);
	Rig.Clock->TogglePause();
	Rig.Tick(30.0);
	TestEqual(TEXT("a paused game stops the offer clock"), Flight->OfferSecondsLeft, 60.0, 1e-9);
	TestEqual(TEXT("and it is still offered"), Flight->Phase, EFlightPhase::Offered);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOfferCountdownIgnoredTest, "AirportOps.Model.Offers.Countdown.LapseIgnored",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOfferCountdownIgnoredTest::RunTest(const FString& Parameters)
{
	// NO INBOX WIDGET HERE AT ALL - the board classifies the lapse on its own, so C's score
	// does not depend on whether the player had the panel open.
	FCountdownRig Rig;
	UFlight* Flight = Rig.Offer(60.0);
	Rig.Tick(30.0);
	Rig.Tick(31.0);
	TestEqual(TEXT("an unanswered offer lapses"), Flight->Phase, EFlightPhase::Expired);
	TestEqual(TEXT("and it could have been taken, so it was ignored"), Flight->LapseReason, ELapseReason::Ignored);
	TestEqual(TEXT("it leaves the inbox"), Rig.Board->PendingOfferCount(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOfferCountdownNeverAcceptableTest, "AirportOps.Model.Offers.Countdown.LapseNeverAcceptable",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOfferCountdownNeverAcceptableTest::RunTest(const FString& Parameters)
{
	FCountdownRig Rig(/*bWithStand*/ false);
	UFlight* Flight = Rig.Offer(60.0);
	Rig.Tick(61.0);
	TestEqual(TEXT("it lapses"), Flight->Phase, EFlightPhase::Expired);
	TestEqual(TEXT("and it never could have been taken - no stand"), Flight->LapseReason, ELapseReason::NeverAcceptable);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOfferCountdownAcceptAfterLapseTest, "AirportOps.Model.Offers.Countdown.AcceptAfterLapseRefused",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOfferCountdownAcceptAfterLapseTest::RunTest(const FString& Parameters)
{
	// The frame the countdown hits zero and a click in the same frame: the lapse wins, and the
	// click must not resurrect it into a stand hold.
	FCountdownRig Rig;
	UFlight* Flight = Rig.Offer(5.0);
	Rig.Tick(6.0);
	TestFalse(TEXT("a lapsed offer cannot be accepted"), Rig.Board->Accept(*Rig.Traffic, *Rig.Net, *Rig.Clock, *Flight));
	TestEqual(TEXT("it stays Expired"), Flight->Phase, EFlightPhase::Expired);

	UFlight* Taken = Rig.Offer(5.0);
	TestTrue(TEXT("one accepted in time"), Rig.Board->Accept(*Rig.Traffic, *Rig.Net, *Rig.Clock, *Taken));
	Rig.Tick(100.0);
	TestEqual(TEXT("does not lapse afterwards - the countdown stops at the accept"), Taken->Phase, EFlightPhase::Accepted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOfferCountdownAcceptSchedulesTest, "AirportOps.Model.Offers.Countdown.AcceptSchedulesFromNow",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOfferCountdownAcceptSchedulesTest::RunTest(const FString& Parameters)
{
	// THE LEAD TIME RUNS FROM THE ACCEPT, not from the offer: a player who took 50 seconds to
	// decide still gets the full lead, and the contract is measured from the decision.
	FCountdownRig Rig;
	Rig.Clock->SetUniformDay(USimClock::SecondsPerDay);
	Rig.Clock->StartAtHour(10.0);
	UFlight* Flight = Rig.Offer(60.0, /*Lead*/ 900.0, /*Contract*/ 4200.0);
	Rig.Clock->Advance(50.0);
	const double At = Rig.Clock->Now();
	TestTrue(TEXT("accepted"), Rig.Board->Accept(*Rig.Traffic, *Rig.Net, *Rig.Clock, *Flight));
	TestEqual(TEXT("the accept is recorded"), Flight->AcceptedAt, At, 1e-9);
	TestEqual(TEXT("it lands a lead time after the accept"), Flight->ArrivesAt, At + 900.0, 1e-9);
	TestEqual(TEXT("and is due airborne a contract after it"), Flight->AirborneBy(), At + 4200.0, 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOfferCountdownSortedTest, "AirportOps.Model.Offers.Countdown.OffersSortedByTimeLeft",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOfferCountdownSortedTest::RunTest(const FString& Parameters)
{
	FCountdownRig Rig;
	Rig.Offer(120.0);
	Rig.Offer(45.0);
	Rig.Offer(60.0);
	const TArray<UFlight*> Offers = Rig.Board->Offers();
	if (!TestEqual(TEXT("three offers"), Offers.Num(), 3)) { return false; }
	TestEqual(TEXT("the most urgent first"), Offers[0]->OfferSecondsLeft, 45.0, 1e-9);
	TestEqual(TEXT("then the next"), Offers[1]->OfferSecondsLeft, 60.0, 1e-9);
	TestEqual(TEXT("the most relaxed last"), Offers[2]->OfferSecondsLeft, 120.0, 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOfferAirborneAtTest, "AirportOps.Model.Offers.Countdown.AirborneAtIsRecorded",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOfferAirborneAtTest::RunTest(const FString& Parameters)
{
	// C scores AirborneAt against AirborneBy - recorded now so C needs no migration.
	FCountdownRig Rig;
	UFlight* Flight = NewObject<UFlight>(GetTransientPackage());
	Flight->Airframe = CountdownAirframe();
	Flight->AgentId = 7;
	Flight->Phase = EFlightPhase::TaxiOut;
	Rig.Board->AddOffer(*Rig.Clock, Flight);
	Rig.Clock->Advance(3.0);
	const double At = Rig.Clock->Now();
	Rig.Board->OnAgentPhase(*Rig.Traffic, *Rig.Net, *Rig.Clock, 7, EAgentPhase::Taxiing, EAgentPhase::Departing);
	TestEqual(TEXT("the moment it departs is written down"), Flight->AirborneAt, At, 1e-9);
	return true;
}

#endif
