#include "CoreMinimal.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/Flight.h"
#include "Model/FlightBoard.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadAgent.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Model/SimClock.h"
#include "Model/StandAllocator.h"

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
	First->ArrivesAt = Clock->Now() + 100.0;
	Second->ArrivesAt = Clock->Now() + 100.0;
	Board->AddOffer(First);
	Board->AddOffer(Second);

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
	URoadNetwork* Net = BoardNetworkWithStands({3600.0});
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>();
	USimClock* Clock = NewObject<USimClock>();
	UFlightBoard* Board = MakeBoard();

	UFlight* Flight = BoardFlightNeeding(3400.0);
	Flight->ArrivesAt = Clock->Now() + 100.0;
	Board->AddOffer(Flight);

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

	// The clock compresses: at the default 1200 real seconds per game day one real second is
	// 72 game seconds, so two is past an ETA 100 game seconds out.
	Clock->Advance(2.0);

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
	UFlightBoard* Board = MakeBoard();

	UFlight* Flight = BoardFlightNeeding(3400.0);
	Flight->AgentId = 5;
	Flight->Phase = EFlightPhase::Landing;
	Board->AddOffer(Flight);

	Board->OnAgentPhase(*Traffic, *Net, 5, EAgentPhase::Arriving, EAgentPhase::Taxiing);
	TestEqual(TEXT("taxiing before the stand is TaxiIn"), Flight->Phase, EFlightPhase::TaxiIn);

	Board->OnAgentPhase(*Traffic, *Net, 5, EAgentPhase::Taxiing, EAgentPhase::Parked);
	TestEqual(TEXT("parked is the turnaround"), Flight->Phase, EFlightPhase::Turnaround);

	// THE POINT OF THE TEST: the same agent phase, the other answer.
	Board->OnAgentPhase(*Traffic, *Net, 5, EAgentPhase::Parked, EAgentPhase::Taxiing);
	TestEqual(TEXT("taxiing after the turnaround is TaxiOut"), Flight->Phase, EFlightPhase::TaxiOut);

	Board->OnAgentPhase(*Traffic, *Net, 5, EAgentPhase::Taxiing, EAgentPhase::Departing);
	TestEqual(TEXT("departing"), Flight->Phase, EFlightPhase::Departing);

	Board->OnAgentPhase(*Traffic, *Net, 5, EAgentPhase::Departing, EAgentPhase::Gone);
	TestEqual(TEXT("gone is departed"), Flight->Phase, EFlightPhase::Departed);
	TestEqual(TEXT("and the agent handle is given back"), Flight->AgentId, INDEX_NONE);

	// An agent nobody owns - a fuel truck - must move no flight at all.
	Board->OnAgentPhase(*Traffic, *Net, 99, EAgentPhase::Taxiing, EAgentPhase::Parked);
	TestEqual(TEXT("a truck's phase change moves no flight"), Flight->Phase, EFlightPhase::Departed);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlightBoardExpiresOffersTest,
	"AirportOps.Model.FlightBoard.ExpiresAnIgnoredOffer",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFlightBoardExpiresOffersTest::RunTest(const FString& Parameters)
{
	USimClock* Clock = NewObject<USimClock>();
	UFlightBoard* Board = MakeBoard();

	UFlight* Offer = BoardFlightNeeding(3400.0);
	Offer->ExpiresAt = Clock->Now() + 50.0;
	Board->AddOffer(Offer);
	TestEqual(TEXT("it is in the inbox to begin with"), Board->PendingOfferCount(), 1);

	Clock->Advance(1.0);   // 72 game seconds: past the expiry
	Board->Tick(*Clock);

	TestEqual(TEXT("an ignored offer lapses"), Offer->Phase, EFlightPhase::Expired);
	TestEqual(TEXT("and leaves the inbox"), Board->PendingOfferCount(), 0);
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
	URoadNetwork* Net = BoardNetworkWithStands({3600.0});
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

	// ArrivesAt == ExpiresAt == Clock->Now() at the accept - see AcceptImmediate's own header
	// on why - so the tiniest advance crosses the ETA and fires the dispatcher.
	Clock->Advance(0.1);
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
	Flight->ExpiresAt = Clock->Now() + 50.0;
	Flight->ArrivesAt = Clock->Now() + 100000.0;
	Board->AddOffer(Flight);
	TestTrue(TEXT("accepted before it would have lapsed"),
		Board->Accept(*Traffic, *Net, *Clock, *Flight));

	Clock->Advance(1.0);
	Board->Tick(*Clock);
	TestEqual(TEXT("an accepted flight is not expired by its old offer deadline"),
		Flight->Phase, EFlightPhase::Accepted);
	return true;
}

#endif
