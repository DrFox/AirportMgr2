#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Build/AnchorLink.h"
#include "Content/AirsideSettings.h"
#include "Entities/EntityDefinition.h"
#include "Model/Flight.h"
#include "Model/FlightBoard.h"
#include "Model/GroundTraffic.h"
#include "Model/AirlineDefinition.h"
#include "Model/AirlineRoster.h"
#include "Model/JobBoard.h"
#include "Model/OfferGenerator.h"
#include "Model/Ledger.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadTraffic.h"
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
#include "Present/AirsideTraffic.h"
#include "Present/OpsRuntime.h"
#include "Present/RoadNetworkActor.h"
#include "Testing/AirsideTestWorld.h"

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

	int32 RuntimeBusTestPhaseCount(const UOpsEventsTestListener& Listener)
	{
		return Listener.Seen.FilterByPredicate([](const FString& S) { return S.StartsWith(TEXT("phase:")); }).Num();
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsRuntimeBusPhaseTest, "AirportOps.Present.Bus.PhaseReachesUiOnNextStep",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsRuntimeBusPhaseTest::RunTest(const FString&)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor to attach to"), TestWorld.Actor)) { return false; }
	UOpsRuntime* Runtime = RuntimeBusTestAttach(TestWorld);
	UOpsEventsTestListener* Listener = NewObject<UOpsEventsTestListener>();
	Runtime->GetEvents()->OnAgentPhaseChanged.AddDynamic(Listener, &UOpsEventsTestListener::OnPhase);

	TestWorld.Actor->GetTraffic()->OnAgentPhaseChanged.Broadcast(42, EAgentPhase::Taxiing, EAgentPhase::Parked);
	TestEqual(TEXT("nothing runs inside Airside's broadcast - it is queued (#193)"), RuntimeBusTestPhaseCount(*Listener), 0);

	Runtime->Tick(0.0);
	const FString Expected = FString::Printf(TEXT("phase:42:%d->%d"),
		static_cast<int32>(EAgentPhase::Taxiing), static_cast<int32>(EAgentPhase::Parked));
	TestTrue(TEXT("the next ops step delivers it to the Presentation tier, arguments intact"), Listener->Seen.Contains(Expected));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsRuntimeBusReattachTest, "AirportOps.Present.Bus.ReattachDoesNotDouble",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsRuntimeBusReattachTest::RunTest(const FString&)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor to attach to"), TestWorld.Actor)) { return false; }
	UOpsRuntime* Runtime = RuntimeBusTestAttach(TestWorld);
	Runtime->Attach(TestWorld.Actor);
	UOpsEventsTestListener* Listener = NewObject<UOpsEventsTestListener>();
	Runtime->GetEvents()->OnAgentPhaseChanged.AddDynamic(Listener, &UOpsEventsTestListener::OnPhase);

	TestWorld.Actor->GetTraffic()->OnAgentPhaseChanged.Broadcast(7, EAgentPhase::Taxiing, EAgentPhase::Parked);
	Runtime->Tick(0.0);
	TestEqual(TEXT("a second Attach re-wires rather than stacking a second set of handlers"),
		RuntimeBusTestPhaseCount(*Listener), 1);
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

	UOpsEventsTestListener* Listener = NewObject<UOpsEventsTestListener>();
	Runtime->GetEvents()->OnAgentPhaseChanged.AddDynamic(Listener, &UOpsEventsTestListener::OnPhase);
	TestWorld.Actor->GetTraffic()->OnAgentPhaseChanged.Broadcast(5, EAgentPhase::Taxiing, EAgentPhase::Gone);
	if (!TestTrue(TEXT("load reads"), Runtime->LoadFromSlot(Slot))) { return false; }
	Runtime->Tick(0.0);
	TestEqual(TEXT("an event queued before a load never reaches anyone: it names an agent that no longer exists"),
		RuntimeBusTestPhaseCount(*Listener), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsRuntimeBusStaleAgentTest, "AirportOps.Present.Bus.PhaseForAnAgentAlreadyGone",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsRuntimeBusStaleAgentTest::RunTest(const FString&)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor to attach to"), TestWorld.Actor)) { return false; }
	UOpsRuntime* Runtime = RuntimeBusTestAttach(TestWorld);
	UOpsEventsTestListener* Listener = NewObject<UOpsEventsTestListener>();
	Runtime->GetEvents()->OnAgentPhaseChanged.AddDynamic(Listener, &UOpsEventsTestListener::OnPhase);

	// A handler runs a frame after the event was raised, so the agent it names may be gone -
	// here it never existed. Both boards must shrug, and the UI still hears it.
	TestWorld.Actor->GetTraffic()->OnAgentPhaseChanged.Broadcast(999999, EAgentPhase::Taxiing, EAgentPhase::Parked);
	Runtime->Tick(0.0);
	TestEqual(TEXT("a phase for an agent that is already gone is survived and still reaches the UI"),
		RuntimeBusTestPhaseCount(*Listener), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsRuntimeBusDetachTest, "AirportOps.Present.Bus.DetachDiscardsQueue",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsRuntimeBusDetachTest::RunTest(const FString&)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor to attach to"), TestWorld.Actor)) { return false; }
	UOpsRuntime* Runtime = RuntimeBusTestAttach(TestWorld);
	UOpsEventsTestListener* Listener = NewObject<UOpsEventsTestListener>();
	Runtime->GetEvents()->OnAgentPhaseChanged.AddDynamic(Listener, &UOpsEventsTestListener::OnPhase);

	TestWorld.Actor->GetTraffic()->OnAgentPhaseChanged.Broadcast(3, EAgentPhase::Taxiing, EAgentPhase::Parked);
	Runtime->Attach(nullptr);
	Runtime->Attach(TestWorld.Actor);
	Runtime->Tick(0.0);
	TestEqual(TEXT("an event queued before a detach is the old actor's - a new level numbers agents from 1 again"),
		RuntimeBusTestPhaseCount(*Listener), 0);
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
	FAnchorLink::Build(Net, UAirsideSettings::ResolveLargestServiceVehicle());

	UOpsRuntime* Runtime = NewObject<UOpsRuntime>();
	Runtime->Attach(Actor);
	const FRoutePlan Plan = TestGraph::Probe(Net, TaxiSouth, Net.GetEntity(Stand)->PoseNode, ETraversalClass::Aircraft);
	if (!TestTrue(TEXT("the aircraft routes to the stand"), Plan.IsValid())) { return false; }
	if (!TestTrue(TEXT("and dispatches"), Actor->DispatchAgent(Plan, UAirsideSettings::ResolveDefaultAirframe()))) { return false; }
	const int32 Aircraft = Actor->GetTraffic()->GetNewestAgentId();
	Runtime->Tick(0.0);

	Actor->GetTraffic()->OnAgentPhaseChanged.Broadcast(Aircraft, EAgentPhase::Taxiing, EAgentPhase::Parked);
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
		Board->OnAgentPhase(*Traffic, *Net, *Clock, E.AgentId, E.From, E.To);
	});
	Bus.EndWiring();
	Traffic->OnAgentPhaseChanged.AddLambda([&Bus](int32 Id, EAgentPhase From, EAgentPhase To)
	{
		Bus.Publish(FAgentPhaseEvent{ Id, From, To });
	});

	UFlight* Flight = NewObject<UFlight>(GetTransientPackage());
	Flight->Airframe = Airframe;
	Flight->ApproachFocus = Airport.Threshold;
	Flight->LandingFee = 1200.0;
	Flight->LeadTimeSeconds = 0.0;
	Board->AddOffer(*Clock, Flight);
	if (!TestTrue(TEXT("the flight is accepted"), Board->Accept(*Traffic, *Net, *Clock, *Flight))) { return false; }
	Clock->Advance(1.0);
	Board->TickQueue(*Traffic, *Net, *Clock);
	if (!TestEqual(TEXT("and dispatched"), Flight->Phase, EFlightPhase::Landing)) { return false; }
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
	Runtime->Tick(1.0 / 30.0);
	TestTrue(TEXT("a depot placed on a quiet airport still gets its fleet (FNetworkChangedEvent)"),
		Runtime->GetJobBoard()->GetVehicles().Num() > Fleet);
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
	TestEqual(TEXT("a solvent airport raises nothing"), Listener->CountOf(TEXT("alert+:")), 0);

	Runtime->GetLedger()->Post(0.0, ELedgerCategory::Upkeep, -(Runtime->GetLedger()->Balance() + 1.0), FText::FromString(TEXT("test")));
	const double OneMinute = UOfferGenerator::TickSeconds / Runtime->GetClock()->TimeScale() * 1.01;
	Runtime->Tick(OneMinute);
	// OVERDRAWN BY NAME: this field has no runway, so the content's airlines cannot come either, and the same
	// pass rightly raises those too.
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
	// cleared BY an accept today - UStandAllocator::Reserve only holds a stand with a pose, and HeldStandIsGone
	// needs one without - so an alert-shaped assertion here would pass with the subscription deleted.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor to attach to"), TestWorld.Actor)) { return false; }
	UOpsRuntime* Runtime = RuntimeBusTestAttach(TestWorld);
	URoadNetwork* Net = TestWorld.Actor->Network;
	if (!TestNotNull(TEXT("a network"), Net)) { return false; }
	UEntityDefinition* StandDef = UEntityDefinition::MakeStandTransient();
	Net->PlaceEntity(StandDef, StandDef->Anchors, FVector2D(0.0, 30000.0), 0.0, 3600.0, StandDef->PoseRole, StandDef->Trucks);

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
	Flight->Phase = EFlightPhase::TaxiOut;
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
	// detach publishes into whatever comes next - the stage 3 review's reason, which the roster had escaped.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor to attach to"), TestWorld.Actor)) { return false; }
	UOpsRuntime* Runtime = RuntimeBusTestAttach(TestWorld);
	TestTrue(TEXT("attached, the roster publishes onto the runtime's bus"), Runtime->GetAirlines()->Bus == &Runtime->GetBus());
	AddExpectedMessagePlain(TEXT("OpsRuntime attached to nothing"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	Runtime->Attach(nullptr);
	TestNull(TEXT("the flight board's bus is cleared"), Runtime->GetFlightBoard()->Bus);
	TestNull(TEXT("the job board's"), Runtime->GetJobBoard()->Bus);
	TestNull(TEXT("the ledger's"), Runtime->GetLedger()->Bus);
	TestNull(TEXT("the alerts'"), Runtime->GetAlerts()->Bus);
	TestNull(TEXT("and the airline roster's"), Runtime->GetAirlines()->Bus);
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
	FAnchorLink::Build(Net, UAirsideSettings::ResolveLargestServiceVehicle());
	// THE HAND-LAID LINES ARE THIS FIELD'S GUIDELINE GRAPH: PlaceNode above derived one, and the runway segments
	// added since would otherwise read as a road the graph is behind - DepartAgent then refuses
	// GraphBeingEdited for ever (URoadNetwork::AreGuidelinesBehindRoad), which is a drag, not this test.
	Net.MarkGuidelinesDerived();

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
	Flight->Phase = EFlightPhase::TaxiIn;
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

#endif
