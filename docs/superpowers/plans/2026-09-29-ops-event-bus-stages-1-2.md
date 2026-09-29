# Ops event bus, stages 1-2 - implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A queued, tiered event bus in AirportOps with today's Airside relays moved onto it (stage 1), then runtime airline satisfaction driven by bus events that scales each airline's offer rate and shows on the inbox row (stage 2).

**Architecture:** `FOpsEventBus` (plain C++, `AirportOps/Model/`) holds a `TVariant` queue drained once per `UOpsRuntime::Tick` in Sim -> Reaction -> Presentation tiers with coalesced dirty passes. `UOpsRuntime::WireBus` is the only place subscriptions are made. Stage 2 adds `UAirlineRoster` (Model/, `IOpsPersistent`) as the Reaction-tier consumer and `UOfferGenerator` reads it for a per-airline rate multiplier.

**Tech Stack:** UE 5.8.2 C++, `Misc/TVariant.h`, UE automation tests (`AirportOpsTests`, `AirportMgr` test files).

**Spec:** `docs/superpowers/specs/2026-09-29-ops-event-bus-design.md`

**Stages 3-5** (JobBoard migration, FlightBoard/`FRunwayFreed`/safety net, leftovers) get their own plan once this lands: they are written against the bus's real API and JobBoard internals this plan does not touch.

## Global Constraints

- Worktree `C:\repos\airportmgr2-ops-event-bus`, branch `feature/ops-event-bus` (stage 1), `feature/airline-satisfaction` stacked on it (stage 2).
- Build: `& "D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat" AirportMgrEditor Win64 Development -Project="C:\repos\airportmgr2-ops-event-bus\AirportMgr.uproject" -WaitMutex -NoHotReloadFromIDE`. A NEW .cpp needs a second build; check the second prints `Compile [x64] <File>.cpp`.
- Tests: `& C:\repos\airportmgr2-ops-event-bus\Tools\Run-AirsideTests.ps1` (all) or `-Filter AirportOps.Model.Bus`. Judge the `N test(s) run, N failed, N crashed` line, never the exit code.
- Model/ never includes Build|Tool|Present|Entities|Content (Check-Architecture rule 1). `OpsEventBus.h` includes only `CoreMinimal.h`, `Misc/TVariant.h`, and the Model/ headers its event structs need.
- One log category per name: `LogOpsBus` declared `extern` in `AirportOpsLog.h`, defined once in `AirportOpsModule.cpp` (rule 2).
- Unity build: file-local helpers/constants in new .cpp files carry a file-specific prefix.
- Comments explain WHY; `// ENFORCED BY: <test name>` beside any claim about other code.
- Event struct names end in `Event` (`FAgentPhaseEvent`) - `FOpsAgentPhaseChanged` etc. are already the dynamic delegate types in `OpsEvents.h`.
- No `Co-Authored-By` trailer in commits (user's global rule). Body ends with `Claude-Session: https://claude.ai/code/session_01CMC6DoYST4vszSfrsca63t`.
- Never kill processes by name.
- Tuning figures (spec §3): start 0.5; on-time +0.03; late -0.02 per 10 game-min capped -0.10; Ignored -0.02; NeverAcceptable -0.01; declined 0; daily drift 20% toward 0.5; multiplier lerp(0.5, 1.5, S); floor airline multiplier >= 1.0.

## Review Focus

1. **One-frame latency of Airside events.** A handler that reads live agent state (e.g. `FindAgent(AgentId)` on Parked) now runs next ops tick. Expect: parked agents still exist; every handler tolerates a missing agent. Test: Task 2 step "phase handled after agent retired".
2. **Attach called twice** (level change). Expect: no duplicated subscriptions (a phase handled once, not twice). Test in Task 2.
3. **Load mid-queue.** `ClearAgents` Gone events must not reach boards after `Restore`. Test in Task 2.
4. **Debug flight (key 7) with no airline** (`AirlineId == NAME_None`). Expect: no satisfaction change, no crash, no Warning spam. Test in Task 5.
5. **Offers from a satisfaction-0 floor airline.** Expect: floor airline still at >= 1.0x and its FloorOffersPerHour untouched. Test in Task 6.

---

## Stage 1 - the bus

### Task 1: `FOpsEventBus` core

**Files:**
- Create: `Plugins/AirportOps/Source/AirportOps/Public/Model/OpsEventBus.h`
- Create: `Plugins/AirportOps/Source/AirportOps/Private/Model/OpsEventBus.cpp`
- Modify: `Plugins/AirportOps/Source/AirportOps/Public/AirportOpsLog.h` (add `LogOpsBus`)
- Modify: `Plugins/AirportOps/Source/AirportOps/Private/AirportOpsModule.cpp` (define it)
- Test: `Plugins/AirportOps/Source/AirportOpsTests/Private/OpsEventBusTest.cpp`

**Interfaces - Produces:**
```cpp
enum class EOpsTier : uint8 { Sim, Reaction, Presentation };
struct FAgentPhaseEvent    { int32 AgentId; EAgentPhase From; EAgentPhase To; static const TCHAR* EventName(); };
struct FArrivalRefusedEvent{ EArrivalRefusal Why; ... };
struct FSpeedChangedEvent  { ESimSpeed Speed; ... };
struct FNotificationEvent  { FString Text; ... };
using FOpsEvent = TVariant<FAgentPhaseEvent, FArrivalRefusedEvent, FSpeedChangedEvent, FNotificationEvent>;
class FOpsEventBus {
  static constexpr int32 MaxRounds = 8;
  static constexpr SIZE_T NumTypes = TVariantSize_V<FOpsEvent>;
  static TArray<const TCHAR*> EventNames();          // index-aligned with FOpsEvent
  void BeginWiring(); void EndWiring();              // brackets WireBus; Subscribe/RegisterPass legal only inside
  void ResetWiring();                                // drops every subscription and pass (re-Attach)
  template<typename T> void Subscribe(EOpsTier, FName Who, TFunction<void(const T&)>);
  void RegisterPass(FName Name, TFunction<void()> Run);
  void MarkDirty(FName Pass);
  void MarkAllDirty();
  template<typename T> void Publish(T&& Event);
  int32 Drain();                                     // events dispatched
  int32 Discard();                                   // events dropped
  TArray<FName> SubscribersOf(SIZE_T TypeIndex) const;
  int32 QueuedCount() const;
};
```

- [ ] **Step 1: Add the log category.** In `AirportOpsLog.h` beside `LogAirportOps`:

```cpp
/** The ops event bus - one line per subscription at wire time, Verbose per event, Error on the round cap. */
AIRPORTOPS_API DECLARE_LOG_CATEGORY_EXTERN(LogOpsBus, Log, All);
```
In `AirportOpsModule.cpp` after `DEFINE_LOG_CATEGORY(LogAirportOps);`: `DEFINE_LOG_CATEGORY(LogOpsBus);`

- [ ] **Step 2: Write the failing tests** in `OpsEventBusTest.cpp`:

```cpp
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsEventBusPresentationPublishTest, "AirportOps.Model.Bus.PresentationMayNotPublish",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsEventBusPresentationPublishTest::RunTest(const FString&)
{
	FOpsEventBus Bus;
	int32 Sim = 0;
	Bus.BeginWiring();
	Bus.Subscribe<FNotificationEvent>(EOpsTier::Sim, TEXT("sim"), [&Sim](const FNotificationEvent&) { ++Sim; });
	Bus.Subscribe<FSpeedChangedEvent>(EOpsTier::Presentation, TEXT("ui"),
		[&Bus](const FSpeedChangedEvent&) { Bus.Publish(FNotificationEvent{ TEXT("from ui") }); });
	Bus.EndWiring();
	Bus.Publish(FSpeedChangedEvent{ ESimSpeed::X2 });
	AddExpectedError(TEXT("Presentation"), EAutomationExpectedErrorFlags::Contains, 1);
	Bus.Drain();
	TestEqual(TEXT("a Presentation handler's publish is dropped - the UI may not steer the sim"), Sim, 0);
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
```

- [ ] **Step 3: Build; expect compile failure** (`OpsEventBus.h` missing). Confirms the test file is compiled.

- [ ] **Step 4: Write `OpsEventBus.h`:**

```cpp
#pragma once

#include "CoreMinimal.h"
#include "Misc/TVariant.h"
#include "Model/ArrivalPlanner.h"
#include "Model/RoadAgent.h"
#include "Model/SimClock.h"

/**
 * Which pass of a drain a handler runs in. Spec 2026-09-29-ops-event-bus §1.
 *
 * THE ORDER IS THE CONTRACT, and it replaces the one UOpsRuntime::OnAgentPhase used to keep by
 * hand ("the service first, then the bus"): every Sim handler for an event has run before any
 * Reaction handler reads the result, and Presentation sees the settled state of both.
 */
enum class EOpsTier : uint8
{
	/** Mutates simulation state: the job board, the flight board. */
	Sim,
	/** Reads Sim's settled result and changes only its own state; may publish. Airlines. */
	Reaction,
	/** Read-only - BP/UMG, toasts, audio. A publish from here is dropped with an ensure. */
	Presentation
};

// THE EVENTS. Small value structs - ids and figures, never a UObject pointer that may be dead by
// the time the queue drains (spec §4 "stale ids"). Each names itself for the log and the wiring test.

/** Airside's agent phase change, bridged by UOpsRuntime. */
struct FAgentPhaseEvent
{
	int32 AgentId = INDEX_NONE;
	EAgentPhase From = EAgentPhase::Gone;
	EAgentPhase To = EAgentPhase::Gone;
	static const TCHAR* EventName() { return TEXT("AgentPhase"); }
};

/** Airside refused an arrival. */
struct FArrivalRefusedEvent
{
	EArrivalRefusal Why = EArrivalRefusal::None;
	static const TCHAR* EventName() { return TEXT("ArrivalRefused"); }
};

/** The player's speed setting changed. */
struct FSpeedChangedEvent
{
	ESimSpeed Speed = ESimSpeed::X1;
	static const TCHAR* EventName() { return TEXT("SpeedChanged"); }
};

/** A line for the toast stack (saved, loaded, ...). */
struct FNotificationEvent
{
	FString Text;
	static const TCHAR* EventName() { return TEXT("Notification"); }
};

/**
 * EVERY EVENT THERE IS, as one closed list. Subscribe<T> and Publish<T> are compile-checked
 * against it, and the wiring test walks it - "lists that must agree are ONE list".
 * FInstancedStruct was rejected: an open set has no answer to "which events exist?".
 */
using FOpsEvent = TVariant<FAgentPhaseEvent, FArrivalRefusedEvent, FSpeedChangedEvent, FNotificationEvent>;

/**
 * The ops event bus. Pattern: Observer through a queue (an event queue / mediator hybrid) - spec
 * 2026-09-29-ops-event-bus §1.
 *
 * PUBLISH ONLY ENQUEUES. Nothing runs at the call site, which is what makes Publish safe from inside
 * UGroundTraffic's agent loop (#193's re-entrancy contract exists because a listener that ran THERE
 * could retire any agent mid-iteration), from a USimClock callback, or from another handler.
 *
 * DRAIN runs in rounds: each round takes the whole queue and, per event in publish order, runs its
 * Sim, then Reaction, then Presentation handlers; then every dirty pass once. Anything published
 * meanwhile is the next round. MaxRounds caps it, loudly.
 *
 * PASSES exist so a handler never does bulk work: it marks a pass dirty, and five jobs opening in
 * one frame cost one bidding pass, not five.
 *
 * SUBSCRIPTIONS ARE MADE IN ONE PLACE - between BeginWiring and EndWiring, which UOpsRuntime::WireBus
 * alone brackets - so the whole subscription map can be read in one function and is logged at wire
 * time. A subscribe anywhere else is a check(): a subscription made from a constructor or a widget is
 * a list nobody can find being consumed.
 *
 * PLAIN C++, NOT A UObject: it holds TFunctions (which UHT cannot see) and nothing about it is saved -
 * the queue is drained before a save and discarded after a load (spec §4). Owned by value by
 * UOpsRuntime; the boards hold a raw pointer to it, which is safe because the runtime owns both.
 */
class AIRPORTOPS_API FOpsEventBus
{
public:
	/** Rounds per Drain before the rest is carried to the next frame. 8 is ample: a chain today is
	 *  at most event -> reaction -> satisfaction change (3), 2026-09-29. */
	static constexpr int32 MaxRounds = 8;

	static constexpr SIZE_T NumTypes = TVariantSize_V<FOpsEvent>;

	/** Each event type's EventName(), index-aligned with FOpsEvent. */
	static TArray<const TCHAR*> EventNames();

	void BeginWiring();
	void EndWiring();

	/** Drop every subscription and pass - Attach re-wires from scratch, so a second Attach does not
	 *  double every handler. The queue is left alone. */
	void ResetWiring();

	template <typename T>
	void Subscribe(EOpsTier Tier, FName Who, TFunction<void(const T&)> Handler)
	{
		check(bWiring && !bDraining);
		constexpr SIZE_T Index = FOpsEvent::template IndexOfType<T>();
		Handlers[Index][static_cast<int32>(Tier)].Add(
			{ Who, [Handler = MoveTemp(Handler)](const FOpsEvent& Event) { Handler(Event.template Get<T>()); } });
		LogSubscription(Who, Tier, T::EventName());
	}

	/** A coalesced bulk step, run once after a round in which it was marked dirty. Order of
	 *  registration is the order dirty passes run. */
	void RegisterPass(FName Name, TFunction<void()> Run);
	void MarkDirty(FName Pass);
	void MarkAllDirty();

	template <typename T>
	void Publish(T&& Event)
	{
		using FEvent = std::decay_t<T>;
		if (bDraining && CurrentTier == EOpsTier::Presentation)
		{
			ensureMsgf(false, TEXT("Presentation handler published %s - dropped; the UI may not steer the sim"),
				FEvent::EventName());
			UE_LOG(LogOpsBus, Error, TEXT("Presentation handler published %s - dropped"), FEvent::EventName());
			return;
		}
		Queue.Emplace(TInPlaceType<FEvent>(), Forward<T>(Event));
	}

	/** Run rounds until quiet or MaxRounds. Returns the number of events dispatched. */
	int32 Drain();

	/** Drop the queue unhandled (after a load). Returns how many were dropped, and logs it. */
	int32 Discard();

	/** Who subscribed to the event at TypeIndex, every tier, in tier order. For the wiring test. */
	TArray<FName> SubscribersOf(SIZE_T TypeIndex) const;

	int32 QueuedCount() const { return Queue.Num(); }

private:
	struct FHandler
	{
		FName Who;
		TFunction<void(const FOpsEvent&)> Run;
	};
	struct FPass
	{
		FName Name;
		TFunction<void()> Run;
		bool bDirty = false;
	};

	static constexpr int32 NumTiers = 3;
	TArray<FHandler> Handlers[NumTypes][NumTiers];
	TArray<FPass> Passes;
	TArray<FOpsEvent> Queue;
	bool bWiring = false;
	bool bDraining = false;
	EOpsTier CurrentTier = EOpsTier::Sim;

	static const TCHAR* NameOf(const FOpsEvent& Event);
	static void LogSubscription(FName Who, EOpsTier Tier, const TCHAR* Event);
	bool AnyDirty() const;
};
```
Note: `LogOpsBus` is used in the inline `Publish`, so the header also includes `"AirportOpsLog.h"`.

- [ ] **Step 5: Write `OpsEventBus.cpp`:**

```cpp
#include "Model/OpsEventBus.h"
#include "AirportOpsLog.h"

namespace
{
	template <typename TVariantType> struct TOpsBusEventNames;
	template <typename... Ts> struct TOpsBusEventNames<TVariant<Ts...>>
	{
		static TArray<const TCHAR*> Get() { return { Ts::EventName()... }; }
	};

	const TCHAR* OpsBusTierName(EOpsTier Tier)
	{
		switch (Tier)
		{
		case EOpsTier::Sim: return TEXT("Sim");
		case EOpsTier::Reaction: return TEXT("Reaction");
		default: return TEXT("Presentation");
		}
	}
}

TArray<const TCHAR*> FOpsEventBus::EventNames()
{
	return TOpsBusEventNames<FOpsEvent>::Get();
}

const TCHAR* FOpsEventBus::NameOf(const FOpsEvent& Event)
{
	return EventNames()[Event.GetIndex()];
}

void FOpsEventBus::LogSubscription(FName Who, EOpsTier Tier, const TCHAR* Event)
{
	UE_LOG(LogOpsBus, Log, TEXT("Bus: %s subscribes to %s (%s)"), *Who.ToString(), Event, OpsBusTierName(Tier));
}

void FOpsEventBus::BeginWiring() { check(!bDraining); bWiring = true; }
void FOpsEventBus::EndWiring() { bWiring = false; }

void FOpsEventBus::ResetWiring()
{
	check(!bDraining);
	for (SIZE_T Type = 0; Type < NumTypes; ++Type)
	{
		for (int32 Tier = 0; Tier < NumTiers; ++Tier)
		{
			Handlers[Type][Tier].Reset();
		}
	}
	Passes.Reset();
}

void FOpsEventBus::RegisterPass(FName Name, TFunction<void()> Run)
{
	check(bWiring && !bDraining);
	Passes.Add({ Name, MoveTemp(Run), false });
	UE_LOG(LogOpsBus, Log, TEXT("Bus: pass %s registered"), *Name.ToString());
}

void FOpsEventBus::MarkDirty(FName Pass)
{
	for (FPass& Each : Passes)
	{
		if (Each.Name == Pass)
		{
			Each.bDirty = true;
			return;
		}
	}
	// A MISSPELT PASS IS A PASS THAT NEVER RUNS - said, not swallowed.
	UE_LOG(LogOpsBus, Warning, TEXT("Bus: MarkDirty(%s) names no registered pass"), *Pass.ToString());
}

void FOpsEventBus::MarkAllDirty()
{
	for (FPass& Each : Passes) { Each.bDirty = true; }
}

bool FOpsEventBus::AnyDirty() const
{
	return Passes.ContainsByPredicate([](const FPass& Each) { return Each.bDirty; });
}

int32 FOpsEventBus::Drain()
{
	check(!bDraining);
	bDraining = true;
	int32 Dispatched = 0;
	int32 Round = 0;
	for (; Round < MaxRounds && (Queue.Num() > 0 || AnyDirty()); ++Round)
	{
		// THE WHOLE QUEUE, taken: anything a handler publishes lands in Queue again and is the next
		// round's, so no handler runs inside another event's dispatch.
		TArray<FOpsEvent> Batch = MoveTemp(Queue);
		Queue.Reset();
		for (const FOpsEvent& Event : Batch)
		{
			UE_LOG(LogOpsBus, Verbose, TEXT("Bus: %s"), NameOf(Event));
			for (int32 Tier = 0; Tier < NumTiers; ++Tier)
			{
				CurrentTier = static_cast<EOpsTier>(Tier);
				for (const FHandler& Handler : Handlers[Event.GetIndex()][Tier])
				{
					Handler.Run(Event);
				}
			}
			++Dispatched;
		}
		// Passes run as Sim: they mutate the sim and may publish.
		CurrentTier = EOpsTier::Sim;
		for (FPass& Pass : Passes)
		{
			if (Pass.bDirty)
			{
				Pass.bDirty = false;
				Pass.Run();
			}
		}
	}
	if (Queue.Num() > 0 || AnyDirty())
	{
		FString Left;
		for (const FOpsEvent& Event : Queue) { Left += FString(NameOf(Event)) + TEXT(" "); }
		for (const FPass& Pass : Passes) { if (Pass.bDirty) { Left += TEXT("pass:") + Pass.Name.ToString() + TEXT(" "); } }
		UE_LOG(LogOpsBus, Error, TEXT("Bus: round cap (%d) hit; carried to next frame: %s"), MaxRounds, *Left.TrimEnd());
	}
	bDraining = false;
	return Dispatched;
}

int32 FOpsEventBus::Discard()
{
	const int32 Dropped = Queue.Num();
	Queue.Reset();
	UE_LOG(LogOpsBus, Log, TEXT("Bus: discarded %d queued event(s)"), Dropped);
	return Dropped;
}

TArray<FName> FOpsEventBus::SubscribersOf(SIZE_T TypeIndex) const
{
	TArray<FName> Out;
	if (TypeIndex < NumTypes)
	{
		for (int32 Tier = 0; Tier < NumTiers; ++Tier)
		{
			for (const FHandler& Handler : Handlers[TypeIndex][Tier]) { Out.Add(Handler.Who); }
		}
	}
	return Out;
}
```

- [ ] **Step 6: Build twice** (new files), confirm `Compile [x64] OpsEventBus.cpp` and `OpsEventBusTest.cpp`.
- [ ] **Step 7: Run** `Run-AirsideTests.ps1 -Filter AirportOps.Model.Bus`. Expect `6 test(s) run, 0 failed, 0 crashed`.
- [ ] **Step 8: Mutation check.** Temporarily swap the tier loop to run Presentation first; TierOrder must go red. Restore with `git checkout -- <file>` (NOT mv - mtime), rebuild, green.
- [ ] **Step 9: Commit** `feat(ops): FOpsEventBus - queued, tiered, closed event list`.

### Task 2: Runtime owns the bus; today's relays go through it

**Files:**
- Modify: `Plugins/AirportOps/Source/AirportOps/Public/Present/OpsRuntime.h`
- Modify: `Plugins/AirportOps/Source/AirportOps/Private/Present/OpsRuntime.cpp`
- Test: `Plugins/AirportOps/Source/AirportOpsTests/Private/OpsRuntimeBusTest.cpp` (new)
- Possibly modify: existing tests that asserted a relay synchronously (see Step 6).

**Interfaces:**
- Consumes: Task 1's `FOpsEventBus`, `FAgentPhaseEvent`, `FArrivalRefusedEvent`, `FSpeedChangedEvent`, `FNotificationEvent`.
- Produces: `FOpsEventBus& UOpsRuntime::GetBus()`; private `void WireBus()`; `UGroundTraffic* UOpsRuntime::LiveModel() const` (null when Target/Network/Traffic missing).

- [ ] **Step 1: Failing tests** in `OpsRuntimeBusTest.cpp`, using `FAirsideTestWorld` like `OpsRuntimeTest.cpp`:
  - `AirportOps.Present.Bus.EveryEventHasASubscriber`: `NewObject<UOpsRuntime>()`, `Attach(TestWorld.Actor)`; for each index `0..FOpsEventBus::NumTypes-1`, `TestTrue(FString::Printf(TEXT("%s has a subscriber"), Names[i]), Bus.SubscribersOf(i).Num() > 0)`. Reason in test text: "an event nobody consumes is the declared-never-read list CLAUDE.md names three times".
  - `AirportOps.Present.Bus.PhaseReachesBoardsThenUi`: bind a `UOpsEventsTestListener` to `Runtime->GetEvents()->OnAgentPhaseChanged`; broadcast `Actor->GetTraffic()->OnAgentPhaseChanged.Broadcast(42, EAgentPhase::Taxiing, EAgentPhase::Parked)`; assert `Listener->Seen` empty (queued, not synchronous); `Runtime->Tick(0.0)`; assert `Seen == {"phase:42:1->3"}` (ordinals as in `OpsEventsTest`).
  - `AirportOps.Present.Bus.ReattachDoesNotDouble`: Attach twice to the same actor, broadcast once, Tick; `Seen.Num() == 1`.
  - `AirportOps.Present.Bus.LoadDiscardsQueue`: Attach, `SaveToSlot(TEXT("BusTest"))`, broadcast a phase, `LoadFromSlot(TEXT("BusTest"))`, `Tick(0.0)`; assert the listener saw no `phase:` entry. Reason: "a Gone from ClearAgents names an agent that no longer exists".
  - `AirportOps.Present.Bus.PhaseAfterAgentRetired`: broadcast Parked for AgentId 999999 (no such agent), Tick; the test passes if it gets past Tick with no crash, and `TestTrue` on `Runtime->GetJobBoard() != nullptr`. Reason: "a handler runs a frame late and must tolerate the agent being gone".

- [ ] **Step 2: Build, run `-Filter AirportOps.Present.Bus`; expect compile failure on `GetBus`.**

- [ ] **Step 3: Header changes** in `OpsRuntime.h`: `#include "Model/OpsEventBus.h"`; public `FOpsEventBus& GetBus() { return Bus; }` with doc comment "The ops event bus - see FOpsEventBus. Subscriptions are made in WireBus and nowhere else."; private members `FOpsEventBus Bus;` (comment: plain C++, not a UPROPERTY - holds TFunctions; not saved, see spec §4), `void WireBus();`, `UGroundTraffic* LiveModel() const;`. Update the class comment's first paragraph: "relays Airside delegates onto the bus" now means PUBLISHES onto `FOpsEventBus`, and `UOpsEvents` is the bus's Presentation tier. Keep every existing sentence that is still true.

- [ ] **Step 4: .cpp changes:**
  - `LiveModel()`: returns `Target->GetTraffic()->GetModel()` when `Target`, `Target->Network`, `Target->GetTraffic()` are all non-null, else nullptr.
  - `WireBus()`:
```cpp
void UOpsRuntime::WireBus()
{
	// THE WHOLE SUBSCRIPTION MAP, in one function - see FOpsEventBus. Reset first: Attach runs
	// again on a level change, and a second wiring on top of the first would handle every
	// event twice.
	// ENFORCED BY: AirportOps.Present.Bus.ReattachDoesNotDouble
	Bus.ResetWiring();
	Bus.BeginWiring();

	// SIM: the boards, job board first - the order OnAgentPhase kept by hand before the bus.
	Bus.Subscribe<FAgentPhaseEvent>(EOpsTier::Sim, TEXT("JobBoard"), [this](const FAgentPhaseEvent& E)
	{
		if (UGroundTraffic* Model = LiveModel())
		{
			JobBoard->OnAgentPhase(*Model, *Target->Network, *Clock, E.AgentId, E.From, E.To);
		}
	});
	Bus.Subscribe<FAgentPhaseEvent>(EOpsTier::Sim, TEXT("FlightBoard"), [this](const FAgentPhaseEvent& E)
	{
		if (UGroundTraffic* Model = LiveModel())
		{
			FlightBoard->OnAgentPhase(*Model, *Target->Network, *Clock, E.AgentId, E.From, E.To);
		}
	});

	// PRESENTATION: UOpsEvents, the BP/UMG face of the bus. Its Notify* functions keep their
	// UE_LOG lines, so the log is unchanged.
	Bus.Subscribe<FAgentPhaseEvent>(EOpsTier::Presentation, TEXT("OpsEvents"),
		[this](const FAgentPhaseEvent& E) { Events->NotifyAgentPhaseChanged(E.AgentId, E.From, E.To); });
	Bus.Subscribe<FArrivalRefusedEvent>(EOpsTier::Presentation, TEXT("OpsEvents"),
		[this](const FArrivalRefusedEvent& E) { Events->NotifyArrivalRefused(E.Why); });
	Bus.Subscribe<FSpeedChangedEvent>(EOpsTier::Presentation, TEXT("OpsEvents"),
		[this](const FSpeedChangedEvent& E) { Events->NotifySpeedChanged(E.Speed); });
	Bus.Subscribe<FNotificationEvent>(EOpsTier::Presentation, TEXT("OpsEvents"),
		[this](const FNotificationEvent& E) { Events->NotifyNotification(E.Text); });

	Bus.EndWiring();
}
```
  - `Attach`: call `WireBus()` right after `Target = Actor;` null check passes (before binding Airside delegates).
  - `OnAgentPhase`: body becomes `Bus.Publish(FAgentPhaseEvent{ AgentId, From, To });` with a comment replacing "THE SERVICE FIRST, THEN THE BUS": "PUBLISHED, NOT HANDLED: this runs inside UGroundTraffic's broadcast, and nothing may act there (#193). The tier order in WireBus is what keeps 'the boards first, then the UI' now."
  - `OnArrivalRefused`: `Bus.Publish(FArrivalRefusedEvent{ Why });`
  - `ApplySpeed`: replace `Events->NotifySpeedChanged(Speed);` with `Bus.Publish(FSpeedChangedEvent{ Speed });`.
  - `SaveToSlot` / `LoadFromSlot`: every `Events->NotifyNotification(X)` becomes `Bus.Publish(FNotificationEvent{ X })`.
  - `SaveToSlot`: first line after the null check: `Bus.Drain();` with comment "DRAINED BEFORE THE SNAPSHOT: an Airside event queued since the last step would otherwise be handled after the save and missing from it."
  - `LoadFromSlot`: after `OpsSave::Restore` succeeds, `Bus.Discard();` with comment "The Gone events ClearAgents just queued name agents that no longer exist; the restored boards must never hear them." Then at the end (after `RearmRepeatingSchedules`) `Bus.MarkAllDirty();` (no passes yet; keeps the spec's rule in place for stage 3).
  - `Tick`: after `Clock->Advance(RealDeltaSeconds);` add `Bus.Drain();` with comment "ONE DRAIN, after the clock: the queue holds Airside's events from the motion tick in publish order, then anything the clock just fired." The JobBoard/FlightBoard Tick calls stay (stages 3-4 move them).

- [ ] **Step 5: Build, run `-Filter AirportOps.Present.Bus`. Expect 5 pass.**

- [ ] **Step 6: Full suite.** Some tests asserted a relay synchronously (a `UOpsEventsTestListener` read right after `StepSpeed`, or JobBoard state right after an Airside broadcast with no `Runtime->Tick`). For each failure: confirm it is that timing and ONLY that (read the assertion), add `Runtime->Tick(0.0);` before the read with a one-line comment "the bus delivers on the next ops step (spec 2026-09-29 §1)". Any failure that is not this shape is a real regression - stop and diagnose. Record each fixed test name for the PR body.

- [ ] **Step 7: Refactor contract.** `git diff origin/main...HEAD -- '*.cpp' | grep -c '^-.*UE_LOG('` vs `'^+.*UE_LOG('` - no net loss in the touched files. Comment lines in `OpsRuntime.cpp` do not fall.

- [ ] **Step 8: Commit** `feat(ops): runtime drains the bus; Airside relays, speed and notifications publish onto it`.

### Task 3: Stage 1 PR

- [ ] Rebase onto `origin/main`, rebuild, full suite, quote the line.
- [ ] `./Tools/Check-Architecture.ps1` clean (it runs first in the suite; quote its verdict line).
- [ ] Push `feature/ops-event-bus`, `gh pr create` - body: summary, build line, test line, the list from Task 2 Step 6, UE_LOG/comment deltas, "Behaviour change: Airside events handled at the next ops step, not inside the broadcast (spec §1)", unverified in PIE.

---

## Stage 2 - airline satisfaction

Branch `feature/airline-satisfaction` from `feature/ops-event-bus`.

### Task 4: Flights carry their airline; the flight board publishes offer and airborne events

**Files:**
- Modify: `Public/Model/Flight.h` (add `AirlineId`), `Private/Model/OfferGenerator.cpp` (`MakeOffer` sets it)
- Modify: `Public/Model/OpsEventBus.h` (add three events), `Public/Model/FlightBoard.h` (add `FOpsEventBus* Bus = nullptr;`), `Private/Model/FlightBoard.cpp` (publish)
- Modify: `Private/Present/OpsRuntime.cpp` (`FlightBoard->Bus = &Bus;` in Attach, beside `FlightBoard->Ledger = Ledger;`)
- Test: `Plugins/AirportOps/Source/AirportOpsTests/Private/FlightBoardEventsTest.cpp` (new)

**Interfaces - Produces:**
```cpp
struct FOfferExpiredEvent  { int32 FlightId; FName AirlineId; ELapseReason Reason; };      // "OfferExpired"
struct FOfferDeclinedEvent { int32 FlightId; FName AirlineId; };                           // "OfferDeclined"
struct FFlightAirborneEvent{ int32 FlightId; FName AirlineId; double LateBySeconds; };     // "FlightAirborne"
UPROPERTY() FName UFlight::AirlineId;   // the UAirlineDefinition's GetFName(); NAME_None for the debug flight
```
`Flight.h` is Model/ and `OpsEventBus.h` must include it for `ELapseReason`; `Flight.h` must NOT include `OpsEventBus.h`.

- [ ] **Step 1: Failing tests** (`FlightBoardEventsTest.cpp`), world-free where the existing `FlightBoardTest.cpp` is (read it first and reuse its fixture helpers; prefix any new helper `FbEvents`):
  - `AirportOps.Model.FlightBoard.Events.Declined`: a board with `Bus` set and a recording Sim subscriber for `FOfferDeclinedEvent`; add an Offered flight with `AirlineId = "Cumbria"`; `Decline`; `Bus.Drain()`; assert one event with that FlightId and AirlineId.
  - `...Events.Expired`: Offered flight, `OfferSecondsLeft = 1`, `TickOffers(..., 2.0)`; drain; one `FOfferExpiredEvent` whose `Reason` equals the flight's `LapseReason`.
  - `...Events.AirborneLateness`: drive the flight to Departing through `OnAgentPhase` as `FlightBoardTest` does, with `AcceptedAt = 100`, `ContractSeconds = 600`, clock at 1300; drain; `LateBySeconds == 600` (1300 - 700). Second case at clock 500: `LateBySeconds == -200` (early is negative, not clamped - the airline decides what early means).
  - `AirportOps.Model.Offers.MakeOfferCarriesAirline`: `MakeOffer` on a `UAirlineDefinition` named via `NewObject<UAirlineDefinition>(GetTransientPackage(), TEXT("TestAir"))`; `Flight->AirlineId == "TestAir"`.

- [ ] **Step 2: Build; compile failure expected.**
- [ ] **Step 3: Implement.**
  - `Flight.h`, beside `AirlineName`: 
```cpp
	/**
	 * WHICH airline, as a key: the UAirlineDefinition's object name. AirlineName is display text
	 * and a display text is not a key - two airlines may share a name, and a rename would orphan
	 * every flight. NAME_None for the debug flight (key 7), which belongs to no airline.
	 */
	UPROPERTY() FName AirlineId;
```
  - `MakeOffer`: `Offer->AirlineId = Airline.GetFName();` after `AirlineName`.
  - Events in `OpsEventBus.h` (with `EventName()`), appended to the `FOpsEvent` list after `FNotificationEvent`.
  - `FlightBoard.h`: `/** Where this board publishes what happened to its flights. Set by UOpsRuntime::Attach; null in a bare NewObject, and every publish checks. Raw: the runtime owns both. */ FOpsEventBus* Bus = nullptr;` (forward-declare `class FOpsEventBus;`, include `Model/OpsEventBus.h` in the .cpp).
  - `FlightBoard.cpp`:
    - `TickOffers`, after the lapse `UE_LOG`: `if (Bus != nullptr) { Bus->Publish(FOfferExpiredEvent{ Each->Id, Each->AirlineId, Each->LapseReason }); }`
    - `Decline`, after its `UE_LOG`: `if (Bus != nullptr) { Bus->Publish(FOfferDeclinedEvent{ Flight.Id, Flight.AirlineId }); }`
    - `OnAgentPhase`, inside `if (Flight->Phase == EFlightPhase::Departing && Flight->AirborneAt <= 0.0)` after setting `AirborneAt`: `if (Bus != nullptr) { Bus->Publish(FFlightAirborneEvent{ Flight->Id, Flight->AirlineId, Flight->AirborneAt - Flight->AirborneBy() }); }` with comment "LATENESS AGAINST THE CONTRACT the row showed at the offer - AirborneBy is AcceptedAt + ContractSeconds."
  - `OpsRuntime.cpp` Attach: `FlightBoard->Bus = &Bus;`
- [ ] **Step 4: Build twice (new test file); run `-Filter AirportOps.Model.FlightBoard.Events+AirportOps.Model.Offers.MakeOfferCarriesAirline`. Expect pass.**
- [ ] **Step 5: The wiring test now FAILS** (three events, no subscriber) - that is correct and Task 5 fixes it. Do not commit a red suite: go straight to Task 5 and commit both together.

### Task 5: `UAirlineRoster` - satisfaction as a Reaction

**Files:**
- Create: `Public/Model/AirlineRoster.h`, `Private/Model/AirlineRoster.cpp`
- Modify: `Public/Model/OpsDefinition.h` (tuning struct on `UScenario`)
- Modify: `Public/Model/OpsEventBus.h` (`FDayEndedEvent`, `FAirlineSatisfactionEvent`)
- Modify: `Public/Present/OpsRuntime.h/.cpp` (own roster, persist it, wire it, publish `FDayEndedEvent`)
- Test: `Plugins/AirportOps/Source/AirportOpsTests/Private/AirlineRosterTest.cpp` (new)

**Interfaces - Produces:**
```cpp
USTRUCT() struct FAirlineSatisfactionTuning { double Start=0.5, OnTimeBonus=0.03, LatePenaltyPerTenMinutes=0.02,
  LatePenaltyCap=0.10, IgnoredPenalty=0.02, NeverAcceptablePenalty=0.01, DailyDriftFraction=0.2,
  MinRateMultiplier=0.5, MaxRateMultiplier=1.5; };                       // UPROPERTY(EditAnywhere) each
USTRUCT() struct FAirlineSatisfactionChange { double Delta=0; FString Cause; };
USTRUCT() struct FAirlineStanding { FName AirlineId; double Satisfaction=0.5; TArray<FAirlineSatisfactionChange> Recent; };
struct FDayEndedEvent { int32 Day; };                                                     // "DayEnded"
struct FAirlineSatisfactionEvent { FName AirlineId; double Old; double New; FString Cause; }; // "AirlineSatisfaction"
UCLASS() class UAirlineRoster : public UObject, public IOpsPersistent {
  FName SaveBlobName() const -> "Airlines";
  UPROPERTY(Transient) FAirlineSatisfactionTuning Tuning;   // from the scenario, never the save
  FOpsEventBus* Bus = nullptr;
  void Ensure(FName AirlineId);                     // adds at Tuning.Start if absent
  const FAirlineStanding* Find(FName AirlineId) const;
  double RateMultiplier(FName AirlineId, bool bIsFloor) const;   // 1.0 if unknown
  void OnFlightAirborne(const FFlightAirborneEvent&);
  void OnOfferExpired(const FOfferExpiredEvent&);
  void OnOfferDeclined(const FOfferDeclinedEvent&);
  void OnDayEnded(const FDayEndedEvent&);
  virtual void OnBeforeRestore() override;          // Standings.Reset()
private:
  UPROPERTY() TArray<FAirlineStanding> Standings;   // saved
  void Apply(FName AirlineId, double Delta, const FString& Cause);   // clamp 0..1, Recent cap 5, publish
};
```
DEVIATION FROM THE SPEC, stated at the class: the spec named a `UAirline` UObject per airline; a USTRUCT row in one roster is used instead, the `FServiceVehicle`-in-`UJobBoard` shape - one object to save and one to wire, and nothing needs a UObject handle per airline yet.

- [ ] **Step 1: Failing tests** (`AirlineRosterTest.cpp`), world-free: a roster with default Tuning, a bus with a recording Presentation subscriber for `FAirlineSatisfactionEvent` (wire the roster's handlers into the bus in the test the same way WireBus will).
  - `AirportOps.Model.Airlines.OnTimeRaises`: `Ensure("A")`; `OnFlightAirborne({1,"A",-30})` -> 0.53; one event, Cause contains "on time".
  - `...LateLowers`: `LateBySeconds = 1500` (25 min -> 2.5 tenths -> 0.05) -> 0.45; `LateBySeconds = 6000` (100 min) -> capped at -0.10 -> 0.40.
  - `...ExpiredByReason`: Ignored -> 0.48; NeverAcceptable -> 0.49 (fresh airlines).
  - `...DeclinedIsFree`: satisfaction unchanged; NO satisfaction event (nothing changed).
  - `...DayDrift`: set to 0.9 via repeated on-time, `OnDayEnded` -> moves 20% of the way to 0.5 (0.9 -> 0.82); from 0.2 -> 0.26.
  - `...Clamped`: 40 late departures -> 0.0 not negative; multiplier 0.5.
  - `...FloorNeverBelowOne`: at 0.0, `RateMultiplier("A", true) == 1.0`, `RateMultiplier("A", false) == 0.5`; at 1.0 floor gets 1.5.
  - `...UnknownAirlineIgnored` (Review Focus 4): `OnFlightAirborne({1, NAME_None, 999})` -> no event, no standing added, no Warning logged; `RateMultiplier(NAME_None, false) == 1.0`.
  - `...RecentCapped`: 7 changes -> `Recent.Num() == 5`, newest last.
  - `...SaveRoundTrip`: set 0.7, `OpsSave::Capture`/`Restore` via `OpsSaveTestHelpers.h`'s helpers (read it; use what exists), roster restored at 0.7 with its Recent.

- [ ] **Step 2: Build twice; compile failure expected.**
- [ ] **Step 3: Implement** the header per Interfaces with WHY comments (why Transient Tuning: "the scenario's figures, not the save's - a save made before a retune must not carry the old tuning forward"; why Reaction tier; why declined is free: "declining is a legitimate choice (spec §3); an airline that punished it would make the inbox a trap"). `Apply`: `Old = S; S = Clamp(S + Delta, 0, 1); if (S == Old) return;` push `{S-Old, Cause}` onto Recent, trim front to 5, `UE_LOG(LogAirportOps, Log, TEXT("Airline %s: satisfaction %.2f -> %.2f (%s)"), ...)`, publish `FAirlineSatisfactionEvent`. Late penalty: `FMath::Min(Tuning.LatePenaltyCap, Tuning.LatePenaltyPerTenMinutes * (LateBySeconds / 600.0))`. Cause strings: `"on time"`, `FString::Printf(TEXT("late departure (%d min)"), FMath::CeilToInt(LateBySeconds / 60.0))`, `"offer ignored"`, `"offer never acceptable"`, `"a day's forgiveness"`. Every handler returns early on `AirlineId.IsNone()` or an unknown id (Verbose log) - never `Ensure` from an event: the roster is seeded from the catalog, so an unknown id is the debug flight or a removed airline. `RateMultiplier`: `Lerp(Min, Max, S)`, `bIsFloor ? Max(m, 1.0) : m`.
  - `OpsDefinition.h`: `FAirlineSatisfactionTuning` USTRUCT (each field `UPROPERTY(EditAnywhere, Category="Airlines")`, header comment "FIRST GUESSES, unjudged (2026-09-29) - tune once seen in play"), and on `UScenario`: `UPROPERTY(EditAnywhere, Category = "Airlines") FAirlineSatisfactionTuning AirlineSatisfaction;`
  - `OpsEventBus.h`: add `FDayEndedEvent`, `FAirlineSatisfactionEvent` to the list.
  - `OpsRuntime.h/.cpp`: `UPROPERTY() TObjectPtr<UAirlineRoster> Airlines;` created in the constructor; `UAirlineRoster* GetAirlines() const`; `Persistents()` adds it (after OfferGenerator); Attach: `Airlines->Tuning = Scenario->AirlineSatisfaction;` in the scenario block, `Airlines->Bus = &Bus;`, and after `AirlineOffers = AirlineOffersFromCatalog();` `for (const FAirlineOffers& Each : AirlineOffers) { if (Each.Airline) Airlines->Ensure(Each.Airline->GetFName()); }`. LoadFromSlot: re-run that Ensure loop after Restore (a save from before this blob restores none). WireBus adds (Reaction tier, name `"Airlines"`): `FFlightAirborneEvent`, `FOfferExpiredEvent`, `FOfferDeclinedEvent`, `FDayEndedEvent` -> the roster handlers; Presentation `"Log"` subscriber for `FAirlineSatisfactionEvent` that does `UE_LOG(LogOpsBus, Log, TEXT("Airline %s satisfaction %.2f -> %.2f: %s"), ...)` (this is the PIE check line). `PostDailyUpkeep`: after `FlightBoard->RollUp`, `Bus.Publish(FDayEndedEvent{ Clock->Day() });`.
- [ ] **Step 4: Build; run `-Filter AirportOps.Model.Airlines+AirportOps.Present.Bus+AirportOps.Model.FlightBoard.Events`. Expect all pass including EveryEventHasASubscriber.**
- [ ] **Step 5: Mutation check:** remove the `FOfferExpiredEvent` subscription in WireBus -> `EveryEventHasASubscriber` red naming `OfferExpired`. Restore via `git checkout -p`/re-edit, touch, rebuild, green.
- [ ] **Step 6: Commit** Tasks 4+5 together: `feat(ops): airline satisfaction - flights carry their airline, roster reacts to offers and departures`.

### Task 6: Satisfaction scales the offer rate

**Files:**
- Modify: `Public/Model/OfferGenerator.h`, `Private/Model/OfferGenerator.cpp`
- Modify: `Private/Present/OpsRuntime.cpp` (wire `AirlineFactorOf`; banner uses it)
- Modify: `Source/AirportMgr/OfferViewModels.h/.cpp`, `Source/AirportMgr/OfferInboxWidget.cpp` (strip passes it)
- Test: `Plugins/AirportOps/Source/AirportOpsTests/Private/OfferGeneratorTest.cpp` (extend), `OpsRuntimeBusTest.cpp` (composition)

**Interfaces - Produces:**
```cpp
static double RateAt(const UAirlineDefinition&, double TimeOfDaySeconds, bool bDaylight, double DemandFactor, double AirlineFactor = 1.0);
static double TotalRateAt(TArrayView<const FAirlineOffers>, double, bool, double DemandFactor, TFunctionRef<double(const UAirlineDefinition&)> AirlineFactorOf);
// the existing 4-arg TotalRateAt forwards with [](const UAirlineDefinition&) { return 1.0; }
TFunction<double(const UAirlineDefinition&)> UOfferGenerator::AirlineFactorOf;   // unset -> 1.0
double UOfferGenerator::AirlineFactor(const UAirlineDefinition&) const;          // AirlineFactorOf or 1.0
static TArray<double> UOfferInboxViewModel::SampleDemand(Airlines, Clock, DemandFactor, Count, TFunctionRef<double(const UAirlineDefinition&)>);  // overload
```

- [ ] **Step 1: Failing tests** (append to `OfferGeneratorTest.cpp`, next to the existing `RateAt` cases at ~line 212, reusing their airline setup):
  - `RateAt(*Airline, 7.5h, true, 1.0, 1.5)` == 1.5x the factor-1 figure (5.0 -> 7.5).
  - With a floor (`FloorOffersPerHour` as in the `Club` case at ~248): `RateAt(*Club, 10h, true, 1.0, 0.5)` never below the floor figure - "the floor is not scaled by the fee, nor by the airline's mood".
  - Composition (`OpsRuntimeBusTest.cpp`, `AirportOps.Present.Bus.SatisfactionMovesRate`): Attach; take the first `GetAirlineOffers()` airline that is not `bIsFloor` (skip test with `AddInfo` if none in content); record `Runtime->GetOfferGenerator()->AirlineFactor(*Airline)`; publish 10 `FFlightAirborneEvent{0, Name, 6000}` on `Runtime->GetBus()`; `Tick(0.0)`; factor strictly less. Reason: "the multiplier reaches the generator through the runtime's wiring, not only in the roster".
- [ ] **Step 2: Build; fails.**
- [ ] **Step 3: Implement.** `RateAt`: `const double Demand = Airline.PeakOffersPerHour * Airline.CurveAt(T) * FMath::Max(DemandFactor, 0.0) * FMath::Max(AirlineFactor, 0.0);` - floor line untouched; extend the header comment with "AND THE AIRLINE'S OWN FACTOR (satisfaction, spec 2026-09-29 §3) scales demand but not the floor, for the fee's reason." In `TickMinute`, `const double Rate = RateAt(Airline, TimeOfDay, bDaylight, Factor, AirlineFactor(Airline));`. Runtime Attach, after the roster Ensure loop: 
```cpp
	// THE AIRLINE'S MOOD reaches demand through the generator's one reader - READ, not subscribed:
	// a rate is a value asked for when needed. Weak for the dispatcher's reason.
	TWeakObjectPtr<UAirlineRoster> WeakAirlines = Airlines;
	OfferGenerator->AirlineFactorOf = [WeakAirlines](const UAirlineDefinition& Airline)
	{
		const UAirlineRoster* Roster = WeakAirlines.Get();
		return Roster != nullptr ? Roster->RateMultiplier(Airline.GetFName(), Airline.bIsFloor) : 1.0;
	};
```
  Banner `TotalRateAt` call and `SampleDemand` call in `OfferInboxWidget.cpp` pass `[Gen](const UAirlineDefinition& A) { return Gen->AirlineFactor(A); }` so the strip draws the rate the generator follows ("ONE FUNCTION" comment already says why).
- [ ] **Step 4: Build; run `-Filter AirportOps.Model.Offers+AirportOps.Present.Bus+AirportMgr`. Expect pass (existing `OfferViewModelsTest` TotalRateAt check still holds via the forwarding overload).**
- [ ] **Step 5: Commit** `feat(offers): airline satisfaction scales its offer rate; floor untouched`.

### Task 7: The inbox row says how the airline feels

**Files:**
- Modify: `Source/AirportMgr/OfferViewModels.h/.cpp`, `Source/AirportMgr/OfferInboxWidget.cpp`
- Test: `Source/AirportMgr/OfferViewModelsTest.cpp` (extend)

**Interfaces - Produces:** `static FText UOfferViewModel::DescribeSatisfaction(const FAirlineStanding*)`; `FText UOfferViewModel::GetSatisfaction() const`; `Refresh(Board, Traffic, Network, Clock, const UAirlineRoster* Airlines = nullptr)` on both viewmodels.

- [ ] **Step 1: Failing test** in `OfferViewModelsTest.cpp`: `DescribeSatisfaction(nullptr)` is empty; a standing at 0.62 with Recent `{-0.04, "late departure (25 min)"}` -> `"62% \u25BC late departure (25 min)"`; at 0.53 with `{+0.03, "on time"}` -> `"53% \u25B2 on time"`; a standing with no Recent -> `"50%"`.
- [ ] **Step 2: Implement** `DescribeSatisfaction` (percent rounded, arrow by sign of the newest Recent delta); row `Refresh` sets `Satisfaction = DescribeSatisfaction(Airlines ? Airlines->Find(Live->AirlineId) : nullptr)`; inbox `Refresh` forwards `Airlines` to each row; widget `Refresh` passes `Runtime->GetAirlines()`; `PaintRows` AirlineText becomes `"{callsign}  {airline}  \u00B7  {satisfaction}"` when satisfaction is non-empty (keep the existing two branches otherwise). `UPROPERTY(Transient) FText Satisfaction;` on the row - a new UPROPERTY, so this task needs a full build (no Live Coding).
- [ ] **Step 3: Build; run `-Filter AirportMgr+AirportOps`. Expect pass.**
- [ ] **Step 4: Commit** `feat(ui): offer row shows the airline's satisfaction and why`.

### Task 8: Stage 2 verification and PR

- [ ] Rebase `feature/airline-satisfaction` on `feature/ops-event-bus` (and that on `origin/main` if main moved), full build, full suite, Check-Architecture; quote lines.
- [ ] Build the MAIN checkout? NO - the user is AFK and the main checkout is theirs; leave it. PIE check on the worktree instead if the editor can be launched: `UnrealEditor.exe C:\repos\airportmgr2-ops-event-bus\AirportMgr.uproject -ModelContextProtocolPort=8002`, start PIE via `Tools/Mcp.py` (`AIRSIDE_MCP_PORT=8002`), set x32, wait for a departure, then `python Tools/Mcp.py log LogOpsBus "satisfaction"` - expect `Airline <id> satisfaction 0.50 -> 0.53: on time` (or a late figure), and `shot out.png editor` of the inbox row. If PIE cannot produce a departure unattended, say so in the PR: "unverified in PIE".
- [ ] Push, `gh pr create --base feature/ops-event-bus` with build/test lines, tuning table marked unjudged, save-blob note ("Airlines" blob; no player saves yet), Review Focus list and which tests pin each.
- [ ] File the found-not-fixed issue: flights restored in Landing..TaxiOut keep a dead `AgentId` (spec §4).
