#pragma once

#include "CoreMinimal.h"
#include "AirportOpsLog.h"
#include "Misc/TVariant.h"
#include "Model/Airport.h"
#include "Model/ArrivalPlanner.h"
#include "Model/BuildPurse.h"
#include "Model/Flight.h"
#include "Model/Ledger.h"
#include "Model/OpsAlerts.h"
#include "Model/RoadAgent.h"
#include "Model/RoadEntity.h"
#include "Model/ServiceJob.h"
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

/** An offer lapsed unanswered. Reason says whether it could ever have been taken; bFloorAirline is
 *  the flight's own UFlight::bFloorAirline - a floor airline's lapse never costs the player (rulings 7-8). */
struct AIRPORTOPS_API FOfferExpiredEvent
{
	int32 FlightId = 0;
	FName AirlineId;
	ELapseReason Reason = ELapseReason::None;
	bool bFloorAirline = false;
	static const TCHAR* EventName() { return TEXT("OfferExpired"); }
	FString Describe() const;
};

/** The player declined an offer. */
struct AIRPORTOPS_API FOfferDeclinedEvent
{
	int32 FlightId = 0;
	FName AirlineId;
	static const TCHAR* EventName() { return TEXT("OfferDeclined"); }
	FString Describe() const;
};

/**
 * A flight left the ground. LateBySeconds is AirborneAt - AirborneBy(): the contract the inbox row
 * showed at the offer. Negative is early, and is not clamped - what early is worth is the listener's
 * decision, not the publisher's.
 */
struct AIRPORTOPS_API FFlightAirborneEvent
{
	int32 FlightId = 0;
	FName AirlineId;
	double LateBySeconds = 0.0;
	static const TCHAR* EventName() { return TEXT("FlightAirborne"); }
	FString Describe() const;
};

/**
 * The attached network changed - a different network object, or a new guideline revision (a road, a
 * stand, a depot drawn or removed). Published by UOpsRuntime::Tick from one compare a frame: the same
 * revision the job board's re-bid and re-offer gates read, so this cannot disagree with them.
 */
struct AIRPORTOPS_API FNetworkChangedEvent
{
	uint32 GuidelineRevision = 0;
	static const TCHAR* EventName() { return TEXT("NetworkChanged"); }
	FString Describe() const;
};

/** A game day ended - published by UOpsRuntime's daily beat, after the upkeep and the roll-up. */
struct AIRPORTOPS_API FDayEndedEvent
{
	int32 Day = 0;
	static const TCHAR* EventName() { return TEXT("DayEnded"); }
	FString Describe() const;
};

/** An airline's satisfaction moved. Published by UAirlineRoster from the Reaction tier. */
struct AIRPORTOPS_API FAirlineSatisfactionEvent
{
	FName AirlineId;
	double Old = 0.0;
	double New = 0.0;
	FString Cause;
	static const TCHAR* EventName() { return TEXT("AirlineSatisfaction"); }
	FString Describe() const;
};

/**
 * A facility bought a module (facility-upgrades spec §3). Published by UFacilityPurchases on success only.
 * Entity is the depot's INDEX - an id, never a pointer (spec 2026-09-29-ops-event-bus §4).
 */
struct AIRPORTOPS_API FFacilityUpgradedEvent
{
	int32 Entity = INDEX_NONE;
	EDepotModule Module = EDepotModule::Shed;
	double Amount = 0.0;
	static const TCHAR* EventName() { return TEXT("FacilityUpgraded"); }
	FString Describe() const;
};

/** Bought or sold. A plain enum - this header has no .generated.h for a UENUM (memory: UHT cannot see it). */
enum class EFleetChange : uint8
{
	Bought,
	Sold
};

/** A vehicle joined or left a depot's fleet. Published by UFacilityPurchases on success only. */
struct AIRPORTOPS_API FFleetChangedEvent
{
	int32 Depot = INDEX_NONE;
	int32 VehicleId = 0;
	FName TypeCode;
	EFleetChange Change = EFleetChange::Bought;
	double Amount = 0.0;
	static const TCHAR* EventName() { return TEXT("FleetChanged"); }
	FString Describe() const;
};

/**
 * An event logged at VERBOSE when published, not Log. Only what fires in bulk belongs here: every agent
 * phase change of every aircraft and vehicle would bury the rest of the file. Everything else is Log,
 * so the default log is a complete trace of what happened (user, 2026-09-29).
 */
template <typename T> struct TOpsEventIsChatty { static constexpr bool Value = false; };
template <> struct TOpsEventIsChatty<FAgentPhaseEvent> { static constexpr bool Value = true; };

/** A standing problem started (UOpsAlerts's diff). Spec 2026-09-29-ops-alerts §1. */
struct AIRPORTOPS_API FAlertRaisedEvent
{
	FOpsAlert Alert;
	static const TCHAR* EventName() { return TEXT("AlertRaised"); }
	FString Describe() const;
};

/** A standing problem stopped being true - whatever made it stop. */
struct AIRPORTOPS_API FAlertClearedEvent
{
	FOpsAlertKey Key;
	static const TCHAR* EventName() { return TEXT("AlertCleared"); }
	FString Describe() const;
};

/** Every alert was forgotten (UOpsAlerts::Reset - a load or an attach): a UI list empties, and the raises
 *  that follow are re-raises (FOpsAlert::bReRaised). */
struct AIRPORTOPS_API FAlertsResetEvent
{
	static const TCHAR* EventName() { return TEXT("AlertsReset"); }
	FString Describe() const;
};

/** Money moved - ULedger::Post, the one funnel for every fee, charge, credit and reversal. */
struct AIRPORTOPS_API FMoneyPostedEvent
{
	int32 EntryId = 0;
	ELedgerCategory Category = ELedgerCategory::LandingFee;
	double Amount = 0.0;
	double Balance = 0.0;
	static const TCHAR* EventName() { return TEXT("MoneyPosted"); }
	FString Describe() const;
};

/** The balance crossed zero - overdrawn locks every paid placement (ULedger::CanAfford). */
struct AIRPORTOPS_API FBalanceSignChangedEvent
{
	bool bOverdrawn = false;
	static const TCHAR* EventName() { return TEXT("BalanceSignChanged"); }
	FString Describe() const;
};

/** A build refused at commit (URoadEditFacade::OnRefused), priced by the purse for the toast. */
struct AIRPORTOPS_API FBuildRefusedEvent
{
	FString What;
	EBuildRefusal Why = EBuildRefusal::CannotAfford;
	/** The purse's own wording of the price ("£120,000") - Airside knows only the base amount. */
	FString Price;
	/** The balance, worded by UPricing::Format - the toast has no pricing of its own to word it with. */
	FString Balance;
	static const TCHAR* EventName() { return TEXT("BuildRefused"); }
	FString Describe() const;
};

/** Key 7 (UOpsRuntime::LandNear) refused before any dispatch - so Airside's OnArrivalRefused never fired. */
struct AIRPORTOPS_API FLandRefusedEvent
{
	EArrivalRefusal Why = EArrivalRefusal::None;
	static const TCHAR* EventName() { return TEXT("LandRefused"); }
	FString Describe() const;
};

/**
 * The player accepted an offer - UFlightBoard::Accept, once the stand is held. Accept is a player command
 * called on the board straight from the game module (OfferViewModels), so before this event an accept
 * dirtied no pass at all. No toast (spec 2026-09-29-ops-batch3 §0): the flight moving into the accepted
 * list is the feedback. No roster score either - accepting is not something the airline experiences.
 */
struct AIRPORTOPS_API FOfferAcceptedEvent
{
	int32 FlightId = 0;
	FName AirlineId;
	/** The stand Accept just held for it. */
	FEntityInstanceId Stand;
	static const TCHAR* EventName() { return TEXT("OfferAccepted"); }
	FString Describe() const;
};

/**
 * An aircraft left for a departing phase - published by UJobBoard::EndTurnaround, the ONE publisher, when the
 * aircraft's Parked -> departing phase change reaches the job board. Two callers: DropAircraft for an aircraft that
 * was turned around, whoever sent it - DepartTheReady or the inspector's manual Depart (batch 3 review I1) - and
 * OnAgentPhase for one that departed without ever being turned around, which leaves Unfuelled (whole-stack review
 * M4). A refused departure changes no phase, so ends nothing;
 * a retire (Gone) is not a departure. Outcome is derived from the litres (UJobBoard::FuelOutcomeOf).
 *
 * NAMES THE AGENT, NOT THE FLIGHT: the job board does not know flights, and must not learn them. The
 * airline roster resolves the flight through UFlightBoard::FlightForAgent when it hears this - the
 * flight is still the agent's then: the flight board unhooks the agent only at its Gone, which is
 * published after this and so dispatched in a later round.
 * ENFORCED BY: AirportOps.Present.Bus.UnfuelledDepartureLowersAirline
 */
struct AIRPORTOPS_API FTurnaroundEndedEvent
{
	int32 AircraftAgentId = INDEX_NONE;
	FEntityInstanceId Stand;
	EFuelOutcome Outcome = EFuelOutcome::Fuelled;
	/** Litres delivered and litres the flight asked for - both 0 for an aircraft that wanted none. */
	double Delivered = 0.0;
	double Wanted = 0.0;
	static const TCHAR* EventName() { return TEXT("TurnaroundEnded"); }
	FString Describe() const;
};

/**
 * The airport's status changed (UAirport::Refresh) - the network lost or gained its last runway, or the player
 * closed or reopened it. NOT published by an attach or a load, which re-derive silently (UAirport::Reseat): entering
 * a closed status cancels flights, and a load must never do that again (spec 2026-09-29-ops-batch3 §3).
 */
struct AIRPORTOPS_API FAirportStatusChangedEvent
{
	EAirportStatus Old = EAirportStatus::Open;
	EAirportStatus New = EAirportStatus::Open;
	static const TCHAR* EventName() { return TEXT("AirportStatusChanged"); }
	FString Describe() const;
};

/**
 * A flight will not come, or will not finish: cancelled by a closure (UFlightBoard::CancelUnarrived) or by the
 * player despawning its aeroplane (UFlightBoard::CancelByAgent). Heard by the roster, which charges only
 * AirportClosed. An offer WITHDRAWN by a closure is not a cancellation and publishes nothing.
 * ENFORCED BY: AirportOps.Model.FlightBoard.CancelUnarrivedCancelsAndWithdraws, AirportOps.Model.FlightBoard.CancelByAgentPublishesUnstuck, AirportOps.Model.Airlines.ClosureCancelScoresOnlyAirportClosed
 */
struct AIRPORTOPS_API FFlightCancelledEvent
{
	int32 FlightId = 0;
	FName AirlineId;
	ECancelReason Reason = ECancelReason::AirportClosed;
	static const TCHAR* EventName() { return TEXT("FlightCancelled"); }
	FString Describe() const;
};

/**
 * A runway's strip was held and is not now - Airside's UGroundTraffic::OnRunwayFreed, bridged by UOpsRuntime::Attach.
 * DERIVED by Airside's diff of the arrival queue's own predicate, so every way a runway frees - a take-off, a landing
 * vacating, a taxiing crossing clearing, a despawn, a deletion - is this one event (ops batch 3 §5).
 * ENFORCED BY: AirportOps.Present.Bus.FreedIsBridged
 */
struct AIRPORTOPS_API FRunwayFreedEvent
{
	/** The strip's seed as Airside's runway summary names it - any member walks the whole chain. */
	FRoadSegmentId Seed;
	static const TCHAR* EventName() { return TEXT("RunwayFreed"); }
	FString Describe() const;
};

/** Stand pose nodes that were held (a flight's reservation, an aircraft's goal or body) and are not now - Airside's
 *  UGroundTraffic::OnStandsFreed, bridged like FRunwayFreedEvent. */
struct AIRPORTOPS_API FStandsFreedEvent
{
	TArray<FGuidelineNodeId> PoseNodes;
	static const TCHAR* EventName() { return TEXT("StandsFreed"); }
	FString Describe() const;
};

/**
 * A flight came due and joined the arrival queue - UFlightBoard::Enqueue, its one site (a Clock.At callback, or the
 * load's RearmSchedules for one already overdue). The queue pass hears it; before PR D the queue was ticked every
 * frame and needed no word.
 * ENFORCED BY: AirportOps.Model.FlightBoard.EnqueuePublishesInbound
 */
struct AIRPORTOPS_API FFlightInboundEvent
{
	int32 FlightId = 0;
	FName AirlineId;
	static const TCHAR* EventName() { return TEXT("FlightInbound"); }
	FString Describe() const;
};

/**
 * EVERY EVENT THERE IS, as one closed list. Subscribe<T> and Publish<T> are compile-checked
 * against it, and the wiring test walks it - "lists that must agree are ONE list".
 * FInstancedStruct was rejected: an open set has no answer to "which events exist?".
 */
using FOpsEvent = TVariant<FAgentPhaseEvent, FArrivalRefusedEvent, FSpeedChangedEvent, FNotificationEvent,
	FOfferExpiredEvent, FOfferDeclinedEvent, FFlightAirborneEvent, FDayEndedEvent, FAirlineSatisfactionEvent,
	FNetworkChangedEvent, FAlertRaisedEvent, FAlertClearedEvent, FAlertsResetEvent, FBuildRefusedEvent, FLandRefusedEvent,
	FMoneyPostedEvent, FBalanceSignChangedEvent, FFacilityUpgradedEvent, FFleetChangedEvent, FOfferAcceptedEvent,
	FTurnaroundEndedEvent, FAirportStatusChangedEvent, FFlightCancelledEvent, FRunwayFreedEvent, FStandsFreedEvent,
	FFlightInboundEvent>;

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
	 * Dirty for the NEXT Drain, not this one - "try again next frame". A pass that re-marked ITSELF
	 * with MarkDirty would run again the very next round and burn the round cap in one frame; a retry
	 * (a departure the runway refused) wants exactly one more look per frame until it resolves.
	 * ENFORCED BY: AirportOps.Model.Bus.NextDrainIsNextFrame
	 */
	void MarkDirtyNextDrain(FName Pass);

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

	/** Whether Pass is marked to run in the next round - for a test pinning who marks it (an attach, a load). */
	bool IsDirtyForTest(FName Pass) const
	{
		return Passes.ContainsByPredicate([Pass](const FPass& Each) { return Each.Name == Pass && Each.bDirty; });
	}

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
		/** See MarkDirtyNextDrain. Promoted to bDirty at the start of the next Drain. */
		bool bDirtyNextDrain = false;
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
