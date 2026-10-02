#pragma once

#include "CoreMinimal.h"
#include "Model/AgentRescue.h"
#include "Model/FacilityPurchases.h"
#include "Model/OfferGenerator.h"
#include "Model/OpsEventBus.h"
#include "Model/OpsSafetyNet.h"
#include "Model/SimClock.h"
#include "UObject/Object.h"
#include "OpsRuntime.generated.h"

class ARoadNetworkActor;
class UAirlineRoster;
class UAirlineHistory;
class UAirport;
class UOpsAlerts;
struct FArrivalQuote;
struct FBuildQuote;
enum class EBuildRefusal : uint8;
class UGroundTraffic;
class UOpsCatalog;
class UOpsEvents;
class UJobBoard;
class UScenario;
class UFlightBoard;
class UOfferGenerator;
class ULedger;
class UPricing;
struct FAirframe;
struct FEntityInstance;
struct FVehicle;
enum class EAgentPhase : uint8;
enum class EArrivalRefusal : uint8;
enum class EChangeKind : uint8;

/**
 * The AirportOps composition root. Owns the clock and the event bus, attaches to the one
 * ARoadNetworkActor, relays Airside delegates onto the bus, pushes the speed multiplier
 * down into the actor each tick, and performs save/load end to end.
 *
 * "THE BUS" IS FOpsEventBus since 2026-09-29 (spec ops-event-bus): the Airside relays PUBLISH
 * onto it, Tick drains it, and WireBus is the one place anything subscribes. UOpsEvents is now
 * the bus's Presentation tier - the BP/UMG face of it - rather than the bus itself.
 *
 * A UObject rather than the subsystem itself so a test can NewObject one, Attach a spawned
 * actor and Tick it by hand - a UGameInstanceSubsystem needs a UGameInstance, which a
 * CreateWorld test does not have. UOpsRuntimeSubsystem is the forwarder that gives this
 * a lifetime in play. Same split as ARoadNetworkActor (composition root) over
 * UAirsideTraffic (testable subobject), for the same reason.
 *
 * It GROWS BY FORWARDING. UJobBoard was the first such subobject: this class gained a
 * pointer, three lines in Attach/Tick/OnAgentPhase, and no logic at all. The flight board, the
 * ledger and the pricing arrived the same way and cost the same: a pointer each and a line in
 * Attach. The job board is next, and gets no more. Logic lands in them, never here.
 *
 * PASS SCHEDULING IS NOT HERE (#445). It was - about nine fields (covered/due/wanted pairs, a funnel per pass, a frame counter for
 * one-clearance-a-frame) that every pass needing to know WHY it ran reinvented, and that review corrected three times - which
 * made the paragraph above false. The cause of a run is the bus's (EPassCause, FPassRun), the net is FOpsSafetyNet's, and
 * one-clearance-a-frame is the flight board's (UFlightBoard::BeginQueueFrame). What is left here is the wiring: which event marks
 * which pass, and what a pass body asks of the boards.
 */
UCLASS()
class AIRPORTOPS_API UOpsRuntime : public UObject
{
	GENERATED_BODY()

public:
	UOpsRuntime();

	USimClock* GetClock() const { return Clock; }
	UOpsEvents* GetEvents() const { return Events; }

	/**
	 * The ops event bus - see FOpsEventBus. Subscriptions are made in WireBus alone; anything may
	 * Publish.
	 * ENFORCED BY: Check-Architecture rule 31 (bus-wired-once)
	 */
	FOpsEventBus& GetBus() { return Bus; }
	UOpsCatalog* GetCatalog() const { return Catalog; }

	/** The fuel jobs. See UJobBoard - this runtime owns it, feeds it the phase events and
	 *  ticks it, and that is the whole of the wiring. */
	UJobBoard* GetJobBoard() const { return JobBoard; }

	/** Every flight, and the one caller of DispatchArrival. See UFlightBoard. */
	UFlightBoard* GetFlightBoard() const { return FlightBoard; }

	/** Where offers come from. Fed the catalog's airlines by this runtime, on the clock. */
	UOfferGenerator* GetOfferGenerator() const { return OfferGenerator; }

	/** The money. See ULedger - this runtime owns it, opens it from the scenario, and hands it
	 *  to the edit facade as the build purse. */
	ULedger* GetLedger() const { return Ledger; }

	/** What things cost and what they earn. See UPricing. */
	UPricing* GetPricing() const { return Pricing; }

	/** The inspector's Unstick. See UAgentRescue - this runtime owns it and hands it the two boards. */
	UAgentRescue* GetAgentRescue() const { return AgentRescue; }

	/**
	 * Sheds and vehicles bought and sold. See UFacilityPurchases - this runtime owns it and wires its two
	 * world hooks at Attach (cleared at Detach).
	 * ENFORCED BY: AirportOps.Present.Facility.ShedPurchaseRelightsASlot, AirportOps.Present.Facility.QuoteSolvesOncePerDepot
	 */
	UFacilityPurchases* GetFacilityPurchases() const { return FacilityPurchases; }

	/**
	 * FORWARDERS to UFacilityPurchases with this runtime's network - the one the driver does not hold, as
	 * CanUnstick supplies it. Refused NotAFacility when unattached. Logic lives in UFacilityPurchases.
	 * ENFORCED BY: AirportOps.Present.Facility.PurchaseWakesTheBoard, AirportOps.Present.Facility.AttachCopiesTheOffers
	 */
	FFacilityQuote QuoteFacility(FEntityInstanceId Entity) const;
	FPurchaseResult BuyModule(FEntityInstanceId Entity, EDepotModule Module);
	FPurchaseResult BuyVehicle(FEntityInstanceId Entity, FName TypeCode);
	FPurchaseResult SellVehicle(int32 VehicleId);

	/**
	 * FORWARDERS to UAgentRescue with this runtime's traffic, network and clock - the three the driver
	 * does not hold, as LandNear supplies them. Refused ("Nothing selected"-shaped) when unattached.
	 * ENFORCED BY: AirportOps.Present.UnstickForwards
	 */
	FUnstickVerdict CanUnstick(int32 AgentId, EUnstickAction Action) const;
	FUnstickVerdict Unstick(int32 AgentId, EUnstickAction Action);

	/**
	 * The FlightCannotLand alert's Cancel (#442): UFlightBoard::CancelByPlayer with this runtime's traffic and clock - the two
	 * the driver does not hold - and the passes a cancel can change dirtied (the stand it freed may let a holding flight
	 * land). False when unattached, or the flight is not one still to arrive. A FORWARDER: the rule is the board's.
	 * ENFORCED BY: AirportOps.Present.Alerts.CancelFlightForwardsToTheBoard
	 */
	bool CancelFlight(int32 FlightId);

	/** How every airline feels about this airport. See UAirlineRoster - a Reaction on the bus. */
	UAirlineRoster* GetAirlines() const { return Airlines; }

	/** A week of why each airline's satisfaction moved - see UAirlineHistory; fed by the roster. */
	UAirlineHistory* GetAirlineHistory() const { return AirlineHistory; }

	/** The standing problems the player must act on - see UOpsAlerts; run as the bus pass "Alerts". */
	UOpsAlerts* GetAlerts() const { return Alerts; }

	/** Open, closed by the player, or without a runway - see UAirport. */
	UAirport* GetAirport() const { return Airport; }

	/**
	 * The player's close / open command (the bar's game.airport). A COMMAND, NOT AN EVENT: it records the intent
	 * and re-derives the status at once; the change it publishes is what cancels the unarrived flights, on the
	 * next drain (WireBus). False, and logged, when there is no attached network to derive against.
	 * ENFORCED BY: AirportOps.Present.Airport.CloseCancelsThroughTheBus
	 */
	bool SetAirportClosed(bool bClosed);

	ARoadNetworkActor* GetTarget() const { return Target; }

	/**
	 * What Stand was built for: UAirsideSettings::ResolveStandDesignVehicleOf on its definition and
	 * its outline's letter. The ONE reader Attach hands UJobBoard::DesignVehicleOf, and the one
	 * the world-free fuel fixture hands it too, so the two cannot read a stand differently. A
	 * forwarder, not logic - the reading lives in Content/.
	 */
	static FVehicle StandDesignVehicleOf(const FEntityInstance& Stand);

	/**
	 * Board's vehicle catalogue and starter fleet, resolved from Scenario's rows and Content's chassis
	 * (UAirsideSettings::ResolveVehicle) through FServiceFleet::ResolveCatalogue (#430). THE ONE RESOLVE the runtime runs -
	 * at attach and after every load, by ApplyScenarioFigures (#449) - and the one the world-free fixtures run, so a test's catalogue is the game's - the same reason StandDesignVehicleOf is
	 * a static here. A forwarder, not logic: the join is the fleet's, the chassis Content's.
	 * ENFORCED BY: AirportOps.Fleet.EveryBuyableTypeHasAChassis (walks the attached runtime's catalogue)
	 */
	static void ResolveVehicleCatalogue(UJobBoard& Board, const UScenario& Scenario);

	/** Binds to the actor's traffic delegates. Safe to call again with a new actor (unbinds the old). */
	void Attach(ARoadNetworkActor* Actor);

	/**
	 * Unbinds from the attached actor and forgets it. PUBLIC since #446: UOpsRuntimeSubsystem calls it when the
	 * world's airport leaves (URoadNetworkRegistry), where a per-tick IsValid used to notice the target was gone and
	 * nothing ever unbound. Attach calls it first; safe with nothing attached.
	 */
	void Detach();

	/** One object that holds a raw pointer to Bus: its name (for a failing test), and the slot that holds it. */
	struct FOpsBusPublisher
	{
		const TCHAR* Name = TEXT("");
		FOpsEventBus** Slot = nullptr;
	};

	/**
	 * EVERY OBJECT THAT PUBLISHES ONTO THIS RUNTIME'S BUS, ONE LIST (#445), like Persistents(): Attach points each slot at the bus and
	 * Detach takes each back in one loop apiece, and the Detach test walks it. It was kept by hand in three places - Attach, Detach and
	 * the test - which already disagreed (UFacilityPurchases was set in the constructor, never cleared, and in no test), so the
	 * facade's raw IBuildPurse* outlived the ledger it pointed at. A publisher is held by its Bus field, so a class that declares one
	 * and is not listed is the defect the lint rule below finds.
	 * PUBLIC so the test can walk it; the slots are this runtime's to write and nothing else's.
	 * ENFORCED BY: AirportOps.Present.Bus.DetachUnhooksEveryPublisher, Check-Architecture rule 71 (every-bus-publisher-is-listed),
	 * rule 4's row 'UOpsRuntime publisher slot written' (the slots are written in OpsRuntime.cpp alone)
	 */
	TArray<FOpsBusPublisher> Publishers();

	/** The names of the Airside bridges Attach binds, in table order (see FAirsideBridge) - for the test that broadcasts every delegate. */
	static TArray<FName> AirsideBridgeNamesForTest();

	/** Advances the clock and pushes the speed multiplier into the actor. Real seconds in. */
	void Tick(double RealDeltaSeconds);

	/**
	 * Speed control. Forwards to USimClock::StepSpeed/TogglePause (the ladder, ResumeSpeed and
	 * the pause policy all moved there in issue #191 - #98 partial - so the SAVED state and the
	 * logic that computes it are the same object; a game saved while paused used to reload with
	 * ResumeSpeed at its constructor default because UOpsRuntime, which held it, is not itself
	 * saved) and then pushes the result into the actor and the event bus, which is Present/'s
	 * job and the reason these stay methods here rather than becoming a bare Clock-> call at
	 * every caller.
	 */
	void StepSpeed(int32 Delta);
	/** Paused <-> the speed that was set before pausing. See StepSpeed's comment. */
	void TogglePause();

	bool SaveToSlot(const FString& SlotName);
	/** Restores clock and network, clears agents and undo history, rebuilds the actor's mesh. */
	bool LoadFromSlot(const FString& SlotName);

	/**
	 * Lands an aircraft near Focus, through the flight board so it belongs to a flight the rest
	 * of the game can track rather than an aeroplane that answers to nothing - see
	 * UFlightBoard::AcceptImmediate's own header for why a direct dispatch is a second door
	 * onto arrival, the thing this exists to avoid.
	 *
	 * Override is the one airframe the caller insists on, or null to resolve the content
	 * default the airport would otherwise land - see UAirsideSettings::ResolveDefaultAirframe.
	 * The CALLER still owns deciding whether an override applies (the driver's Land panel
	 * passes the type the player picked - a choice on the driver, not a fact about the
	 * airport) - this only decides what happens once one is or is not given.
	 *
	 * Returns EArrivalRefusal::NoRunway when there is no attached network to check a runway
	 * against at all (Target null, or its Network or ground traffic not yet built), and
	 * whatever AcceptImmediate itself refused for otherwise - the same sentence
	 * ArrivalPlanner::DescribeRefusal would print for it.
	 *
	 * MOVED FROM ARoadBuildController::LandAircraftNearViewFocus / LandThroughTheBoard by issue
	 * #191: which aeroplane lands and how it reaches the board is this class's decision - it
	 * already owns the board, the clock and the traffic model AcceptImmediate needs - not a
	 * PlayerController's, and the old home could only be exercised by driving PIE.
	 */
	EArrivalRefusal LandNear(const FVector2D& Focus, const FAirframe* Override);

	/**
	 * Would LandNear(Near, &Airframe) be accepted now - asked without accepting anything (#432). UFlightBoard::
	 * QuoteArrival: the plan and the airport's gate TryAccept asks, so the Land panel renders the game's verdict rather
	 * than its own (it judged the nearest runway alone, stale since #412). Refused, worded, with no attached network.
	 * ENFORCED BY: AirportMgr.UI.LandChoicesAgreeWithThePlanner
	 */
	FArrivalQuote QuoteLanding(const FAirframe& Airframe, const FVector2D& Near) const;

	/** True once Attach has armed the generator's minute tick. False before Attach. */
	bool HasOfferScheduledForTest() const { return OfferHandle != INDEX_NONE; }

	/**
	 * Cancel and re-book the two repeaters - the generator's minute tick, from the clock's CURRENT Now, and the daily
	 * upkeep, whose first firing is the next MIDNIGHT after it (#442: it was a day from Now, so a load at 05:59 postponed the
	 * 06:00 upkeep by a day).
	 *
	 * AFTER A LOAD, and not optional (review I1, 2026-09-28): USimClock does not save its queue
	 * and books absolute due times, so repeaters armed at Attach still pointed at the pre-load
	 * time. A later save fired the minute tick once per missed minute in one frame; an earlier
	 * one went silent until the clock caught up. Attach books them through this too, so there is
	 * one place that knows the two exist.
	 * ENFORCED BY: AirportOps.Present.OffersRearmOnLoad
	 */
	void RearmRepeatingSchedules();

	/** How many times OfferTick has run. For the load re-arm test. */
	int32 OfferTicksForTest() const { return OfferTicks; }

	/** How many plot solves ReservedSlotsOf has run - see its memo. */
	int32 ReservationSolvesForTest() const { return ReservationSolves; }

	/** How many times this runtime has set the actor's sim time scale - for the test that a quiet frame sets it
	 *  no times (ops batch 3 PR E). Counted here, the one production caller, not on the actor. */
	int32 TimeScaleSetsForTest() const { return TimeScaleSets; }

	/** Whether the safety net (SafetyNet) is booked on the clock - for any pass that wants it. */
	bool IsSafetyNetArmedForTest() const { return SafetyNet.IsArmed(); }

	/**
	 * Game seconds between the safety net's runs - see FOpsSafetyNet. 30 (ops event bus spec §2): long
	 * enough that a stranded flight is a visible defect rather than a smooth fallback, short enough that one never
	 * holds for minutes in play. At x1 on the default scenario (2400 real s of daylight for 14 h, 480 of night for 10 h)
	 * 30 game s is about 1.4 real s by day and 0.4 by night (2026-09-30).
	 */
	static constexpr double SafetyNetSeconds = 30.0;

	/**
	 * Every airline with its fleet resolved to airframes, built once at Attach. What the
	 * generator ticks over and what the inbox's demand strip samples - one list, so the strip
	 * cannot show an airline the generator does not have.
	 */
	const TArray<FAirlineOffers>& GetAirlineOffers() const { return AirlineOffers; }

private:
	/**
	 * Every model object that owns saved state, in a fixed order.
	 *
	 * ONE LIST. A system added to this runtime and forgotten here is a system that silently
	 * stops being saved - so there is one place to forget rather than the two call sites
	 * (SaveToSlot and LoadFromSlot) that each used to name their own subset positionally. The
	 * order matters only in being the same on both sides of a round trip.
	 */
	TArray<IOpsPersistent*> Persistents() const;

	UPROPERTY() TObjectPtr<USimClock> Clock;
	UPROPERTY() TObjectPtr<UOpsEvents> Events;
	UPROPERTY() TObjectPtr<UOpsCatalog> Catalog;
	UPROPERTY() TObjectPtr<UJobBoard> JobBoard;
	UPROPERTY() TObjectPtr<UFlightBoard> FlightBoard;
	UPROPERTY() TObjectPtr<UOfferGenerator> OfferGenerator;
	UPROPERTY() TObjectPtr<ULedger> Ledger;
	UPROPERTY() TObjectPtr<UPricing> Pricing;
	UPROPERTY() TObjectPtr<UAgentRescue> AgentRescue;
	UPROPERTY() TObjectPtr<UFacilityPurchases> FacilityPurchases;

	/**
	 * UFacilityPurchases::ReservedSlotsOf's production answer: DepotKit::ReservationOf's ceiling over the
	 * actor's one kit table, MEMOISED per (network object, EditRevision, depot) - the inspector re-quotes every
	 * tick and the ceiling is a plot solve. Modules do not change what a plot holds, and no mutator moves or
	 * re-plots a live depot today (2026-09-30), so the invalidations are: a different network object (clear,
	 * undo replace it); a moved URoadNetwork::GetEditRevision - a road edit, and EVERY IN-PLACE RESTORE
	 * (a rolled-back edit through RestoreFrom, #437; a load through Serialize, #426), which keep the
	 * pointer and move the revision forward, so a key of the pointer alone would keep quoting the plot
	 * the restore took away; and
	 * Detach. A module purchase moves none of them: it is an entity edit, and EditRevision is scoped to
	 * nodes and segments. A FUTURE depot move / re-plot mutator must drop that depot's entry here, or the
	 * card keeps quoting the old plot's ceiling.
	 * ENFORCED BY: AirportOps.Present.Facility.QuoteSolvesOncePerDepot, AirportOps.Present.Facility.NewNetworkResolvesTheCeiling,
	 * AirportOps.Present.Facility.RollbackResolvesTheCeiling
	 */
	int32 ReservedSlotsOf(FEntityInstanceId Id, const FEntityInstance& Depot, EDepotModule Module);
	TWeakObjectPtr<const URoadNetwork> ReservationMemoNetwork;
	/** URoadNetwork::GetEditRevision when the memo was filled - see ReservedSlotsOf. */
	uint32 ReservationMemoRevision = 0;
	TMap<FEntityInstanceId, TArray<int32>> ReservationMemo;
	int32 ReservationSolves = 0;

	UPROPERTY() TObjectPtr<UAirlineRoster> Airlines;
	UPROPERTY() TObjectPtr<UAirlineHistory> AirlineHistory;
	UPROPERTY() TObjectPtr<UOpsAlerts> Alerts;
	UPROPERTY() TObjectPtr<UAirport> Airport;

	/** The facade's OnRefused, bridged onto the bus as FBuildRefusedEvent - see OnBuildRefused. Bound by the "BuildRefused" bridge. */
	void OnBuildRefused(const FBuildQuote& Quote, EBuildRefusal Why);

	/** Live sources for UOpsAlerts::Recompute, read fresh - the network object can be replaced. */
	void RecomputeAlerts();

	/** Every catalog airline gets a row - at attach, and again after a load, whose snapshot may predate
	 *  the "Airlines" blob. Seeded here and never from an event: see UAirlineRoster's class comment. */
	void SeedAirlines();
	UPROPERTY(Transient) TObjectPtr<ARoadNetworkActor> Target;

	/**
	 * PLAIN C++, NOT A UPROPERTY: it holds TFunctions, which UHT cannot see, and none of it is
	 * saved - SaveToSlot drains it first and LoadFromSlot discards it (spec 2026-09-29 §4).
	 */
	FOpsEventBus Bus;

	/**
	 * Every subscription and pass - see FOpsEventBus. Called by Attach, which may run again on a
	 * level change; it resets the wiring before re-making it.
	 * ENFORCED BY: Check-Architecture rule 31 (bus-wired-once), AirportOps.Present.Bus.ReattachDoesNotDouble
	 */
	void WireBus();

	/** The actor's traffic model, or null when there is no target, network or traffic yet - the
	 *  one guard every Sim handler needs, since an event can outlive the frame it was raised in. */
	UGroundTraffic* LiveModel() const;

	/**
	 * The ONE clock entry that wakes the job board at its next deadline (UJobBoard::NextDeadline) -
	 * cancelled and re-booked after every Step, so it always names the earliest. Not saved, like every
	 * USimClock entry: a load runs the pass once (MarkAllDirty), which re-books it from restored state.
	 * ENFORCED BY: AirportOps.Present.FuelServiceWired (the serve ends on the first step past StepEndsAt)
	 */
	int32 JobBoardDeadlineHandle = INDEX_NONE;
	void ArmJobBoardDeadline();

	/**
	 * THE ARRIVAL QUEUE'S PASS (ops batch 3 §5), in place of the per-frame UFlightBoard::TickQueue call: run when an
	 * event that can let a holding flight land dirties it - a runway or stand freed, an accept, a flight joining the
	 * queue, the network or the airport's status changing, the speed (a paused pass consumed its dirt) - and at a load.
	 *
	 * ONE CLEARANCE A FRAME is the queue's rule now (UFlightBoard::BeginQueueFrame, called by Tick): a flight cleared this frame
	 * makes TickQueue say bDeferred to any second run, and this pass asks again next frame - the new agent's Arriving phase event is
	 * what wakes it - so a second runway gets its flight one frame later, with the first one's claim already in the table.
	 * ENFORCED BY: AirportOps.Present.ArrivalQueue.SecondRunwayNextFrame, AirportOps.Present.ArrivalQueue.EachEventDirtiesIt
	 */
	void RunArrivalQueue(const FPassRun& Run);

	/**
	 * THE SAFETY NET (ops event bus spec §2) - see FOpsSafetyNet. One object for every pass that wants it: the arrival queue while
	 * flights hold, the job board while a due turnaround's departure is refused. Bound to the bus and the clock in the constructor
	 * (both are this runtime's for its life); each pass says whether it wants the net after every run; a load and a detach cancel it.
	 * ENFORCED BY: AirportOps.Present.ArrivalQueue.SafetyNetCatchesAMissedEvent, AirportOps.Present.PushGroundFreed.SafetyNetDepartsAMissedOne,
	 * AirportOps.Model.Bus.ThirdNetWatchedPassNeedsNoRuntimeField
	 */
	FOpsSafetyNet SafetyNet;

	/**
	 * THE DEADLOCK LOOK (#446): the one condition the alerts pass cannot learn from an event - a ring of agents each stalled past
	 * FTrafficRules::StallSeconds, which matures with no phase change and no network edit. It was caught by the alerts pass being dirtied
	 * every offer minute whatever was on the ground, which at x32 is about every frame. It is a clock entry of its own now, with ONE BOOKING RULE
	 * (ArmDeadlockLook): while ANY AGENT IS ON A ROUTE (FRoadAgent::IsOnRoute) a look is kept at the stall-threshold cadence - a first stall,
	 * which nothing announces, can only begin on a route, so it is caught within about two thresholds - and a STALLED agent books the moment its
	 * stall would cross the threshold when that is sooner. Armed by the agent-phase event (it dirties the alerts pass, whose run ends in the arm) when
	 * an agent enters a route phase, cancelled by the run the last one's leaving causes: a field of parked or stranded aircraft, or none, books
	 * nothing. StallSeconds is in MOTION seconds, which run at the speed multiplier alone, so the game seconds in them are the day's compression:
	 * 21 game s per motion s by day and 75 by night on the default scenario (2026-09-30), so the three-second threshold is ~63 game s by day and ~225
	 * at night. Capped at the band edge and re-booked there (USimClock::GameSecondsToBandEdge) so it is not up to 3.6x late across dawn.
	 * ENFORCED BY: AirportOps.Present.Alerts.NoseToNoseJamIsAlertedWithNoOtherEvent, AirportOps.Present.Alerts.DeadlockLookKeepsWatchWhileAnAgentMoves,
	 * AirportOps.Present.Alerts.DeadlockLookIgnoresAFieldWithNothingMoving, AirportOps.Present.Alerts.DeadlockLookIsAClockEntryNotAnOfferMinuteTick,
	 * AirportOps.Present.Alerts.DeadlockLookIsReBookedAtTheBandEdge
	 */
	int32 DeadlockLookHandle = INDEX_NONE;
	void ArmDeadlockLook();

	/**
	 * THE AIRSIDE BRIDGES, ONE TABLE (#445): every Airside delegate this runtime turns into a bus event, with how it is bound to the attached
	 * actor and how it is taken back. Each used to cost a handle field, a forwarder declared here and defined in the .cpp, a bind line in Attach
	 * and a remove line in Detach - four sites apiece, hand-paired, and only the phase bridge was tested for a second Attach doubling it. Attach
	 * binds the table and Detach unbinds it, one loop each; the handle lives in the entry. A bridge is one entry in AirsideBridges().
	 * WHAT THE TEST COVERS IS THE TABLE, BY NAME: AirportOps.Present.Bus.ReattachDoesNotDouble lists the delegates it broadcasts and compares
	 * that list, by name, with AirsideBridgeNamesForTest() - so a bridge added to the table with no broadcast in the test, or left in the test
	 * after it was cut, is red naming it - then broadcasts each once after two attaches and counts one event apiece.
	 * ENFORCED BY: AirportOps.Present.Bus.ReattachDoesNotDouble, AirportOps.Present.Bus.FreedIsBridged, AirportOps.Present.Bus.PushGroundFreedIsBridged
	 */
	struct FAirsideBridge
	{
		const TCHAR* Name = TEXT("");
		/** Binds to the attached actor and returns the handle; an invalid handle means the actor lacks what it bridges (no facade). */
		TFunction<FDelegateHandle(UOpsRuntime&, ARoadNetworkActor&)> Bind;
		/** Takes the handle Bind returned back off the actor - which may have lost what it bound to since. */
		TFunction<void(ARoadNetworkActor&, FDelegateHandle)> Unbind;
		FDelegateHandle Handle;
	};
	static TArray<FAirsideBridge> AirsideBridges();
	TArray<FAirsideBridge> Bridges;

	/**
	 * Airside's ARoadNetworkActor::OnNetworkChanged, bridged onto the bus as FNetworkChangedEvent (#446) -
	 * the "NetworkChanged" bridge. It REPLACED A PER-FRAME POLL: Tick compared
	 * the network pointer and GuidelineRevision against a remembered pair (SeenNetwork, SeenGuidelineRevision)
	 * every frame, so a change was published up to a frame late - which is why SaveToSlot had to refresh the
	 * airport's status itself - and a Detach reset the pair so the first Tick after an Attach published a
	 * catch-up. The event is published in the rebuild that made the change; Attach's MarkAllDirty and Reseat
	 * are the catch-up. ENFORCED BY: AirportOps.Present.Bus.NetworkChangedPublishedOnceWithNoTick;
	 * Check-Architecture rule 51 (network-change-announced)
	 */
	void OnNetworkChanged(EChangeKind Kind, const URoadNetwork& Network);

	/** The repeating offer callback, so Detach can cancel it. INDEX_NONE when unattached. */
	int32 OfferHandle = INDEX_NONE;

	/** The repeating daily upkeep callback. Same sentinel and same cancellation as OfferHandle:
	 *  a handle left armed across a Detach fires against a runtime with no network. */
	int32 UpkeepHandle = INDEX_NONE;

	/**
	 * One day's upkeep for everything standing, posted as a single entry. Bound in Attach.
	 *
	 * SHRUNK BY ISSUE #191: this used to decide the skip-if-zero rule and post the entry
	 * itself; both moved to ULedger::PostDailyUpkeep, which see for the roll-up decision this
	 * left behind. What is left here is exactly what CLAUDE.md's Architecture section asks
	 * for - the ONE place a content default (BuildCost::DailyUpkeep, which lives in Build/,
	 * which Model/ may not include) gets resolved - plus the FlightBoard beat that has to stay
	 * next to it (see the .cpp).
	 */
	void PostDailyUpkeep();

	/** See OfferTicksForTest. A session counter, not saved. */
	int32 OfferTicks = 0;

	/** See TimeScaleSetsForTest. A session counter, not saved. */
	int32 TimeScaleSets = 0;

	/** See GetAirlineOffers. Not a UPROPERTY: the airlines are the catalog's to hold. */
	TArray<FAirlineOffers> AirlineOffers;

	/** The catalog's airlines, flattened into airframes Model/ may read. See the .cpp. */
	TArray<FAirlineOffers> AirlineOffersFromCatalog() const;

	/** One game minute of the generator, on the clock. Bound in Attach. */
	void OfferTick();

	/**
	 * Every design figure the scenario sets, onto its receiver - the clock's day, the vehicle catalogue and starter fleet
	 * (ResolveVehicleCatalogue), the refill rate, the module offers, the inbox cap, the airline tuning. THE ONE DOOR (#449), run at Attach and after every load: each
	 * field it writes is Transient on its receiver, the ruling UAirlineRoster::Tuning set, so a save carries the game and
	 * never the design. New-game acts (the start hour, the opening balance, the roster reset) stay in Attach.
	 * ENFORCED BY: AirportOps.Present.RuntimeLoad.DesignFiguresAreTheScenarios
	 */
	void ApplyScenarioFigures(const class UScenario& Scenario);
	void ApplySpeed(ESimSpeed Speed);
};
