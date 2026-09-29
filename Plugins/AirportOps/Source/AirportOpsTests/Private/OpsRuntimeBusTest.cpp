#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/JobBoard.h"
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

#endif
