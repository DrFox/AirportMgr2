#include "CoreMinimal.h"
#include "Entities/EntityDefinition.h"
#include "Math/RandomStream.h"
#include "Misc/AutomationTest.h"
#include "Model/Flight.h"
#include "Model/FlightBoard.h"
#include "Model/GroundTraffic.h"
#include "Model/OpsEventBus.h"
#include "Model/RoadAgent.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Model/SimClock.h"
#include "Model/StandAllocator.h"
#include "Model/TrafficOccupancy.h"
#include "Profiles/RoadProfile.h"
#include "Testing/AirsideTestGraph.h"
#include "OpsTransitionTestHelpers.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** One stand per wingspan. Copied from StandAllocatorTest rather than shared: two small
	 *  fixtures beat a header both files then have to agree about. */
	URoadNetwork* BoardNetworkWithStands(const TArray<double>& Wingspans)
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
		double X = 0.0;
		for (const double Wingspan : Wingspans)
		{
			Net->PlaceEntity(Stand, Stand->Anchors, FVector2D(X, 0.0), 0.0, Wingspan,
				Stand->PoseRole, Stand->Trucks);
			X += 20000.0;
		}
		return Net;
	}

	UFlight* BoardFlightNeeding(double Wingspan)
	{
		UFlight* Flight = NewObject<UFlight>(GetTransientPackage());
		Flight->Airframe.Wingspan = Wingspan;
		return Flight;
	}

	/**
	 * A runway, an exit, a taxiway and stands sized for Wingspan - for the tests that DISPATCH.
	 * Since the arrival queue's clearance gate (2026-09-28) a flight is only cleared when the
	 * real plan says it could land, so a stand row with no runway never clears - correctly.
	 */
	URoadNetwork* BoardField(double Wingspan, int32 Stands = 1)
	{
		FAirframe Airframe;
		Airframe.Wingspan = Wingspan;
		FTestAirportOptions Options;
		Options.StandCount = Stands;
		return FTestAirport::Build(Airframe, Options).Net;
	}

	UFlightBoard* MakeBoard()
	{
		UFlightBoard* Board = NewObject<UFlightBoard>(GetTransientPackage());
		Board->Allocator = NewObject<UStandAllocator>(GetTransientPackage());
		return Board;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlightBoardAcceptReservesTest,
	"AirportOps.Model.FlightBoard.AcceptReservesAStand",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFlightBoardAcceptReservesTest::RunTest(const FString& Parameters)
{
	// ONE stand, two offers. The second must be refused rather than double-booked: that
	// refusal IS "the player cannot over-commit". ON A FIELD SINCE #431 - an accept is the
	// arrival plan's, and a stand row with no runway is refused before any stand is asked.
	URoadNetwork* Net = BoardField(3400.0);
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>();
	USimClock* Clock = NewObject<USimClock>();
	UFlightBoard* Board = MakeBoard();

	UFlight* First = BoardFlightNeeding(3400.0);
	UFlight* Second = BoardFlightNeeding(3400.0);
	First->LeadTimeSeconds = 100.0;
	Second->LeadTimeSeconds = 100.0;
	Board->AddOffer(*Clock, First);
	Board->AddOffer(*Clock, Second);

	TestTrue(TEXT("the first offer is accepted"), Board->Accept(*Traffic, *Net, *Clock, *First));
	TestEqual(TEXT("and becomes Accepted"), First->GetPhase(), EFlightPhase::Accepted);
	TestTrue(TEXT("holding a named stand"), First->Stand.IsSet());

	TestFalse(TEXT("the second is REFUSED rather than given the same stand"),
		Board->Accept(*Traffic, *Net, *Clock, *Second));
	TestEqual(TEXT("and is left in the inbox for the player to see"),
		Second->GetPhase(), EFlightPhase::Offered);
	TestEqual(TEXT("so one offer still stands"), Board->PendingOfferCount(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlightBoardAcceptHoldsTheReachableStandTest,
	"AirportOps.Model.FlightBoard.AcceptHoldsTheReachableStand",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFlightBoardAcceptHoldsTheReachableStandTest::RunTest(const FString& Parameters)
{
	// #431's PIN: AN UNCONNECTED CODE A STAND AND A CONNECTED CODE C ONE, and a Code A flight. Reserve took the smallest
	// stand that fits - the unconnected A - while the arrival lands on the nearest stand it can REACH, the C; a second
	// accept then took the C and this flight waited NoFreeStand until that one had gone. The hold is the plan's stand.
	FAirframe Small;
	Small.Wingspan = 1100.0;   // 11 m: Code A
	const FTestAirport Field = FTestAirport::Build(Small);
	if (!TestEqual(TEXT("one connected stand"), Field.Stands.Num(), 1)) { return false; }
	const FEntityInstanceId Connected = Field.Stands[0];

	// FAR FROM EVERY TAXIWAY, and sized for Code A - admitted by size, reached by nothing.
	UEntityDefinition* Def = UEntityDefinition::MakeStandTransient();
	const FEntityInstanceId Unconnected = Field.Net->PlaceEntity(Def, Def->Anchors, FVector2D(-300000.0, -300000.0), 0.0,
		1400.0, Def->PoseRole, Def->Trucks);
	if (!TestTrue(TEXT("the unconnected stand is placed"), Unconnected.IsSet())) { return false; }

	// CONTROL: THE OLD EVALUATOR PICKS THE UNCONNECTED ONE - so this field tells the two apart.
	{
		UGroundTraffic* Probe = NewObject<UGroundTraffic>();
		UFlight* ProbeFlight = BoardFlightNeeding(Small.Wingspan);
		ProbeFlight->Id = 99;
		if (!TestTrue(TEXT("CONTROL: Reserve holds something"), NewObject<UStandAllocator>()->Reserve(*Probe, *Field.Net, *ProbeFlight))) { return false; }
		TestEqual(TEXT("CONTROL: and it is the smaller, unconnected stand"), ProbeFlight->Stand, Unconnected);
	}

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>();
	USimClock* Clock = NewObject<USimClock>();
	UFlightBoard* Board = MakeBoard();
	UFlight* Flight = BoardFlightNeeding(Small.Wingspan);
	Flight->LeadTimeSeconds = 100.0;
	Flight->RunwayPreference = Field.Threshold;
	Board->AddOffer(*Clock, Flight);

	const FArrivalQuote Quote = Board->TryAccept(*Traffic, *Field.Net, *Clock, *Flight);
	TestTrue(FString::Printf(TEXT("accepted ('%s')"), *Quote.Sentence), Quote.IsAccepted());
	TestEqual(TEXT("holding the stand it can taxi to"), Flight->Stand, Connected);
	TestEqual(TEXT("which is the one the quote named"), Quote.Stand, Connected);
	const FEntityInstance* Far = Field.Net->GetEntity(Unconnected);
	TestTrue(TEXT("and the unreachable one is left free"), Far != nullptr && !Traffic->IsStandHeld(Far->PoseNode, 0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlightBoardDispatchesAtTheEtaTest,
	"AirportOps.Model.FlightBoard.DispatchesAtTheEta",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFlightBoardDispatchesAtTheEtaTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = BoardField(3400.0);
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>();
	USimClock* Clock = NewObject<USimClock>();
	UFlightBoard* Board = MakeBoard();

	UFlight* Flight = BoardFlightNeeding(3400.0);
	Flight->LeadTimeSeconds = 100.0;
	Board->AddOffer(*Clock, Flight);

	int32 Calls = 0;
	bool bHeldAtDispatch = true;
	Board->Dispatcher = [&](const FVector2D&, const FAirframe&)
	{
		++Calls;
		// THE HOLD MUST BE GONE BY NOW. ArrivalPlanner asks IsHeld excluding the AGENT, and
		// this hold is under the flight's negative id, so a hold still standing would refuse
		// the arrival its own stand with NoFreeStand.
		const FEntityInstance* Stand = Net->GetEntity(Flight->Stand);
		bHeldAtDispatch = Stand != nullptr && Traffic->IsStandHeld(Stand->PoseNode, 0);
		return true;
	};

	TestTrue(TEXT("accepted"), Board->Accept(*Traffic, *Net, *Clock, *Flight));
	TestEqual(TEXT("nothing is dispatched before the ETA"), Calls, 0);

	// The clock compresses: at the default night rate (480 real s for ten hours) one real
	// second is 75 game seconds, so two is past an ETA 100 game seconds out.
	Clock->Advance(2.0);
	// THE ETA PUTS IT IN THE QUEUE; the queue clears it (spec 2026-09-28-arrival-queue). Nothing
	// holds the runway, so the first tick clears it.
	Board->TickQueue(*Traffic, *Net, *Clock);

	TestEqual(TEXT("the dispatcher ran exactly once, at the ETA"), Calls, 1);
	TestFalse(TEXT("the stand hold was released BEFORE the dispatch"), bHeldAtDispatch);
	TestEqual(TEXT("and the flight is landing"), Flight->GetPhase(), EFlightPhase::Landing);

	Clock->Advance(10.0);
	TestEqual(TEXT("and it is not dispatched twice"), Calls, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlightBoardFollowsTheAgentTest,
	"AirportOps.Model.FlightBoard.FollowsTheAgentPhases",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFlightBoardFollowsTheAgentTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = BoardNetworkWithStands({3600.0});
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>();
	USimClock* Clock = NewObject<USimClock>();
	UFlightBoard* Board = MakeBoard();

	UFlight* Flight = BoardFlightNeeding(3400.0);
	Flight->AgentId = 5;
	Flight->SetPhaseForTest(EFlightPhase::Landing);
	Board->AddOffer(*Clock, Flight);

	Board->OnAgentPhase(*Net, *Clock, OpsTestTransition(5, EAgentPhase::Arriving, EAgentPhase::Taxiing, EAgentEvent::Vacated));
	TestEqual(TEXT("taxiing before the stand is TaxiIn"), Flight->GetPhase(), EFlightPhase::TaxiIn);

	// PARKED IS THE TURNAROUND ONLY AT A STAND (#405, spec 2026-09-29-ops-batch3 §4) - and this Parked names no node
	// (GoalAtEvent unset, #436), so it is at no stand. Parked at a real one is AirportOps.Model.Bus.FallbackParkStaysTaxiIn's
	// last step; this fixture has no aeroplane to put there.
	Board->OnAgentPhase(*Net, *Clock, OpsTestTransition(5, EAgentPhase::Taxiing, EAgentPhase::Parked, EAgentEvent::Parked));
	TestEqual(TEXT("parked at no stand is still the taxi in"), Flight->GetPhase(), EFlightPhase::TaxiIn);

	// THE REAL SEQUENCE NOW GOES THROUGH THE MANOEUVRE. An aeroplane is pushed off its stand
	// before it taxis out, so the board has to show that rather than jumping from Turnaround
	// to TaxiOut - and this step is also what makes the NEXT assertion mean something: with the
	// Parked above moving nothing (#405), it is Manoeuvring, not Turnaround, that the taxi reads
	// as OUT.
	Board->OnAgentPhase(*Net, *Clock, OpsTestTransition(5, EAgentPhase::Parked, EAgentPhase::Manoeuvring, EAgentEvent::DepartOrdered));
	TestEqual(TEXT("coming off the stand is the manoeuvre"),
		Flight->GetPhase(), EFlightPhase::Manoeuvring);

	// THE POINT OF THE TEST: the same agent phase, the other answer. Taxiing is the agent's
	// phase both into the stand and out of it, and only the flight's own progress tells them
	// apart - which is why EFlightPhase's declaration order is load-bearing.
	Board->OnAgentPhase(*Net, *Clock, OpsTestTransition(5, EAgentPhase::Manoeuvring, EAgentPhase::Taxiing, EAgentEvent::PushedBack));
	TestEqual(TEXT("taxiing after the turnaround is TaxiOut"), Flight->GetPhase(), EFlightPhase::TaxiOut);

	Board->OnAgentPhase(*Net, *Clock, OpsTestTransition(5, EAgentPhase::Taxiing, EAgentPhase::Departing, EAgentEvent::LinedUp));
	TestEqual(TEXT("departing"), Flight->GetPhase(), EFlightPhase::Departing);

	Board->OnAgentPhase(*Net, *Clock, OpsTestTransition(5, EAgentPhase::Departing, EAgentPhase::Gone, EAgentEvent::Gone));
	TestEqual(TEXT("gone is departed"), Flight->GetPhase(), EFlightPhase::Departed);
	TestEqual(TEXT("and the agent handle is given back"), Flight->AgentId, INDEX_NONE);

	// An agent nobody owns - a fuel truck - must move no flight at all.
	Board->OnAgentPhase(*Net, *Clock, OpsTestTransition(99, EAgentPhase::Taxiing, EAgentPhase::Parked, EAgentEvent::Parked));
	TestEqual(TEXT("a truck's phase change moves no flight"), Flight->GetPhase(), EFlightPhase::Departed);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlightBoardExpiresOffersTest,
	"AirportOps.Model.FlightBoard.ExpiresAnIgnoredOffer",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFlightBoardExpiresOffersTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = BoardNetworkWithStands({3600.0});
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>();
	USimClock* Clock = NewObject<USimClock>();
	UFlightBoard* Board = MakeBoard();

	UFlight* Offer = BoardFlightNeeding(3400.0);
	Offer->OfferSecondsLeft = 50.0;
	Board->AddOffer(*Clock, Offer);
	TestEqual(TEXT("it is in the inbox to begin with"), Board->PendingOfferCount(), 1);

	// REAL seconds now (spec 2026-09-28) - TickOffers, not Clock.Advance, drains the window.
	Board->TickOffers(*Traffic, *Net, *Clock, 51.0);

	TestEqual(TEXT("an ignored offer lapses"), Offer->GetPhase(), EFlightPhase::Expired);
	TestEqual(TEXT("and leaves the inbox"), Board->PendingOfferCount(), 0);
	return true;
}

/**
 * PR #137 REVIEW, kept through the move to a real-time countdown: a declined offer that is
 * left to sit past its own window must stay Declined, not be flipped to Expired by a
 * countdown still running on it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlightBoardDeclineCancelsExpiryTest,
	"AirportOps.Model.FlightBoard.DeclineCancelsExpiry",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFlightBoardDeclineCancelsExpiryTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = BoardNetworkWithStands({3600.0});
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>();
	USimClock* Clock = NewObject<USimClock>();
	UFlightBoard* Board = MakeBoard();

	UFlight* Offer = BoardFlightNeeding(3400.0);
	Offer->OfferSecondsLeft = 50.0;
	Board->AddOffer(*Clock, Offer);

	Board->Decline(*Clock, *Offer);
	TestEqual(TEXT("declining sets the phase at once"), Offer->GetPhase(), EFlightPhase::Declined);

	Board->TickOffers(*Traffic, *Net, *Clock, 100.0);
	TestEqual(TEXT("a declined offer stays Declined - nothing counts it down any more"),
		Offer->GetPhase(), EFlightPhase::Declined);
	TestEqual(TEXT("and it never records a lapse"), Offer->LapseReason, ELapseReason::None);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlightBoardAcceptImmediateTest,
	"AirportOps.Model.FlightBoard.AcceptImmediate",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFlightBoardAcceptImmediateTest::RunTest(const FString& Parameters)
{
	// ONE stand. AcceptImmediate has to do everything the debug land key used to do by hand:
	// make the flight, aim IT (not the board) at this call's focus, add it, accept it, and
	// say why not if it could not be.
	//
	// THE POINT OF THE TEST: a RECORDING DISPATCHER, not just reading Flight->RunwayPreference
	// back. Asserting the flight's own property proves AcceptImmediate SET it; it does not
	// prove DispatchNow or WhyNotAcceptable ever READ it rather than the board's - the exact
	// "last writer wins" bug issue #96 fixes. Only watching what actually gets dispatched
	// catches a regression back to the board field. Revert FlightBoard.cpp's DispatchNow/
	// WhyNotAcceptable to read the board's RunwayPreference again and this test goes red.
	URoadNetwork* Net = BoardField(3400.0);
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>();
	USimClock* Clock = NewObject<USimClock>();
	UFlightBoard* Board = MakeBoard();

	TArray<FVector2D> DispatchedNear;
	Board->Dispatcher = [&DispatchedNear](const FVector2D& Near, const FAirframe&)
	{
		DispatchedNear.Add(Near);
		return true;
	};

	FAirframe Airframe;
	Airframe.Wingspan = 3400.0;
	Airframe.TypeCode = FName(TEXT("A320"));
	const FVector2D Focus(500.0, 250.0);
	const FText Airline = FText::FromString(TEXT("(key 7)"));

	const EArrivalRefusal Why = Board->AcceptImmediate(*Traffic, *Net, *Clock, Airframe, Focus, Airline);
	TestEqual(TEXT("the only stand admits it"), Why, EArrivalRefusal::None);
	TestEqual(TEXT("the board's own field is never written by AcceptImmediate"),
		Board->RunwayPreference, FVector2D::ZeroVector);

	const TArray<UFlight*> Live = Board->Live();
	TestEqual(TEXT("one flight is now live"), Live.Num(), 1);
	if (Live.Num() != 1) { return false; }

	UFlight* Flight = Live[0];
	TestEqual(TEXT("it is Accepted, holding the stand"), Flight->GetPhase(), EFlightPhase::Accepted);
	TestTrue(TEXT("the airline travelled onto the flight"), Flight->AirlineName.EqualTo(Airline));
	TestEqual(TEXT("the type name comes off the airframe's own code"),
		Flight->TypeName.ToString(), Airframe.TypeCode.ToString());

	// A second call, aimed elsewhere, once the one stand is gone. THE POINT OF THE TEST: its
	// focus must not disturb the first flight's - the "last writer wins" bug this seam
	// replaces, see UFlight::RunwayPreference. The second flight is refused, so it never gets
	// scheduled and never appears in DispatchedNear below.
	const FVector2D SecondFocus(-900.0, 100.0);
	const EArrivalRefusal SecondWhy =
		Board->AcceptImmediate(*Traffic, *Net, *Clock, Airframe, SecondFocus, Airline);
	TestTrue(TEXT("the second is refused: the one stand is already held"),
		SecondWhy != EArrivalRefusal::None);
	TestEqual(TEXT("the board's own field is STILL untouched, even by the refused call"),
		Board->RunwayPreference, FVector2D::ZeroVector);

	// ArrivesAt == Clock->Now() at the accept (no lead time) - see AcceptImmediate's own
	// header on why - so the tiniest advance crosses the ETA and fires the dispatcher.
	Clock->Advance(0.1);
	Board->TickQueue(*Traffic, *Net, *Clock);
	TestEqual(TEXT("the dispatcher ran exactly once - only the accepted flight was ever due"),
		DispatchedNear.Num(), 1);
	if (DispatchedNear.Num() == 1)
	{
		// Focus, SecondFocus and ZeroVector are three distinct points, so this one equality
		// rules out all three wrong answers at once: the second call's focus, and the
		// board's own field (which stayed ZeroVector throughout, asserted above).
		TestEqual(TEXT("dispatched at the FIRST flight's own focus, not the second's or the board's"),
			DispatchedNear[0], Focus);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlightBoardDefaultRunwayPreferenceTest,
	"AirportOps.Model.FlightBoard.DefaultRunwayPreferenceIsTheLongestRunway",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFlightBoardDefaultRunwayPreferenceTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	FVector2D Focus = FVector2D(999.0, 999.0);

	TestFalse(TEXT("an airport with no runway has no default focus"),
		UFlightBoard::DefaultRunwayPreference(*Net, Focus));
	TestEqual(TEXT("and OutFocus is left untouched on refusal"), Focus, FVector2D(999.0, 999.0));

	URoadProfile* Runway = TestProfiles::Runway();

	// The SHORTER runway, placed first.
	const FRoadNodeId A0 = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId A1 = Net->AddNode(FVector2D(30000.0, 0.0));
	Net->AddStraightSegment(A0, A1, Runway);

	TestTrue(TEXT("one runway is the default"), UFlightBoard::DefaultRunwayPreference(*Net, Focus));
	TestEqual(TEXT("its own threshold"), Focus, FVector2D(0.0, 0.0));

	// A LONGER, separate runway - THE POINT OF THE TEST: the longest wins, not the first
	// found or the last added, matching AirsideCapability's own LongestRunway().
	const FRoadNodeId B0 = Net->AddNode(FVector2D(0.0, 100000.0));
	const FRoadNodeId B1 = Net->AddNode(FVector2D(60000.0, 100000.0));
	Net->AddStraightSegment(B0, B1, Runway);

	TestTrue(TEXT("the longer runway is still a default"),
		UFlightBoard::DefaultRunwayPreference(*Net, Focus));
	TestEqual(TEXT("its threshold, not the shorter runway's"), Focus, FVector2D(0.0, 100000.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlightBoardAcceptedNeverExpiresTest,
	"AirportOps.Model.FlightBoard.AnAcceptedFlightNeverLapses",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFlightBoardAcceptedNeverExpiresTest::RunTest(const FString& Parameters)
{
	// The expiry clock must stop at the accept. Otherwise a flight the player accepted early
	// would lapse on its way in, holding a stand for an aeroplane the board had written off.
	// ON A FIELD SINCE #431 - see AcceptReservesAStand.
	URoadNetwork* Net = BoardField(3400.0);
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>();
	USimClock* Clock = NewObject<USimClock>();
	UFlightBoard* Board = MakeBoard();
	Board->Dispatcher = [](const FVector2D&, const FAirframe&) { return true; };

	UFlight* Flight = BoardFlightNeeding(3400.0);
	Flight->OfferSecondsLeft = 50.0;
	Flight->LeadTimeSeconds = 100000.0;
	Board->AddOffer(*Clock, Flight);
	TestTrue(TEXT("accepted before it would have lapsed"),
		Board->Accept(*Traffic, *Net, *Clock, *Flight));

	// Well past the window: TickOffers only counts down what is still Offered.
	Board->TickOffers(*Traffic, *Net, *Clock, 100.0);
	TestEqual(TEXT("an accepted flight is not expired by its old offer deadline"),
		Flight->GetPhase(), EFlightPhase::Accepted);
	return true;
}

/**
 * ISSUE #188: Flights held every flight ever created for the whole session, so FindByAgent,
 * FindById, Offers(), Live() and every save scanned or serialised it in full forever. This
 * test measures the fix directly - it asserts the COUNT stays bounded, not merely that a
 * RollUp method exists - and goes red on a revert the same way FFlightBoardIndexMatchesThe
 * LinearScanTest below goes red on a maintained index that drifted from Flights/History.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlightBoardHistoryStaysBoundedTest,
	"AirportOps.Model.FlightBoard.HistoryStaysBoundedAcrossManySimulatedDays",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFlightBoardHistoryStaysBoundedTest::RunTest(const FString& Parameters)
{
	USimClock* Clock = NewObject<USimClock>();
	UFlightBoard* Board = MakeBoard();
	Board->MaxDays = 3;

	// ONE DECLINED FLIGHT PER SIMULATED DAY, TerminatedAt BACKDATED BY HAND: RollUp ages
	// against UFlight::TerminatedAt, and backdating it directly is what lets this test control
	// "how old" a History entry is without driving a real clock across ten days of game time.
	for (int32 Day = 0; Day < 10; ++Day)
	{
		UFlight* Flight = BoardFlightNeeding(3400.0);
		Board->AddOffer(*Clock, Flight);
		Board->Decline(*Clock, *Flight);
		Flight->TerminatedAt = Day * USimClock::SecondsPerDay;
	}
	TestEqual(TEXT("every declined flight left Flights at once"), Board->Live().Num()
		+ Board->Offers().Num(), 0);
	TestEqual(TEXT("and all ten are sitting in History before any RollUp"),
		Board->GetHistoryCountForTest(), 10);

	// "TODAY" IS DAY 9: with MaxDays == 3, the cutoff is day 6, so days 6, 7, 8 and 9 survive -
	// four entries, not ten and not zero, which are the two answers a broken cutoff would give.
	Board->RollUp(9.0 * USimClock::SecondsPerDay);
	TestEqual(TEXT("only entries within MaxDays of \"now\" survive a roll-up"),
		Board->GetHistoryCountForTest(), 4);

	// A SECOND ROLL-UP, TEN DAYS LATER: everything still held ages out, because MaxDays is a
	// WINDOW behind "now", not a one-shot amnesty at the moment a flight first qualified.
	Board->RollUp(19.0 * USimClock::SecondsPerDay);
	TestEqual(TEXT("and eventually every entry ages out, forgotten rather than accumulating"),
		Board->GetHistoryCountForTest(), 0);
	return true;
}

/**
 * ISSUE #188 ITEM 2: FindByAgent/FindById now answer from a TMap rather than a scan of
 * Flights. A map that silently drifted from the flights it claims to index would look correct
 * at every single-flight call site the rest of this file already exercises - only checking it
 * against the O(n) oracle across a MIX of every phase a flight can reach shows a difference,
 * which is the whole reason FindByAgentLinearForTest/FindByIdLinearForTest exist at all.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlightBoardIndexMatchesTheLinearScanTest,
	"AirportOps.Model.FlightBoard.MaintainedIndexMatchesTheLinearScan",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFlightBoardIndexMatchesTheLinearScanTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>();
	USimClock* Clock = NewObject<USimClock>();
	UFlightBoard* Board = MakeBoard();

	// A FIXED SEED: deterministic and reproducible if this ever goes red, not a flaky roll of
	// the dice on every run.
	FRandomStream Rng(188);
	int32 NextTestAgentId = 1;
	int32 CreatedCount = 0;

	for (int32 I = 0; I < 40; ++I)
	{
		UFlight* Flight = BoardFlightNeeding(3400.0);
		switch (Rng.RandRange(0, 3))
		{
		case 0:
			// Stays Offered.
			Board->AddOffer(*Clock, Flight);
			break;
		case 1:
			// Declined - retired into History by MoveToHistory.
			Board->AddOffer(*Clock, Flight);
			Board->Decline(*Clock, *Flight);
			break;
		case 2:
			// LIVE, mid-flight, holding a real agent id - the FFlightBoardFollowsTheAgentTest
			// shape (AgentId set before AddOffer, not through DispatchNow), exercised at
			// volume instead of once.
			Flight->AgentId = NextTestAgentId++;
			Flight->SetPhaseForTest(EFlightPhase::Landing);
			Board->AddOffer(*Clock, Flight);
			break;
		default:
			{
				// DEPARTED - held a real agent id, then gave it back and moved to History.
				const int32 AgentId = NextTestAgentId++;
				Flight->AgentId = AgentId;
				Flight->SetPhaseForTest(EFlightPhase::Departing);
				Board->AddOffer(*Clock, Flight);
				Board->OnAgentPhase(*Net, *Clock,
					OpsTestTransition(AgentId, EAgentPhase::Departing, EAgentPhase::Gone, EAgentEvent::Gone));
			}
			break;
		}
		++CreatedCount;
	}

	// A BATCH OF LAPSING OFFERS, resolved in one TickOffers rather than one at a time: the
	// production path that lapses offers walks a snapshot of Flights and moves some of it to
	// History - the shape issue #188's load sweep had too, before it went (2026-09-30).
	for (int32 I = 0; I < 10; ++I)
	{
		UFlight* Flight = BoardFlightNeeding(3400.0);
		Flight->OfferSecondsLeft = 0.5;
		Board->AddOffer(*Clock, Flight);
		++CreatedCount;
	}
	Board->TickOffers(*Traffic, *Net, *Clock, 1.0);

	bool bAllAgentsAgree = true;
	for (int32 AgentId = INDEX_NONE; AgentId <= NextTestAgentId; ++AgentId)
	{
		if (Board->FindByAgentForTest(AgentId) != Board->FindByAgentLinearForTest(AgentId))
		{
			bAllAgentsAgree = false;
			AddError(FString::Printf(
				TEXT("FindByAgent(%d) disagrees with the linear scan"), AgentId));
		}
	}
	TestTrue(TEXT("the maintained ByAgent index agrees with a full scan of Flights for every "
		"agent id this fixture touched, and a few it never assigned"), bAllAgentsAgree);

	bool bAllIdsAgree = true;
	for (int32 Id = 1; Id <= CreatedCount + 2; ++Id)
	{
		if (Board->FindByIdForTest(Id) != Board->FindByIdLinearForTest(Id))
		{
			bAllIdsAgree = false;
			AddError(FString::Printf(TEXT("FindById(%d) disagrees with the linear scan"), Id));
		}
	}
	TestTrue(TEXT("the maintained ById index agrees with a full scan of Flights and History "
		"for every id this fixture ever handed out, and a couple past the end"), bAllIdsAgree);
	return true;
}

// ============================================================================================================================
// THE ONE WRITER OF A FLIGHT'S PHASE (#442): UFlightBoard::TransitionTo and its rows, each measured through the door that reaches
// it. A row that is unwired - an effect the row owns but its door stopped getting - fails the test named for it.
// ============================================================================================================================
namespace
{
	FAirframe TransitionAirframe()
	{
		FAirframe Out;
		Out.Wingspan = 1200.0;
		Out.TurnaroundSeconds = 1800.0;
		return Out;
	}

	/**
	 * A board on a field an arrival can use, with a bus the board publishes onto and a recorder for every event a row publishes.
	 * 1 game s per real s, so a lead time is a clock Advance. Named and prefixed for the unity build: a second "FQueueRig" from
	 * another test file would collide with ArrivalQueueTest's.
	 */
	struct FTransitionRig
	{
		FTestAirport Airport;
		UGroundTraffic* Traffic = nullptr;
		USimClock* Clock = nullptr;
		UFlightBoard* Board = nullptr;
		FOpsEventBus Bus;
		int32 Dispatched = 0;
		TArray<FOfferAcceptedEvent> Accepted;
		TArray<FFlightInboundEvent> Inbound;
		TArray<FOfferDeclinedEvent> Declined;
		TArray<FOfferExpiredEvent> Expired;
		TArray<FFlightCancelledEvent> Cancelled;
		TArray<FFlightAirborneEvent> Airborne;

		explicit FTransitionRig(int32 Stands = 4)
		{
			FTestAirportOptions Options;
			Options.StandCount = Stands;
			Airport = FTestAirport::Build(TransitionAirframe(), Options);
			Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
			Clock = NewObject<USimClock>(GetTransientPackage());
			Clock->SetUniformDay(USimClock::SecondsPerDay);
			Board = NewObject<UFlightBoard>(GetTransientPackage());
			Board->Allocator = NewObject<UStandAllocator>(GetTransientPackage());
			Board->Dispatcher = [this](const FVector2D&, const FAirframe&) { ++Dispatched; return true; };
			Board->Bus = &Bus;
			Bus.BeginWiring();
			Bus.Subscribe<FOfferAcceptedEvent>(EOpsTier::Sim, TEXT("test"), [this](const FOfferAcceptedEvent& E) { Accepted.Add(E); });
			Bus.Subscribe<FFlightInboundEvent>(EOpsTier::Sim, TEXT("test"), [this](const FFlightInboundEvent& E) { Inbound.Add(E); });
			Bus.Subscribe<FOfferDeclinedEvent>(EOpsTier::Sim, TEXT("test"), [this](const FOfferDeclinedEvent& E) { Declined.Add(E); });
			Bus.Subscribe<FOfferExpiredEvent>(EOpsTier::Sim, TEXT("test"), [this](const FOfferExpiredEvent& E) { Expired.Add(E); });
			Bus.Subscribe<FFlightCancelledEvent>(EOpsTier::Sim, TEXT("test"), [this](const FFlightCancelledEvent& E) { Cancelled.Add(E); });
			Bus.Subscribe<FFlightAirborneEvent>(EOpsTier::Sim, TEXT("test"), [this](const FFlightAirborneEvent& E) { Airborne.Add(E); });
			Bus.EndWiring();
		}

		UFlight* Offer(double Lead = 10.0)
		{
			UFlight* Flight = NewObject<UFlight>(GetTransientPackage());
			Flight->Airframe = TransitionAirframe();
			Flight->AirlineId = TEXT("Cumbria");
			Flight->Callsign = TEXT("CU 204");
			Flight->OfferWindowSeconds = 60.0;
			Flight->OfferSecondsLeft = 60.0;
			Flight->LeadTimeSeconds = Lead;
			Flight->RunwayPreference = Airport.Threshold;
			Board->AddOffer(*Clock, Flight);
			return Flight;
		}

		UFlight* Accept(double Lead = 10.0)
		{
			UFlight* Flight = Offer(Lead);
			return Board->Accept(*Traffic, *Airport.Net, *Clock, *Flight) ? Flight : nullptr;
		}

		/** An accepted flight whose ETA has come and whose runway is held, so it waits in the queue with its stand. */
		UFlight* Holding()
		{
			HoldRunway();
			UFlight* Flight = Accept(1.0);
			Clock->Advance(2.0);
			return Flight;
		}

		void HoldRunway()
		{
			for (const FTrafficResource& Surface : Airport.Net->RunwaySurfaces(Airport.ThresholdSegment))
			{
				FTrafficClaim Claim;
				Claim.AgentId = 99;
				Claim.Resource = Surface;
				Claim.bOccupied = true;
				FTrafficClaim Blocker;
				Traffic->OccupancyForTest().TryClaim(Claim, Blocker);
			}
		}

		void FreeRunway() { Traffic->OccupancyForTest().ReleaseAll(99); }

		bool StandHeldFor(const UFlight& Flight) const
		{
			const FEntityInstance* Stand = Airport.Net->GetEntity(Flight.Stand);
			return Stand != nullptr && Traffic->IsStandHeld(Stand->PoseNode, 0);
		}

		void Tick() { Board->TickQueue(*Traffic, *Airport.Net, *Clock); }
		void Drain() { Bus.Drain(); }
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlightBoardTransitionTableTest,
	"AirportOps.Model.FlightBoard.TransitionTable",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFlightBoardTransitionTableTest::RunTest(const FString& Parameters)
{
	// EVERY ROW WHOSE PHASE IS ENTERED THROUGH A DOOR ON THE BOARD, in the order a flight meets them. Each asserts what the row
	// owns - the pending-offer count, the publish, History, the aeroplane's hook, the revision - through the public door, so an
	// effect dropped from TransitionTo (or a door that stopped reaching it) reads red here rather than in a PIE session.
	FTransitionRig R;
	R.Clock->Advance(5.0);   // a clock that is not at zero, so a TerminatedAt stamp is a measurement and not the default

	// -- Declined: the count, FOfferDeclinedEvent, History, the revision.
	UFlight* Declined = R.Offer();
	if (!TestEqual(TEXT("an offer is pending"), R.Board->PendingOfferCount(), 1)) { return false; }
	const uint32 RevisionBeforeDecline = R.Board->Revision();
	R.Board->Decline(*R.Clock, *Declined);
	R.Drain();
	TestEqual(TEXT("Declined: the phase"), Declined->GetPhase(), EFlightPhase::Declined);
	TestEqual(TEXT("Declined: no longer pending"), R.Board->PendingOfferCount(), 0);
	TestEqual(TEXT("Declined: published once"), R.Declined.Num(), 1);
	TestTrue(TEXT("Declined: not an offer, not live, still found by id"),
		!R.Board->Offers().Contains(Declined) && !R.Board->Live().Contains(Declined) && R.Board->FindByIdForTest(Declined->Id) == Declined);
	TestEqual(TEXT("Declined: filed in History"), R.Board->GetHistoryCountForTest(), 1);
	TestEqual(TEXT("Declined: stamped terminal at the clock"), Declined->TerminatedAt, R.Clock->Now(), 1e-9);
	TestTrue(TEXT("Declined: the revision moved"), R.Board->Revision() != RevisionBeforeDecline);

	// -- Expired: the count, FOfferExpiredEvent carrying the lapse reason, History.
	UFlight* Lapsing = R.Offer();
	Lapsing->OfferSecondsLeft = 1.0;
	R.Board->TickOffers(*R.Traffic, *R.Airport.Net, *R.Clock, 2.0);
	R.Drain();
	TestEqual(TEXT("Expired: the phase"), Lapsing->GetPhase(), EFlightPhase::Expired);
	TestEqual(TEXT("Expired: no longer pending"), R.Board->PendingOfferCount(), 0);
	if (!TestEqual(TEXT("Expired: published once"), R.Expired.Num(), 1)) { return false; }
	TestEqual(TEXT("Expired: with the reason the flight carries"), R.Expired[0].Reason, Lapsing->LapseReason);
	TestTrue(TEXT("Expired: not live, still found by id"), !R.Board->Live().Contains(Lapsing) && R.Board->FindByIdForTest(Lapsing->Id) == Lapsing);
	TestEqual(TEXT("Expired: filed in History"), R.Board->GetHistoryCountForTest(), 2);

	// -- Withdrawn: the count and History, and NOTHING published (no OfferExpired - no Ignored penalty; not a cancellation either).
	UFlight* Withdrawn = R.Offer();
	R.Board->CancelUnarrived(*R.Traffic, *R.Clock, ECancelReason::NoRunway);
	R.Drain();
	TestEqual(TEXT("Withdrawn: the phase"), Withdrawn->GetPhase(), EFlightPhase::Withdrawn);
	TestEqual(TEXT("Withdrawn: no longer pending"), R.Board->PendingOfferCount(), 0);
	TestTrue(TEXT("Withdrawn: published nothing - not an expiry, not a cancellation"), R.Expired.Num() == 1 && R.Cancelled.Num() == 0);
	TestEqual(TEXT("Withdrawn: filed in History"), R.Board->GetHistoryCountForTest(), 3);

	// -- Accepted: the count, the arrival armed, FOfferAcceptedEvent.
	UFlight* Flight = R.Offer(/*Lead=*/50.0);
	TestEqual(TEXT("a fresh offer is pending"), R.Board->PendingOfferCount(), 1);
	R.HoldRunway();   // so the queue pass below does not dispatch it the moment it joins
	if (!TestTrue(TEXT("accepted"), R.Board->Accept(*R.Traffic, *R.Airport.Net, *R.Clock, *Flight))) { return false; }
	R.Drain();
	TestEqual(TEXT("Accepted: the phase"), Flight->GetPhase(), EFlightPhase::Accepted);
	TestEqual(TEXT("Accepted: no longer pending"), R.Board->PendingOfferCount(), 0);
	if (!TestEqual(TEXT("Accepted: published once"), R.Accepted.Num(), 1)) { return false; }
	TestTrue(TEXT("Accepted: naming the stand it holds"), R.Accepted[0].Stand == Flight->Stand && R.StandHeldFor(*Flight));
	TestEqual(TEXT("Accepted: still to arrive"), R.Board->UnarrivedCount(), 1);
	TestEqual(TEXT("Accepted: the arrival is armed on the clock - one entry waiting"), R.Clock->PendingForTest(), 1);

	// -- Inbound: the ETA callback's row - HoldingSince is the ETA, FFlightInboundEvent, the stand KEPT.
	R.Clock->Advance(60.0);
	R.Drain();
	TestEqual(TEXT("Inbound: the ETA armed by the Accepted row put it in the queue"), Flight->GetPhase(), EFlightPhase::Inbound);
	TestEqual(TEXT("Inbound: HoldingSince is the ETA, not the moment the callback ran"), Flight->HoldingSince, Flight->ArrivesAt, 1e-9);
	TestEqual(TEXT("Inbound: published once"), R.Inbound.Num(), 1);
	TestEqual(TEXT("Inbound: the arrival fired, so nothing is waiting on the clock"), R.Clock->PendingForTest(), 0);
	TestTrue(TEXT("Inbound: and the stand is still held"), R.StandHeldFor(*Flight));

	// -- Landing: the aeroplane hooked under the flight, the stand released BEFORE the dispatch.
	R.FreeRunway();
	R.Tick();
	TestEqual(TEXT("Landing: dispatched once"), R.Dispatched, 1);
	TestEqual(TEXT("Landing: the phase"), Flight->GetPhase(), EFlightPhase::Landing);
	TestTrue(TEXT("Landing: the aeroplane is hooked - the board finds the flight by its agent"),
		Flight->AgentId != INDEX_NONE && R.Board->FlightForAgent(Flight->AgentId) == Flight);
	TestFalse(TEXT("Landing: the stand hold was released for the planner"), R.StandHeldFor(*Flight));
	const int32 Agent = Flight->AgentId;

	// -- The ground rows follow the agent (OnAgentPhase decides which, TransitionTo applies): Departing stamps and publishes once.
	R.Board->OnAgentPhase(*R.Airport.Net, *R.Clock, OpsTestTransition(Agent, EAgentPhase::Arriving, EAgentPhase::Taxiing, EAgentEvent::Vacated));
	TestEqual(TEXT("TaxiIn"), Flight->GetPhase(), EFlightPhase::TaxiIn);
	R.Board->OnAgentPhase(*R.Airport.Net, *R.Clock, OpsTestTransition(Agent, EAgentPhase::Parked, EAgentPhase::Manoeuvring, EAgentEvent::DepartOrdered));
	R.Board->OnAgentPhase(*R.Airport.Net, *R.Clock, OpsTestTransition(Agent, EAgentPhase::Manoeuvring, EAgentPhase::Taxiing, EAgentEvent::PushedBack));
	TestEqual(TEXT("TaxiOut"), Flight->GetPhase(), EFlightPhase::TaxiOut);
	TestEqual(TEXT("nothing is airborne yet"), Flight->AirborneAt, 0.0, 1e-9);
	R.Board->OnAgentPhase(*R.Airport.Net, *R.Clock, OpsTestTransition(Agent, EAgentPhase::Taxiing, EAgentPhase::Departing, EAgentEvent::LinedUp));
	R.Drain();
	TestEqual(TEXT("Departing: the phase"), Flight->GetPhase(), EFlightPhase::Departing);
	TestEqual(TEXT("Departing: AirborneAt is the clock"), Flight->AirborneAt, R.Clock->Now(), 1e-9);
	TestEqual(TEXT("Departing: published once"), R.Airborne.Num(), 1);

	// -- Departed: History, and the aeroplane let go of. Nothing published for the departure itself.
	R.Board->OnAgentPhase(*R.Airport.Net, *R.Clock, OpsTestTransition(Agent, EAgentPhase::Departing, EAgentPhase::Gone, EAgentEvent::Gone));
	R.Drain();
	TestEqual(TEXT("Departed: the phase"), Flight->GetPhase(), EFlightPhase::Departed);
	TestTrue(TEXT("Departed: not live, still found by id"), !R.Board->Live().Contains(Flight) && R.Board->FindByIdForTest(Flight->Id) == Flight);
	TestEqual(TEXT("Departed: filed in History"), R.Board->GetHistoryCountForTest(), 4);
	TestTrue(TEXT("Departed: the aeroplane is let go of - no agent, and the board finds no flight by it"),
		Flight->AgentId == INDEX_NONE && R.Board->FlightForAgent(Agent) == nullptr);
	TestEqual(TEXT("Departed: and no second FlightAirborne"), R.Airborne.Num(), 1);

	// -- Moving a flight to the phase it is in is not a transition: a terminal flight is not filed twice, nothing publishes again.
	R.Board->OnAgentPhase(*R.Airport.Net, *R.Clock, OpsTestTransition(Agent, EAgentPhase::Departing, EAgentPhase::Gone, EAgentEvent::Gone));
	TestEqual(TEXT("an event for a finished flight moves nothing"), Flight->GetPhase(), EFlightPhase::Departed);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlightBoardCancelledRowTest,
	"AirportOps.Model.FlightBoard.CancelledRowIsOneRowForEveryDoor",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFlightBoardCancelledRowTest::RunTest(const FString& Parameters)
{
	// THE THREE "CANCELLED" WRITERS DID THREE DIFFERENT THINGS (the issue): CancelUnarrived disarmed, released and published;
	// CancelByAgent unhooked and published; the load's cancel did neither, correct only for where LoadFromSlot calls it. Now it is
	// ONE ROW, and each door differs from the others in exactly what it should: the closure and the player publish with their own
	// reason, the agent's despawn publishes Unstuck and unhooks, the load publishes NOTHING. What every door shares is asserted
	// for every door: History, the stand released, the arrival disarmed.
	enum class EDoor { Closure, Player, Load };
	enum class EFrom { Accepted, Inbound };
	struct FCase
	{
		const TCHAR* Name;
		EDoor Door;
		EFrom From;
	};
	const FCase Cases[] = {
		{ TEXT("closure of an accepted flight"),  EDoor::Closure, EFrom::Accepted },
		{ TEXT("closure of a holding flight"),    EDoor::Closure, EFrom::Inbound },
		{ TEXT("player cancel of an accepted flight"), EDoor::Player, EFrom::Accepted },
		{ TEXT("player cancel of a holding flight"),   EDoor::Player, EFrom::Inbound },
		{ TEXT("load of an accepted flight"),     EDoor::Load, EFrom::Accepted },
		{ TEXT("load of a holding flight"),       EDoor::Load, EFrom::Inbound },
	};
	for (const FCase& Case : Cases)
	{
		const FString Label = Case.Name;
		FTransitionRig R;
		UFlight* Flight = Case.From == EFrom::Inbound ? R.Holding() : R.Accept(/*Lead=*/1000.0);
		if (!TestNotNull(*(Label + TEXT(": a flight to cancel")), Flight)) { return false; }
		if (!TestEqual(*(Label + TEXT(": in the phase the case starts from")), Flight->GetPhase(),
			Case.From == EFrom::Inbound ? EFlightPhase::Inbound : EFlightPhase::Accepted)) { return false; }
		if (!TestTrue(*(Label + TEXT(": holding a stand")), R.StandHeldFor(*Flight))) { return false; }
		R.Drain();
		R.Cancelled.Reset();
		// AN ACCEPTED FLIGHT HAS ITS ARRIVAL ARMED, a holding one has none left (it fired): the cancel must take the one there is.
		const int32 PendingBefore = R.Clock->PendingForTest();
		if (!TestEqual(*(Label + TEXT(": the arrival is armed exactly when it has not fired")), PendingBefore,
			Case.From == EFrom::Accepted ? 1 : 0)) { return false; }

		switch (Case.Door)
		{
		case EDoor::Closure:
			TestEqual(*(Label + TEXT(": the closure cancels it")), R.Board->CancelUnarrived(*R.Traffic, *R.Clock, ECancelReason::AirportClosed), 1);
			break;
		case EDoor::Player:
			TestTrue(*(Label + TEXT(": the player's cancel is taken")), R.Board->CancelByPlayer(*R.Traffic, *R.Clock, Flight->Id));
			break;
		case EDoor::Load:
			TestEqual(*(Label + TEXT(": the load's cancel takes it")),
				R.Board->CancelUnarrivedAtLoad(R.Clock->Now(), R.Traffic, R.Clock), 1);
			break;
		}
		R.Drain();

		TestEqual(*(Label + TEXT(": the phase")), Flight->GetPhase(), EFlightPhase::Cancelled);
		TestTrue(*(Label + TEXT(": not live, still found by id")), !R.Board->Live().Contains(Flight) && R.Board->FindByIdForTest(Flight->Id) == Flight);
		TestEqual(*(Label + TEXT(": filed in History")), R.Board->GetHistoryCountForTest(), 1);
		TestFalse(*(Label + TEXT(": its stand is released")), R.StandHeldFor(*Flight));
		TestEqual(*(Label + TEXT(": and nothing is waiting on the queue")), R.Board->Queue().Num(), 0);
		TestEqual(*(Label + TEXT(": its arrival is DISARMED - the clock holds nothing for it")), R.Clock->PendingForTest(), 0);

		// THE ARRIVAL DISARMED: its ETA passes and it is STILL Cancelled, not put back in the queue.
		R.Clock->Advance(2000.0);
		R.Drain();
		TestEqual(*(Label + TEXT(": still cancelled after its ETA")), Flight->GetPhase(), EFlightPhase::Cancelled);
		TestEqual(*(Label + TEXT(": and never queued")), R.Inbound.Num(), Case.From == EFrom::Inbound ? 1 : 0);

		// WHAT DIFFERS BETWEEN THE DOORS: the publish.
		switch (Case.Door)
		{
		case EDoor::Closure:
			if (TestEqual(*(Label + TEXT(": published once")), R.Cancelled.Num(), 1))
			{
				TestEqual(*(Label + TEXT(": with the closure's reason")), R.Cancelled[0].Reason, ECancelReason::AirportClosed);
			}
			break;
		case EDoor::Player:
			if (TestEqual(*(Label + TEXT(": published once")), R.Cancelled.Num(), 1))
			{
				TestEqual(*(Label + TEXT(": with the player's reason")), R.Cancelled[0].Reason, ECancelReason::PlayerCancelled);
			}
			break;
		case EDoor::Load:
			TestEqual(*(Label + TEXT(": a load's cancel is unscored, so NOTHING is published")), R.Cancelled.Num(), 0);
			break;
		}
	}

	// THE AGENT'S DOOR, from the ground: CancelByAgent unhooks the aeroplane and publishes Unstuck, and holds no stand to release.
	{
		FTransitionRig R;
		UFlight* Flight = R.Accept(/*Lead=*/1.0);
		R.Clock->Advance(2.0);
		R.Tick();
		if (!TestEqual(TEXT("by agent: the flight is landing"), Flight->GetPhase(), EFlightPhase::Landing)) { return false; }
		const int32 Agent = Flight->AgentId;
		R.Drain();
		R.Cancelled.Reset();
		TestTrue(TEXT("by agent: the despawn cancels the flight"), R.Board->CancelByAgent(Agent, R.Clock->Now()));
		R.Drain();
		TestEqual(TEXT("by agent: the phase"), Flight->GetPhase(), EFlightPhase::Cancelled);
		TestTrue(TEXT("by agent: the aeroplane is let go of"), Flight->AgentId == INDEX_NONE && R.Board->FlightForAgent(Agent) == nullptr);
		if (TestEqual(TEXT("by agent: published once"), R.Cancelled.Num(), 1))
		{
			TestEqual(TEXT("by agent: as Unstuck"), R.Cancelled[0].Reason, ECancelReason::Unstuck);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlightBoardPlayerCancelRefusalsTest,
	"AirportOps.Model.FlightBoard.PlayerCancelRefusesWhatHasNotToCancel",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFlightBoardPlayerCancelRefusalsTest::RunTest(const FString& Parameters)
{
	// THE PLAYER'S CANCEL IS FOR A FLIGHT STILL TO ARRIVE (#442): an offer is declined, not cancelled; a flight on the runway or the
	// ground is committed and is the aircraft card's; History is over. Each refusal changes nothing and publishes nothing.
	FTransitionRig R;
	R.Drain();
	UFlight* Offered = R.Offer();
	UFlight* Landing = R.Accept(1.0);
	R.Clock->Advance(2.0);
	R.Tick();
	UFlight* Declined = R.Offer();
	R.Board->Decline(*R.Clock, *Declined);
	if (!TestEqual(TEXT("a flight is landing"), Landing->GetPhase(), EFlightPhase::Landing)) { return false; }
	R.Drain();
	R.Cancelled.Reset();
	const uint32 Revision = R.Board->Revision();

	TestFalse(TEXT("an open offer is declined, not cancelled"), R.Board->CancelByPlayer(*R.Traffic, *R.Clock, Offered->Id));
	TestFalse(TEXT("a landing flight is committed to the runway"), R.Board->CancelByPlayer(*R.Traffic, *R.Clock, Landing->Id));
	TestFalse(TEXT("a flight already in History is over"), R.Board->CancelByPlayer(*R.Traffic, *R.Clock, Declined->Id));
	TestFalse(TEXT("an id nothing has"), R.Board->CancelByPlayer(*R.Traffic, *R.Clock, 9999));
	R.Drain();
	TestEqual(TEXT("the offer is still an offer"), Offered->GetPhase(), EFlightPhase::Offered);
	TestEqual(TEXT("the landing flight is still landing"), Landing->GetPhase(), EFlightPhase::Landing);
	TestEqual(TEXT("nothing was published"), R.Cancelled.Num(), 0);
	TestEqual(TEXT("and the board did not move"), R.Board->Revision(), Revision);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlightBoardLoadCancelPositionTest,
	"AirportOps.Model.FlightSave.LoadCancelDoesNotDependOnItsPosition",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFlightBoardLoadCancelPositionTest::RunTest(const FString& Parameters)
{
	// THE LOAD'S CANCEL WAS "CORRECT ONLY BECAUSE OF WHERE IT SITS IN LoadFromSlot" (the issue): it skipped the release and the
	// disarm because, called first, there was nothing yet to release or disarm. Called AFTER the holds were re-made and the
	// arrivals re-armed - the order a refactor of the load could put it in - it left a stand held and an arrival armed for a
	// flight that can never land. It goes through the Cancelled row now, which does both when it is handed the means.
	FTransitionRig R;
	UFlight* Accepted = R.Accept(/*Lead=*/1000.0);
	if (!TestNotNull(TEXT("an accepted flight"), Accepted)) { return false; }

	// THE WRONG ORDER ON PURPOSE: holds re-made (OnGraphRebuilt), arrivals re-armed (RearmSchedules), THEN the cancel.
	R.Board->OnGraphRebuilt(*R.Traffic, *R.Airport.Net);
	R.Board->RearmSchedules(*R.Traffic, *R.Airport.Net, *R.Clock);
	if (!TestTrue(TEXT("PRECONDITION: its stand is held, as the load's third step leaves it"), R.StandHeldFor(*Accepted))) { return false; }
	if (!TestEqual(TEXT("PRECONDITION: and its arrival is re-armed, as the load's fourth step leaves it"), R.Clock->PendingForTest(), 1)) { return false; }
	R.Drain();
	R.Cancelled.Reset();

	TestEqual(TEXT("the cancel takes it"), R.Board->CancelUnarrivedAtLoad(R.Clock->Now(), R.Traffic, R.Clock), 1);
	R.Drain();
	TestEqual(TEXT("cancelled"), Accepted->GetPhase(), EFlightPhase::Cancelled);
	TestFalse(TEXT("its stand is released even though the holds were already made"), R.StandHeldFor(*Accepted));
	TestEqual(TEXT("and its RE-ARMED arrival is disarmed - the clock holds nothing for it"), R.Clock->PendingForTest(), 0);
	R.Clock->Advance(2000.0);
	R.Drain();
	TestEqual(TEXT("and its re-armed arrival was disarmed: still cancelled after its ETA"), Accepted->GetPhase(), EFlightPhase::Cancelled);
	TestEqual(TEXT("unscored: nothing published"), R.Cancelled.Num(), 0);
	return true;
}


#endif
