#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/OpsEventBus.h"
#include "Model/OpsSafetyNet.h"
#include "Model/SimClock.h"

#if WITH_DEV_AUTOMATION_TESTS

// THE PAYLOAD THESE MECHANICS TESTS CARRY is FSaveSlotEvent's slot name: they need an event whose instances a string tells
// apart, and the one they used, the catch-all FNotificationEvent{Text}, was retired by #445 item 7 (Check-Architecture rule 4
// keeps it out of tests too). Nothing here is about saving.
namespace
{
	/** Wires a bus that records "<who>:<event>" per handler call. Prefixed: unity build. */
	void BusTestWireRecorder(FOpsEventBus& Bus, TArray<FString>& Seen)
	{
		Bus.BeginWiring();
		Bus.Subscribe<FSaveSlotEvent>(EOpsTier::Presentation, TEXT("ui"),
			[&Seen](const FSaveSlotEvent& E) { Seen.Add(TEXT("ui:") + E.Slot); });
		Bus.Subscribe<FSaveSlotEvent>(EOpsTier::Sim, TEXT("sim"),
			[&Seen](const FSaveSlotEvent& E) { Seen.Add(TEXT("sim:") + E.Slot); });
		Bus.Subscribe<FSaveSlotEvent>(EOpsTier::Reaction, TEXT("react"),
			[&Seen](const FSaveSlotEvent& E) { Seen.Add(TEXT("react:") + E.Slot); });
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
	Bus.Publish(FSaveSlotEvent{ EOpsSaveOutcome::Saved, TEXT("a") });
	Bus.Publish(FSaveSlotEvent{ EOpsSaveOutcome::Saved, TEXT("b") });
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
	Bus.Subscribe<FSaveSlotEvent>(EOpsTier::Sim, TEXT("sim"), [&Bus, &Seen](const FSaveSlotEvent& E)
	{
		Seen.Add(TEXT("sim:") + E.Slot);
		if (E.Slot == TEXT("first")) { Bus.Publish(FSaveSlotEvent{ EOpsSaveOutcome::Saved, TEXT("chained") }); }
	});
	Bus.Subscribe<FSaveSlotEvent>(EOpsTier::Presentation, TEXT("ui"),
		[&Seen](const FSaveSlotEvent& E) { Seen.Add(TEXT("ui:") + E.Slot); });
	Bus.EndWiring();
	Bus.Publish(FSaveSlotEvent{ EOpsSaveOutcome::Saved, TEXT("first") });
	Bus.Publish(FSaveSlotEvent{ EOpsSaveOutcome::Saved, TEXT("second") });
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
	Bus.RegisterPass(TEXT("Assign"), [&Runs](const FPassRun&) { ++Runs; });
	Bus.Subscribe<FSaveSlotEvent>(EOpsTier::Sim, TEXT("board"),
		[&Bus](const FSaveSlotEvent&) { Bus.MarkDirty(TEXT("Assign")); });
	Bus.EndWiring();
	for (int32 Index = 0; Index < 5; ++Index) { Bus.Publish(FSaveSlotEvent{ EOpsSaveOutcome::Saved, TEXT("job") }); }
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
	Bus.Subscribe<FSaveSlotEvent>(EOpsTier::Sim, TEXT("loop"), [&Bus, &Calls](const FSaveSlotEvent& E)
	{
		++Calls;
		Bus.Publish(FSaveSlotEvent{ EOpsSaveOutcome::Saved, E.Slot });
	});
	Bus.EndWiring();
	Bus.Publish(FSaveSlotEvent{ EOpsSaveOutcome::Saved, TEXT("x") });
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
	Bus.Subscribe<FSaveSlotEvent>(EOpsTier::Presentation, TEXT("toast"),
		[&Seen](const FSaveSlotEvent& E) { Seen.Add(TEXT("toast:") + E.Slot); });
	// A UI handler that runs a player command - an autosave on a speed change, say - whose own
	// announcement must still reach the toast rather than being dropped (stage 1 review).
	Bus.Subscribe<FSpeedChangedEvent>(EOpsTier::Presentation, TEXT("autosave"),
		[&Bus](const FSpeedChangedEvent&) { Bus.Publish(FSaveSlotEvent{ EOpsSaveOutcome::Saved, TEXT("Saved") }); });
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
	Bus.Publish(FSaveSlotEvent{ EOpsSaveOutcome::Saved, TEXT("stale") });
	TestEqual(TEXT("Discard reports what it dropped"), Bus.Discard(), 1);
	Bus.Drain();
	TestEqual(TEXT("a discarded event never reaches a handler (the load path)"), Seen.Num(), 0);
	TestEqual(TEXT("subscribers are listed by name"), Bus.SubscribersOf(FOpsEvent::IndexOfType<FSaveSlotEvent>()).Num(), 3);
	TestEqual(TEXT("EventNames is index-aligned with FOpsEvent"),
		FString(FOpsEventBus::EventNames()[FOpsEvent::IndexOfType<FSpeedChangedEvent>()]), FString(TEXT("SpeedChanged")));
	return true;
}

namespace
{
	/** One default-constructed instance of every type in the variant, in variant order. */
	template <typename TVariantType> struct TOpsBusTestEveryEvent;
	template <typename... Ts> struct TOpsBusTestEveryEvent<TVariant<Ts...>>
	{
		static TArray<FOpsEvent> Get() { return { FOpsEvent(TInPlaceType<Ts>(), Ts())... }; }
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsEventBusDescribeTest, "AirportOps.Model.Bus.EveryEventDescribesItself",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsEventBusDescribeTest::RunTest(const FString&)
{
	// THE LOG IS THE TRACE (user, 2026-09-29): every event, even a default one, says something - an
	// empty Describe() would log "Bus: + X {}" and hide the fields the line exists to show.
	const TArray<FOpsEvent> Every = TOpsBusTestEveryEvent<FOpsEvent>::Get();
	const TArray<const TCHAR*> Names = FOpsEventBus::EventNames();
	TestEqual(TEXT("one sample per event type"), Every.Num(), Names.Num());
	for (int32 Index = 0; Index < Every.Num(); ++Index)
	{
		TestFalse(FString::Printf(TEXT("%s describes its fields"), Names[Index]), FOpsEventBus::Describe(Every[Index]).IsEmpty());
	}
	FAgentPhaseEvent Phase;
	Phase.AgentId = 7;
	Phase.From = EAgentPhase::Taxiing;
	Phase.To = EAgentPhase::Parked;
	TestTrue(TEXT("and names the ids that tell events apart"), Phase.Describe().Contains(TEXT("agent 7")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsEventBusNextDrainTest, "AirportOps.Model.Bus.NextDrainIsNextFrame",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsEventBusNextDrainTest::RunTest(const FString&)
{
	FOpsEventBus Bus;
	int32 Runs = 0;
	Bus.BeginWiring();
	// A retry: a pass that has not resolved its work asks to be looked at again NEXT frame.
	Bus.RegisterPass(TEXT("Retry"), [&Bus, &Runs](const FPassRun&) { ++Runs; Bus.MarkDirtyNextDrain(TEXT("Retry")); });
	Bus.EndWiring();
	Bus.MarkAllDirty();
	Bus.Drain();
	TestEqual(TEXT("a pass that re-dirties itself for next drain runs once in this one - no round-cap spin"), Runs, 1);
	Bus.Drain();
	Bus.Drain();
	TestEqual(TEXT("and exactly once per drain after it, for as long as it keeps asking"), Runs, 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsEventBusPassOrderTest, "AirportOps.Model.Bus.PassesRunInDeclaredOrder",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsEventBusPassOrderTest::RunTest(const FString&)
{
	// #445: pass order was registration-line order in WireBus. A pass now DECLARES what it runs After, and EndWiring orders by it - so the
	// order does not depend on which line came first, and is readable (PassOrder) by a test.
	FOpsEventBus Bus;
	TArray<FName> Ran;
	Bus.BeginWiring();
	// REGISTERED BACKWARDS: C needs B, B needs A, and nothing is listed in the order it must run.
	Bus.RegisterPass(TEXT("C"), [&Ran](const FPassRun&) { Ran.Add(TEXT("C")); }, { TEXT("B") });
	Bus.RegisterPass(TEXT("Free"), [&Ran](const FPassRun&) { Ran.Add(TEXT("Free")); });
	Bus.RegisterPass(TEXT("B"), [&Ran](const FPassRun&) { Ran.Add(TEXT("B")); }, { TEXT("A") });
	Bus.RegisterPass(TEXT("A"), [&Ran](const FPassRun&) { Ran.Add(TEXT("A")); });
	Bus.EndWiring();
	// STABLE: the first pass with nothing pending keeps its registration place, so the declared chain moves only what it must.
	TestEqual(TEXT("the passes are ordered by what they declare, registration order breaking ties"),
		Bus.PassOrder(), TArray<FName>({ TEXT("Free"), TEXT("A"), TEXT("B"), TEXT("C") }));
	TestEqual(TEXT("a pass's declared dependencies are readable"), Bus.PassesAfter(TEXT("C")), TArray<FName>({ TEXT("B") }));
	Bus.MarkAllDirty();
	Bus.Drain();
	TestEqual(TEXT("and they RUN in that order in a drain"), Ran, TArray<FName>({ TEXT("Free"), TEXT("A"), TEXT("B"), TEXT("C") }));

	// A DEPENDENCY NOBODY REGISTERED, AND A CYCLE, are said at wire time (an Error a test sees), not silently mis-ordered.
	FOpsEventBus Broken;
	Broken.BeginWiring();
	Broken.RegisterPass(TEXT("Lost"), [](const FPassRun&) {}, { TEXT("Nobody") });
	Broken.RegisterPass(TEXT("P"), [](const FPassRun&) {}, { TEXT("Q") });
	Broken.RegisterPass(TEXT("Q"), [](const FPassRun&) {}, { TEXT("P") });
	AddExpectedError(TEXT("cannot be ordered"), EAutomationExpectedErrorFlags::Contains, 3);
	Broken.EndWiring();
	TestEqual(TEXT("an unorderable pass is still kept - the airport runs while the defect is read"), Broken.PassOrder().Num(), 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsEventBusCauseTest, "AirportOps.Model.Bus.PassRunCarriesItsCause",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsEventBusCauseTest::RunTest(const FString&)
{
	// #445: the job board's and the arrival queue's passes told "an event asked" from "only the net asked" by two hand-set flags each. The bus
	// carries it for EVERY pass: the strongest cause that marked it since it last ran.
	FOpsEventBus Bus;
	TArray<EPassCause> Causes;
	Bus.BeginWiring();
	Bus.RegisterPass(TEXT("P"), [&Causes](const FPassRun& Run) { Causes.Add(Run.Cause); });
	Bus.EndWiring();

	Bus.MarkDirty(TEXT("P"), EPassCause::SafetyNet);
	Bus.Drain();
	Bus.MarkDirty(TEXT("P"), EPassCause::SafetyNet);
	Bus.MarkDirty(TEXT("P"));
	Bus.Drain();
	Bus.MarkDirtyNextDrain(TEXT("P"));
	Bus.Drain();
	Bus.MarkAllDirty();
	Bus.Drain();
	Bus.MarkDirty(TEXT("P"), EPassCause::SafetyNet);
	Bus.Drain();
	TestEqual(TEXT("one run per marking"), Causes.Num(), 5);
	if (Causes.Num() == 5)
	{
		TestEqual(TEXT("marked by the net alone: a safety run"), Causes[0], EPassCause::SafetyNet);
		TestEqual(TEXT("marked by the net and then an event in one drain: the event's - something asked, finding work is no defect"), Causes[1], EPassCause::Event);
		TestEqual(TEXT("a retry's own cause"), Causes[2], EPassCause::Retry);
		TestEqual(TEXT("a catch-up (MarkAllDirty) is an event's - the game asked"), Causes[3], EPassCause::Event);
		TestEqual(TEXT("and the cause does not outlive its run: the net alone again is a safety run"), Causes[4], EPassCause::SafetyNet);
	}
	FPassRun Net{ EPassCause::SafetyNet };
	FPassRun Asked{ EPassCause::Retry };
	TestTrue(TEXT("only the net's cause is 'safety only'"), Net.IsSafetyOnly() && !Asked.IsSafetyOnly());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsEventBusThirdNetPassTest, "AirportOps.Model.Bus.ThirdNetWatchedPassNeedsNoRuntimeField",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsEventBusThirdNetPassTest::RunTest(const FString&)
{
	// #445's PIN: a pass that polls something no event announces costs one Want() call - no handle, no wanted/due/covered flags, no funnel, no
	// lint rule - because the "why did it run" is the bus's and the "is it still waiting" is the net's. A world-free bus, clock and net, and
	// a THIRD watched pass, with nothing added to UOpsRuntime.
	USimClock* Clock = NewObject<USimClock>(GetTransientPackage());
	Clock->SetUniformDay(USimClock::SecondsPerDay);   // one game second per real second
	FOpsEventBus Bus;
	FOpsSafetyNet Net;
	Net.Bind(Bus, *Clock, 30.0);
	TMap<FName, TArray<EPassCause>> Runs;
	Bus.BeginWiring();
	for (const TCHAR* Name : { TEXT("Queue"), TEXT("Board"), TEXT("Third") })
	{
		const FName Pass = Name;
		Bus.RegisterPass(Pass, [&Runs, Pass](const FPassRun& Run) { Runs.FindOrAdd(Pass).Add(Run.Cause); });
	}
	Bus.EndWiring();

	TestFalse(TEXT("nothing wants the net: nothing is booked"), Net.IsArmed());
	Net.Want(TEXT("Third"), true);
	TestTrue(TEXT("the third pass wants it: one clock entry is booked"), Net.IsArmed());
	Net.Want(TEXT("Board"), true);
	Clock->Advance(31.0);
	Bus.Drain();
	TestEqual(TEXT("the net marked the pass that wants it, as the net"), Runs.FindOrAdd(TEXT("Third")), TArray<EPassCause>({ EPassCause::SafetyNet }));
	TestEqual(TEXT("and the other that wants it"), Runs.FindOrAdd(TEXT("Board")), TArray<EPassCause>({ EPassCause::SafetyNet }));
	TestEqual(TEXT("and not the one that does not"), Runs.FindOrAdd(TEXT("Queue")).Num(), 0);

	Bus.MarkDirty(TEXT("Third"));
	Bus.Drain();
	TestEqual(TEXT("an event's run of the same pass is not the net's"), Runs.FindOrAdd(TEXT("Third")).Last(), EPassCause::Event);

	Net.Want(TEXT("Third"), false);
	TestTrue(TEXT("while another still wants it the one entry stays booked - not one per pass"), Net.IsArmed());
	Net.Want(TEXT("Board"), false);
	TestFalse(TEXT("nobody wants it: cancelled"), Net.IsArmed());
	const int32 Before = Runs.FindOrAdd(TEXT("Third")).Num();
	Clock->Advance(100.0);
	Bus.Drain();
	TestEqual(TEXT("and no pass is run for nothing afterwards"), Runs.FindOrAdd(TEXT("Third")).Num(), Before);

	Net.Want(TEXT("Third"), true);
	Net.CancelAll();
	TestFalse(TEXT("CancelAll (a load, a detach) drops every want and the entry"), Net.IsArmed() || Net.IsWantedBy(TEXT("Third")));
	return true;
}

#endif
