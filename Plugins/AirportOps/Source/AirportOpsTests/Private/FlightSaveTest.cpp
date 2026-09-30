#include "CoreMinimal.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Content/AirsideSettings.h"
#include "Model/Ledger.h"
#include "Model/OpsEventBus.h"
#include "OpsSaveTestHelpers.h"
#include "Model/Flight.h"
#include "Model/FlightBoard.h"
#include "Model/JobBoard.h"
#include "Model/GroundTraffic.h"
#include "Model/OpsSave.h"
#include "Model/RoadNetwork.h"
#include "Model/SimClock.h"
#include "Model/StandAllocator.h"
#include "Testing/AirsideTestGraph.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/**
	 * A runway, an exit, a taxiway and one stand for a 34 m span. A RUNWAY since the arrival
	 * queue (2026-09-28): a flight is only cleared when the real plan says it could land, and
	 * the arrival tests here dispatch.
	 */
	URoadNetwork* SaveTestNetwork()
	{
		FAirframe Airframe;
		Airframe.Wingspan = 3400.0;
		return FTestAirport::Build(Airframe).Net;
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
	Flight->LeadTimeSeconds = 1000.0;
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
	FreshClock->Advance(20.0);   // 75 game s per real s at night: past an ETA 1000 game s out
	Reloaded->TickQueue(*Traffic, *Net, *FreshClock);

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
	TestEqual(TEXT("a flight already due joins the queue at once, not dropped"), Flight->Phase, EFlightPhase::Inbound);
	Board->TickQueue(*Traffic, *Net, *Clock);
	TestEqual(TEXT("and the queue clears it - nothing holds the runway"), Calls, 1);
	TestEqual(TEXT("so it is landing, not still waiting"), Flight->Phase, EFlightPhase::Landing);
	return true;
}

/**
 * The countdown survives a save as a plain number (snapshot v5): the offer comes back with
 * exactly the real seconds it had left, and keeps draining at the real rate whatever speed
 * the reloaded game runs at. Replaced PR #137's two load-path tests of the game-time expiry
 * schedule - there is no schedule to re-arm any more, and a real-time window cannot pass
 * while the game is shut.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlightOfferCountdownSurvivesSaveTest,
	"AirportOps.Model.FlightSave.CountdownSurvivesSave",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFlightOfferCountdownSurvivesSaveTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = SaveTestNetwork();
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>();
	USimClock* Clock = NewObject<USimClock>();
	UFlightBoard* Board = SaveTestBoard();
	UJobBoard* Fuel = NewObject<UJobBoard>(GetTransientPackage());

	UFlight* Flight = NewObject<UFlight>(GetTransientPackage());
	Flight->Airframe.Wingspan = 3400.0;
	Flight->OfferWindowSeconds = 60.0;
	Flight->OfferSecondsLeft = 60.0;
	Board->AddOffer(*Clock, Flight);
	Board->TickOffers(*Traffic, *Net, *Clock, 20.0);

	FOpsSnapshot Snapshot;
	OpsSave::Capture(OpsSaveTest::Persistents(*Clock, *Board, *Fuel), *Net, Snapshot);

	URoadNetwork* RestoredNet = NewObject<URoadNetwork>(GetTransientPackage());
	USimClock* RestoredClock = NewObject<USimClock>();
	UFlightBoard* RestoredBoard = SaveTestBoard();
	UJobBoard* RestoredFuel = NewObject<UJobBoard>(GetTransientPackage());
	if (!TestTrue(TEXT("restore succeeds"),
		OpsSave::Restore(Snapshot, OpsSaveTest::Persistents(*RestoredClock, *RestoredBoard, *RestoredFuel), *RestoredNet))) { return false; }
	RestoredBoard->RearmSchedules(*Traffic, *RestoredNet, *RestoredClock);

	const TArray<UFlight*> Offers = RestoredBoard->Offers();
	if (!TestEqual(TEXT("the offer came back"), Offers.Num(), 1)) { return false; }
	UFlight* Restored = Offers[0];
	TestEqual(TEXT("with exactly the seconds it had left"), Restored->OfferSecondsLeft, 40.0, 1e-9);

	RestoredClock->SetSpeed(ESimSpeed::X4);
	RestoredBoard->TickOffers(*Traffic, *RestoredNet, *RestoredClock, 10.0);
	TestEqual(TEXT("and drains at the real rate at x4 too"), Restored->OfferSecondsLeft, 30.0, 1e-9);

	RestoredBoard->TickOffers(*Traffic, *RestoredNet, *RestoredClock, 31.0);
	TestEqual(TEXT("then lapses when it runs out"), Restored->Phase, EFlightPhase::Expired);
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
	Flight->LeadTimeSeconds = 1000.0;
	Board->AddOffer(*Clock, Flight);
	TestTrue(TEXT("accepted before the save"), Board->Accept(*Traffic, *Net, *Clock, *Flight));

	// ZEROED BY HAND: a real v2 save could not have written this field, since it did not
	// exist yet. Leaving it at whatever AcceptImmediate-style code set it to would test a
	// blob no v2 game ever actually produced.
	Flight->ApproachFocus = FVector2D::ZeroVector;

	UJobBoard* Fuel = NewObject<UJobBoard>(GetTransientPackage());

	FOpsSnapshot Snapshot;
	OpsSave::Capture(OpsSaveTest::Persistents(*Clock, *Board, *Fuel), *Net, Snapshot);
	Snapshot.Version = 2;

	URoadNetwork* RestoredNet = NewObject<URoadNetwork>(GetTransientPackage());
	USimClock* RestoredClock = NewObject<USimClock>();
	UFlightBoard* RestoredBoard = SaveTestBoard();
	UJobBoard* RestoredFuel = NewObject<UJobBoard>(GetTransientPackage());
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
	KeptOffer->OfferSecondsLeft = 60.0;
	Board->AddOffer(*Clock, KeptOffer);

	UFlight* OldDeclined = NewObject<UFlight>(GetTransientPackage());
	OldDeclined->Airframe.Wingspan = 3400.0;
	Board->AddOffer(*Clock, OldDeclined);
	OldDeclined->Phase = EFlightPhase::Declined;
	const int32 DeclinedId = OldDeclined->Id;

	UJobBoard* Fuel = NewObject<UJobBoard>(GetTransientPackage());
	FOpsSnapshot Snapshot;
	OpsSave::Capture(OpsSaveTest::Persistents(*Clock, *Board, *Fuel), *Net, Snapshot);

	URoadNetwork* RestoredNet = NewObject<URoadNetwork>(GetTransientPackage());
	USimClock* RestoredClock = NewObject<USimClock>();
	UFlightBoard* RestoredBoard = SaveTestBoard();
	UJobBoard* RestoredFuel = NewObject<UJobBoard>(GetTransientPackage());
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

/**
 * #404 (spec 2026-09-29-ops-batch3 §4): agents are not saved - UOpsRuntime::LoadFromSlot clears them - so a flight
 * saved past Inbound came back pointing at an agent that no longer existed, and sat in its phase for ever. The
 * arrivals side goes round again (Inbound, stand re-held, at the back of the queue); the ground side retires as
 * departed, unscored. The load sequence is LoadFromSlot's own, by hand: clear agents, discard the queue, restore,
 * re-apply holds, re-arm, tick - and the flight must LAND AGAIN, its fee charged once across the whole of it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlightMidFlightGoesRoundTest,
	"AirportOps.Model.FlightSave.MidFlightGoesRoundOrRetires",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFlightMidFlightGoesRoundTest::RunTest(const FString& Parameters)
{
	const FAirframe Airframe = UAirsideSettings::ResolveDefaultAirframe();
	const FTestAirport Field = FTestAirport::Build(Airframe);
	URoadNetwork* Net = Field.Net;
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	USimClock* Clock = NewObject<USimClock>(GetTransientPackage());
	ULedger* Ledger = NewObject<ULedger>(GetTransientPackage());
	Ledger->Clock = Clock;
	UFlightBoard* Board = SaveTestBoard();
	Board->Ledger = Ledger;
	Board->RestoreClock = Clock;
	Board->Dispatcher = [Traffic, Net](const FVector2D& Near, const FAirframe& Frame)
	{
		return Traffic->DispatchArrival(*Net, Near, Frame, 1.0) != 0;
	};

	// THE BOARD AND CLOCK THE BUS HANDS PHASES TO - swapped for the restored pair at the load, as the runtime's
	// subscription reaches whatever board it owns.
	UFlightBoard* LiveBoard = Board;
	USimClock* LiveClock = Clock;
	FOpsEventBus Bus;
	int32 Scored = 0;   // anything the airline roster would score
	Bus.BeginWiring();
	Bus.Subscribe<FAgentPhaseEvent>(EOpsTier::Sim, TEXT("FlightBoard"), [&](const FAgentPhaseEvent& E)
	{
		LiveBoard->OnAgentPhase(*Traffic, *Net, *LiveClock, E.AgentId, E.From, E.To);
	});
	Bus.Subscribe<FFlightAirborneEvent>(EOpsTier::Reaction, TEXT("test"), [&Scored](const FFlightAirborneEvent&) { ++Scored; });
	Bus.Subscribe<FFlightCancelledEvent>(EOpsTier::Reaction, TEXT("test"), [&Scored](const FFlightCancelledEvent&) { ++Scored; });
	Bus.Subscribe<FTurnaroundEndedEvent>(EOpsTier::Reaction, TEXT("test"), [&Scored](const FTurnaroundEndedEvent&) { ++Scored; });
	Bus.EndWiring();
	Traffic->OnAgentPhaseChanged.AddLambda([&Bus](int32 Id, EAgentPhase From, EAgentPhase To)
	{
		Bus.Publish(FAgentPhaseEvent{ Id, From, To });
	});
	Board->Bus = &Bus;

	const auto LandingFees = [Ledger]()
	{
		return Ledger->Entries().FilterByPredicate([](const FLedgerEntry& E) { return E.Category == ELedgerCategory::LandingFee; }).Num();
	};

	// A: LANDED FOR REAL, charged, and taxiing in when the game is saved.
	UFlight* Taxiing = NewObject<UFlight>(GetTransientPackage());
	Taxiing->Airframe = Airframe;
	Taxiing->ApproachFocus = Field.Threshold;
	Taxiing->LandingFee = 1200.0;
	Board->AddOffer(*Clock, Taxiing);
	if (!TestTrue(TEXT("accepted"), Board->Accept(*Traffic, *Net, *Clock, *Taxiing))) { return false; }
	Clock->Advance(1.0);
	Board->TickQueue(*Traffic, *Net, *Clock);
	Bus.Drain();
	const int32 OldAgent = Taxiing->AgentId;
	if (!TestEqual(TEXT("dispatched"), Taxiing->Phase, EFlightPhase::Landing)) { return false; }
	for (int32 Step = 0; Step < 20 * 600 && Taxiing->Phase != EFlightPhase::TaxiIn; ++Step)
	{
		Traffic->Advance(0.05, Net);
		Bus.Drain();
	}
	if (!TestEqual(TEXT("taxiing in when saved"), Taxiing->Phase, EFlightPhase::TaxiIn)) { return false; }
	if (!TestEqual(TEXT("its landing fee charged once already"), LandingFees(), 1)) { return false; }
	if (!TestTrue(TEXT("with the stand it was accepted onto"), Taxiing->Stand.IsSet())) { return false; }

	// B: ON ITS STAND when saved - planted, as a restore would find it: a phase and an agent id.
	UFlight* OnStand = NewObject<UFlight>(GetTransientPackage());
	OnStand->Airframe = Airframe;
	OnStand->AirlineId = TEXT("SaveTestAirline");
	OnStand->AgentId = 4242;
	OnStand->Phase = EFlightPhase::Turnaround;
	Board->AddOffer(*Clock, OnStand);

	UJobBoard* Fuel = NewObject<UJobBoard>(GetTransientPackage());
	FOpsSnapshot Snapshot;
	OpsSave::Capture(OpsSaveTest::Persistents(*Clock, *Board, *Fuel), *Net, Snapshot);
	const double SavedAt = Clock->Now();

	// THE LOAD, as UOpsRuntime::LoadFromSlot runs it.
	Traffic->ClearAgents();
	Bus.Discard();
	URoadNetwork* RestoredNet = NewObject<URoadNetwork>(GetTransientPackage());
	USimClock* RestoredClock = NewObject<USimClock>(GetTransientPackage());
	UFlightBoard* Restored = SaveTestBoard();
	UJobBoard* RestoredFuel = NewObject<UJobBoard>(GetTransientPackage());
	Restored->RestoreClock = RestoredClock;
	Restored->Ledger = Ledger;
	Restored->Bus = &Bus;
	Restored->Dispatcher = Board->Dispatcher;
	if (!TestTrue(TEXT("restore succeeds"),
		OpsSave::Restore(Snapshot, OpsSaveTest::Persistents(*RestoredClock, *Restored, *RestoredFuel), *RestoredNet))) { return false; }
	LiveBoard = Restored;
	LiveClock = RestoredClock;

	UFlight* Again = Restored->FindByIdForTest(Taxiing->Id);
	UFlight* Retired = Restored->FindByIdForTest(OnStand->Id);
	if (!TestNotNull(TEXT("the taxiing flight came back"), Again) || !TestNotNull(TEXT("and the one on its stand"), Retired)) { return false; }
	TestEqual(TEXT("taxiing in when saved: it goes round again - Inbound"), Again->Phase, EFlightPhase::Inbound);
	TestEqual(TEXT("with no agent - its aeroplane was not saved"), Again->AgentId, static_cast<int32>(INDEX_NONE));
	TestNull(TEXT("and nothing found by the dead agent id"), Restored->FlightForAgent(OldAgent));
	TestEqual(TEXT("joining the queue now, at the loaded time"), Again->HoldingSince, SavedAt, 1e-9);
	TestTrue(TEXT("its landing fee still marked paid"), Again->bLandingFeePaid);
	TestEqual(TEXT("on its stand when saved: retired as departed"), Retired->Phase, EFlightPhase::Departed);
	TestEqual(TEXT("with no agent"), Retired->AgentId, static_cast<int32>(INDEX_NONE));
	TestFalse(TEXT("out of the live list"), Restored->Live().Contains(Retired));
	TestNull(TEXT("and nothing found by its dead agent id"), Restored->FlightForAgent(4242));
	TestEqual(TEXT("dated the load"), Retired->TerminatedAt, SavedAt, 1e-9);
	Bus.Drain();
	TestEqual(TEXT("nothing the roster would score was published - the save system is not the player's fault"), Scored, 0);

	// THE REST OF THE LOAD: holds re-made, arrivals re-armed - then the queue must clear it.
	Restored->OnGraphRebuilt(*Traffic, *Net);
	const FEntityInstance* Stand = Net->GetEntity(Again->Stand);
	TestTrue(TEXT("its stand is held again"), Stand != nullptr && Traffic->IsStandHeld(Stand->PoseNode, 0));
	Restored->RearmSchedules(*Traffic, *Net, *RestoredClock);
	TestEqual(TEXT("re-arming leaves it holding"), Again->Phase, EFlightPhase::Inbound);
	TestEqual(TEXT("at the time it joined"), Again->HoldingSince, SavedAt, 1e-9);
	RestoredClock->Advance(1.0);
	Restored->TickQueue(*Traffic, *Net, *RestoredClock);
	TestEqual(TEXT("and it is cleared to land again"), Again->Phase, EFlightPhase::Landing);
	TestTrue(TEXT("as a new aeroplane"), Again->AgentId != INDEX_NONE && Traffic->FindAgent(Again->AgentId) != nullptr);
	Bus.Drain();
	TestEqual(TEXT("its second landing charges nothing - the fee was charged once"), LandingFees(), 1);
	return true;
}

#endif
