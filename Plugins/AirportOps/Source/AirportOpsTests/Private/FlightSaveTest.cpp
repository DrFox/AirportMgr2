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
#include "Model/OfferGenerator.h"
#include "Model/OpsSave.h"
#include "Model/RoadNetwork.h"
#include "Model/SimClock.h"
#include "Model/StandAllocator.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"
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

/**
 * #404 (spec 2026-09-29-ops-batch3 §4): agents are not saved - UOpsRuntime::LoadFromSlot clears them - so a flight
 * saved past Inbound came back pointing at an agent that no longer existed, and sat in its phase for ever. The
 * arrivals side goes round again (Inbound, stand re-held, at the back of the queue); the ground side retires as
 * departed, unscored. The load is LoadFromSlot's: clear agents, discard the queue, restore, then the board's own
 * UFlightBoard::RestoreAfterLoad - demote, re-apply holds, re-arm - and a tick; and the flight must LAND AGAIN, its fee
 * charged once across the whole of it. RestoreAfterLoad, not its four steps typed out here (issue #426): this test used
 * to re-type the order by hand, so a production reorder would have left it green.
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
	Restored->Ledger = Ledger;
	Restored->Bus = &Bus;
	Restored->Dispatcher = Board->Dispatcher;
	if (!TestTrue(TEXT("restore succeeds"),
		OpsSave::Restore(Snapshot, OpsSaveTest::Persistents(*RestoredClock, *Restored, *RestoredFuel), *RestoredNet))) { return false; }
	LiveBoard = Restored;
	LiveClock = RestoredClock;
	// THE REST OF THE LOAD, IN THE BOARD'S OWN ORDER, as LoadFromSlot calls it once the network is adopted: dated by the
	// restored clock, at an airport that admits arrivals.
	Restored->RestoreAfterLoad(Traffic, *Net, *RestoredClock, /*bAirportAdmits*/ true);

	UFlight* Again = Restored->FindByIdForTest(Taxiing->Id);
	UFlight* Retired = Restored->FindByIdForTest(OnStand->Id);
	if (!TestNotNull(TEXT("the taxiing flight came back"), Again) || !TestNotNull(TEXT("and the one on its stand"), Retired)) { return false; }
	TestEqual(TEXT("taxiing in when saved: it goes round again - Inbound"), Again->Phase, EFlightPhase::Inbound);
	TestTrue(TEXT("and is the one flight in the queue"), Restored->Queue().Num() == 1 && Restored->Queue()[0] == Again);
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

	// THE HOLDS RE-MADE AND THE ARRIVALS RE-ARMED by the same call - then the queue must clear it.
	const FEntityInstance* Stand = Net->GetEntity(Again->Stand);
	TestTrue(TEXT("its stand is held again, under its own holder id"), Stand != nullptr
		&& Traffic->IsStandHeld(Stand->PoseNode, 0) && !Traffic->IsStandHeld(Stand->PoseNode, Again->HolderId()));
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

/**
 * #426 review: WITH NO TRAFFIC MODEL, a load still demotes and cancels. Steps 1-2 of UFlightBoard::RestoreAfterLoad need
 * no model - and UOpsRuntime::LoadFromSlot ran them regardless before the board owned the order - so only the re-hold
 * and re-arm, which are claims on and dispatches into the model, are skipped.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlightRestoreWithoutTrafficTest,
	"AirportOps.Model.FlightSave.RestoreWithoutTrafficStillDemotes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFlightRestoreWithoutTrafficTest::RunTest(const FString& Parameters)
{
	USimClock* Clock = NewObject<USimClock>(GetTransientPackage());
	UFlightBoard* Board = SaveTestBoard();
	// PLANTED AS A RESTORE WOULD FIND THEM: a phase, and for the one on its stand an agent id.
	UFlight* OnStand = NewObject<UFlight>(GetTransientPackage());
	Board->AddOffer(*Clock, OnStand);
	OnStand->AgentId = 4242;
	OnStand->Phase = EFlightPhase::Turnaround;
	UFlight* Accepted = NewObject<UFlight>(GetTransientPackage());
	Board->AddOffer(*Clock, Accepted);
	Accepted->Phase = EFlightPhase::Accepted;

	Board->RestoreAfterLoad(nullptr, *NewObject<URoadNetwork>(GetTransientPackage()), *Clock, /*bAirportAdmits*/ false);
	TestEqual(TEXT("step 1 ran with no model: the flight on its stand retired as departed"), OnStand->Phase, EFlightPhase::Departed);
	TestEqual(TEXT("step 2 ran with no model: the accepted flight at a closed airport was cancelled"), Accepted->Phase,
		EFlightPhase::Cancelled);
	return true;
}

namespace
{
	/** A two-stand FTestAirport and the pieces a re-queue test drives. Prefixed: the test module is a unity build. */
	struct FRequeueRig
	{
		FAirframe Airframe;
		FTestAirport Field;
		UGroundTraffic* Traffic = nullptr;
		USimClock* Clock = nullptr;
		UFlightBoard* Board = nullptr;

		FRequeueRig()
		{
			Airframe = UAirsideSettings::ResolveDefaultAirframe();
			FTestAirportOptions Options;
			Options.StandCount = 2;
			Field = FTestAirport::Build(Airframe, Options);
			Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
			Clock = NewObject<USimClock>(GetTransientPackage());
			Board = SaveTestBoard();
		}

		/** A flight as a restore finds one saved mid-taxi: a phase, a dead agent id, the stand it was accepted onto. */
		UFlight* Restored(EFlightPhase Phase, int32 DeadAgent, FEntityInstanceId Stand)
		{
			UFlight* Flight = NewObject<UFlight>(GetTransientPackage());
			Flight->Airframe = Airframe;
			Flight->ApproachFocus = Field.Threshold;
			Flight->AgentId = DeadAgent;
			Flight->Phase = Phase;
			Flight->Stand = Stand;
			Board->AddOffer(*Clock, Flight);
			return Flight;
		}

		/** Held, and by this flight alone: IsStandHeld excluding its own holder id says nobody else. */
		bool HeldBy(const UFlight& Flight) const
		{
			const FEntityInstance* Stand = Field.Net->GetEntity(Flight.Stand);
			return Stand != nullptr && Traffic->IsStandHeld(Stand->PoseNode, 0) && !Traffic->IsStandHeld(Stand->PoseNode, Flight.HolderId());
		}
	};
}

/**
 * REVIEW I1: a re-queued flight names the stand it was ACCEPTED onto, which it gave up at its dispatch - and which
 * another flight may have been accepted onto since. The load re-holds that promise first; the re-queued flight is
 * refused it, gives it up, and reserves the free one during the load. FIRST IN Flights on purpose, so a re-hold
 * that followed the list would reach it first.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlightRequeueKeepsAcceptedStandTest,
	"AirportOps.Model.FlightSave.RequeueDoesNotTakeAnAcceptedStand",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFlightRequeueKeepsAcceptedStandTest::RunTest(const FString& Parameters)
{
	FRequeueRig Rig;
	UFlight* Requeue = Rig.Restored(EFlightPhase::TaxiIn, 77, FEntityInstanceId());
	UFlight* Promised = NewObject<UFlight>(GetTransientPackage());
	Promised->Airframe = Rig.Airframe;
	Promised->ApproachFocus = Rig.Field.Threshold;
	Promised->LeadTimeSeconds = 1.0e7;
	Rig.Board->AddOffer(*Rig.Clock, Promised);
	if (!TestTrue(TEXT("the other flight is accepted onto a stand"), Rig.Board->Accept(*Rig.Traffic, *Rig.Field.Net, *Rig.Clock, *Promised))) { return false; }
	const FEntityInstanceId PromisedStand = Promised->Stand;
	// THE SHAPE: the taxiing flight's accepted stand is the one promised since. A load's rebuilt graph holds nothing.
	Requeue->Stand = PromisedStand;
	Rig.Traffic->ReleaseHold(Promised->HolderId());

	const TArray<UFlight*> Requeued = Rig.Board->DemoteRestoredMidFlight(Rig.Clock->Now());
	Rig.Board->OnGraphRebuilt(*Rig.Traffic, *Rig.Field.Net, Requeued);

	TestTrue(TEXT("the accepted flight keeps the stand it was promised"), Promised->Stand == PromisedStand && Rig.HeldBy(*Promised));
	TestTrue(TEXT("the re-queued flight gave it up"), Requeue->Stand != PromisedStand);
	TestTrue(TEXT("and holds the free stand, during the load, under its own id"), Requeue->Stand.IsSet() && Rig.HeldBy(*Requeue));
	return true;
}

/** REVIEW I1, the dead-stand case: the stand a re-queued flight was accepted onto was deleted before the save. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlightRequeueOffDeadStandTest,
	"AirportOps.Model.FlightSave.RequeueOffADeadStandReserves",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFlightRequeueOffDeadStandTest::RunTest(const FString& Parameters)
{
	FRequeueRig Rig;
	const FEntityInstanceId Dead = Rig.Field.Stands[0];
	const FEntityInstanceId Alive = Rig.Field.Stands[1];
	UFlight* Requeue = Rig.Restored(EFlightPhase::Landing, 78, Dead);
	Rig.Field.Net->RemoveEntity(Dead);
	TestGraph::Rebuild(*Rig.Field.Net);

	AddExpectedMessagePlain(TEXT("which is gone from the graph"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	const TArray<UFlight*> Requeued = Rig.Board->DemoteRestoredMidFlight(Rig.Clock->Now());
	Rig.Board->OnGraphRebuilt(*Rig.Traffic, *Rig.Field.Net, Requeued);
	TestTrue(TEXT("its dead stand is replaced, not kept as a hold on nothing"), Requeue->Stand != Dead);
	TestTrue(TEXT("and the live one is reserved for it during the load"), Requeue->Stand == Alive && Rig.HeldBy(*Requeue));
	return true;
}

/**
 * REVIEW M6: bLandingFeePaid is LEFT AS SAVED by the re-queue, not forced true. A flight saved Landing before its
 * Arriving was heard was never charged; forcing the flag would lose its fee for ever. It lands again: charged once.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlightUnchargedLandingTest,
	"AirportOps.Model.FlightSave.UnchargedLandingIsChargedOnce",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFlightUnchargedLandingTest::RunTest(const FString& Parameters)
{
	FRequeueRig Rig;
	ULedger* Ledger = NewObject<ULedger>(GetTransientPackage());
	Ledger->Clock = Rig.Clock;
	Rig.Board->Ledger = Ledger;
	UGroundTraffic* Traffic = Rig.Traffic;
	URoadNetwork* Net = Rig.Field.Net;
	Rig.Board->Dispatcher = [Traffic, Net](const FVector2D& Near, const FAirframe& Frame)
	{
		return Traffic->DispatchArrival(*Net, Near, Frame, 1.0) != 0;
	};
	UFlight* Flight = Rig.Restored(EFlightPhase::Landing, 88, Rig.Field.Stands[0]);
	Flight->LandingFee = 500.0;
	Flight->bLandingFeePaid = false;

	const TArray<UFlight*> Requeued = Rig.Board->DemoteRestoredMidFlight(Rig.Clock->Now());
	Rig.Board->OnGraphRebuilt(*Traffic, *Net, Requeued);
	Rig.Clock->Advance(1.0);
	Rig.Board->TickQueue(*Traffic, *Net, *Rig.Clock);
	if (!TestEqual(TEXT("it lands again"), Flight->Phase, EFlightPhase::Landing)) { return false; }
	// THE ARRIVING THE BUS WOULD DELIVER, twice: the second is the "more than one phase maps to Landing" case.
	Rig.Board->OnAgentPhase(*Traffic, *Net, *Rig.Clock, Flight->AgentId, EAgentPhase::Gone, EAgentPhase::Arriving);
	Rig.Board->OnAgentPhase(*Traffic, *Net, *Rig.Clock, Flight->AgentId, EAgentPhase::Gone, EAgentPhase::Arriving);
	const int32 Rows = Ledger->Entries().FilterByPredicate([](const FLedgerEntry& E) { return E.Category == ELedgerCategory::LandingFee; }).Num();
	TestEqual(TEXT("saved uncharged, it is charged exactly once when it lands"), Rows, 1);
	return true;
}

namespace
{
	/** An open offer with figures a restore must bring back. */
	UFlight* ByValueOffer(UFlightBoard& Board, USimClock& Clock, const TCHAR* Callsign)
	{
		UFlight* Flight = NewObject<UFlight>(GetTransientPackage());
		Flight->Airframe.Wingspan = 3400.0;
		Flight->Callsign = Callsign;
		Flight->LandingFee = 1234.0;
		Flight->OfferWindowSeconds = 60.0;
		Flight->OfferSecondsLeft = 60.0;
		Board.AddOffer(Clock, Flight);
		return Flight;
	}
}

/**
 * ISSUE #425's PIN. The "Flights" blob held object PATHS: FObjectAndNameAsStringProxyArchive writes a UObject reference
 * as its path and re-finds it on load. In one session the path found the LIVE flight, so a load "restored" whatever the
 * flight was NOW (every test above passed that way - its original was still alive); in the next session it found
 * nothing, so every flight was dropped and the board's Allocator came back null, refusing every accept with no log.
 * THE NEXT SESSION, SIMULATED: capture, mutate the originals as the player would after saving, then destroy every object
 * of the saving "session" - collected, not merely unreferenced - and restore into fresh ones.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlightSaveRestoresByValueTest,
	"AirportOps.Model.FlightSave.RestoresByValue",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFlightSaveRestoresByValueTest::RunTest(const FString& Parameters)
{
	FOpsSnapshot Snapshot;
	int32 DeclinedLaterId = 0;
	int32 DrainedLaterId = 0;
	int32 DeclinedBeforeId = 0;
	TWeakObjectPtr<UFlight> OriginalDeclinedLater;
	TWeakObjectPtr<UFlight> OriginalDrainedLater;
	TArray<UObject*> SavingSession;
	{
		USimClock* Clock = NewObject<USimClock>(GetTransientPackage());
		UFlightBoard* Board = SaveTestBoard();
		UJobBoard* Fuel = NewObject<UJobBoard>(GetTransientPackage());
		ULedger* Ledger = NewObject<ULedger>(GetTransientPackage());
		UPricing* Pricing = NewObject<UPricing>(GetTransientPackage());
		UOfferGenerator* Generator = NewObject<UOfferGenerator>(GetTransientPackage());
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		Ledger->Clock = Clock;
		Ledger->Pricing = Pricing;
		Board->Ledger = Ledger;
		Board->Pricing = Pricing;
		Fuel->Ledger = Ledger;
		Fuel->Pricing = Pricing;
		Generator->Pricing = Pricing;

		UFlight* DeclinedLater = ByValueOffer(*Board, *Clock, TEXT("PIN 1"));
		UFlight* DrainedLater = ByValueOffer(*Board, *Clock, TEXT("PIN 2"));
		UFlight* DeclinedBefore = ByValueOffer(*Board, *Clock, TEXT("PIN 3"));
		Board->Decline(*Clock, *DeclinedBefore);   // into History before the save: History is saved by value too
		DeclinedLaterId = DeclinedLater->Id;
		DrainedLaterId = DrainedLater->Id;
		DeclinedBeforeId = DeclinedBefore->Id;
		OriginalDeclinedLater = DeclinedLater;
		OriginalDrainedLater = DrainedLater;

		TArray<IOpsPersistent*> Persistents = OpsSaveTest::Persistents(*Clock, *Board, *Fuel, *Ledger, *Pricing);
		Persistents.Add(Generator);
		OpsSave::Capture(Persistents, *Net, Snapshot);

		// AFTER THE SAVE, as the player would: a same-session load used to bring these states back, not the saved ones.
		Board->Decline(*Clock, *DeclinedLater);
		DrainedLater->OfferSecondsLeft = 1.0;

		SavingSession = { Clock, Board, Board->Allocator.Get(), Fuel, Ledger, Pricing, Generator, Net,
			DeclinedLater, DrainedLater, DeclinedBefore };
	}

	// THE SAVING SESSION ENDS. Garbage AND collected: a path the load re-finds must find nothing, as it would in a new
	// process. Nothing below may be created before this line - a raw local is no GC root, and would go with them.
	for (UObject* Each : SavingSession)
	{
		Each->MarkAsGarbage();
	}
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	if (!TestFalse(TEXT("the saving session's flights are destroyed, not merely unreferenced - else this measures nothing"),
		OriginalDeclinedLater.IsValid(/*bEvenIfGarbage*/ true))) { return false; }

	USimClock* Clock = NewObject<USimClock>(GetTransientPackage());
	UFlightBoard* Board = SaveTestBoard();
	UStandAllocator* OwnAllocator = Board->Allocator;
	UJobBoard* Fuel = NewObject<UJobBoard>(GetTransientPackage());
	ULedger* Ledger = NewObject<ULedger>(GetTransientPackage());
	UPricing* Pricing = NewObject<UPricing>(GetTransientPackage());
	UOfferGenerator* Generator = NewObject<UOfferGenerator>(GetTransientPackage());
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	Ledger->Clock = Clock;
	Ledger->Pricing = Pricing;
	Board->Ledger = Ledger;
	Board->Pricing = Pricing;
	Fuel->Ledger = Ledger;
	Fuel->Pricing = Pricing;
	Generator->Pricing = Pricing;
	TArray<IOpsPersistent*> Persistents = OpsSaveTest::Persistents(*Clock, *Board, *Fuel, *Ledger, *Pricing);
	Persistents.Add(Generator);
	if (!TestTrue(TEXT("restore succeeds"), OpsSave::Restore(Snapshot, Persistents, *Net))) { return false; }

	// THE WIRING IS NOT STATE: each pointer stays what this session wired, not a path to the last session's object.
	TestTrue(TEXT("the board keeps its own allocator - #425 left it null, and Accept refused every offer silently"),
		Board->Allocator.Get() == OwnAllocator && OwnAllocator != nullptr);
	TestTrue(TEXT("and its own ledger and pricing"), Board->Ledger.Get() == Ledger && Board->Pricing.Get() == Pricing);
	TestTrue(TEXT("the ledger keeps its own clock and pricing"), Ledger->Clock.Get() == Clock && Ledger->Pricing.Get() == Pricing);
	TestTrue(TEXT("the job board keeps its own ledger and pricing"), Fuel->Ledger.Get() == Ledger && Fuel->Pricing.Get() == Pricing);
	TestTrue(TEXT("the generator keeps its own pricing"), Generator->Pricing.Get() == Pricing);

	TestEqual(TEXT("both offers open at the save come back - #425 dropped every flight in a new session"),
		Board->Offers().Num(), 2);
	TestEqual(TEXT("and are counted as pending"), Board->PendingOfferCount(), 2);

	UFlight* Declined = Board->FindByIdForTest(DeclinedLaterId);
	if (TestNotNull(TEXT("the offer declined after the save is back"), Declined))
	{
		TestEqual(TEXT("as the save had it - Offered, not the later Decline"), Declined->Phase, EFlightPhase::Offered);
		TestEqual(TEXT("with its callsign"), Declined->Callsign, FString(TEXT("PIN 1")));
		TestEqual(TEXT("and its fee"), Declined->LandingFee, 1234.0, 1e-9);
		TestTrue(TEXT("a distinct object, not the saving session's"), TWeakObjectPtr<UFlight>(Declined) != OriginalDeclinedLater);
		TestTrue(TEXT("owned by the board that restored it"), Declined->GetOuter() == Board);
	}
	UFlight* Drained = Board->FindByIdForTest(DrainedLaterId);
	if (TestNotNull(TEXT("the offer drained after the save is back"), Drained))
	{
		TestEqual(TEXT("with the seconds it had AT THE SAVE"), Drained->OfferSecondsLeft, 60.0, 1e-9);
		TestTrue(TEXT("a distinct object too"), TWeakObjectPtr<UFlight>(Drained) != OriginalDrainedLater);
	}
	UFlight* Retired = Board->FindByIdForTest(DeclinedBeforeId);
	if (TestNotNull(TEXT("the flight in History at the save is back"), Retired))
	{
		TestEqual(TEXT("still Declined"), Retired->Phase, EFlightPhase::Declined);
		TestEqual(TEXT("and in History, not the live list"), Board->GetHistoryCountForTest(), 1);
	}

	// TRANSIENT IS A SAVE FLAG, NOT A LIFETIME ONE: the board's arrays still hold the restored flights through a
	// collection. The board is rooted for it - a raw local is no GC root - and nothing above is used after it.
	const TWeakObjectPtr<UFlight> Held = Declined;
	Board->AddToRoot();
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	Board->RemoveFromRoot();
	TestTrue(TEXT("a collection keeps the restored flights - a Transient array is still a reference"), Held.IsValid());
	return true;
}

/**
 * #425's OTHER HALF: a load into the SAME board replaces every flight with a new object, so anything still holding a
 * pre-load one - a viewmodel row, the inspector's cached lookup, the board's own indices - must be re-pointed, not left
 * reading the pre-load state. The board retires the replaced flights (a weak pointer reads null at once) and moves its
 * Revision, which every viewmodel keys its re-read on.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlightSaveRestoreRetiresReplacedFlightsTest,
	"AirportOps.Model.FlightSave.RestoreRetiresReplacedFlights",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFlightSaveRestoreRetiresReplacedFlightsTest::RunTest(const FString& Parameters)
{
	USimClock* Clock = NewObject<USimClock>(GetTransientPackage());
	UFlightBoard* Board = SaveTestBoard();
	UJobBoard* Fuel = NewObject<UJobBoard>(GetTransientPackage());
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	UFlight* PreLoad = ByValueOffer(*Board, *Clock, TEXT("PIN 4"));
	const int32 Id = PreLoad->Id;
	const TWeakObjectPtr<UFlight> Reader = PreLoad;

	FOpsSnapshot Snapshot;
	OpsSave::Capture(OpsSaveTest::Persistents(*Clock, *Board, *Fuel), *Net, Snapshot);
	Board->Decline(*Clock, *PreLoad);
	const uint32 RevisionBefore = Board->Revision();
	if (!TestTrue(TEXT("restore succeeds"), OpsSave::Restore(Snapshot, OpsSaveTest::Persistents(*Clock, *Board, *Fuel), *Net)))
	{
		return false;
	}

	UFlight* Restored = Board->FindByIdForTest(Id);
	if (!TestNotNull(TEXT("the board finds the flight by id"), Restored)) { return false; }
	TestTrue(TEXT("and it is the restored object, not the pre-load one"), Restored != PreLoad);
	TestEqual(TEXT("carrying the saved phase"), Restored->Phase, EFlightPhase::Offered);
	TestTrue(TEXT("the inbox lists the restored object"), Board->Offers().Num() == 1 && Board->Offers()[0] == Restored);
	TestFalse(TEXT("a reader still holding the pre-load flight reads null, not the pre-load state"), Reader.IsValid());
	TestNotEqual(TEXT("the revision moved, so every viewmodel keyed on it re-reads the board"), Board->Revision(), RevisionBefore);
	return true;
}

/**
 * #452 REVIEW: the pre-v6 path. A blob from before #425 is the board's tags and nothing after them - its flights were
 * paths, which the Transient arrays' tags are now skipped for. It restores NO flights and says so; and the load still
 * retires the board's own pre-load flights and moves Revision, so nothing keeps showing them as if they had loaded.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlightSavePreV6BlobRestoresNoFlightsTest,
	"AirportOps.Model.FlightSave.PreV6BlobRestoresNoFlights",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFlightSavePreV6BlobRestoresNoFlightsTest::RunTest(const FString& Parameters)
{
	USimClock* Clock = NewObject<USimClock>(GetTransientPackage());
	UFlightBoard* Board = SaveTestBoard();
	UFlight* PreLoad = ByValueOffer(*Board, *Clock, TEXT("PIN 5"));
	const int32 Id = PreLoad->Id;
	const TWeakObjectPtr<UFlight> Reader = PreLoad;

	// A PRE-v6 BLOB, written as OpsSave::SerializeObject wrote one then: the same archive, the tagged pass ALONE -
	// UObject::Serialize, not the board's override, so no by-value section follows the tags.
	TArray<uint8> Bytes;
	{
		FMemoryWriter Writer(Bytes, /*bIsPersistent*/ true);
		FObjectAndNameAsStringProxyArchive Ar(Writer, /*bInLoadIfFindFails*/ false);
		Board->UObject::Serialize(Ar);
	}
	if (!TestTrue(TEXT("the tags alone serialised to something"), Bytes.Num() > 0)) { return false; }

	const uint32 RevisionBefore = Board->Revision();
	AddExpectedMessagePlain(TEXT("no flights by value"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	OpsSave::DeserializeObject(*Board, Bytes);

	TestEqual(TEXT("no offer is restored - a pre-v6 blob holds none by value"), Board->Offers().Num(), 0);
	TestEqual(TEXT("nothing live either"), Board->Live().Num(), 0);
	TestEqual(TEXT("nor in History"), Board->GetHistoryCountForTest(), 0);
	TestEqual(TEXT("and none counted as pending"), Board->PendingOfferCount(), 0);
	TestNull(TEXT("the pre-load flight is no longer found by id"), Board->FindByIdForTest(Id));
	TestFalse(TEXT("and a reader still holding it reads null - retired, not left showing"), Reader.IsValid());
	TestNotEqual(TEXT("the revision moved, so every viewmodel re-reads the now-empty board"), Board->Revision(), RevisionBefore);
	return true;
}

#endif
