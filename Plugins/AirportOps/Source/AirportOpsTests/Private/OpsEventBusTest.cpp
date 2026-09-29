#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/OpsEventBus.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** Wires a bus that records "<who>:<event>" per handler call. Prefixed: unity build. */
	void BusTestWireRecorder(FOpsEventBus& Bus, TArray<FString>& Seen)
	{
		Bus.BeginWiring();
		Bus.Subscribe<FNotificationEvent>(EOpsTier::Presentation, TEXT("ui"),
			[&Seen](const FNotificationEvent& E) { Seen.Add(TEXT("ui:") + E.Text); });
		Bus.Subscribe<FNotificationEvent>(EOpsTier::Sim, TEXT("sim"),
			[&Seen](const FNotificationEvent& E) { Seen.Add(TEXT("sim:") + E.Text); });
		Bus.Subscribe<FNotificationEvent>(EOpsTier::Reaction, TEXT("react"),
			[&Seen](const FNotificationEvent& E) { Seen.Add(TEXT("react:") + E.Text); });
		Bus.EndWiring();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsEventBusTierOrderTest, "AirportOps.Model.Bus.TierOrder",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsEventBusTierOrderTest::RunTest(const FString&)
{
	FOpsEventBus Bus;
	TArray<FString> Seen;
	BusTestWireRecorder(Bus, Seen);
	Bus.Publish(FNotificationEvent{ TEXT("a") });
	Bus.Publish(FNotificationEvent{ TEXT("b") });
	TestEqual(TEXT("Publish only enqueues - nothing runs at the call site"), Seen.Num(), 0);
	TestEqual(TEXT("both dispatched"), Bus.Drain(), 2);
	// Registration order was ui, sim, react: the TIER decides, not who subscribed first.
	const TArray<FString> Expected = { TEXT("sim:a"), TEXT("react:a"), TEXT("ui:a"),
		TEXT("sim:b"), TEXT("react:b"), TEXT("ui:b") };
	TestEqual(TEXT("per event, Sim then Reaction then Presentation, events in publish order"), Seen, Expected);
	TestEqual(TEXT("queue empty after drain"), Bus.QueuedCount(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsEventBusNextRoundTest, "AirportOps.Model.Bus.PublishInHandlerIsNextRound",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsEventBusNextRoundTest::RunTest(const FString&)
{
	FOpsEventBus Bus;
	TArray<FString> Seen;
	Bus.BeginWiring();
	Bus.Subscribe<FNotificationEvent>(EOpsTier::Sim, TEXT("sim"), [&Bus, &Seen](const FNotificationEvent& E)
	{
		Seen.Add(TEXT("sim:") + E.Text);
		if (E.Text == TEXT("first")) { Bus.Publish(FNotificationEvent{ TEXT("chained") }); }
	});
	Bus.Subscribe<FNotificationEvent>(EOpsTier::Presentation, TEXT("ui"),
		[&Seen](const FNotificationEvent& E) { Seen.Add(TEXT("ui:") + E.Text); });
	Bus.EndWiring();
	Bus.Publish(FNotificationEvent{ TEXT("first") });
	Bus.Publish(FNotificationEvent{ TEXT("second") });
	TestEqual(TEXT("chained event drains in the same Drain call"), Bus.Drain(), 3);
	// "chained" is published mid-round, so it waits for the whole first round - including
	// "second" and every Presentation handler - rather than cutting in.
	const TArray<FString> Expected = { TEXT("sim:first"), TEXT("ui:first"), TEXT("sim:second"),
		TEXT("ui:second"), TEXT("sim:chained"), TEXT("ui:chained") };
	TestEqual(TEXT("an event published by a handler lands in the NEXT round"), Seen, Expected);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsEventBusPassCoalesceTest, "AirportOps.Model.Bus.PassesCoalesce",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsEventBusPassCoalesceTest::RunTest(const FString&)
{
	FOpsEventBus Bus;
	int32 Runs = 0;
	Bus.BeginWiring();
	Bus.RegisterPass(TEXT("Assign"), [&Runs]() { ++Runs; });
	Bus.Subscribe<FNotificationEvent>(EOpsTier::Sim, TEXT("board"),
		[&Bus](const FNotificationEvent&) { Bus.MarkDirty(TEXT("Assign")); });
	Bus.EndWiring();
	for (int32 Index = 0; Index < 5; ++Index) { Bus.Publish(FNotificationEvent{ TEXT("job") }); }
	Bus.Drain();
	TestEqual(TEXT("five triggers in one round cost ONE pass - the reason passes exist"), Runs, 1);
	Bus.Drain();
	TestEqual(TEXT("a clean pass does not run again"), Runs, 1);
	Bus.MarkAllDirty();
	Bus.Drain();
	TestEqual(TEXT("MarkAllDirty runs it with no event (the attach/load catch-up)"), Runs, 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsEventBusCapTest, "AirportOps.Model.Bus.RoundCap",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsEventBusCapTest::RunTest(const FString&)
{
	FOpsEventBus Bus;
	int32 Calls = 0;
	Bus.BeginWiring();
	// A handler that republishes forever - the loop the cap exists to break.
	Bus.Subscribe<FNotificationEvent>(EOpsTier::Sim, TEXT("loop"), [&Bus, &Calls](const FNotificationEvent& E)
	{
		++Calls;
		Bus.Publish(FNotificationEvent{ E.Text });
	});
	Bus.EndWiring();
	Bus.Publish(FNotificationEvent{ TEXT("x") });
	AddExpectedError(TEXT("round cap"), EAutomationExpectedErrorFlags::Contains, 1);
	Bus.Drain();
	TestEqual(TEXT("exactly MaxRounds rounds ran, then it stopped"), Calls, FOpsEventBus::MaxRounds);
	TestEqual(TEXT("the leftover carries to the next frame rather than being dropped"), Bus.QueuedCount(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsEventBusPresentationPublishTest, "AirportOps.Model.Bus.PresentationPublishIsNextRound",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsEventBusPresentationPublishTest::RunTest(const FString&)
{
	FOpsEventBus Bus;
	TArray<FString> Seen;
	Bus.BeginWiring();
	Bus.Subscribe<FNotificationEvent>(EOpsTier::Presentation, TEXT("toast"),
		[&Seen](const FNotificationEvent& E) { Seen.Add(TEXT("toast:") + E.Text); });
	// A UI handler that runs a player command - an autosave on a speed change, say - whose own
	// announcement must still reach the toast rather than being dropped (stage 1 review).
	Bus.Subscribe<FSpeedChangedEvent>(EOpsTier::Presentation, TEXT("autosave"),
		[&Bus](const FSpeedChangedEvent&) { Bus.Publish(FNotificationEvent{ TEXT("Saved") }); });
	Bus.EndWiring();
	Bus.Publish(FSpeedChangedEvent{ ESimSpeed::X2 });
	TestEqual(TEXT("both the change and the command's announcement are dispatched in one Drain"), Bus.Drain(), 2);
	TestEqual(TEXT("the announcement reaches the UI on the next round"), Seen, TArray<FString>{ TEXT("toast:Saved") });
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsEventBusDiscardTest, "AirportOps.Model.Bus.Discard",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsEventBusDiscardTest::RunTest(const FString&)
{
	FOpsEventBus Bus;
	TArray<FString> Seen;
	BusTestWireRecorder(Bus, Seen);
	Bus.Publish(FNotificationEvent{ TEXT("stale") });
	TestEqual(TEXT("Discard reports what it dropped"), Bus.Discard(), 1);
	Bus.Drain();
	TestEqual(TEXT("a discarded event never reaches a handler (the load path)"), Seen.Num(), 0);
	TestEqual(TEXT("subscribers are listed by name"), Bus.SubscribersOf(FOpsEvent::IndexOfType<FNotificationEvent>()).Num(), 3);
	TestEqual(TEXT("EventNames is index-aligned with FOpsEvent"),
		FString(FOpsEventBus::EventNames()[FOpsEvent::IndexOfType<FSpeedChangedEvent>()]), FString(TEXT("SpeedChanged")));
	return true;
}

#endif
