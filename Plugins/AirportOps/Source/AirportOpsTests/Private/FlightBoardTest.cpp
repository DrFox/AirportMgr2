#include "CoreMinimal.h"
#include "Entities/EntityDefinition.h"
#include "Math/RandomStream.h"
#include "Misc/AutomationTest.h"
#include "Model/Flight.h"
#include "Model/FlightBoard.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadAgent.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Model/SimClock.h"
#include "Model/StandAllocator.h"
#include "Profiles/RoadProfile.h"
#include "Testing/AirsideTestGraph.h"

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
	// refusal IS "the player cannot over-commit".
	URoadNetwork* Net = BoardNetworkWithStands({3600.0});
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
	TestEqual(TEXT("and becomes Accepted"), First->Phase, EFlightPhase::Accepted);
	TestTrue(TEXT("holding a named stand"), First->Stand.IsSet());

	TestFalse(TEXT("the second is REFUSED rather than given the same stand"),
		Board->Accept(*Traffic, *Net, *Clock, *Second));
	TestEqual(TEXT("and is left in the inbox for the player to see"),
		Second->Phase, EFlightPhase::Offered);
	TestEqual(TEXT("so one offer still stands"), Board->PendingOfferCount(), 1);
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
	TestEqual(TEXT("and the flight is landing"), Flight->Phase, EFlightPhase::Landing);

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
	Flight->Phase = EFlightPhase::Landing;
	Board->AddOffer(*Clock, Flight);

	Board->OnAgentPhase(*Traffic, *Net, *Clock, 5, EAgentPhase::Arriving, EAgentPhase::Taxiing);
	TestEqual(TEXT("taxiing before the stand is TaxiIn"), Flight->Phase, EFlightPhase::TaxiIn);

	// PARKED IS THE TURNAROUND ONLY AT A STAND (#405, spec 2026-09-29-ops-batch3 §4) - and agent 5 is no agent this
	// traffic model has, so it is at no stand. Parked at a real one is AirportOps.Model.Bus.FallbackParkStaysTaxiIn's
	// last step; this fixture has no aeroplane to put there.
	Board->OnAgentPhase(*Traffic, *Net, *Clock, 5, EAgentPhase::Taxiing, EAgentPhase::Parked);
	TestEqual(TEXT("parked at no stand is still the taxi in"), Flight->Phase, EFlightPhase::TaxiIn);

	// THE REAL SEQUENCE NOW GOES THROUGH THE MANOEUVRE. An aeroplane is pushed off its stand
	// before it taxis out, so the board has to show that rather than jumping from Turnaround
	// to TaxiOut - and this step is also what makes the NEXT assertion mean something: with the
	// Parked above moving nothing (#405), it is Manoeuvring, not Turnaround, that the taxi reads
	// as OUT.
	Board->OnAgentPhase(*Traffic, *Net, *Clock, 5, EAgentPhase::Parked, EAgentPhase::Manoeuvring);
	TestEqual(TEXT("coming off the stand is the manoeuvre"),
		Flight->Phase, EFlightPhase::Manoeuvring);

	// THE POINT OF THE TEST: the same agent phase, the other answer. Taxiing is the agent's
	// phase both into the stand and out of it, and only the flight's own progress tells them
	// apart - which is why EFlightPhase's declaration order is load-bearing.
	Board->OnAgentPhase(*Traffic, *Net, *Clock, 5, EAgentPhase::Manoeuvring, EAgentPhase::Taxiing);
	TestEqual(TEXT("taxiing after the turnaround is TaxiOut"), Flight->Phase, EFlightPhase::TaxiOut);

	Board->OnAgentPhase(*Traffic, *Net, *Clock, 5, EAgentPhase::Taxiing, EAgentPhase::Departing);
	TestEqual(TEXT("departing"), Flight->Phase, EFlightPhase::Departing);

	Board->OnAgentPhase(*Traffic, *Net, *Clock, 5, EAgentPhase::Departing, EAgentPhase::Gone);
	TestEqual(TEXT("gone is departed"), Flight->Phase, EFlightPhase::Departed);
	TestEqual(TEXT("and the agent handle is given back"), Flight->AgentId, INDEX_NONE);

	// An agent nobody owns - a fuel truck - must move no flight at all.
	Board->OnAgentPhase(*Traffic, *Net, *Clock, 99, EAgentPhase::Taxiing, EAgentPhase::Parked);
	TestEqual(TEXT("a truck's phase change moves no flight"), Flight->Phase, EFlightPhase::Departed);
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

	TestEqual(TEXT("an ignored offer lapses"), Offer->Phase, EFlightPhase::Expired);
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
	TestEqual(TEXT("declining sets the phase at once"), Offer->Phase, EFlightPhase::Declined);

	Board->TickOffers(*Traffic, *Net, *Clock, 100.0);
	TestEqual(TEXT("a declined offer stays Declined - nothing counts it down any more"),
		Offer->Phase, EFlightPhase::Declined);
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
	// THE POINT OF THE TEST: a RECORDING DISPATCHER, not just reading Flight->ApproachFocus
	// back. Asserting the flight's own property proves AcceptImmediate SET it; it does not
	// prove DispatchNow or WhyNotAcceptable ever READ it rather than the board's - the exact
	// "last writer wins" bug issue #96 fixes. Only watching what actually gets dispatched
	// catches a regression back to the board field. Revert FlightBoard.cpp's DispatchNow/
	// WhyNotAcceptable to read the board's ApproachFocus again and this test goes red.
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
		Board->ApproachFocus, FVector2D::ZeroVector);

	const TArray<UFlight*> Live = Board->Live();
	TestEqual(TEXT("one flight is now live"), Live.Num(), 1);
	if (Live.Num() != 1) { return false; }

	UFlight* Flight = Live[0];
	TestEqual(TEXT("it is Accepted, holding the stand"), Flight->Phase, EFlightPhase::Accepted);
	TestTrue(TEXT("the airline travelled onto the flight"), Flight->AirlineName.EqualTo(Airline));
	TestEqual(TEXT("the type name comes off the airframe's own code"),
		Flight->TypeName.ToString(), Airframe.TypeCode.ToString());

	// A second call, aimed elsewhere, once the one stand is gone. THE POINT OF THE TEST: its
	// focus must not disturb the first flight's - the "last writer wins" bug this seam
	// replaces, see UFlight::ApproachFocus. The second flight is refused, so it never gets
	// scheduled and never appears in DispatchedNear below.
	const FVector2D SecondFocus(-900.0, 100.0);
	const EArrivalRefusal SecondWhy =
		Board->AcceptImmediate(*Traffic, *Net, *Clock, Airframe, SecondFocus, Airline);
	TestTrue(TEXT("the second is refused: the one stand is already held"),
		SecondWhy != EArrivalRefusal::None);
	TestEqual(TEXT("the board's own field is STILL untouched, even by the refused call"),
		Board->ApproachFocus, FVector2D::ZeroVector);

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
	FFlightBoardDefaultApproachFocusTest,
	"AirportOps.Model.FlightBoard.DefaultApproachFocusIsTheLongestRunway",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFlightBoardDefaultApproachFocusTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	FVector2D Focus = FVector2D(999.0, 999.0);

	TestFalse(TEXT("an airport with no runway has no default focus"),
		UFlightBoard::DefaultApproachFocus(*Net, Focus));
	TestEqual(TEXT("and OutFocus is left untouched on refusal"), Focus, FVector2D(999.0, 999.0));

	URoadProfile* Runway = TestProfiles::Runway();

	// The SHORTER runway, placed first.
	const FRoadNodeId A0 = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId A1 = Net->AddNode(FVector2D(30000.0, 0.0));
	Net->AddStraightSegment(A0, A1, Runway);

	TestTrue(TEXT("one runway is the default"), UFlightBoard::DefaultApproachFocus(*Net, Focus));
	TestEqual(TEXT("its own threshold"), Focus, FVector2D(0.0, 0.0));

	// A LONGER, separate runway - THE POINT OF THE TEST: the longest wins, not the first
	// found or the last added, matching AirsideCapability's own LongestRunway().
	const FRoadNodeId B0 = Net->AddNode(FVector2D(0.0, 100000.0));
	const FRoadNodeId B1 = Net->AddNode(FVector2D(60000.0, 100000.0));
	Net->AddStraightSegment(B0, B1, Runway);

	TestTrue(TEXT("the longer runway is still a default"),
		UFlightBoard::DefaultApproachFocus(*Net, Focus));
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
	URoadNetwork* Net = BoardNetworkWithStands({3600.0});
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
		Flight->Phase, EFlightPhase::Accepted);
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
			Flight->Phase = EFlightPhase::Landing;
			Board->AddOffer(*Clock, Flight);
			break;
		default:
			{
				// DEPARTED - held a real agent id, then gave it back and moved to History.
				const int32 AgentId = NextTestAgentId++;
				Flight->AgentId = AgentId;
				Flight->Phase = EFlightPhase::Departing;
				Board->AddOffer(*Clock, Flight);
				Board->OnAgentPhase(*Traffic, *Net, *Clock, AgentId,
					EAgentPhase::Departing, EAgentPhase::Gone);
			}
			break;
		}
		++CreatedCount;
	}

	// A BATCH OF LAPSING OFFERS, resolved in one TickOffers rather than one at a time: the
	// production path that lapses offers walks a snapshot of Flights and moves some of it to
	// History - the same shape as issue #188's own migration sweep in OnAfterRestore.
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

#endif
