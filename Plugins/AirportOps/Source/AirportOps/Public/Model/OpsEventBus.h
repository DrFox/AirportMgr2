#pragma once

#include "CoreMinimal.h"
#include "AirportOpsLog.h"
#include "Misc/TVariant.h"
#include "Model/Airport.h"
#include "Model/AirlineRoster.h"
#include "Model/ArrivalPlanner.h"
#include "Model/BuildPurse.h"
#include "Model/Flight.h"
#include "Model/Ledger.h"
#include "Model/OpsAlerts.h"
#include "Model/OpsEvents.h"
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

/**
 * WHY A PASS IS BEING MARKED DIRTY (#445) - the fact the job board's and the arrival queue's passes each used to keep as a
 * hand-set pair of flags on UOpsRuntime (a "covered" one their event funnel set and a "safety due" one the net set), so a
 * run the net alone asked for could be told from one something asked for: the net finding work NO EVENT covered is a missing
 * event, and it says so as a Warning. The bus keeps it for EVERY pass now, so a third net-watched pass costs no field.
 *
 * THE ORDINAL IS THE PRECEDENCE: a pass marked by the net and then by an event in the same drain is an Event run - something
 * asked for it, so finding work is no defect. Retry is an event's own "look again next frame" (MarkDirtyNextDrain) and counts
 * as asked-for: the retry is the tail of a run something asked for.
 * ENFORCED BY: AirportOps.Model.Bus.PassRunCarriesItsCause, Check-Architecture rule 59 (safety-cause-minted-by-the-net)
 */
enum class EPassCause : uint8
{
	/** Only FOpsSafetyNet's clock entry asked. The one cause a run is suspect for. */
	SafetyNet,
	/** The pass asked to be looked at again next frame. */
	Retry,
	/** An event, a command, a deadline, an attach or a load's catch-up asked - the default. */
	Event
};

/** What a pass is told when it runs: the strongest cause that marked it since it last ran. */
struct FPassRun
{
	EPassCause Cause = EPassCause::Event;

	/** True when nothing but the safety net asked for this run - a run that does work is then a defect to name. */
	bool IsSafetyOnly() const { return Cause == EPassCause::SafetyNet; }
};

// THE EVENTS. Small value structs - ids and figures, never a UObject pointer that may be dead by
// the time the queue drains (spec §4 "stale ids"). Each names itself for the log and the wiring test,
// and DESCRIBES itself - its fields, for the log line FOpsEventBus writes when it is published. An event
// without a Describe() does not compile: the bus's describer visits every type in FOpsEvent.
// ENFORCED BY: AirportOps.Model.Bus.EveryEventDescribesItself

/**
 * Airside's agent phase change, bridged by UOpsRuntime: the FAgentTransition itself (#436), Cause and GoalAtEvent
 * included, so a handler a drain later reads what was true when the change was MADE rather than asking the live
 * agent. DERIVED, not wrapped or copied field by field: the event IS the transition - one struct per thing - and
 * a copy of its fields here is where a new one would be dropped on the way across.
 */
struct AIRPORTOPS_API FAgentPhaseEvent : FAgentTransition
{
	static const TCHAR* EventName() { return TEXT("AgentPhase"); }
	FString Describe() const;
};

/** Airside refused an arrival - a dispatch the planner turned down. */
struct AIRPORTOPS_API FArrivalRefusedEvent
{
	EArrivalRefusal Why = EArrivalRefusal::None;
	/** The plan's own sentence (UGroundTraffic::OnArrivalRefused carries it), which the toast shows - FLandRefusedEvent's
	 *  reason for carrying one too (#471). */
	FString Sentence;
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

/**
 * A save or a load came to Outcome, on Slot (#445 item 7). REPLACED FNotificationEvent{FString}, the catch-all that carried
 * the runtime's own English line ("Save to 'X' failed") to the toast as plain Info. The case is typed (EOpsSaveOutcome,
 * declared beside the face that carries it, UOpsEvents::OnSaveSlot) and the toast widget words it.
 * ENFORCED BY: Check-Architecture rule 4 ('FNotificationEvent (retired)'), AirportMgr.UI.ToastsWordSavesAndPurchases
 */
struct AIRPORTOPS_API FSaveSlotEvent
{
	EOpsSaveOutcome Outcome = EOpsSaveOutcome::Saved;
	FString Slot;
	static const TCHAR* EventName() { return TEXT("SaveSlot"); }
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
 * A flight left its stand - OFF BLOCKS, the end of its turnaround contract (#398; it was "left the ground", scored at the
 * line-up, until movement was measured eating most of the contract). LateBySeconds is how far OffBlocksAt overran OffBlocksBy(), i.e.
 * OffBlocksAt - (OnBlocksAt + ContractSeconds): the contract the inbox row showed at the offer, run on the stand only.
 * Negative is early, and is not clamped - what early is worth is the listener's decision, not the publisher's. Published only
 * for a flight that was on blocks: no turnaround, no contract, nothing to score.
 * ENFORCED BY: AirportOps.Model.FlightBoard.Events.OffBlocksLateness
 */
struct AIRPORTOPS_API FFlightOffBlocksEvent
{
	int32 FlightId = 0;
	FName AirlineId;
	double LateBySeconds = 0.0;
	static const TCHAR* EventName() { return TEXT("FlightOffBlocks"); }
	FString Describe() const;
};

/**
 * The attached network changed - any committed rebuild: a road, a stand, a depot drawn or removed, a runway's
 * facts, a module bought, a network replaced (a clear, an undo, a load). Published by UOpsRuntime::OnNetworkChanged,
 * the bridge from ARoadNetworkActor::OnNetworkChanged, IN THE REBUILD THAT MADE THE CHANGE (#446) - it was one
 * compare a frame in UOpsRuntime::Tick, a frame late and blind to fact edits. GuidelineRevision is the network's
 * after the rebuild: the same clock the job board's re-bid and re-offer gates read, so this cannot disagree with them.
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

/** Fuel reached the tanks - a contract day or a spot order (spec 2026-10-02 §7). Litres arrived, Added fitted. */
struct AIRPORTOPS_API FFuelDeliveredEvent
{
	double Litres = 0.0;
	double Added = 0.0;
	bool bContract = false;
	static const TCHAR* EventName() { return TEXT("FuelDelivered"); }
	FString Describe() const;
};

/** An airline's satisfaction moved. Published by UAirlineRoster from the Reaction tier. */
struct AIRPORTOPS_API FAirlineSatisfactionEvent
{
	FName AirlineId;
	double Old = 0.0;
	double New = 0.0;
	FString Cause;
	/** The cause as a kind; Cause is the text with its figures. */
	EAirlineSatisfactionCause Kind = EAirlineSatisfactionCause::OnTime;
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

/**
 * Modules a depot's plot could not seat were removed and refunded (#266) - UFacilityPurchases::RemoveUnseated, the repair.
 * One per depot and kind. Entity is the depot's INDEX; Count how many went; Amount what was credited (0 for a kind the shop
 * does not sell). ITS OWN EVENT, not an FFacilityUpgradedEvent with a direction: nothing the player did caused it, and the
 * toast it becomes is a Warning, where a purchase's is a receipt.
 */
struct AIRPORTOPS_API FModulesRefundedEvent
{
	int32 Entity = INDEX_NONE;
	EDepotModule Module = EDepotModule::Shed;
	int32 Count = 0;
	double Amount = 0.0;
	static const TCHAR* EventName() { return TEXT("ModulesRefunded"); }
	FString Describe() const;
};

/**
 * How a vehicle joined or left a depot's fleet - one value per way in and out, so a subscriber sees EVERY change and not
 * only the ones the player paid for (#443: seeding and a depot's removal published nothing, so "Bowser #3 credited, depot
 * removed" could never reach the feed). A plain enum - this header has no .generated.h for a UENUM (memory: UHT cannot
 * see it). Bought and Seeded come in; Sold and Withdrawn go out. FServiceFleet::Add/Withdraw take the origin and reason
 * that map to these.
 */
enum class EFleetChange : uint8
{
	/** The player bought it: Amount is what was charged. */
	Bought,
	/** The player sold it: Amount is what was credited. */
	Sold,
	/** The starter fleet a placed depot begins with (Trucks > 0): free, so Amount is 0. */
	Seeded,
	/** Its depot was removed (a bulldoze, an undo of the placement): Amount is what was credited. */
	Withdrawn
};

/**
 * A vehicle joined or left a depot's fleet. Published by FServiceFleet - the fleet's one door - on every change, whoever
 * asked for it: a purchase, a sale, the starter seeding, a depot's removal.
 * ENFORCED BY: AirportOps.Model.Fleet.DepotRemovalPublishesFleetChanged, AirportOps.Model.Fleet.SeedingPublishesFleetChanged;
 * Check-Architecture rule 43 (fleet-one-door)
 */
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

/**
 * A STANDING problem's words changed while the problem stayed true (#445): "runway too short" became "no stand big enough" for
 * the same airline, "No fuel for stand 3: no depot" became "...: no pump". UOpsAlerts used to refresh the text and the focus
 * and publish NOTHING, so a UI that mirrored the list kept the old reason - and told the player to build the wrong thing. The
 * event INVALIDATES; the model is the truth: the alerts window re-reads UOpsAlerts::GetAlerts() when it hears this, and it
 * names only the key. NOT a raise - no toast, and RaisedAt is untouched. A MOVING FOCUS (an aircraft that creeps) is not a
 * change: a row's Go reads the model when it is clicked.
 * ENFORCED BY: AirportOps.Model.Alerts.ChangedReasonIsAnnounced
 */
struct AIRPORTOPS_API FAlertChangedEvent
{
	FOpsAlertKey Key;
	static const TCHAR* EventName() { return TEXT("AlertChanged"); }
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

/**
 * A land tile was bought (land purchase spec R10) - bridged from URoadEditFacade::OnLandBought, after the facade charged
 * the purse and grew the land. Tile is the grid cell; Amount what the ledger took, priced by UPricing.
 */
struct AIRPORTOPS_API FLandPurchasedEvent
{
	FIntPoint Tile = FIntPoint::ZeroValue;
	double Amount = 0.0;
	static const TCHAR* EventName() { return TEXT("LandPurchased"); }
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
	/** The refusal's own sentence - the plan's (figures, admission) or the airport's gate - which the toast shows. The
	 *  reason alone reads "not admitted to that runway" for an arrivals-only field (#456 review). */
	FString Sentence;
	static const TCHAR* EventName() { return TEXT("LandRefused"); }
	FString Describe() const;
};

/**
 * The player accepted an offer - UFlightBoard::TryAccept, once the stand is held. An accept is a player command
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
 * An aircraft left for a departing phase - published by FTurnarounds::EndTurnaround (the job board's turnaround owner
 * since #427), the ONE publisher, when the aircraft's Parked -> departing phase change reaches the job board. Two
 * callers: FTurnarounds::Drop for an aircraft that was turned around, whoever sent it - DepartTheReady or the
 * inspector's manual Depart (batch 3 review I1) - and FTurnarounds::OnAircraftPhase for one that departed without ever
 * being turned around, which leaves Unfuelled (whole-stack review M4). A refused departure changes no phase, so ends nothing;
 * a retire (Gone) is not a departure. Outcome is derived from the litres (FTurnarounds::FuelOutcomeOf).
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
 * A flight will not come, or will not finish: cancelled by a closure (UFlightBoard::CancelUnarrived), by the
 * player despawning its aeroplane (UFlightBoard::CancelByAgent) or by the player cancelling a flight that had not
 * arrived (UFlightBoard::CancelByPlayer, #442). Heard by the roster, which charges AirportClosed and PlayerCancelled
 * the same penalty. An offer WITHDRAWN by a closure is not a cancellation and publishes nothing; nor is a LOAD's cancel,
 * whose Cancelled row publishes nothing (the closure was scored when it happened).
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
 * A parked aircraft's pushback is no longer blocked by other traffic - Airside's UGroundTraffic::OnPushGroundFreed,
 * bridged like FRunwayFreedEvent. DERIVED by Airside's push watch, which re-asks DepartAgent's own question for every
 * aircraft it refused PushbackBlocked: the job board's refused departure waits on this, not on a per-frame retry.
 * ENFORCED BY: AirportOps.Present.Bus.PushGroundFreedIsBridged, AirportOps.Present.PushGroundFreed.DepartsTheFrameAfter
 */
struct AIRPORTOPS_API FPushGroundFreedEvent
{
	int32 AgentId = 0;
	static const TCHAR* EventName() { return TEXT("PushGroundFreed"); }
	FString Describe() const;
};

/**
 * The taxi reservation table released a window since an arrival was refused a taxi-in plan - Airside's
 * UGroundTraffic::OnTaxiPlansFreed, bridged like FRunwayFreedEvent. What a flight holding "awaiting taxi-in route"
 * (EArrivalRefusal::NoTaxiPlan) waits on: the arrival queue's pass is dirtied by it (taxi planning PR 2, 2026-10-02).
 * ENFORCED BY: AirportOps.Present.Bus.TaxiPlansFreedIsBridged
 */
struct AIRPORTOPS_API FTaxiPlansFreedEvent
{
	static const TCHAR* EventName() { return TEXT("TaxiPlansFreed"); }
	FString Describe() const;
};

/**
 * Which aircraft taxi without a plan changed - Airside's UGroundTraffic::OnTaxiUnplannedChanged, bridged like
 * FTaxiPlansFreedEvent. The Alerts pass is dirtied by it: "lost its plan after a layout edit" raises and clears on it
 * (taxi planning PR 3, 2026-10-02).
 * ENFORCED BY: AirportOps.Present.Bus.TaxiUnplannedChangedIsBridged
 */
struct AIRPORTOPS_API FTaxiUnplannedChangedEvent
{
	static const TCHAR* EventName() { return TEXT("TaxiUnplannedChanged"); }
	FString Describe() const;
};

/**
 * A taxiway edit split a piece off a taxiway (URoadEditFacade::OnTaxiwaySplit, bridged) - "C split off from A".
 * ENFORCED BY: AirportOps.Present.Bus.ReattachDoesNotDouble, AirportOps.Present.TaxiwaySplitReachesUi
 */
struct AIRPORTOPS_API FTaxiwaySplitEvent
{
	FString SplitOff;
	FString From;
	static const TCHAR* EventName() { return TEXT("TaxiwaySplit"); }
	FString Describe() const;
};

/**
 * A flight came due and joined the arrival queue - FArrivalQueue::Enqueue (UFlightBoard's until #442 item 4), its one site (a
 * Clock.At callback, or the load's RearmSchedules for one already overdue). The queue pass hears it; before PR D the queue was
 * ticked every frame and needed no word.
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
 * A flight changed phase - published by UFlightBoard::TransitionTo, the one writer of a flight's phase, for EVERY change it makes
 * and for nothing else (a "change" to the phase the flight is already in is not one, and says nothing) (#442 item 4). What the
 * billing reaction hears ("Billing", FlightBilling::OnFlightPhaseChanged): the money UFlightBoard::OnAgentPhase used to post inline
 * is a Sim-tier reaction to the phase now, a round later in the same drain.
 *
 * GENERIC, BESIDE THE SPECIFIC ONES, NOT INSTEAD OF THEM: FFlightInboundEvent, FFlightOffBlocksEvent and FFlightCancelledEvent carry
 * what their listeners need (the airline, the lateness, the reason) and keep their own rules - a load's cancel publishes no
 * FFlightCancelledEvent, so the roster never scores it. This carries only the change, so a listener that cares about a phase
 * (billing: Landing, Turnaround, TaxiOut) needs no event of its own per phase. A LOAD'S CHANGES ARE PUBLISHED TOO - the re-queue's
 * Inbound, the retire's Departed, the closed airport's Cancelled - and enter none of the phases billing reacts to.
 *
 * At IS THE GAME TIME THE CHANGE WAS DATED (the transition's FTransitionCause::At), so a fee priced a round later is priced and
 * dated exactly as it was inline - not by whatever the clock reads when the event is heard.
 * ENFORCED BY: AirportOps.Model.FlightBoard.EveryChangeIsPublishedOnce, AirportOps.Present.Bus.BillingIsWired,
 * AirportOps.Model.FlightSave.MidFlightGoesRoundOrRetires (a load's changes post no fee)
 */
struct AIRPORTOPS_API FFlightPhaseChangedEvent
{
	int32 FlightId = 0;
	EFlightPhase From = EFlightPhase::Offered;
	EFlightPhase To = EFlightPhase::Offered;
	double At = 0.0;
	static const TCHAR* EventName() { return TEXT("FlightPhaseChanged"); }
	FString Describe() const;
};

/**
 * An airline's verdict on the airport changed (#446, #445): it could come and cannot, could not and can, or still cannot and
 * the reason moved (a runway lengthened, and what stops it is now a stand). Published by UOfferGenerator::TickMinute - the one
 * place the verdict is made - the moment it differs from what was last announced. The alerts pass hears it and looks again:
 * it used to look every offer minute whether or not anything had moved, which at x32 is about every frame.
 * ENFORCED BY: AirportOps.Model.Offers.AdmissionChangeIsAnnounced
 */
struct AIRPORTOPS_API FAirlineAdmissionChangedEvent
{
	FName AirlineId;
	bool bCouldCome = true;
	/** The refusal's own sentence while it cannot come, else empty. */
	FString Reason;
	static const TCHAR* EventName() { return TEXT("AirlineAdmissionChanged"); }
	FString Describe() const;
};

/**
 * EVERY EVENT THERE IS, as one closed list. Subscribe<T> and Publish<T> are compile-checked
 * against it, and the wiring test walks it - "lists that must agree are ONE list".
 * FInstancedStruct was rejected: an open set has no answer to "which events exist?".
 */
using FOpsEvent = TVariant<FAgentPhaseEvent, FArrivalRefusedEvent, FSpeedChangedEvent, FSaveSlotEvent,
	FOfferExpiredEvent, FOfferDeclinedEvent, FFlightOffBlocksEvent, FDayEndedEvent, FAirlineSatisfactionEvent,
	FNetworkChangedEvent, FAlertRaisedEvent, FAlertClearedEvent, FAlertsResetEvent, FBuildRefusedEvent, FLandRefusedEvent,
	FMoneyPostedEvent, FBalanceSignChangedEvent, FFacilityUpgradedEvent, FFleetChangedEvent, FOfferAcceptedEvent,
	FTurnaroundEndedEvent, FAirportStatusChangedEvent, FFlightCancelledEvent, FRunwayFreedEvent, FStandsFreedEvent,
	FFlightInboundEvent, FPushGroundFreedEvent, FModulesRefundedEvent, FAlertChangedEvent, FAirlineAdmissionChangedEvent,
	FFlightPhaseChangedEvent, FTaxiwaySplitEvent, FTaxiPlansFreedEvent, FTaxiUnplannedChangedEvent, FLandPurchasedEvent, FFuelDeliveredEvent>;

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

	/**
	 * A coalesced bulk step, run once after a round in which it was marked dirty, and told WHY (FPassRun).
	 *
	 * AFTER IS THE ORDER, DECLARED (#445): the passes whose result Run reads, or whose work it must see finished. It used to
	 * be registration-line order in UOpsRuntime::WireBus - JobBoard before ArrivalQueue before Alerts, which the same-round
	 * FlightCannotLand alert depends on - a correctness rule held by where a line sat, and pinned by nothing. EndWiring now
	 * orders the passes by After (a stable topological sort: registration order breaks ties), and an After naming a pass
	 * nobody registered, or a cycle, is an Error at wire time, where a test can see it. PassOrder() says the result.
	 *
	 * WHAT AN ORDER MEANS, which was undocumented: a pass that marks a LATER pass dirty from its Run gets that pass run in the
	 * SAME round (the round's pass walk has not reached it yet); one that marks an EARLIER pass dirty waits a round, and a
	 * round costs one of MaxRounds. So a dependency is declared on the pass that READS, and a chain that needs the reader
	 * first is a chain that spends rounds.
	 * ENFORCED BY: AirportOps.Model.Bus.PassesRunInDeclaredOrder, AirportOps.Present.Bus.WiringOrderIsDeclared
	 */
	void RegisterPass(FName Name, TFunction<void(const FPassRun&)> Run, std::initializer_list<FName> After = {});

	/** Dirty for this round, asked for by Cause (an event, by default - a caller that is not the safety net says nothing). */
	void MarkDirty(FName Pass, EPassCause Cause = EPassCause::Event);
	void MarkAllDirty();

	/**
	 * Dirty for the NEXT Drain, not this one - "try again next frame". A pass that re-marked ITSELF
	 * with MarkDirty would run again the very next round and burn the round cap in one frame; a retry
	 * (a departure the runway refused) wants exactly one more look per frame until it resolves.
	 * ENFORCED BY: AirportOps.Model.Bus.NextDrainIsNextFrame
	 */
	void MarkDirtyNextDrain(FName Pass, EPassCause Cause = EPassCause::Retry);

	/** The passes in the order they run - what After and registration made of it. For the wiring test and the wire-time log. */
	TArray<FName> PassOrder() const;

	/** The passes Pass declared it runs After - empty for an unknown pass. For the wiring test. */
	TArray<FName> PassesAfter(FName Pass) const;

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

	/**
	 * How many events of type T the drains have dispatched (handlers run for) - not published: an event a load or a detach
	 * discards is published and never dispatched. The observation post for a test of the bus's own delivery now that
	 * UOpsEvents has no per-event face for every type to hang a listener on (#445: OnAgentPhaseChanged was cut for having none).
	 */
	template <typename T>
	int32 DispatchedCountOfForTest() const { return DispatchedCounts[FOpsEvent::IndexOfType<T>()]; }

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
		TFunction<void(const FPassRun&)> Run;
		TArray<FName> After;
		bool bDirty = false;
		/** The strongest cause that marked it since it last ran - SafetyNet (the weakest) while clean. */
		EPassCause Cause = EPassCause::SafetyNet;
		/** See MarkDirtyNextDrain. Promoted to bDirty at the start of the next Drain, with NextCause. */
		bool bDirtyNextDrain = false;
		EPassCause NextCause = EPassCause::Retry;
	};

	static constexpr int32 NumTiers = 3;
	TArray<FHandler> Handlers[NumTypes][NumTiers];
	TArray<FPass> Passes;
	TArray<FOpsEvent> Queue;
	int32 DispatchedCounts[NumTypes] = {};
	bool bWiring = false;
	bool bDraining = false;

	static const TCHAR* NameOf(const FOpsEvent& Event);
	static void LogSubscription(FName Who, EOpsTier Tier, const TCHAR* Event);
	bool AnyDirty() const;
	/** EndWiring's ordering: Passes by After, stable. Errors name an unknown dependency or a cycle. */
	void OrderPasses();
};
