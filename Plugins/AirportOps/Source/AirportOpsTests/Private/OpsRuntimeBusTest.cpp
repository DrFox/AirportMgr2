#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Build/AnchorLink.h"
#include "Content/AirsideSettings.h"
#include "Entities/EntityDefinition.h"
#include "Model/Flight.h"
#include "Model/FlightBoard.h"
#include "Model/GroundTraffic.h"
#include "Model/JobBoard.h"
#include "Model/Ledger.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadTraffic.h"
#include "Model/RoutePolicy.h"
#include "Model/RouteSearch.h"
#include "Model/SimClock.h"
#include "Model/StandAllocator.h"
#include "Testing/AirsideTestGraph.h"
#include "Model/OpsEventBus.h"
#include "Model/OpsEvents.h"
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

#endif
