#include "CoreMinimal.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "OpsSaveTestHelpers.h"
#include "Model/Flight.h"
#include "Model/FlightBoard.h"
#include "Model/FuelService.h"
#include "Model/GroundTraffic.h"
#include "Model/OpsSave.h"
#include "Model/RoadNetwork.h"
#include "Model/SimClock.h"
#include "Model/StandAllocator.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	URoadNetwork* SaveTestNetwork()
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
		Net->PlaceEntity(Stand, Stand->Anchors, FVector2D::ZeroVector, 0.0, 3600.0,
			Stand->PoseRole, Stand->Trucks);
		return Net;
	}

	UFlightBoard* SaveTestBoard()
	{
		UFlightBoard* Board = NewObject<UFlightBoard>(GetTransientPackage());
		Board->Allocator = NewObject<UStandAllocator>(GetTransientPackage());
		return Board;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlightSurvivesASaveTest,
	"AirportOps.Model.FlightSave.AnInboundFlightStillArrives",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFlightSurvivesASaveTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = SaveTestNetwork();
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>();
	USimClock* Clock = NewObject<USimClock>();
	UFlightBoard* Board = SaveTestBoard();
	Board->Dispatcher = [](const FVector2D&, const FAirframe&) { return true; };

	UFlight* Flight = NewObject<UFlight>(GetTransientPackage());
	Flight->Airframe.Wingspan = 3400.0;
	Flight->ArrivesAt = Clock->Now() + 1000.0;
	Board->AddOffer(*Clock, Flight);
	TestTrue(TEXT("accepted before the save"), Board->Accept(*Traffic, *Net, *Clock, *Flight));

	TArray<uint8> Bytes;
	OpsSave::SerializeObject(*Board, Bytes);
	TestTrue(TEXT("the board serialised to something"), Bytes.Num() > 0);

	// A FRESH board and a FRESH clock: this is a reload, not a copy. USimClock deliberately
	// does not save its callback queue, so the restored flight has an ETA and NOTHING armed.
	UFlightBoard* Reloaded = SaveTestBoard();
	OpsSave::DeserializeObject(*Reloaded, Bytes);

	TestEqual(TEXT("the accepted flight came back"), Reloaded->Live().Num(), 1);

	int32 Calls = 0;
	Reloaded->Dispatcher = [&Calls](const FVector2D&, const FAirframe&) { ++Calls; return true; };

	USimClock* FreshClock = NewObject<USimClock>();
	FreshClock->Advance(1.0);
	TestEqual(TEXT("nothing arrives from a restore alone - the queue was not saved"), Calls, 0);

	Reloaded->RearmSchedules(*Traffic, *Net, *FreshClock);
	FreshClock->Advance(20.0);   // 72 game s per real s: past an ETA 1000 game s out

	TestEqual(TEXT("a reloaded inbound flight still arrives, once re-armed"), Calls, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlightDueWhileClosedTest,
	"AirportOps.Model.FlightSave.AFlightDueWhileTheGameWasShutIsNotLost",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFlightDueWhileClosedTest::RunTest(const FString& Parameters)
{
	// Its slot passed while the game was closed. Dropping it silently is the failure this
	// guards: the player accepted a flight and it simply never came.
	//
	// The flight is put in the Accepted state DIRECTLY, because that is what a restore does -
	// it deserialises phases and ETAs, and arms nothing. Accepting it through the board here
	// would arm the clock, the clock would fire on its own, and the test would prove that
	// the ordinary path works rather than that the load path does.
	URoadNetwork* Net = SaveTestNetwork();
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>();
	USimClock* Clock = NewObject<USimClock>();
	UFlightBoard* Board = SaveTestBoard();

	int32 Calls = 0;
	Board->Dispatcher = [&Calls](const FVector2D&, const FAirframe&) { ++Calls; return true; };

	UFlight* Flight = NewObject<UFlight>(GetTransientPackage());
	Flight->Airframe.Wingspan = 3400.0;
	Flight->ArrivesAt = 10.0;
	Flight->Phase = EFlightPhase::Accepted;
	Board->AddOffer(*Clock, Flight);

	Clock->Advance(1.0);   // 72 game seconds: the ETA is already behind us
	TestEqual(TEXT("nothing has been dispatched, because nothing was armed"), Calls, 0);

	Board->RearmSchedules(*Traffic, *Net, *Clock);
	TestEqual(TEXT("a flight already due is dispatched at once, not dropped"), Calls, 1);
	TestEqual(TEXT("and it is landing, not still waiting"), Flight->Phase, EFlightPhase::Landing);
	return true;
}

/**
 * PR #137 REVIEW: RearmSchedules' Offered branch (re-arming an offer's expiry on load) had
 * no test. The window is still open at load time, so the offer must survive the round trip
 * still Offered, and must still lapse once its rearmed schedule catches up to it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlightOfferedExpirySurvivesLoadTest,
	"AirportOps.Model.FlightSave.AnOfferedFlightsExpiryIsRearmedOnLoad",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFlightOfferedExpirySurvivesLoadTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = SaveTestNetwork();
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>();
	USimClock* Clock = NewObject<USimClock>();
	UFlightBoard* Board = SaveTestBoard();
	UFuelService* Fuel = NewObject<UFuelService>(GetTransientPackage());

	UFlight* Flight = NewObject<UFlight>(GetTransientPackage());
	Flight->Airframe.Wingspan = 3400.0;
	Flight->ExpiresAt = Clock->Now() + 1000.0;
	Board->AddOffer(*Clock, Flight);
	TestEqual(TEXT("offered before the save"), Flight->Phase, EFlightPhase::Offered);

	FOpsSnapshot Snapshot;
	OpsSave::Capture(OpsSaveTest::Persistents(*Clock, *Board, *Fuel), *Net, Snapshot);

	URoadNetwork* RestoredNet = NewObject<URoadNetwork>(GetTransientPackage());
	USimClock* RestoredClock = NewObject<USimClock>();
	UFlightBoard* RestoredBoard = SaveTestBoard();
	UFuelService* RestoredFuel = NewObject<UFuelService>(GetTransientPackage());
	if (!TestTrue(TEXT("restore succeeds"),
		OpsSave::Restore(Snapshot, OpsSaveTest::Persistents(*RestoredClock, *RestoredBoard, *RestoredFuel), *RestoredNet))) { return false; }

	const TArray<UFlight*> Offers = RestoredBoard->Offers();
	if (!TestEqual(TEXT("the offer came back"), Offers.Num(), 1)) { return false; }
	UFlight* Restored = Offers[0];

	RestoredBoard->RearmSchedules(*Traffic, *RestoredNet, *RestoredClock);
	TestEqual(TEXT("still offered right after rearming - its window has not passed"),
		Restored->Phase, EFlightPhase::Offered);

	RestoredClock->Advance(20.0);   // 72 game s per real s: past an ExpiresAt 1000 game s out
	TestEqual(TEXT("and it lapses once its rearmed schedule catches up"),
		Restored->Phase, EFlightPhase::Expired);
	return true;
}

/**
 * PR #137 REVIEW: the other half of the Offered branch - an offer whose window already
 * passed while the game was shut must lapse AT ONCE on rearm, the same "act now, and say so"
 * rule FFlightDueWhileClosedTest already covers for an Accepted flight's arrival.
 *
 * AddOffer arms a real (if never-fired) expiry schedule the instant it is called, same as
 * FFlightDueWhileClosedTest's Accept would arm a real dispatch - so neither test calls
 * Clock->Advance() before the load-path call under test, which is what keeps that schedule
 * from firing on its own and proving the wrong path.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlightOfferedExpiredWhileClosedTest,
	"AirportOps.Model.FlightSave.AnOfferedFlightPastItsWindowExpiresOnLoad",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFlightOfferedExpiredWhileClosedTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = SaveTestNetwork();
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>();
	USimClock* Clock = NewObject<USimClock>();
	UFlightBoard* Board = SaveTestBoard();

	UFlight* Flight = NewObject<UFlight>(GetTransientPackage());
	Flight->Airframe.Wingspan = 3400.0;
	Flight->ExpiresAt = -5.0;   // already behind a fresh clock's Now() == 0.0
	Board->AddOffer(*Clock, Flight);
	TestEqual(TEXT("still offered - nothing has driven the clock yet"),
		Flight->Phase, EFlightPhase::Offered);

	Board->RearmSchedules(*Traffic, *Net, *Clock);
	TestEqual(TEXT("an offer already past its window is expired at once, not left standing"),
		Flight->Phase, EFlightPhase::Expired);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlightV2LoadAimsAtTheBoardsOldFocusTest,
	"AirportOps.Model.FlightSave.AV2LoadAimsEveryFlightAtTheBoardsOldFocus",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFlightV2LoadAimsAtTheBoardsOldFocusTest::RunTest(const FString& Parameters)
{
	// BEFORE UFlight::ApproachFocus (issue #96), every flight shared the board's ONE
	// ApproachFocus. A genuine v1/v2 blob's flights therefore carry no per-flight focus at
	// all - OpsSave::Restore must recreate it from the board's own field (which DID exist
	// and DID serialise) rather than leave every restored flight aimed at the world origin.
	URoadNetwork* Net = SaveTestNetwork();
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>();
	USimClock* Clock = NewObject<USimClock>();
	UFlightBoard* Board = SaveTestBoard();
	Board->Dispatcher = [](const FVector2D&, const FAirframe&) { return true; };
	Board->ApproachFocus = FVector2D(12345.0, -678.0);

	UFlight* Flight = NewObject<UFlight>(GetTransientPackage());
	Flight->Airframe.Wingspan = 3400.0;
	Flight->ArrivesAt = Clock->Now() + 1000.0;
	Board->AddOffer(*Clock, Flight);
	TestTrue(TEXT("accepted before the save"), Board->Accept(*Traffic, *Net, *Clock, *Flight));

	// ZEROED BY HAND: a real v2 save could not have written this field, since it did not
	// exist yet. Leaving it at whatever AcceptImmediate-style code set it to would test a
	// blob no v2 game ever actually produced.
	Flight->ApproachFocus = FVector2D::ZeroVector;

	UFuelService* Fuel = NewObject<UFuelService>(GetTransientPackage());

	FOpsSnapshot Snapshot;
	OpsSave::Capture(OpsSaveTest::Persistents(*Clock, *Board, *Fuel), *Net, Snapshot);
	Snapshot.Version = 2;

	URoadNetwork* RestoredNet = NewObject<URoadNetwork>(GetTransientPackage());
	USimClock* RestoredClock = NewObject<USimClock>();
	UFlightBoard* RestoredBoard = SaveTestBoard();
	UFuelService* RestoredFuel = NewObject<UFuelService>(GetTransientPackage());
	if (!TestTrue(TEXT("restore succeeds"),
		OpsSave::Restore(Snapshot, OpsSaveTest::Persistents(*RestoredClock, *RestoredBoard, *RestoredFuel), *RestoredNet))) { return false; }

	const TArray<UFlight*> Live = RestoredBoard->Live();
	TestEqual(TEXT("the flight came back"), Live.Num(), 1);
	if (Live.Num() != 1) { return false; }

	TestEqual(TEXT("a v2 load aims the flight at the board's OWN restored focus"),
		Live[0]->ApproachFocus, RestoredBoard->ApproachFocus);
	TestEqual(TEXT("which is the focus that was actually saved, not the origin"),
		Live[0]->ApproachFocus, FVector2D(12345.0, -678.0));
	return true;
}

/**
 * ISSUE #188: before History existed, EVERY flight ever created - live or long since declined
 * - sat in the Flights blob, because there was nowhere else for one to go. This proves a save
 * shaped like that still loads, and that the migration sweep in OnAfterRestore puts the old
 * terminal flight where a fresh game would have put it, rather than either losing it or
 * leaving it clogging Flights forever.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlightBoardHistorySplitMigratesAnOldSaveTest,
	"AirportOps.Model.FlightSave.AnOldSaveWithADeclinedFlightStillInFlightsStillLoads",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFlightBoardHistorySplitMigratesAnOldSaveTest::RunTest(const FString& Parameters)
{
	// A BYTE-FOR-BYTE PRE-#188 SAVE, reproduced by building the board the OLD way: AddOffer,
	// then flipping Phase BY HAND rather than through Decline - which today would already move
	// the flight into History before this test ever serialises anything, and so could never
	// reproduce the shape a real old save has. Flipping the field directly is what leaves a
	// Declined flight sitting in Flights, exactly as a genuine pre-#188 blob would.
	URoadNetwork* Net = SaveTestNetwork();
	USimClock* Clock = NewObject<USimClock>();
	UFlightBoard* Board = SaveTestBoard();

	UFlight* KeptOffer = NewObject<UFlight>(GetTransientPackage());
	KeptOffer->Airframe.Wingspan = 3400.0;
	KeptOffer->ArrivesAt = Clock->Now() + 1000.0;
	Board->AddOffer(*Clock, KeptOffer);

	UFlight* OldDeclined = NewObject<UFlight>(GetTransientPackage());
	OldDeclined->Airframe.Wingspan = 3400.0;
	Board->AddOffer(*Clock, OldDeclined);
	OldDeclined->Phase = EFlightPhase::Declined;
	const int32 DeclinedId = OldDeclined->Id;

	UFuelService* Fuel = NewObject<UFuelService>(GetTransientPackage());
	FOpsSnapshot Snapshot;
	OpsSave::Capture(OpsSaveTest::Persistents(*Clock, *Board, *Fuel), *Net, Snapshot);

	URoadNetwork* RestoredNet = NewObject<URoadNetwork>(GetTransientPackage());
	USimClock* RestoredClock = NewObject<USimClock>();
	UFlightBoard* RestoredBoard = SaveTestBoard();
	UFuelService* RestoredFuel = NewObject<UFuelService>(GetTransientPackage());
	if (!TestTrue(TEXT("restore succeeds"),
		OpsSave::Restore(Snapshot, OpsSaveTest::Persistents(*RestoredClock, *RestoredBoard, *RestoredFuel), *RestoredNet))) { return false; }

	TestEqual(TEXT("the still-offered flight came back live"),
		RestoredBoard->Offers().Num() + RestoredBoard->Live().Num(), 1);
	TestEqual(TEXT("the pre-#188 declined flight was swept OUT of the live list on load"),
		RestoredBoard->GetHistoryCountForTest(), 1);

	UFlight* RestoredDeclined = RestoredBoard->FindByIdForTest(DeclinedId);
	if (TestNotNull(TEXT("and is still reachable by its old id - a load forgets nothing, "
		"only RollUp does"), RestoredDeclined))
	{
		TestEqual(TEXT("still Declined, not silently reset by the sweep"),
			RestoredDeclined->Phase, EFlightPhase::Declined);
	}
	return true;
}

#endif
