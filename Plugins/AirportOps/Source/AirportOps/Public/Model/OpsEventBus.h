#pragma once

#include "CoreMinimal.h"
#include "AirportOpsLog.h"
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
	/**
	 * Read-only by convention - BP/UMG, toasts, audio. NOT ENFORCED by dropping publishes (it was,
	 * until the stage 1 review): a Presentation handler that triggers a player command - an autosave
	 * on a notification, a speed step - mutates the sim through that command anyway, and dropping the
	 * command's own announcement ("Saved", SpeedChanged) only desynced the UI from what happened. So
	 * what it publishes queues for the next round like anything else.
	 * ENFORCED BY: AirportOps.Model.Bus.PresentationPublishIsNextRound
	 */
	Presentation
};

// THE EVENTS. Small value structs - ids and figures, never a UObject pointer that may be dead by
// the time the queue drains (spec §4 "stale ids"). Each names itself for the log and the wiring test,
// and DESCRIBES itself - its fields, for the log line FOpsEventBus writes when it is published. An event
// without a Describe() does not compile: the bus's describer visits every type in FOpsEvent.
// ENFORCED BY: AirportOps.Model.Bus.EveryEventDescribesItself

/** Airside's agent phase change, bridged by UOpsRuntime. */
struct AIRPORTOPS_API FAgentPhaseEvent
{
	int32 AgentId = INDEX_NONE;
	EAgentPhase From = EAgentPhase::Gone;
	EAgentPhase To = EAgentPhase::Gone;
	static const TCHAR* EventName() { return TEXT("AgentPhase"); }
	FString Describe() const;
};

/** Airside refused an arrival. */
struct AIRPORTOPS_API FArrivalRefusedEvent
{
	EArrivalRefusal Why = EArrivalRefusal::None;
	static const TCHAR* EventName() { return TEXT("ArrivalRefused"); }
	FString Describe() const;
};

/** The player's speed setting changed. */
struct AIRPORTOPS_API FSpeedChangedEvent
{
	ESimSpeed Speed = ESimSpeed::X1;
	static const TCHAR* EventName() { return TEXT("SpeedChanged"); }
	FString Describe() const;
};

/** A line for the toast stack (saved, loaded, ...). */
struct AIRPORTOPS_API FNotificationEvent
{
	FString Text;
	static const TCHAR* EventName() { return TEXT("Notification"); }
	FString Describe() const;
};

/**
 * An event logged at VERBOSE when published, not Log. Only what fires in bulk belongs here: every agent
 * phase change of every aircraft and vehicle would bury the rest of the file. Everything else is Log,
 * so the default log is a complete trace of what happened (user, 2026-09-29).
 */
template <typename T> struct TOpsEventIsChatty { static constexpr bool Value = false; };
template <> struct TOpsEventIsChatty<FAgentPhaseEvent> { static constexpr bool Value = true; };

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
 * UOpsRuntime; anything given a raw pointer to it must be owned by the runtime too, so the two die
 * together.
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
		constexpr SIZE_T Index = FOpsEvent::IndexOfType<T>();
		Handlers[Index][static_cast<int32>(Tier)].Add(
			{ Who, [Handler = MoveTemp(Handler)](const FOpsEvent& Event) { Handler(Event.Get<T>()); } });
		LogSubscription(Who, Tier, T::EventName());
	}

	/** A coalesced bulk step, run once after a round in which it was marked dirty. Order of
	 *  registration is the order dirty passes run. */
	void RegisterPass(FName Name, TFunction<void()> Run);
	void MarkDirty(FName Pass);
	void MarkAllDirty();

	/**
	 * Queue Event, and log it with its fields - "Bus: + OfferExpired {flight 12, airline Cumbria,
	 * Ignored}". LOGGED HERE, AT THE SOURCE, not only when dispatched: the line then sits beside
	 * whatever raised it, and an event dropped by a load's Discard still appears once.
	 */
	template <typename T>
	void Publish(T&& Event)
	{
		using FEvent = std::decay_t<T>;
		if constexpr (TOpsEventIsChatty<FEvent>::Value)
		{
			UE_LOG(LogOpsBus, Verbose, TEXT("Bus: + %s {%s}"), FEvent::EventName(), *Event.Describe());
		}
		else
		{
			UE_LOG(LogOpsBus, Log, TEXT("Bus: + %s {%s}"), FEvent::EventName(), *Event.Describe());
		}
		Queue.Emplace(TInPlaceType<FEvent>(), Forward<T>(Event));
	}

	/** Any queued event's fields, as Describe() gives them. For the dispatch and discard lines. */
	static FString Describe(const FOpsEvent& Event);

	/** Run rounds until quiet or MaxRounds. Returns the number of events dispatched. */
	int32 Drain();

	/** Drop the queue unhandled (a load, a detach). Returns how many were dropped, and logs it. */
	int32 Discard();

	/** True inside Drain - for a caller that would otherwise re-enter it (a save from a handler). */
	bool IsDraining() const { return bDraining; }

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

	static const TCHAR* NameOf(const FOpsEvent& Event);
	static void LogSubscription(FName Who, EOpsTier Tier, const TCHAR* Event);
	bool AnyDirty() const;
};
