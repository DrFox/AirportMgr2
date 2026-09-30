#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Build/AnchorLink.h"
#include "Content/AirsideSettings.h"
#include "Entities/EntityDefinition.h"
#include "Model/ArrivalPlanner.h"
#include "Model/Flight.h"
#include "Model/FlightBoard.h"
#include "Model/GroundTraffic.h"
#include "Model/AirlineDefinition.h"
#include "Model/AirlineRoster.h"
#include "Model/DeparturePlanner.h"
#include "Model/Airport.h"
#include "Model/JobBoard.h"
#include "Model/OfferGenerator.h"
#include "Model/Ledger.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadTraffic.h"
#include "Model/TaxiwayStrip.h"
#include "Model/RoutePolicy.h"
#include "Model/RouteSearch.h"
#include "Model/SimClock.h"
#include "Profiles/RoadProfile.h"
#include "Model/StandAllocator.h"
#include "Testing/AirsideTestGraph.h"
#include "Model/OpsEventBus.h"
#include "Model/OpsEvents.h"
#include "Model/OpsSave.h"
#include "Model/RoadAgent.h"
#include "OpsEventsTestListener.h"
#include "Misc/ScopeExit.h"
#include "Present/AirsideTraffic.h"
#include "Present/OpsRuntime.h"
#include "Present/RoadEditFacade.h"
#include "Present/RoadNetworkActor.h"
#include "Testing/AirsideTestWorld.h"
#include "OpsTransitionTestHelpers.h"

#if WITH_DEV_AUTOMATION_TESTS

// THE COMPOSITION TESTS FOR THE BUS (spec 2026-09-29 §4): each fails if UOpsRuntime leaves a seam
// of the bus unwired, which FOpsEventBus's own world-free tests cannot see.

namespace
{
	/** An attached runtime over a world with a real network (LoadFromSlot refuses without one).
	 *  Prefixed: the test module is a unity build. */
	UOpsRuntime* RuntimeBusTestAttach(FAirsideTestWorld& World)
	{
		ARoadNetworkActor* Actor = World.Actor;
		const int32 A = Actor->PlaceNode(FVector2D(0.0, 0.0));
		const int32 B = Actor->PlaceNode(FVector2D(20000.0, 0.0));
		Actor->ConnectNodes(A, B);
		UOpsRuntime* Runtime = NewObject<UOpsRuntime>();
		Runtime->Attach(Actor);
		return Runtime;
	}

	/** How many agent-phase events the runtime's drains have dispatched. The bus's own count (#445): UOpsEvents has no phase face to hang a
	 *  listener on - it had none that anything bound, and was cut - so a test of the bus's delivery reads the bus. */
	int32 RuntimeBusTestPhaseCount(UOpsRuntime& Runtime)
	{
		return Runtime.GetBus().DispatchedCountOf<FAgentPhaseEvent>();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsRuntimeBusSubscribedTest, "AirportOps.Present.Bus.EveryEventHasASubscriber",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsRuntimeBusSubscribedTest::RunTest(const FString&)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor to attach to"), TestWorld.Actor)) { return false; }
	UOpsRuntime* Runtime = RuntimeBusTestAttach(TestWorld);
	const TArray<const TCHAR*> Names = FOpsEventBus::EventNames();
	TestEqual(TEXT("EventNames covers the whole variant"), Names.Num(), static_cast<int32>(FOpsEventBus::NumTypes));
	for (int32 Index = 0; Index < Names.Num(); ++Index)
	{
		// BY NAME, so a failure says which event: an event nobody consumes is the declared-never-read
		// list CLAUDE.md names three times.
		TestTrue(FString::Printf(TEXT("%s has a subscriber after WireBus"), Names[Index]),
			Runtime->GetBus().SubscribersOf(Index).Num() > 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsRuntimeBusPhaseTest, "AirportOps.Present.Bus.PhaseIsQueuedThenDispatchedOnNextStep",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsRuntimeBusPhaseTest::RunTest(const FString&)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor to attach to"), TestWorld.Actor)) { return false; }
	UOpsRuntime* Runtime = RuntimeBusTestAttach(TestWorld);
	Runtime->Tick(0.0);
	const int32 Queued = Runtime->GetBus().QueuedCount();
	const int32 Before = RuntimeBusTestPhaseCount(*Runtime);

	TestWorld.Actor->GetTraffic()->OnAgentPhaseChanged.Broadcast(OpsTestTransition(42, EAgentPhase::Taxiing, EAgentPhase::Parked, EAgentEvent::Parked));
	TestEqual(TEXT("nothing runs inside Airside's broadcast - it is queued (#193)"), RuntimeBusTestPhaseCount(*Runtime), Before);
	TestEqual(TEXT("one event waits in the queue"), Runtime->GetBus().QueuedCount(), Queued + 1);

	Runtime->Tick(0.0);
	TestEqual(TEXT("the next ops step dispatches it - to the Sim tier's handlers"), RuntimeBusTestPhaseCount(*Runtime), Before + 1);
	TestEqual(TEXT("and the queue is empty again"), Runtime->GetBus().QueuedCount(), Queued);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsRuntimeBusReattachTest, "AirportOps.Present.Bus.ReattachDoesNotDouble",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsRuntimeBusReattachTest::RunTest(const FString&)
{
	// EVERY SEAM A SECOND ATTACH COULD DOUBLE (#445, generalised from the phase bridge alone): the subscription map - WireBus resets and
	// re-makes it - and each Airside delegate the runtime bridges onto the bus. A doubled bridge publishes twice per broadcast; a
	// doubled subscription is a name twice in SubscribersOf. Both are read for EVERY event and EVERY delegate, so a bridge added
	// without its Detach line, or a WireBus that stopped resetting, goes red by name.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor to attach to"), TestWorld.Actor)) { return false; }
	UOpsRuntime* Runtime = RuntimeBusTestAttach(TestWorld);
	FOpsEventBus& Bus = Runtime->GetBus();
	const TArray<const TCHAR*> Names = FOpsEventBus::EventNames();
	TArray<TArray<FName>> Wired;
	for (int32 Index = 0; Index < Names.Num(); ++Index) { Wired.Add(Bus.SubscribersOf(Index)); }

	Runtime->Attach(TestWorld.Actor);
	Runtime->Tick(0.0);   // the attach's catch-up runs, so nothing below is measured against it
	for (int32 Index = 0; Index < Names.Num(); ++Index)
	{
		TestEqual(FString::Printf(TEXT("a second Attach re-wires %s rather than stacking a second set of handlers"), Names[Index]),
			Bus.SubscribersOf(Index), Wired[Index]);
	}

	UAirsideTraffic* Traffic = TestWorld.Actor->GetTraffic();
	if (!TestNotNull(TEXT("the actor's traffic"), Traffic)) { return false; }
	// EACH DELEGATE BROADCAST ONCE, and the bus dispatches exactly one event for it: the bridge is bound once, not once per attach.
	int32 Phases = 0, Refusals = 0, Runways = 0, Stands = 0, Pushes = 0, Changes = 0, Builds = 0;
	auto Sync = [&]()
	{
		Phases = Bus.DispatchedCountOf<FAgentPhaseEvent>();
		Refusals = Bus.DispatchedCountOf<FArrivalRefusedEvent>();
		Runways = Bus.DispatchedCountOf<FRunwayFreedEvent>();
		Stands = Bus.DispatchedCountOf<FStandsFreedEvent>();
		Pushes = Bus.DispatchedCountOf<FPushGroundFreedEvent>();
		Changes = Bus.DispatchedCountOf<FNetworkChangedEvent>();
		Builds = Bus.DispatchedCountOf<FBuildRefusedEvent>();
	};
	// A counter read AFTER the tick, so each lambda re-syncs around its own broadcast.
	auto Check = [&](const TCHAR* What, int32& Counter, TFunctionRef<void()> Broadcast)
	{
		Sync();
		const int32 Before = Counter;
		Broadcast();
		Runtime->Tick(0.0);
		Sync();
		TestEqual(FString::Printf(TEXT("%s: one broadcast, one event, after two attaches"), What), Counter - Before, 1);
	};
	Check(TEXT("the phase bridge"), Phases, [&]() { Traffic->OnAgentPhaseChanged.Broadcast(OpsTestTransition(7, EAgentPhase::Taxiing, EAgentPhase::Parked, EAgentEvent::Parked)); });
	Check(TEXT("the arrival-refused bridge"), Refusals, [&]() { Traffic->OnArrivalRefused.Broadcast(EArrivalRefusal::NoRunway); });
	Check(TEXT("the runway-freed bridge"), Runways, [&]() { Traffic->OnRunwayFreed.Broadcast(FRoadSegmentId()); });
	Check(TEXT("the stands-freed bridge"), Stands, [&]() { Traffic->OnStandsFreed.Broadcast(TArray<FGuidelineNodeId>{ FGuidelineNodeId() }); });
	Check(TEXT("the push-ground-freed bridge"), Pushes, [&]() { Traffic->OnPushGroundFreed.Broadcast(3); });
	Check(TEXT("the network-changed bridge"), Changes, [&]() { TestWorld.Actor->OnNetworkChanged.Broadcast(EChangeKind::Topology, *TestWorld.Actor->Network); });
	URoadEditFacade* Facade = TestWorld.Actor->GetEditFacade();
	if (TestNotNull(TEXT("the actor's facade"), Facade))
	{
		Check(TEXT("the build-refused bridge"), Builds, [&]() { Facade->OnRefused.Broadcast(FBuildQuote(), EBuildRefusal::CannotAfford); });
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsRuntimeBusLoadTest, "AirportOps.Present.Bus.LoadDiscardsQueue",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsRuntimeBusLoadTest::RunTest(const FString&)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor to attach to"), TestWorld.Actor)) { return false; }
	UOpsRuntime* Runtime = RuntimeBusTestAttach(TestWorld);
	const FString Slot = TEXT("AirportOpsTest_BusLoad");
	if (!TestTrue(TEXT("save writes"), Runtime->SaveToSlot(Slot))) { return false; }

	const int32 Before = RuntimeBusTestPhaseCount(*Runtime);
	TestWorld.Actor->GetTraffic()->OnAgentPhaseChanged.Broadcast(OpsTestTransition(5, EAgentPhase::Taxiing, EAgentPhase::Gone, EAgentEvent::Retired));
	if (!TestTrue(TEXT("load reads"), Runtime->LoadFromSlot(Slot))) { return false; }
	Runtime->Tick(0.0);
	TestEqual(TEXT("an event queued before a load never reaches anyone: it names an agent that no longer exists"),
		RuntimeBusTestPhaseCount(*Runtime), Before);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsRuntimeBusStaleAgentTest, "AirportOps.Present.Bus.PhaseForAnAgentAlreadyGone",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsRuntimeBusStaleAgentTest::RunTest(const FString&)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor to attach to"), TestWorld.Actor)) { return false; }
	UOpsRuntime* Runtime = RuntimeBusTestAttach(TestWorld);
	Runtime->Tick(0.0);
	const int32 Before = RuntimeBusTestPhaseCount(*Runtime);

	// A handler runs a frame after the event was raised, so the agent it names may be gone -
	// here it never existed. Both boards must shrug, and the bus still delivers it.
	TestWorld.Actor->GetTraffic()->OnAgentPhaseChanged.Broadcast(OpsTestTransition(999999, EAgentPhase::Taxiing, EAgentPhase::Parked, EAgentEvent::Parked));
	Runtime->Tick(0.0);
	TestEqual(TEXT("a phase for an agent that is already gone is survived and still dispatched"),
		RuntimeBusTestPhaseCount(*Runtime), Before + 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsRuntimeBusDetachTest, "AirportOps.Present.Bus.DetachDiscardsQueue",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsRuntimeBusDetachTest::RunTest(const FString&)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor to attach to"), TestWorld.Actor)) { return false; }
	UOpsRuntime* Runtime = RuntimeBusTestAttach(TestWorld);
	Runtime->Tick(0.0);
	const int32 Before = RuntimeBusTestPhaseCount(*Runtime);

	TestWorld.Actor->GetTraffic()->OnAgentPhaseChanged.Broadcast(OpsTestTransition(3, EAgentPhase::Taxiing, EAgentPhase::Parked, EAgentEvent::Parked));
	Runtime->Attach(nullptr);
	Runtime->Attach(TestWorld.Actor);
	Runtime->Tick(0.0);
	TestEqual(TEXT("an event queued before a detach is the old actor's - a new level numbers agents from 1 again"),
		RuntimeBusTestPhaseCount(*Runtime), Before);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsRuntimeBusSaveFromHandlerTest, "AirportOps.Present.Bus.SaveFromAHandler",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsRuntimeBusSaveFromHandlerTest::RunTest(const FString&)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor to attach to"), TestWorld.Actor)) { return false; }
	UOpsRuntime* Runtime = RuntimeBusTestAttach(TestWorld);
	UOpsEventsTestListener* Listener = NewObject<UOpsEventsTestListener>();
	Listener->SaveOnNote = Runtime;
	Listener->SaveSlot = TEXT("AirportOpsTest_BusAutosave");
	Runtime->GetEvents()->OnNotification.AddDynamic(Listener, &UOpsEventsTestListener::OnNoteSave);

	// A Blueprint autosave bound to a notification: SaveToSlot called from INSIDE Drain, which used
	// to re-enter Drain and assert.
	Runtime->GetBus().Publish(FNotificationEvent{ TEXT("autosave") });
	Runtime->Tick(0.0);
	TestTrue(TEXT("a save made from inside an ops event handler completes rather than asserting"), Listener->bSavedFromHandler);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsRuntimeBusStaleParkedTest, "AirportOps.Present.Bus.StaleParkedOpensNoTurnaround",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsRuntimeBusStaleParkedTest::RunTest(const FString&)
{
	// THE STAGE 1 REVIEW'S SCENARIO (finding 1): the bus delivers Parked a step late, and by then
	// UGroundTraffic::ReofferStands may have redirected the aircraft to a stand that freed - so its
	// GoalNode is a STAND while it is still taxiing. Staged directly: an aircraft taxiing to a stand,
	// and a Parked event for it that is no longer true.
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("an actor"), Actor)) { return false; }
	Actor->PlaceNode(FVector2D(0.0, 40000.0));
	if (!TestNotNull(TEXT("a network"), Actor->Network.Get())) { return false; }
	URoadNetwork& Net = *Actor->Network;

	const FGuidelineNodeId TaxiSouth = Net.AddGuidelineNode(FVector2D(-10000.0, -10000.0));
	const FGuidelineNodeId TaxiNorth = Net.AddGuidelineNode(FVector2D(-10000.0, 10000.0));
	{
		FGuidelineEdge Edge;
		Edge.A = TaxiSouth;
		Edge.B = TaxiNorth;
		Edge.Control = FVector2D(-10000.0, 0.0);
		Edge.AllowedTraffic = FTrafficMask::Only(ETraversalClass::Aircraft);
		Edge.Direction = EGuidelineDir::Bidirectional;
		Edge.Width = 600.0;
		Edge.bDerived = true;
		Net.AddGuidelineEdge(MoveTemp(Edge));
	}
	UEntityDefinition* StandDef = UEntityDefinition::MakeStandTransient();
	const FEntityInstanceId Stand = Net.PlaceEntity(StandDef, StandDef->Anchors, FVector2D(0.0, 0.0), 0.0, 3600.0,
		StandDef->PoseRole, StandDef->Trucks);
	// THE DERIVATION'S TAIL over the hand-laid line (#438), not FAnchorLink::Build typed here.
	TestGraph::Link(Net);

	UOpsRuntime* Runtime = NewObject<UOpsRuntime>();
	Runtime->Attach(Actor);
	const FRoutePlan Plan = TestGraph::Probe(Net, TaxiSouth, Net.GetEntity(Stand)->PoseNode, ETraversalClass::Aircraft);
	if (!TestTrue(TEXT("the aircraft routes to the stand"), Plan.IsValid())) { return false; }
	if (!TestTrue(TEXT("and dispatches"), Actor->DispatchAgent(Plan, UAirsideSettings::ResolveDefaultAirframe()))) { return false; }
	const int32 Aircraft = Actor->GetTraffic()->GetNewestAgentId();
	Runtime->Tick(0.0);

	// THE NODE IT PARKED ON is the junction it waited at, not the stand it has been sent to since - which is what a
	// stale Parked carries since #436, and what the board decides on. The live agent's GoalNode IS the stand: a board
	// that read it instead of the event would open the turnaround this test forbids.
	Actor->GetTraffic()->OnAgentPhaseChanged.Broadcast(
		OpsTestTransition(Aircraft, EAgentPhase::Taxiing, EAgentPhase::Parked, EAgentEvent::Parked, TaxiSouth));
	Runtime->Tick(0.0);
	TestEqual(TEXT("a Parked event for an aircraft that is still taxiing opens no turnaround and no job"),
		Runtime->GetJobBoard()->GetJobs().Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsBusLandingFeeTest, "AirportOps.Model.Bus.LandingFeeIsCharged",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsBusLandingFeeTest::RunTest(const FString&)
{
	// THE LANDING FEE, charged through the path play takes (stage 1 review, finding 2). The dispatcher
	// runs UGroundTraffic::DispatchArrival, whose Admit broadcasts Gone -> Arriving INSIDE the dispatch
	// - before UFlightBoard::DispatchNow has recorded which agent is the flight's. Handled there, the
	// board could not find the flight and the fee (posted when a flight reaches Landing) was never
	// charged; through the bus it is handled after DispatchNow returns. Wired here as WireBus and the
	// Airside relay wire it, so the ORDER is the real one.
	const FAirframe Airframe = UAirsideSettings::ResolveDefaultAirframe();
	const FTestAirport Airport = FTestAirport::Build(Airframe);
	URoadNetwork* Net = Airport.Net;
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>();
	USimClock* Clock = NewObject<USimClock>();
	ULedger* Ledger = NewObject<ULedger>();
	Ledger->Clock = Clock;
	UFlightBoard* Board = NewObject<UFlightBoard>(GetTransientPackage());
	Board->Allocator = NewObject<UStandAllocator>(GetTransientPackage());
	Board->Ledger = Ledger;
	Board->Dispatcher = [Traffic, Net](const FVector2D& Near, const FAirframe& Frame)
	{
		return Traffic->DispatchArrival(*Net, Near, Frame, 1.0) != 0;
	};

	FOpsEventBus Bus;
	Bus.BeginWiring();
	Bus.Subscribe<FAgentPhaseEvent>(EOpsTier::Sim, TEXT("FlightBoard"), [&](const FAgentPhaseEvent& E)
	{
		Board->OnAgentPhase(*Net, *Clock, E);
	});
	Bus.EndWiring();
	Traffic->OnAgentPhaseChanged.AddLambda([&Bus](const FAgentTransition& Transition)
	{
		Bus.Publish(FAgentPhaseEvent{ Transition });
	});

	UFlight* Flight = NewObject<UFlight>(GetTransientPackage());
	Flight->Airframe = Airframe;
	Flight->RunwayPreference = Airport.Threshold;
	Flight->LandingFee = 1200.0;
	Flight->LeadTimeSeconds = 0.0;
	Board->AddOffer(*Clock, Flight);
	if (!TestTrue(TEXT("the flight is accepted"), Board->Accept(*Traffic, *Net, *Clock, *Flight))) { return false; }
	Clock->Advance(1.0);
	Board->TickQueue(*Traffic, *Net, *Clock);
	if (!TestEqual(TEXT("and dispatched"), Flight->GetPhase(), EFlightPhase::Landing)) { return false; }
	Bus.Drain();

	const bool bCharged = Ledger->Entries().ContainsByPredicate([](const FLedgerEntry& Entry)
		{ return Entry.Category == ELedgerCategory::LandingFee; });
	TestTrue(TEXT("landing is charged once the arrival's Gone -> Arriving reaches a board that knows its agent"), bCharged);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsRuntimeBusSatisfactionRateTest, "AirportOps.Present.Bus.SatisfactionMovesRate",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsRuntimeBusSatisfactionRateTest::RunTest(const FString&)
{
	// THE WHOLE CHAIN, through the runtime's own wiring: an airborne event on the bus -> the roster's
	// Reaction -> the generator's rate. The roster's world-free tests cannot see a WireBus that forgot a
	// subscription, or an Attach that never handed the generator its reader. ITS OWN AIRLINE, not the
	// content's, so the test measures something whatever content ships (stage 2 review).
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor to attach to"), TestWorld.Actor)) { return false; }
	UOpsRuntime* Runtime = RuntimeBusTestAttach(TestWorld);
	UAirlineDefinition* Airline = NewObject<UAirlineDefinition>(GetTransientPackage(), TEXT("BusTestAirline"));
	Airline->bIsFloor = false;
	Runtime->GetAirlines()->Ensure(Airline->GetFName());
	const double Before = Runtime->GetOfferGenerator()->AirlineFactor(*Airline);
	for (int32 Index = 0; Index < 10; ++Index)
	{
		Runtime->GetBus().Publish(FFlightAirborneEvent{ 0, Airline->GetFName(), 6000.0 });
	}
	Runtime->Tick(0.0);
	const double After = Runtime->GetOfferGenerator()->AirlineFactor(*Airline);
	TestTrue(FString::Printf(TEXT("ten very late departures lower the airline's offer rate (%.2f -> %.2f)"), Before, After), After < Before);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsRuntimeBusOldSnapshotTest, "AirportOps.Present.Bus.OldSnapshotSeedsAirlines",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsRuntimeBusOldSnapshotTest::RunTest(const FString&)
{
	// A SAVE FROM BEFORE THE "Airlines" BLOB: every catalog airline must come back at the start, not
	// be missing - OnBeforeRestore clears the rows, and only SeedAirlines puts them back.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor to attach to"), TestWorld.Actor)) { return false; }
	UOpsRuntime* Runtime = RuntimeBusTestAttach(TestWorld);
	if (Runtime->GetAirlineOffers().Num() == 0 || Runtime->GetAirlineOffers()[0].Airline == nullptr)
	{
		AddError(TEXT("no catalog airline to seed - the content this test needs is missing"));
		return false;
	}
	const FName First = Runtime->GetAirlineOffers()[0].Airline->GetFName();
	const FString Slot = TEXT("AirportOpsTest_BusOldSnapshot");
	if (!TestTrue(TEXT("save writes"), Runtime->SaveToSlot(Slot))) { return false; }
	FOpsSnapshot Snapshot;
	if (!TestTrue(TEXT("and reads back"), OpsSave::ReadSlot(Slot, Snapshot))) { return false; }
	TestTrue(TEXT("the save carries the airlines' blob"), Snapshot.Blobs.Contains(TEXT("Airlines")));
	Snapshot.Blobs.Remove(TEXT("Airlines"));
	if (!TestTrue(TEXT("the older snapshot writes"), OpsSave::WriteSlot(Slot, Snapshot))) { return false; }
	if (!TestTrue(TEXT("and loads"), Runtime->LoadFromSlot(Slot))) { return false; }
	const FAirlineStanding* Row = Runtime->GetAirlines()->Find(First);
	if (!TestNotNull(TEXT("a catalog airline is seeded after loading a snapshot with no Airlines blob"), Row)) { return false; }
	TestEqual(TEXT("at the tuning's start"), Row->Satisfaction, Runtime->GetAirlines()->Tuning.Start, 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsRuntimeBusQuietBoardTest, "AirportOps.Present.Bus.QuietBoardDoesNoWork",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsRuntimeBusQuietBoardTest::RunTest(const FString&)
{
	// STAGE 3's POINT, measured: a depot with its fleet at home and nothing to do costs the job board no
	// Step at all, where UJobBoard::Tick used to walk every depot and vehicle thirty times a second.
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("an actor"), Actor)) { return false; }
	Actor->PlaceNode(FVector2D(0.0, 40000.0));
	if (!TestNotNull(TEXT("a network"), Actor->Network.Get())) { return false; }
	URoadNetwork& Net = *Actor->Network;
	UEntityDefinition* DepotDef = UEntityDefinition::MakeFuelDepotTransient();
	Net.PlaceEntity(DepotDef, DepotDef->Anchors, FVector2D(12000.0, 0.0), 0.0, 0.0, DepotDef->PoseRole, DepotDef->Trucks);

	UOpsRuntime* Runtime = NewObject<UOpsRuntime>();
	Runtime->Attach(Actor);
	for (int32 Tick = 0; Tick < 5; ++Tick) { Runtime->Tick(1.0 / 30.0); }
	TestTrue(TEXT("the first steps seeded the depot's fleet"), Runtime->GetJobBoard()->GetVehicles().Num() > 0);
	const int32 Settled = Runtime->GetJobBoard()->StepCountForTest();
	for (int32 Tick = 0; Tick < 300; ++Tick) { Runtime->Tick(1.0 / 30.0); }
	TestEqual(TEXT("ten quiet seconds run no job board step at all"), Runtime->GetJobBoard()->StepCountForTest(), Settled);

	// AND THE PLAYER DRAWING SOMETHING WAKES IT - a second depot seeds on the next step.
	const int32 Fleet = Runtime->GetJobBoard()->GetVehicles().Num();
	Net.PlaceEntity(DepotDef, DepotDef->Anchors, FVector2D(-12000.0, 0.0), 0.0, 0.0, DepotDef->PoseRole, DepotDef->Trucks);
	// THE REBUILD EVERY EDIT ENDS IN (#446): FNetworkChangedEvent is the actor's announcement of a rebuild now, not a
	// per-frame compare of the network's revision - a raw model write with no rebuild after it is no edit the game makes.
	Actor->RebuildMesh();
	Runtime->Tick(1.0 / 30.0);
	TestTrue(TEXT("a depot placed on a quiet airport still gets its fleet (FNetworkChangedEvent)"),
		Runtime->GetJobBoard()->GetVehicles().Num() > Fleet);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsRuntimeBusNetworkChangedOnceTest, "AirportOps.Present.Bus.NetworkChangedPublishedOnceWithNoTick",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsRuntimeBusNetworkChangedOnceTest::RunTest(const FString&)
{
	// #446: FNetworkChangedEvent WAS A POLL - UOpsRuntime::Tick compared the network pointer and GuidelineRevision
	// every frame. It is the actor's announcement of a rebuild now, bridged in the rebuild itself: ONE event per
	// committed rebuild, with NO Tick between the edit and the queue; none for a drag frame (its commit publishes); and
	// exactly one for a save game's load (#426's RestoreInPlace -> adopt -> rebuild), whose queue the load discards first.
	// Counted from the bus's own publish line, so an event queued and then dropped still counts as published.
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("an actor"), Actor)) { return false; }
	UOpsRuntime* Runtime = RuntimeBusTestAttach(TestWorld);
	Runtime->Tick(1.0 / 30.0);

	FLogLineSpy Spy(FName(TEXT("LogOpsBus")));
	GLog->AddOutputDevice(&Spy);
	ON_SCOPE_EXIT { GLog->RemoveOutputDevice(&Spy); };
	auto Published = [&Spy]()
	{
		return Spy.CapturedLines.FilterByPredicate([](const FString& Line) { return Line.Contains(TEXT("Bus: + NetworkChanged")); }).Num();
	};

	const int32 Node = Actor->PlaceNode(FVector2D(0.0, -30000.0));
	TestEqual(TEXT("a road edit publishes FNetworkChangedEvent once, with no Tick"), Published(), 1);

	URoadEditFacade* Facade = Actor->GetEditFacade();
	if (!TestNotNull(TEXT("the actor has a facade"), Facade)) { return false; }
	Facade->BeginInteractiveEdit(TEXT("drag"));
	TestTrue(TEXT("setup: a drag frame moves the node"), Actor->MoveNode(Node, FVector2D(500.0, -30000.0)));
	TestEqual(TEXT("a drag frame publishes nothing - nothing is committed, and the passes would run every frame"), Published(), 1);
	Facade->EndInteractiveEdit(/*bKeep*/ true);
	TestEqual(TEXT("the drag's commit publishes once"), Published(), 2);

	if (!TestTrue(TEXT("setup: the airport saves"), Runtime->SaveToSlot(TEXT("AirportOpsTest_NetworkChangedOnce")))) { return false; }
	const int32 BeforeLoad = Published();
	if (!TestTrue(TEXT("setup: and loads"), Runtime->LoadFromSlot(TEXT("AirportOpsTest_NetworkChangedOnce")))) { return false; }
	TestEqual(TEXT("a load publishes FNetworkChangedEvent exactly once"), Published(), BeforeLoad + 1);
	return true;
}

// --- Ops alerts, stage 1 (spec 2026-09-29-ops-alerts): the runtime's wiring of refusals and the alerts pass.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsRuntimeBuildRefusedTest, "AirportOps.Present.Alerts.BuildRefusalReachesUi",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsRuntimeBuildRefusedTest::RunTest(const FString&)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor to attach to"), TestWorld.Actor)) { return false; }
	UOpsRuntime* Runtime = RuntimeBusTestAttach(TestWorld);
	UOpsEventsTestListener* Listener = NewObject<UOpsEventsTestListener>();
	Runtime->GetEvents()->OnBuildRefused.AddDynamic(Listener, &UOpsEventsTestListener::OnBuildRefused);
	if (URoadProfile* Profile = TestWorld.Actor->ResolveProfile())
	{
		Profile->CostPerMetre = 300.0;
	}
	// BROKE: the scenario's opening balance, spent.
	Runtime->GetLedger()->Post(0.0, ELedgerCategory::Upkeep, -(Runtime->GetLedger()->Balance() + 1.0), FText::FromString(TEXT("test")));
	const int32 A = TestWorld.Actor->PlaceNode(FVector2D(0.0, 60000.0));
	const int32 B = TestWorld.Actor->PlaceNode(FVector2D(10000.0, 60000.0));
	TestFalse(TEXT("a taxiway the player cannot pay for is refused"), TestWorld.Actor->ConnectNodes(A, B, ERoadKind::Taxiway, INDEX_NONE));
	Runtime->Tick(0.0);
	TestEqual(TEXT("and the refusal reaches the UI's face of the bus - no longer only a log line"), Listener->CountOf(TEXT("refused:")), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsRuntimeLandRefusedTest, "AirportOps.Present.Alerts.LandRefusalReachesUi",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsRuntimeLandRefusedTest::RunTest(const FString&)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor to attach to"), TestWorld.Actor)) { return false; }
	UOpsRuntime* Runtime = RuntimeBusTestAttach(TestWorld);
	UOpsEventsTestListener* Listener = NewObject<UOpsEventsTestListener>();
	Runtime->GetEvents()->OnLandRefused.AddDynamic(Listener, &UOpsEventsTestListener::OnLandRefused);
	TestEqual(TEXT("no runway, so key 7 is refused"), Runtime->LandNear(FVector2D::ZeroVector, nullptr), EArrivalRefusal::NoRunway);
	Runtime->Tick(0.0);
	TestEqual(TEXT("and the refusal reaches the UI - it never went through Airside's OnArrivalRefused"),
		Listener->CountOf(TEXT("land:")), 1);
	// WITH ITS WORDS (#456 review): the reason alone reads "not admitted to that runway" for an arrivals-only field, so
	// the sentence the refusal was worded with travels with it - here the planner's own for no runway.
	TestEqual(TEXT("carrying the refusal's own sentence"), Listener->LastLandSentence,
		ArrivalPlanner::DescribeRefusal(EArrivalRefusal::NoRunway));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsRuntimeAlertsPassTest, "AirportOps.Present.Alerts.PassRaisesThroughTheRuntime",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsRuntimeAlertsPassTest::RunTest(const FString&)
{
	// THE PASS IS WIRED AND DIRTIED: an overdraft becomes an alert through the runtime, with no hand call to
	// Recompute. Stage 1 dirties the pass on the offer minute among others; stage 3 adds MoneyPosted.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor to attach to"), TestWorld.Actor)) { return false; }
	UOpsRuntime* Runtime = RuntimeBusTestAttach(TestWorld);
	UOpsEventsTestListener* Listener = NewObject<UOpsEventsTestListener>();
	Runtime->GetEvents()->OnAlertRaised.AddDynamic(Listener, &UOpsEventsTestListener::OnAlertRaised);
	Runtime->Tick(0.0);
	// THIS FIELD HAS NO RUNWAY (spec 2026-09-29-ops-batch3 §3): that one alert, and nothing else - no airline is
	// asked whether it can come while the airport is not open, so none is reported unable to.
	const FString NoRunway = TEXT("alert+:") + UEnum::GetValueAsString(EAlertKind::NoRunway);
	TestEqual(TEXT("a solvent runway-less airport raises only NoRunway"), Listener->CountOf(TEXT("alert+:")), 1);
	TestEqual(TEXT("that one"), Listener->CountOf(NoRunway), 1);

	Runtime->GetLedger()->Post(0.0, ELedgerCategory::Upkeep, -(Runtime->GetLedger()->Balance() + 1.0), FText::FromString(TEXT("test")));
	const double OneMinute = UOfferGenerator::TickSeconds / Runtime->GetClock()->TimeScale() * 1.01;
	Runtime->Tick(OneMinute);
	// OVERDRAWN BY NAME: the NoRunway alert above stands beside it.
	const FString Overdrawn = TEXT("alert+:") + UEnum::GetValueAsString(EAlertKind::Overdrawn);
	auto HeldOverdrawn = [Runtime]()
	{
		return Runtime->GetAlerts()->GetAlerts().FilterByPredicate(
			[](const FOpsAlert& A) { return A.Key.Kind == EAlertKind::Overdrawn; }).Num();
	};
	TestEqual(TEXT("overdrawn, the next pass raises it"), Listener->CountOf(Overdrawn), 1);
	TestEqual(TEXT("and the runtime holds it"), HeldOverdrawn(), 1);

	const FString Slot = TEXT("AirportOpsTest_AlertsLoad");
	if (!TestTrue(TEXT("save writes"), Runtime->SaveToSlot(Slot))) { return false; }
	if (!TestTrue(TEXT("load reads"), Runtime->LoadFromSlot(Slot))) { return false; }
	Runtime->GetEvents()->OnAlertsReset.AddDynamic(Listener, &UOpsEventsTestListener::OnAlertsReset);
	if (!TestTrue(TEXT("load reads again"), Runtime->LoadFromSlot(Slot))) { return false; }
	Runtime->Tick(0.0);
	TestEqual(TEXT("a load tells the UI its alert list is stale"), Listener->CountOf(TEXT("reset")), 1);
	TestEqual(TEXT("then re-raises, once, what is still true of the loaded airport (no tick came between the two loads)"), Listener->CountOf(Overdrawn), 2);
	TestEqual(TEXT("and holds it once"), HeldOverdrawn(), 1);
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsRuntimeAcceptDirtiesAlertsTest, "AirportOps.Present.Alerts.AcceptDirtiesAlerts",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsRuntimeAcceptDirtiesAlertsTest::RunTest(const FString&)
{
	// ACCEPT IS A PLAYER COMMAND ON THE BOARD (OfferViewModels calls UFlightBoard::Accept directly), so before
	// FOfferAcceptedEvent it dirtied no pass: a condition about accepted flights waited for something unrelated
	// to re-derive it. Measured as the pass RUNNING, not as a particular alert: no alert kind can be raised or
	// cleared BY an accept today - an accept holds only the stand its plan taxis to (UStandAllocator::Hold), which has
	// a pose, and HeldStandIsGone needs one without - so an alert-shaped assertion here would pass with the
	// subscription deleted.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor to attach to"), TestWorld.Actor)) { return false; }
	// NOT RuntimeBusTestAttach: its road lies along y = 0, where the field below lays its runway.
	TestWorld.Actor->PlaceNode(FVector2D(0.0, 90000.0));
	URoadNetwork* Net = TestWorld.Actor->Network;
	if (!TestNotNull(TEXT("a network"), Net)) { return false; }
	// A FIELD AN ARRIVAL CAN USE - runway, exit, taxiway, stand (#431): an accept is the arrival plan's now, so a strip nothing can land on, or a stand nothing reaches, accepts nothing - and a closed airport, one without a runway too, accepts nothing (ruling I1, 2026-09-30).
	FAirframe Airframe;
	Airframe.Wingspan = 3400.0;
	FTestAirport::Build(Airframe, FTestAirportOptions(), Net);
	UOpsRuntime* Runtime = NewObject<UOpsRuntime>();
	Runtime->Attach(TestWorld.Actor);

	// QUIET FIRST: the attach and the stand just drawn each dirty the pass; let them settle.
	for (int32 Tick = 0; Tick < 3; ++Tick) { Runtime->Tick(0.0); }
	const int32 Settled = Runtime->GetAlerts()->RecomputeCountForTest();
	Runtime->Tick(0.0);
	if (!TestEqual(TEXT("a quiet frame runs no alerts pass - the baseline the accept is measured against"),
		Runtime->GetAlerts()->RecomputeCountForTest(), Settled)) { return false; }

	UFlight* Flight = NewObject<UFlight>(GetTransientPackage());
	Flight->Airframe.Wingspan = 3400.0;
	Flight->AirlineId = TEXT("Cumbria");
	// A LONG LEAD, so the arrival's own clock entry cannot come due inside this test and dirty the pass for a
	// reason that is not the accept (review M3).
	Flight->LeadTimeSeconds = 1.0e7;
	UFlightBoard* Board = Runtime->GetFlightBoard();
	Board->AddOffer(*Runtime->GetClock(), Flight);
	UGroundTraffic* Model = TestWorld.Actor->GetTraffic()->GetModel();
	if (!TestTrue(TEXT("the offer is accepted, as the inbox's Accept button does it"),
		Board->Accept(*Model, *Net, *Runtime->GetClock(), *Flight))) { return false; }
	Runtime->Tick(0.0);
	TestEqual(TEXT("the accept dirtied the alerts pass, which ran once on the next frame (FOfferAcceptedEvent)"),
		Runtime->GetAlerts()->RecomputeCountForTest(), Settled + 1);
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsRuntimeTurnaroundShortfallTest, "AirportOps.Present.Bus.TurnaroundShortfallReachesAirline",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsRuntimeTurnaroundShortfallTest::RunTest(const FString&)
{
	// THE CHAIN THROUGH THE RUNTIME'S OWN WIRING: the job board publishes onto the runtime's bus (Attach), the
	// roster hears TurnaroundEnded in the Reaction tier (WireBus) and resolves the airline through the
	// runtime's flight board. The roster's world-free tests wire a bus by hand and cannot see either line missing.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor to attach to"), TestWorld.Actor)) { return false; }
	UOpsRuntime* Runtime = RuntimeBusTestAttach(TestWorld);
	TestTrue(TEXT("the job board publishes onto the runtime's bus - set by Attach"), Runtime->GetJobBoard()->Bus == &Runtime->GetBus());

	const FName Airline = TEXT("BusTestShortfallAirline");
	Runtime->GetAirlines()->Ensure(Airline);
	UFlight* Flight = NewObject<UFlight>(GetTransientPackage());
	Flight->AirlineId = Airline;
	Flight->AgentId = 7;
	Flight->SetPhaseForTest(EFlightPhase::TaxiOut);
	Runtime->GetFlightBoard()->AddOffer(*Runtime->GetClock(), Flight);
	const double Before = Runtime->GetAirlines()->Find(Airline)->Satisfaction;
	// A ZERO PENALTY WOULD MAKE THE ASSERTION BELOW TRUE WITH NOTHING WIRED (review M4).
	if (!TestTrue(TEXT("the scenario's shortfall penalty is a real cost"), Runtime->GetAirlines()->Tuning.ShortfallPenalty > 0.0)) { return false; }

	FTurnaroundEndedEvent Event;
	Event.AircraftAgentId = 7;
	Event.Outcome = EFuelOutcome::Unfuelled;
	Event.Wanted = 2500.0;
	Runtime->GetBus().Publish(MoveTemp(Event));
	Runtime->Tick(0.0);
	TestEqual(TEXT("an unfuelled departure costs the flight's airline the scenario's shortfall penalty"),
		Runtime->GetAirlines()->Find(Airline)->Satisfaction, Before - Runtime->GetAirlines()->Tuning.ShortfallPenalty, 1e-9);
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsRuntimeDetachUnhooksTest, "AirportOps.Present.Bus.DetachUnhooksEveryPublisher",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsRuntimeDetachUnhooksTest::RunTest(const FString&)
{
	// EVERY SUBOBJECT ATTACH GAVE THE BUS, TAKEN BACK (review M7): a publisher left pointing at the bus after a
	// detach publishes into whatever comes next - the stage 3 review's reason, which the roster had escaped. THE LIST IS
	// UOpsRuntime::Publishers() (#445), the one Attach and Detach loop over - this test walked its own six by hand, and the seventh
	// (UFacilityPurchases) was in none of them: Check-Architecture rule 71 holds that the list names every class with a Bus field.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor to attach to"), TestWorld.Actor)) { return false; }
	UOpsRuntime* Runtime = RuntimeBusTestAttach(TestWorld);
	const TArray<UOpsRuntime::FOpsBusPublisher> Publishers = Runtime->Publishers();
	if (!TestTrue(TEXT("the runtime lists its publishers"), Publishers.Num() > 0)) { return false; }
	for (const UOpsRuntime::FOpsBusPublisher& Each : Publishers)
	{
		TestTrue(FString::Printf(TEXT("attached, the %s publishes onto the runtime's bus"), Each.Name), *Each.Slot == &Runtime->GetBus());
	}
	// THE TWO THE OLD HAND LISTS LEFT OUT, by name: a list that iterates cannot see a member missing from it.
	TestTrue(TEXT("the facility purchases are on the list (the constructor set theirs, and nothing ever cleared it)"),
		Publishers.ContainsByPredicate([Runtime](const UOpsRuntime::FOpsBusPublisher& Each) { return Each.Slot == &Runtime->GetFacilityPurchases()->Bus; }));
	TestTrue(TEXT("and the offer generator, which announces an airline's verdict"),
		Publishers.ContainsByPredicate([Runtime](const UOpsRuntime::FOpsBusPublisher& Each) { return Each.Slot == &Runtime->GetOfferGenerator()->Bus; }));

	AddExpectedMessagePlain(TEXT("OpsRuntime attached to nothing"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	Runtime->Attach(nullptr);
	for (const UOpsRuntime::FOpsBusPublisher& Each : Runtime->Publishers())
	{
		TestNull(*FString::Printf(TEXT("the %s's bus is cleared by a detach"), Each.Name), *Each.Slot);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsRuntimeLoadFromHandlerTest, "AirportOps.Present.Bus.LoadFromAHandlerIsRefused",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsRuntimeLoadFromHandlerTest::RunTest(const FString&)
{
	// SaveFromAHandler's twin (#445): a Blueprint bound to a UOpsEvents delegate may LOAD from inside the drain, which would Discard the
	// queue while Drain still held its moved-out batch - the rest of it, events naming the agents of the airport being replaced, then
	// dispatched against the restored boards. Refused, said, and the rest of the batch still delivered.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor to attach to"), TestWorld.Actor)) { return false; }
	UOpsRuntime* Runtime = RuntimeBusTestAttach(TestWorld);
	const FString Slot = TEXT("AirportOpsTest_BusLoadFromHandler");
	if (!TestTrue(TEXT("setup: a save to load"), Runtime->SaveToSlot(Slot))) { return false; }
	Runtime->Tick(0.0);

	UOpsEventsTestListener* Listener = NewObject<UOpsEventsTestListener>();
	Listener->LoadOnNote = Runtime;
	Listener->LoadSlot = Slot;
	Runtime->GetEvents()->OnNotification.AddDynamic(Listener, &UOpsEventsTestListener::OnNoteLoad);
	const int32 PhasesBefore = RuntimeBusTestPhaseCount(*Runtime);

	// THE LOAD'S TRIGGER, THEN AN EVENT BEHIND IT in the same batch - the one a discard would have dropped.
	Runtime->GetBus().Publish(FNotificationEvent{ TEXT("autoload") });
	Runtime->GetBus().Publish(FAgentPhaseEvent{ OpsTestTransition(11, EAgentPhase::Taxiing, EAgentPhase::Parked, EAgentEvent::Parked) });
	AddExpectedMessagePlain(TEXT("refused: called from inside an ops event handler"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	Runtime->Tick(0.0);

	TestTrue(TEXT("the handler ran and asked for a load"), Listener->bLoadAnswered);
	TestFalse(TEXT("LoadFromSlot refused it from inside the drain"), Listener->bLoadedFromHandler);
	TestEqual(TEXT("and the rest of the batch was still dispatched, not handed to restored boards"),
		RuntimeBusTestPhaseCount(*Runtime), PhasesBefore + 1);
	TestTrue(TEXT("CONTROL: the same load from outside a drain works"), Runtime->LoadFromSlot(Slot));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsRuntimeWiringOrderTest, "AirportOps.Present.Bus.WiringOrderIsDeclared",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsRuntimeWiringOrderTest::RunTest(const FString&)
{
	// THE ORDERINGS THAT CARRY CORRECTNESS, READ OFF THE RUNTIME (#445), by name. They were line order in WireBus: the job board's pass
	// before the arrival queue's before the alerts' (the FlightCannotLand alert reads the clearance the queue pass computes, in the same
	// round), and the job board's handler of a phase event before the flight board's ("the service first"). A reordered line was a
	// silent change. The passes declare what they run After; this pins the result, and the Sim subscribers of the one event every
	// board hears.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor to attach to"), TestWorld.Actor)) { return false; }
	UOpsRuntime* Runtime = RuntimeBusTestAttach(TestWorld);
	const FOpsEventBus& Bus = Runtime->GetBus();
	const TArray<FName> Expected = { TEXT("FleetSeed"), TEXT("ModuleRepair"), TEXT("JobBoard"), TEXT("ArrivalQueue"), TEXT("Alerts") };
	TestEqual(TEXT("the passes run in this order: seeding and repair, then the boards, then what reads them"), Bus.PassOrder(), Expected);
	TestEqual(TEXT("the job board runs After the fleet seeding and the module repair, which put vehicles and pumps under it"),
		Bus.PassesAfter(TEXT("JobBoard")), TArray<FName>({ TEXT("FleetSeed"), TEXT("ModuleRepair") }));
	TestEqual(TEXT("the arrival queue runs After the job board"), Bus.PassesAfter(TEXT("ArrivalQueue")), TArray<FName>({ TEXT("JobBoard") }));
	TestEqual(TEXT("the alerts run After both boards' passes, so a clearance and the alert it makes land in one round"),
		Bus.PassesAfter(TEXT("Alerts")), TArray<FName>({ TEXT("JobBoard"), TEXT("ArrivalQueue") }));

	// THE SUBSCRIBERS OF THE EVENT EVERY BOARD HEARS, in the order they run - tiers first (Sim, then Reaction), registration within a tier.
	// No Presentation subscriber: the phase's UOpsEvents face was cut (#445), nothing listened.
	const TArray<FName> Phase = { TEXT("JobBoard"), TEXT("FlightBoard"), TEXT("ArrivalQueue"), TEXT("Alerts") };
	TestEqual(TEXT("an agent-phase event is handled by the job board, then the flight board, then the arrival queue (Sim), then the alerts (Reaction)"),
		Bus.SubscribersOf(FOpsEvent::IndexOfType<FAgentPhaseEvent>()), Phase);
	return true;
}

namespace
{
	/** RuntimeE2E: a guideline line in the actor's network, the fuel fixture's shape. Prefixed: unity build. */
	void RuntimeE2ELayLine(URoadNetwork& Net, const FVector2D& From, const FVector2D& To,
		FGuidelineNodeId& OutA, FGuidelineNodeId& OutB)
	{
		OutA = Net.AddGuidelineNode(From);
		OutB = Net.AddGuidelineNode(To);
		FGuidelineEdge Edge;
		Edge.A = OutA;
		Edge.B = OutB;
		Edge.Control = (From + To) * 0.5;
		Edge.AllowedTraffic = FTrafficMask::Only(ETraversalClass::Aircraft);
		Edge.AllowedTraffic.Add(ETraversalClass::Emergency);
		Edge.Direction = EGuidelineDir::Bidirectional;
		Edge.Width = 600.0;
		Edge.bDerived = true;
		Net.AddGuidelineEdge(MoveTemp(Edge));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsRuntimeUnfuelledDepartureTest, "AirportOps.Present.Bus.UnfuelledDepartureLowersAirline",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsRuntimeUnfuelledDepartureTest::RunTest(const FString&)
{
	// END TO END, NOTHING PUBLISHED BY HAND (review M1): an aircraft parks at a field with no fuel depot, its
	// turnaround runs out, the job board sends it, and its flight's airline minds. What this proves that no
	// other test does: the job board's publish, from inside a drain, reaches the roster while the flight is
	// still the agent's - the order of publication within the drain, not any tier, is what keeps it so.
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("an actor"), Actor)) { return false; }
	Actor->PlaceNode(FVector2D(0.0, 60000.0));
	if (!TestNotNull(TEXT("a network"), Actor->Network.Get())) { return false; }
	URoadNetwork& Net = *Actor->Network;

	// THE FUEL FIXTURE'S AIRPORT (FuelServiceTest's FFuelFixture::Build + LayRunway), minus the depot: a
	// north-south taxiway, a runway north of it, a stand facing +X whose pose ray meets the taxiway.
	FGuidelineNodeId TaxiSouth, TaxiNorth;
	RuntimeE2ELayLine(Net, FVector2D(-10000.0, -10000.0), FVector2D(-10000.0, 10000.0), TaxiSouth, TaxiNorth);
	URoadProfile* Strip = TestProfiles::Runway();
	const FRoadNodeId West = Net.AddNode(FVector2D(-50000.0, 20000.0));
	const FRoadNodeId Mid = Net.AddNode(FVector2D(-10000.0, 20000.0));
	const FRoadNodeId East = Net.AddNode(FVector2D(50000.0, 20000.0));
	Net.AddStraightSegment(West, Mid, Strip);
	Net.AddStraightSegment(Mid, East, Strip);
	const FGuidelineNodeId OnStrip = Net.AddGuidelineNode(FVector2D(-10000.0, 20000.0), false);
	{
		FGuidelineEdge ToStrip;
		ToStrip.A = TaxiNorth;
		ToStrip.B = OnStrip;
		ToStrip.Control = FVector2D(-10000.0, 15000.0);
		ToStrip.AllowedTraffic = FTrafficMask::Only(ETraversalClass::Aircraft);
		ToStrip.AllowedTraffic.Add(ETraversalClass::Emergency);
		ToStrip.Direction = EGuidelineDir::Bidirectional;
		ToStrip.Width = 600.0;
		ToStrip.bDerived = true;
		Net.AddGuidelineEdge(MoveTemp(ToStrip));
	}
	UEntityDefinition* StandDef = UEntityDefinition::MakeStandTransient();
	const FEntityInstanceId Stand = Net.PlaceEntity(StandDef, StandDef->Anchors, FVector2D(0.0, 0.0), 0.0,
		3600.0, StandDef->PoseRole, StandDef->Trucks);
	// THE HAND-LAID LINES ARE THIS FIELD'S GUIDELINE GRAPH: PlaceNode above derived one, and the runway segments
	// added since would otherwise read as a road the graph is behind - DepartAgent then refuses
	// GraphBeingEdited for ever (URoadNetwork::AreGuidelinesBehindRoad), which is a drag, not this test.
	// THE DERIVATION'S TAIL (#438) links the stand and stamps the graph; was FAnchorLink::Build + MarkGuidelinesDerived.
	TestGraph::Link(Net);

	UOpsRuntime* Runtime = NewObject<UOpsRuntime>();
	Runtime->Attach(Actor);
	const FName Airline = TEXT("BusTestUnfuelledAirline");
	Runtime->GetAirlines()->Ensure(Airline);

	const FRoutePlan Plan = TestGraph::Probe(Net, TaxiSouth, Net.GetEntity(Stand)->PoseNode, ETraversalClass::Aircraft);
	if (!TestTrue(TEXT("the aircraft routes to the stand"), Plan.IsValid())) { return false; }
	FAirframe Airframe = UAirsideSettings::ResolveDefaultAirframe();
	Airframe.TurnaroundSeconds = 60.0;
	if (!TestTrue(TEXT("and dispatches"), Actor->DispatchAgent(Plan, Airframe))) { return false; }
	const int32 Aircraft = Actor->GetTraffic()->GetNewestAgentId();

	// ITS FLIGHT, flown by that agent before it parks - what the roster resolves the airline through.
	UFlight* Flight = NewObject<UFlight>(GetTransientPackage());
	Flight->AirlineId = Airline;
	Flight->AgentId = Aircraft;
	Flight->SetPhaseForTest(EFlightPhase::TaxiIn);
	Flight->FuelLitres = 2000.0;
	Flight->Airframe = Airframe;
	Runtime->GetFlightBoard()->AddOffer(*Runtime->GetClock(), Flight);

	constexpr float Step = 1.0f / 30.0f;
	const FAirlineStanding* Row = Runtime->GetAirlines()->Find(Airline);
	bool bLeft = false;
	for (int32 Tick = 0; Tick < 30 * 120 && Row->Recent.Num() == 0; ++Tick)
	{
		Actor->Tick(Step);
		Runtime->Tick(Step);
		const FRoadAgent* Agent = Actor->GetTraffic()->GetModel()->FindAgent(Aircraft);
		bLeft |= Agent == nullptr || (Agent->Phase != EAgentPhase::Parked && Agent->Phase != EAgentPhase::Taxiing);
	}
	TestTrue(TEXT("the aircraft left its stand on its own"), bLeft);
	if (!TestEqual(TEXT("and its airline heard one thing"), Row->Recent.Num(), 1)) { return false; }
	TestEqual(TEXT("that it left unfuelled - the depot-less field could not serve it"), Row->Recent[0].Cause, FString(TEXT("left unfuelled")));
	TestEqual(TEXT("costing the whole shortfall penalty"), Row->Recent[0].Delta, -Runtime->GetAirlines()->Tuning.ShortfallPenalty, 1e-9);
	return true;
}

// ---------------------------------------------------------------------------------------------------------
// #405 (spec 2026-09-29-ops-batch3 §4): a flight enters Turnaround only when its aeroplane parks AT A STAND.

namespace
{
	/**
	 * Airside.Model.Traffic.StandRetarget's scenario with the two boards listening: a two-stand field, a flight
	 * dispatched through the board, both boards on a bus fed by OnAgentPhaseChanged and drained after each
	 * Advance - so the Parked event arrives a step late, as it does in production. Prefixed: unity build.
	 */
	struct FFallbackParkRig
	{
		FTestAirport Field;
		UGroundTraffic* Traffic = nullptr;
		USimClock* Clock = nullptr;
		UFlightBoard* Board = nullptr;
		UJobBoard* Jobs = nullptr;
		/** The airline the flight flies for, scoring TurnaroundEnded as the runtime wires it (whole-stack review M4). */
		UAirlineRoster* Airlines = nullptr;
		FOpsEventBus Bus;
		TArray<FTurnaroundEndedEvent> Ended;
		UFlight* Flight = nullptr;
		int32 Agent = 0;
		/** Every flight phase the board showed, one entry per drain that changed it. */
		TArray<EFlightPhase> Seen;

		FFallbackParkRig()
		{
			const FAirframe Airframe = UAirsideSettings::ResolveDefaultAirframe();
			FTestAirportOptions Options;
			Options.StandCount = 2;
			Field = FTestAirport::Build(Airframe, Options);
			Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
			Clock = NewObject<USimClock>(GetTransientPackage());
			Board = NewObject<UFlightBoard>(GetTransientPackage());
			Board->Allocator = NewObject<UStandAllocator>(GetTransientPackage());
			Jobs = NewObject<UJobBoard>(GetTransientPackage());
			Jobs->Bus = &Bus;
			Airlines = NewObject<UAirlineRoster>(GetTransientPackage());
			Airlines->Ensure(TEXT("FallbackTestAirline"));
			URoadNetwork* Net = Field.Net;
			UGroundTraffic* Model = Traffic;
			Board->Dispatcher = [Model, Net](const FVector2D& Near, const FAirframe& Frame)
			{
				return Model->DispatchArrival(*Net, Near, Frame, 1.0) != 0;
			};
			// JOB BOARD FIRST, the order WireBus subscribes them in.
			Bus.BeginWiring();
			Bus.Subscribe<FAgentPhaseEvent>(EOpsTier::Sim, TEXT("JobBoard"), [this](const FAgentPhaseEvent& E)
			{
				Jobs->OnAgentPhase(*Traffic, *Field.Net, *Clock, E);
			});
			Bus.Subscribe<FAgentPhaseEvent>(EOpsTier::Sim, TEXT("FlightBoard"), [this](const FAgentPhaseEvent& E)
			{
				Board->OnAgentPhase(*Field.Net, *Clock, E);
			});
			Bus.Subscribe<FTurnaroundEndedEvent>(EOpsTier::Reaction, TEXT("test"), [this](const FTurnaroundEndedEvent& E) { Ended.Add(E); });
			Bus.Subscribe<FTurnaroundEndedEvent>(EOpsTier::Reaction, TEXT("Airlines"),
				[this](const FTurnaroundEndedEvent& E) { Airlines->OnTurnaroundEnded(E, Board); });
			Bus.EndWiring();
			Traffic->OnAgentPhaseChanged.AddLambda([this](const FAgentTransition& Transition)
			{
				Bus.Publish(FAgentPhaseEvent{ Transition });
			});

			Flight = NewObject<UFlight>(GetTransientPackage());
			Flight->Airframe = Airframe;
			Flight->RunwayPreference = Field.Threshold;
			Flight->AirlineId = TEXT("FallbackTestAirline");
			Board->AddOffer(*Clock, Flight);
		}

		/** Accepted, cleared and dispatched; false if any step refused. */
		bool Land()
		{
			if (!Board->Accept(*Traffic, *Field.Net, *Clock, *Flight))
			{
				return false;
			}
			Clock->Advance(1.0);
			Board->TickQueue(*Traffic, *Field.Net, *Clock);
			Agent = Flight->AgentId;
			Drain();
			return Flight->GetPhase() == EFlightPhase::Landing && Agent != INDEX_NONE;
		}

		void Drain()
		{
			Bus.Drain();
			if (Seen.Num() == 0 || Seen.Last() != Flight->GetPhase())
			{
				Seen.Add(Flight->GetPhase());
			}
		}

		const FRoadAgent* Aircraft() const { return Traffic->FindAgent(Agent); }

		/** Advance and drain until Pred or Seconds of sim time; then one more drain for the late event. !bDrain leaves
		 *  every event queued - the frame a same-frame redirect happens in. */
		template <typename P>
		bool RunUntil(double Seconds, P Pred, bool bDrain = true)
		{
			for (double T = 0.0; T < Seconds; T += 0.05)
			{
				Traffic->Advance(0.05, Field.Net);
				if (bDrain)
				{
					Drain();
				}
				if (Pred())
				{
					if (bDrain)
					{
						Drain();
					}
					return true;
				}
			}
			return false;
		}

		/** Both stands deleted mid-arrival, then parked where it waits: the fallback junction. !bDrain: the Parked
		 *  (and the taxi before it) left unheard. */
		bool ParkOnFallback(bool bDrain = true)
		{
			Traffic->Advance(0.05, Field.Net);
			Drain();
			for (const FEntityInstanceId Stand : Field.Stands)
			{
				Field.Net->RemoveEntity(Stand);
			}
			TestGraph::Rebuild(*Field.Net);
			Traffic->OnGraphRebuilt(*Field.Net);
			Drain();
			const FRoadAgent* P = Aircraft();
			return P != nullptr && P->bAwaitingStand
				&& RunUntil(600.0, [this]() { const FRoadAgent* A = Aircraft(); return A != nullptr && A->Phase == EAgentPhase::Parked; }, bDrain);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFallbackParkStaysTaxiInTest, "AirportOps.Model.Bus.FallbackParkStaysTaxiIn",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFallbackParkStaysTaxiInTest::RunTest(const FString&)
{
	// #405: parked on the fallback junction (no stand left), the flight read Turnaround, so the re-offer's
	// redirect (Parked -> Taxiing) read TaxiOut on its way IN. It is still taxiing in until it reaches a stand.
	FFallbackParkRig Rig;
	if (!TestTrue(TEXT("the flight lands"), Rig.Land())) { return false; }
	if (!TestTrue(TEXT("and parks on the fallback junction, both stands gone"), Rig.ParkOnFallback())) { return false; }
	TestEqual(TEXT("parked on the fallback junction it is still taxiing in"), Rig.Flight->GetPhase(), EFlightPhase::TaxiIn);
	TestNull(TEXT("no turnaround opens at a junction"), Rig.Jobs->TurnaroundFor(Rig.Agent));
	TestEqual(TEXT("and its parking clock has not started"), Rig.Flight->ParkedAt, 0.0);

	// A STAND IS BUILT: the rebuild re-offers it and the aeroplane goes.
	UEntityDefinition* StandDef = UEntityDefinition::MakeStandTransient();
	const FEntityInstanceId NewStand = Rig.Field.Net->PlaceEntity(StandDef, StandDef->Anchors, Rig.Field.ExitAt + FVector2D(9000.0, -10000.0), 0.0);
	TestGraph::Rebuild(*Rig.Field.Net);
	Rig.Traffic->OnGraphRebuilt(*Rig.Field.Net);
	Rig.Traffic->Advance(0.05, Rig.Field.Net);   // the re-offer runs at the end of a tick
	Rig.Drain();
	if (!TestEqual(TEXT("redirected: taxiing again"), Rig.Aircraft()->Phase, EAgentPhase::Taxiing)) { return false; }
	TestEqual(TEXT("the redirect reads TaxiIn - on its way in, not out"), Rig.Flight->GetPhase(), EFlightPhase::TaxiIn);
	TestEqual(TEXT("and no TurnaroundEnded was published for the junction"), Rig.Ended.Num(), 0);

	if (!TestTrue(TEXT("it parks at the new stand"), Rig.RunUntil(600.0, [&Rig, NewStand]()
		{ const FRoadAgent* A = Rig.Aircraft(); return A != nullptr && A->Phase == EAgentPhase::Parked && A->GoalNode == Rig.Field.Pose(NewStand); })))
	{
		return false;
	}
	TestEqual(TEXT("AT A STAND it is the turnaround"), Rig.Flight->GetPhase(), EFlightPhase::Turnaround);
	TestTrue(TEXT("and its parking clock starts there"), Rig.Flight->ParkedAt > 0.0);
	TestTrue(TEXT("on the stand it actually reached"), Rig.Flight->Stand == NewStand);
	TestNotNull(TEXT("and the job board opened its turnaround there"), Rig.Jobs->TurnaroundFor(Rig.Agent));
	TestFalse(TEXT("TaxiOut was never shown on the way in"), Rig.Seen.Contains(EFlightPhase::TaxiOut));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDepartFromFallbackTest, "AirportOps.Model.Bus.DepartFromFallbackReadsTaxiOut",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FDepartFromFallbackTest::RunTest(const FString&)
{
	// THE MIRROR OF #405's FIX: an aeroplane parked on the fallback junction never reached Turnaround, and the
	// inspector's Depart (bCanDepart is "Parked") sends it off from there. Its taxi is OUT, though the flight never
	// passed Turnaround - which is the one fact the TaxiIn/TaxiOut choice used to read.
	//
	// DepartAgent's STRAIGHT-OUT BRANCH, staged: that branch is RedirectAgent onto DeparturePlanner::PlanAny's route
	// from the goal node. On this fixture the parked heading points away from the runway, so DepartAgent itself
	// takes the pushback branch (refused here: no arm) - and a pushback enters Manoeuvring, which maps absolutely
	// and needs no rule. The straight-out move is the one whose Parked -> Taxiing the board must read as OUT.
	FFallbackParkRig Rig;
	if (!TestTrue(TEXT("the flight lands"), Rig.Land())) { return false; }
	if (!TestTrue(TEXT("and parks on the fallback junction"), Rig.ParkOnFallback())) { return false; }
	const int32 Before = Rig.Seen.Num();
	const double Satisfaction = Rig.Airlines->Find(TEXT("FallbackTestAirline"))->Satisfaction;
	// THE FLIGHT'S OWN FIGURE, as the runtime wires it (UOpsRuntime::Attach: the offer's FuelLitres) - and one no
	// default could produce, so a site that fell back to DefaultLitres reads wrong (whole-stack re-review m1).
	constexpr double OfferedLitres = 1234.0;
	Rig.Jobs->LitresOwedFor = [](int32, const FAirframe&) { return OfferedLitres; };
	const FRoadAgent* Parked = Rig.Aircraft();
	const FDeparturePlan Plan = DeparturePlanner::PlanAny(*Rig.Field.Net, Parked->GoalNode, *Parked->AsAircraft(), Parked->Class,
		&Rig.Traffic->GetOccupancy());
	if (!TestTrue(TEXT("a departure plans from the junction"), Plan.IsValid())) { return false; }
	// WITH THE CAUSE THE BRANCH GIVES IT - DepartOrdered (Airside.Model.PushbackStraightOut pins that it does). This stage
	// used to redirect with none: the board then asked the live agent whether it was armed for a runway (#436).
	if (!TestTrue(TEXT("and it drives straight out onto it"),
		Rig.Traffic->RedirectAgent(Rig.Agent, Rig.Field.Net, Plan.Route, EAgentEvent::DepartOrdered))) { return false; }
	Rig.Drain();
	// READ STRAIGHT AFTER THE MOVE, not only from Seen: Seen records CHANGES, and a flight left reading TaxiIn
	// (the phase it waited in) would add nothing to it.
	TestEqual(TEXT("the taxi away from the junction reads TaxiOut"), Rig.Flight->GetPhase(), EFlightPhase::TaxiOut);
	Rig.RunUntil(600.0, [&Rig]() { return Rig.Flight->GetPhase() == EFlightPhase::Departing; });
	const TArray<EFlightPhase> After(Rig.Seen.GetData() + Before, Rig.Seen.Num() - Before);
	AddInfo(FString::Printf(TEXT("phases after the depart: %s"),
		*FString::JoinBy(After, TEXT(", "), [](EFlightPhase P) { return UEnum::GetValueAsString(P); })));
	TestFalse(TEXT("departing from the junction never reads TaxiIn"), After.Contains(EFlightPhase::TaxiIn));
	TestEqual(TEXT("and it goes"), Rig.Flight->GetPhase(), EFlightPhase::Departing);

	// NEVER TURNED AROUND, SO NEVER FUELLED (whole-stack review M4, ruling 2026-09-30): it leaves Unfuelled, owed what
	// its flight was offered at (LitresOwedFor, wired above), and its airline scores the shortfall - once, as a real
	// turnaround's would be.
	const double Owed = OfferedLitres;
	if (!TestNotEqual(TEXT("the offered figure is not the default - or this cannot tell them apart"),
		Owed, UJobBoard::DefaultLitres(Rig.Flight->Airframe))) { return false; }
	if (TestEqual(TEXT("one TurnaroundEnded for the departure"), Rig.Ended.Num(), 1))
	{
		TestEqual(TEXT("Unfuelled"), Rig.Ended[0].Outcome, EFuelOutcome::Unfuelled);
		TestEqual(TEXT("nothing delivered"), Rig.Ended[0].Delivered, 0.0);
		TestEqual(TEXT("owed what the flight was owed"), Rig.Ended[0].Wanted, Owed, 1e-6);
		TestEqual(TEXT("for this aircraft"), Rig.Ended[0].AircraftAgentId, Rig.Agent);
	}
	TestEqual(TEXT("and its airline minds: the whole shortfall penalty"),
		Rig.Airlines->Find(TEXT("FallbackTestAirline"))->Satisfaction, Satisfaction - Rig.Airlines->Tuning.ShortfallPenalty, 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSameFrameRedirectTest, "AirportOps.Model.Bus.SameFrameRedirectStaysTaxiIn",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FSameFrameRedirectTest::RunTest(const FString&)
{
	// REVIEW M7: the Parked event is heard a step late, and ReofferStands may have redirected the aeroplane to a
	// freed stand before it is - so its LIVE goal IS a stand while it is taxiing. The flight must not enter Turnaround
	// from that stale Parked. Since #436 the boards decide on the event's GoalAtEvent - the junction it parked on -
	// and this is the half of that rule FallbackParkStaysTaxiIn cannot reach: a board that read the live goal instead
	// (as the removed StillParked check once had to guard) opens the turnaround this test forbids.
	FFallbackParkRig Rig;
	if (!TestTrue(TEXT("the flight lands"), Rig.Land())) { return false; }
	if (!TestTrue(TEXT("and parks on the fallback junction, its Parked not yet heard"), Rig.ParkOnFallback(/*bDrain=*/false))) { return false; }

	UEntityDefinition* StandDef = UEntityDefinition::MakeStandTransient();
	Rig.Field.Net->PlaceEntity(StandDef, StandDef->Anchors, Rig.Field.ExitAt + FVector2D(9000.0, -10000.0), 0.0);
	TestGraph::Rebuild(*Rig.Field.Net);
	Rig.Traffic->OnGraphRebuilt(*Rig.Field.Net);
	Rig.Traffic->Advance(0.05, Rig.Field.Net);   // the re-offer, in the frame the Parked was queued in
	if (!TestEqual(TEXT("redirected before the Parked is heard"), Rig.Aircraft()->Phase, EAgentPhase::Taxiing)) { return false; }
	if (!TestTrue(TEXT("its goal is the new stand - what a stale Parked would read"), StandAtGoalForTest(*Rig.Field.Net, *Rig.Aircraft()).IsSet())) { return false; }
	Rig.Drain();
	TestEqual(TEXT("a stale Parked moves no flight into Turnaround"), Rig.Flight->GetPhase(), EFlightPhase::TaxiIn);
	TestFalse(TEXT("Turnaround was never shown"), Rig.Seen.Contains(EFlightPhase::Turnaround));
	TestNull(TEXT("and no turnaround opened"), Rig.Jobs->TurnaroundFor(Rig.Agent));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRedirectStandGoesTest, "AirportOps.Model.Bus.RedirectStaysTaxiInWhenItsStandGoes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FRedirectStandGoesTest::RunTest(const FString&)
{
	// REVIEW M1: the re-offer redirects the aeroplane to a new stand, and that stand is deleted before the Parked ->
	// Taxiing is heard. Its goal is then no stand - which a "goal is not a stand, so it is departing" rule read as
	// TaxiOut. It is not armed for a departure; it is still taxiing in.
	FFallbackParkRig Rig;
	if (!TestTrue(TEXT("the flight lands"), Rig.Land())) { return false; }
	if (!TestTrue(TEXT("and parks on the fallback junction"), Rig.ParkOnFallback())) { return false; }

	UEntityDefinition* StandDef = UEntityDefinition::MakeStandTransient();
	const FEntityInstanceId NewStand = Rig.Field.Net->PlaceEntity(StandDef, StandDef->Anchors, Rig.Field.ExitAt + FVector2D(9000.0, -10000.0), 0.0);
	TestGraph::Rebuild(*Rig.Field.Net);
	Rig.Traffic->OnGraphRebuilt(*Rig.Field.Net);
	Rig.Traffic->Advance(0.05, Rig.Field.Net);   // the re-offer; its Parked -> Taxiing is queued, not heard
	if (!TestEqual(TEXT("redirected"), Rig.Aircraft()->Phase, EAgentPhase::Taxiing)) { return false; }
	Rig.Field.Net->RemoveEntity(NewStand);
	TestGraph::Rebuild(*Rig.Field.Net);
	Rig.Traffic->OnGraphRebuilt(*Rig.Field.Net);
	if (!TestFalse(TEXT("its goal is no stand when the event is heard, or this proves nothing"), StandAtGoalForTest(*Rig.Field.Net, *Rig.Aircraft()).IsSet())) { return false; }
	TestFalse(TEXT("and it is not armed for a departure"), Rig.Aircraft()->bDepartureArmed);
	Rig.Drain();
	TestEqual(TEXT("a redirect whose stand went is still the taxi in"), Rig.Flight->GetPhase(), EFlightPhase::TaxiIn);
	TestFalse(TEXT("TaxiOut was never shown"), Rig.Seen.Contains(EFlightPhase::TaxiOut));
	return true;
}

/**
 * #426 ROW 7 AND THE #425 REVIEW'S AUDIT OF THE "Network" BLOB: A SAVE-GAME LOAD GETS EVERY REPAIR A LEVEL LOAD GETS.
 * The load's own comment said it replicated "THE LOAD-TIME REPAIRS A LEVEL GETS FROM PostLoad AND
 * PostRegisterAllComponents" and ran two of the four: no EnsureStandNumbers, no RefreshResolvedAnchors. Now both go
 * through ARoadNetworkActor::RepairLoadedNetwork, so this measures each repair across one real LoadFromSlot.
 *
 * THE PROFILE: a default-width taxiway is laid with the actor's own fallback profile, which ResolveProfile makes in the
 * TRANSIENT package - and DefaultProfile names the same object. OpsSave writes both as a PATH, which a later session
 * resolves to nothing (then ProfileFor falls back, correctly) or to whatever same-named transient object that process
 * happens to hold - a previous PIE's actor's, or a test's. A LEVEL save writes such a reference as null; the repair makes a
 * save game agree, so the taxiway follows THIS session's default. Simulated here by the level's default changing
 * between the save and the load, which leaves the saved path resolving to a profile the actor no longer uses.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRuntimeLoadRunsEveryRepairTest, "AirportOps.Present.RuntimeLoad.RunsEveryLoadRepair",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FRuntimeLoadRunsEveryRepairTest::RunTest(const FString&)
{
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("an actor to attach to"), Actor)) { return false; }
	const int32 A = Actor->PlaceNode(FVector2D(0.0, 0.0));
	const int32 B = Actor->PlaceNode(FVector2D(20000.0, 0.0));
	Actor->ConnectNodes(A, B);
	URoadNetwork* Net = Actor->Network;
	const URoadProfile* SavedDefault = Actor->ResolveProfile();
	const FRoadSegment* Laid = Net->GetSegments().FindByPredicate([](const FRoadSegment& S) { return S.bAlive; });
	if (!TestNotNull(TEXT("a taxiway was laid"), Laid)
		|| !TestTrue(TEXT("with the actor's fallback profile, which lives in the transient package - or the profile check proves nothing"),
			Laid->Profile == SavedDefault && SavedDefault != nullptr && SavedDefault->IsIn(GetTransientPackage())))
	{
		return false;
	}

	// A STAND SAVED UNNUMBERED, as one from before 2026-09-29 is.
	UEntityDefinition* StandDefinition = UEntityDefinition::MakeStandTransient();
	const FEntityInstanceId Stand = Net->PlaceEntity(StandDefinition, StandDefinition->Anchors, FVector2D(-40000.0, 40000.0), 0.0);
	FRoadNetworkTestAccess(*Net).ClearStandNumbersForTest();
	// AN ENTITY WHOSE DEFINITION IS RE-AUTHORED AFTER THE SAVE.
	UEntityDefinition* Kit = NewObject<UEntityDefinition>(GetTransientPackage());
	Kit->PoseRole = EServiceRole::Baggage;
	FEntityAnchor Belt;
	Belt.Id = TEXT("belt");
	Belt.Role = EServiceRole::Baggage;
	Belt.LocalHeading = 1.0;
	Kit->Anchors.Add(Belt);
	const FEntityInstanceId Placed = Net->PlaceEntity(Kit, Kit->Anchors, FVector2D(40000.0, 40000.0), 0.0, 0.0, EServiceRole::Baggage);
	if (!TestTrue(TEXT("both entities placed"), Stand.IsSet() && Placed.IsSet())
		|| !TestEqual(TEXT("and the stand really is unnumbered"), Net->GetEntity(Stand)->StandNumber, 0))
	{
		return false;
	}

	UOpsRuntime* Runtime = NewObject<UOpsRuntime>();
	Runtime->Attach(Actor);
	const FString Slot = TEXT("AirportOpsTest_LoadRepairs");
	if (!TestTrue(TEXT("save writes"), Runtime->SaveToSlot(Slot))) { return false; }

	// BETWEEN THE SESSIONS.
	Kit->Anchors[0].LocalHeading = 2.0;
	URoadProfile* NewDefault = TestProfiles::Taxiway();
	Actor->Profile = NewDefault;
	if (!TestTrue(TEXT("load reads"), Runtime->LoadFromSlot(Slot))) { return false; }

	const FEntityInstance* LoadedStand = Actor->Network->GetEntity(Stand);
	TestTrue(TEXT("EnsureStandNumbers ran: the unnumbered stand has a number"),
		LoadedStand != nullptr && LoadedStand->StandNumber > 0);
	double Heading = -1.0;
	TestTrue(TEXT("the re-authored entity's anchor resolves"), Actor->Network->GetAnchorWorldHeading(Placed, TEXT("belt"), Heading));
	TestEqual(TEXT("RefreshResolvedAnchors ran: it reports the definition's heading now, not the saved snapshot"), Heading, 2.0, 1e-9);
	const FRoadSegment* Loaded = Actor->Network->GetSegments().FindByPredicate([](const FRoadSegment& S) { return S.bAlive; });
	if (TestNotNull(TEXT("the taxiway came back"), Loaded))
	{
		TestTrue(TEXT("and follows THIS session's default profile, not the transient object the saved path resolved to"),
			Actor->Network->ProfileFor(*Loaded) == NewDefault);
	}
	TestTrue(TEXT("DefaultProfile names this actor's own default"), Actor->Network->DefaultProfile == NewDefault);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsRuntimeFleetToastsTest, "AirportOps.Present.Fleet.EveryFleetChangeIsToastedExceptTheSeeding",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsRuntimeFleetToastsTest::RunTest(const FString&)
{
	// THE FEED'S HALF OF #443: FleetChanged now carries four ways (Bought, Sold, Seeded, Withdrawn), and the runtime's
	// Presentation subscriber toasts them through the one notification face. A seeded vehicle is not news - nobody did or
	// paid anything - and a withdrawn one must say where its credit came from ("Bowser credited, depot removed"), which the
	// feed could never say while a removed depot's vehicles left silently.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor to attach to"), TestWorld.Actor)) { return false; }
	UOpsRuntime* Runtime = RuntimeBusTestAttach(TestWorld);
	UOpsEventsTestListener* Listener = NewObject<UOpsEventsTestListener>();
	Runtime->GetEvents()->OnNotification.AddDynamic(Listener, &UOpsEventsTestListener::OnNote);
	Runtime->Tick(0.0);
	const int32 Before = Listener->CountOf(TEXT("note:"));

	Runtime->GetBus().Publish(FFleetChangedEvent{ 1, 7, TEXT("FUEL"), EFleetChange::Seeded, 0.0 });
	Runtime->Tick(0.0);
	TestEqual(TEXT("a seeded vehicle raises no toast"), Listener->CountOf(TEXT("note:")), Before);

	Runtime->GetBus().Publish(FFleetChangedEvent{ 1, 8, TEXT("FUEL"), EFleetChange::Withdrawn, 45000.0 });
	Runtime->Tick(0.0);
	if (!TestEqual(TEXT("a withdrawn one raises exactly one"), Listener->CountOf(TEXT("note:")), Before + 1)) { return false; }
	const FString Withdrawn = Listener->Seen.Last();
	TestTrue(FString::Printf(TEXT("that says the depot went ('%s')"), *Withdrawn), Withdrawn.Contains(TEXT("Depot removed")));
	TestTrue(TEXT("and that the vehicle was credited"), Withdrawn.Contains(TEXT("credited")));

	Runtime->GetBus().Publish(FFleetChangedEvent{ 1, 9, TEXT("FUEL"), EFleetChange::Bought, 90000.0 });
	Runtime->GetBus().Publish(FFleetChangedEvent{ 1, 9, TEXT("FUEL"), EFleetChange::Sold, 45000.0 });
	Runtime->Tick(0.0);
	TestEqual(TEXT("and a purchase and a sale still toast, as before"), Listener->CountOf(TEXT("note:")), Before + 3);
	TestTrue(TEXT("a paid sale names its money"), Listener->Seen.Last().Contains(TEXT("\u2014")));

	// A SALE WORTH NOTHING (#487: a seeded vehicle fetches no resale) toasts without a figure, as a removal that credited nothing
	// says "withdrawn" rather than "credited $0".
	Runtime->GetBus().Publish(FFleetChangedEvent{ 1, 10, TEXT("FUEL"), EFleetChange::Sold, 0.0 });
	Runtime->Tick(0.0);
	if (!TestEqual(TEXT("a worthless sale still toasts"), Listener->CountOf(TEXT("note:")), Before + 4)) { return false; }
	const FString Worthless = Listener->Seen.Last();
	TestTrue(FString::Printf(TEXT("and says the vehicle was sold ('%s')"), *Worthless), Worthless.Contains(TEXT("Sold")));
	TestFalse(TEXT("without the em dash and a $0 after it"), Worthless.Contains(TEXT("\u2014")));
	return true;
}

/**
 * #459 PIN: A SAVE WHOSE TAXIWAYS USE THE FALLBACK PROFILE, LOADED IN A NEW PROCESS. The fallback lives in the transient
 * package, so a new process cannot re-find it by path: the roads came back with no profile of their own, and the first
 * rebuild - StandTurnOffMarkingBuilder, which reads every taxiway's width and runs whenever a taxiway has a strip -
 * dereferenced the null. SIMULATED by renaming the fallback after the save, so its old path names nothing, exactly as
 * in a fresh process. Then: no crash, and every road's profile live and this actor's own default.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRuntimeLoadFallbackNewProcessTest, "AirportOps.Present.RuntimeLoad.FallbackProfileSurvivesANewProcess",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FRuntimeLoadFallbackNewProcessTest::RunTest(const FString&)
{
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("an actor to attach to"), Actor)) { return false; }
	const int32 A = Actor->PlaceNode(FVector2D(0.0, 0.0));
	const int32 B = Actor->PlaceNode(FVector2D(20000.0, 0.0));
	const int32 C = Actor->PlaceNode(FVector2D(20000.0, 20000.0));
	Actor->ConnectNodes(A, B);
	Actor->ConnectNodes(B, C);
	URoadProfile* Fallback = Actor->ResolveProfile();
	const URoadNetwork* Net = Actor->Network;
	int32 Taxiways = 0;
	for (int32 Index = 0; Index < Net->GetSegments().Num(); ++Index)
	{
		const FRoadSegmentId Id = Net->SegmentIdAt(Index);
		Taxiways += Id.IsSet() && Net->GetSegments()[Index].Profile == Fallback && TaxiwayStrip::HasStrip(*Net, Id);
	}
	if (!TestTrue(TEXT("the actor lays with its transient fallback"), Fallback != nullptr && Fallback->IsIn(GetTransientPackage()))
		|| !TestEqual(TEXT("and both taxiways use it and have a strip - or the rebuild below reads nothing"), Taxiways, 2))
	{
		return false;
	}

	UOpsRuntime* Runtime = NewObject<UOpsRuntime>();
	Runtime->Attach(Actor);
	const FString Slot = TEXT("AirportOpsTest_FallbackNewProcess");
	if (!TestTrue(TEXT("save writes"), Runtime->SaveToSlot(Slot))) { return false; }

	// A NEW PROCESS, AS FAR AS A PATH CAN TELL: the fallback keeps being the actor's (same object), under a name the
	// saved path does not have.
	Fallback->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
	if (!TestTrue(TEXT("load reads - and the rebuild it runs does not crash"), Runtime->LoadFromSlot(Slot))) { return false; }

	int32 Live = 0;
	int32 OnTheDefault = 0;
	for (int32 Index = 0; Index < Actor->Network->GetSegments().Num(); ++Index)
	{
		if (!Actor->Network->SegmentIdAt(Index).IsSet())
		{
			continue;
		}
		++Live;
		OnTheDefault += Actor->Network->ProfileFor(Actor->Network->GetSegments()[Index]) == Actor->ResolveProfile();
	}
	TestEqual(TEXT("both taxiways came back"), Live, 2);
	TestEqual(TEXT("and every one's profile is live and this actor's own default"), OnTheDefault, Live);
	TestTrue(TEXT("DefaultProfile is live - re-resolved from the loading actor"), Actor->Network->DefaultProfile == Actor->ResolveProfile());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsRuntimeRoadDrawnNoFlickerTest, "AirportOps.Present.Alerts.UnserviceableJobSurvivesARoadDrawn",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsRuntimeRoadDrawnNoFlickerTest::RunTest(const FString&)
{
	// #445's PIN, through the runtime's own passes: raise an unserviceable job, commit an UNRELATED road, tick three frames. Zero clear/raise
	// pairs, and one raise (= one toast: the toast stack toasts every raise that is not a load's re-raise) in total. The road moves the
	// guideline revision, the job board's pass re-offers the refused job (Open for the frame before its bid), and the alerts pass - which
	// runs after it in the same round - used to see no Unserviceable job, clear the alert, and raise it again the frame after.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor to attach to"), TestWorld.Actor)) { return false; }
	UOpsRuntime* Runtime = RuntimeBusTestAttach(TestWorld);
	URoadNetwork* Net = TestWorld.Actor->Network;
	UOpsEventsTestListener* Listener = NewObject<UOpsEventsTestListener>();
	Runtime->GetEvents()->OnAlertRaised.AddDynamic(Listener, &UOpsEventsTestListener::OnAlertRaised);
	Runtime->GetEvents()->OnAlertCleared.AddDynamic(Listener, &UOpsEventsTestListener::OnAlertCleared);
	Runtime->Tick(0.0);

	FServiceJob& Job = Runtime->GetJobBoard()->AddJobForTest(5, EServiceJobState::Unserviceable, EServiceRefusal::NoDepot, Net->GetGuidelineRevision());
	Job.Stand.Index = 0;
	Runtime->GetBus().MarkDirty(TEXT("Alerts"));   // what the job board's pass does when a step refuses a job
	for (int32 Frame = 0; Frame < 3; ++Frame) { Runtime->Tick(0.0); }
	const FString Raised = TEXT("alert+:") + UEnum::GetValueAsString(EAlertKind::JobUnserviceable);
	const FString Cleared = TEXT("alert-:") + UEnum::GetValueAsString(EAlertKind::JobUnserviceable);
	if (!TestEqual(TEXT("PRECONDITION: the refused job is alerted, once"), Listener->CountOf(Raised), 1)) { return false; }

	// AN UNRELATED ROAD, committed far from anything: the network changes, the guideline revision with it.
	const uint32 RevisionBefore = Net->GetGuidelineRevision();
	const int32 C = TestWorld.Actor->PlaceNode(FVector2D(0.0, 50000.0));
	const int32 D = TestWorld.Actor->PlaceNode(FVector2D(20000.0, 50000.0));
	TestWorld.Actor->ConnectNodes(C, D);
	if (!TestTrue(TEXT("PRECONDITION: the road moved the guideline revision, so the refusal is stale and the job is re-offered"),
		Net->GetGuidelineRevision() != RevisionBefore)) { return false; }
	for (int32 Frame = 0; Frame < 3; ++Frame) { Runtime->Tick(0.0); }

	TestEqual(TEXT("three frames after the road: no clear"), Listener->CountOf(Cleared), 0);
	TestEqual(TEXT("and no second raise - one toast in total"), Listener->CountOf(Raised), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsRuntimeQuietMinutesTest, "AirportOps.Present.Alerts.QuietMinutesRunNoAlertsPass",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsRuntimeQuietMinutesTest::RunTest(const FString&)
{
	// #446: the offer minute used to dirty the alerts pass every game minute - ~40 times a real second at x32 - as the catch-all for the two
	// conditions with no event of their own. An airport with nothing moving costs the pass nothing now: ten quiet game minutes run it not once.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor to attach to"), TestWorld.Actor)) { return false; }
	UOpsRuntime* Runtime = RuntimeBusTestAttach(TestWorld);
	for (int32 Frame = 0; Frame < 3; ++Frame) { Runtime->Tick(0.0); }
	const int32 Settled = Runtime->GetAlerts()->RecomputeCountForTest();
	const int32 OfferTicks = Runtime->OfferTicksForTest();
	const double OneMinute = UOfferGenerator::TickSeconds / Runtime->GetClock()->TimeScale() * 1.01;
	for (int32 Minute = 0; Minute < 10; ++Minute) { Runtime->Tick(OneMinute); }
	TestTrue(TEXT("PRECONDITION: the offer minute ran meanwhile - the old catch-all's trigger"), Runtime->OfferTicksForTest() >= OfferTicks + 9);
	TestEqual(TEXT("and ten quiet game minutes ran the alerts pass not once"), Runtime->GetAlerts()->RecomputeCountForTest(), Settled);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsRuntimeDeadlockLookTest, "AirportOps.Present.Alerts.DeadlockLookIsAClockEntryNotAnOfferMinuteTick",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsRuntimeDeadlockLookTest::RunTest(const FString&)
{
	// #446: a deadlock matures when agents have been stalled past FTrafficRules::StallSeconds - no phase change, no edit. With something on
	// the ground the alerts pass looks again once per stall period, from a clock entry booked by the pass itself; with nothing on the ground
	// there is no entry. (The offer minute that used to do this is gone: QuietMinutesRunNoAlertsPass.)
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("an actor"), Actor)) { return false; }
	Actor->PlaceNode(FVector2D(0.0, 60000.0));
	if (!TestNotNull(TEXT("a network"), Actor->Network.Get())) { return false; }
	URoadNetwork& Net = *Actor->Network;
	FGuidelineNodeId A, B;
	RuntimeE2ELayLine(Net, FVector2D(-10000.0, -10000.0), FVector2D(-10000.0, 10000.0), A, B);
	UOpsRuntime* Runtime = NewObject<UOpsRuntime>();
	Runtime->Attach(Actor);
	for (int32 Frame = 0; Frame < 3; ++Frame) { Runtime->Tick(0.0); }
	const int32 Empty = Runtime->GetAlerts()->RecomputeCountForTest();
	Runtime->Tick(60.0);   // 30 game minutes at x1 by day - no midnight, so no upkeep post to dirty the pass
	TestEqual(TEXT("an airport with nothing on the ground is not looked at on a clock"), Runtime->GetAlerts()->RecomputeCountForTest(), Empty);

	const FRoutePlan Plan = TestGraph::Probe(Net, A, B, ETraversalClass::Aircraft);
	if (!TestTrue(TEXT("a route"), Plan.IsValid())) { return false; }
	if (!TestTrue(TEXT("an aircraft is dispatched"), Actor->DispatchAgent(Plan, UAirsideSettings::ResolveDefaultAirframe()))) { return false; }
	const int32 Aircraft = Actor->GetTraffic()->GetNewestAgentId();
	Runtime->Tick(0.0);   // its phase event dirties the pass, and the run books the look
	const int32 OnGround = Runtime->GetAlerts()->RecomputeCountForTest();
	UGroundTraffic* Model = Actor->GetTraffic()->GetModel();
	const double Period = Runtime->GetClock()->GameSecondsOfMovement(Model->Rules.StallSeconds);
	const double RealForAPeriod = Period / Runtime->GetClock()->TimeScale();

	Runtime->Tick(RealForAPeriod * 0.5);
	TestEqual(TEXT("half a stall period on: not yet"), Runtime->GetAlerts()->RecomputeCountForTest(), OnGround);
	Runtime->Tick(RealForAPeriod * 0.75);
	TestEqual(TEXT("a stall period on: the alerts pass looked once - a clock entry, not an offer minute"), Runtime->GetAlerts()->RecomputeCountForTest(), OnGround + 1);
	Runtime->Tick(RealForAPeriod * 1.01);
	TestEqual(TEXT("and the look re-books itself while the aircraft is there"), Runtime->GetAlerts()->RecomputeCountForTest(), OnGround + 2);

	Model->RetireAgent(Aircraft);
	Runtime->Tick(0.0);
	const int32 Gone = Runtime->GetAlerts()->RecomputeCountForTest();
	Runtime->Tick(RealForAPeriod * 10.0);
	TestEqual(TEXT("the aircraft gone, the look is not booked again"), Runtime->GetAlerts()->RecomputeCountForTest(), Gone);
	return true;
}

#endif
