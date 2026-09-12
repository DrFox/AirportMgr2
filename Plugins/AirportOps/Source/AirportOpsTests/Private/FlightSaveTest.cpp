#include "CoreMinimal.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/Flight.h"
#include "Model/FlightBoard.h"
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
	Board->AddOffer(Flight);
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
	Board->AddOffer(Flight);

	Clock->Advance(1.0);   // 72 game seconds: the ETA is already behind us
	TestEqual(TEXT("nothing has been dispatched, because nothing was armed"), Calls, 0);

	Board->RearmSchedules(*Traffic, *Net, *Clock);
	TestEqual(TEXT("a flight already due is dispatched at once, not dropped"), Calls, 1);
	TestEqual(TEXT("and it is landing, not still waiting"), Flight->Phase, EFlightPhase::Landing);
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
	Board->AddOffer(Flight);
	TestTrue(TEXT("accepted before the save"), Board->Accept(*Traffic, *Net, *Clock, *Flight));

	// ZEROED BY HAND: a real v2 save could not have written this field, since it did not
	// exist yet. Leaving it at whatever AcceptImmediate-style code set it to would test a
	// blob no v2 game ever actually produced.
	Flight->ApproachFocus = FVector2D::ZeroVector;

	FOpsSnapshot Snapshot;
	OpsSave::Capture(*Clock, *Net, *Board, Snapshot);
	Snapshot.Version = 2;

	URoadNetwork* RestoredNet = NewObject<URoadNetwork>(GetTransientPackage());
	USimClock* RestoredClock = NewObject<USimClock>();
	UFlightBoard* RestoredBoard = SaveTestBoard();
	if (!TestTrue(TEXT("restore succeeds"),
		OpsSave::Restore(Snapshot, *RestoredClock, *RestoredNet, *RestoredBoard))) { return false; }

	const TArray<UFlight*> Live = RestoredBoard->Live();
	TestEqual(TEXT("the flight came back"), Live.Num(), 1);
	if (Live.Num() != 1) { return false; }

	TestEqual(TEXT("a v2 load aims the flight at the board's OWN restored focus"),
		Live[0]->ApproachFocus, RestoredBoard->ApproachFocus);
	TestEqual(TEXT("which is the focus that was actually saved, not the origin"),
		Live[0]->ApproachFocus, FVector2D(12345.0, -678.0));
	return true;
}

#endif
