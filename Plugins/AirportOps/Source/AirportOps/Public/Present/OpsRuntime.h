#pragma once

#include "CoreMinimal.h"
#include "Model/AgentRescue.h"
#include "Model/OfferGenerator.h"
#include "Model/OpsEventBus.h"
#include "Model/SimClock.h"
#include "UObject/Object.h"
#include "OpsRuntime.generated.h"

class ARoadNetworkActor;
class UAirlineRoster;
class UGroundTraffic;
class UOpsCatalog;
class UOpsEvents;
class UJobBoard;
class UFlightBoard;
class UOfferGenerator;
class ULedger;
class UPricing;
struct FAirframe;
struct FEntityInstance;
struct FVehicle;
enum class EAgentPhase : uint8;
enum class EArrivalRefusal : uint8;

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
	 * FORWARDERS to UAgentRescue with this runtime's traffic, network and clock - the three the driver
	 * does not hold, as LandNear supplies them. Refused ("Nothing selected"-shaped) when unattached.
	 * ENFORCED BY: AirportOps.Present.UnstickForwards
	 */
	FUnstickVerdict CanUnstick(int32 AgentId, EUnstickAction Action) const;
	FUnstickVerdict Unstick(int32 AgentId, EUnstickAction Action);

	/** How every airline feels about this airport. See UAirlineRoster - a Reaction on the bus. */
	UAirlineRoster* GetAirlines() const { return Airlines; }

	ARoadNetworkActor* GetTarget() const { return Target; }

	/**
	 * What Stand was built for: UAirsideSettings::ResolveStandDesignVehicleOf on its definition and
	 * its outline's letter. The ONE reader Attach hands UJobBoard::DesignVehicleOf, and the one
	 * the world-free fuel fixture hands it too, so the two cannot read a stand differently. A
	 * forwarder, not logic - the reading lives in Content/.
	 */
	static FVehicle StandDesignVehicleOf(const FEntityInstance& Stand);

	/** Binds to the actor's traffic delegates. Safe to call again with a new actor (unbinds the old). */
	void Attach(ARoadNetworkActor* Actor);

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

	/** True once Attach has armed the generator's minute tick. False before Attach. */
	bool HasOfferScheduledForTest() const { return OfferHandle != INDEX_NONE; }

	/**
	 * Cancel and re-book the two repeaters - the generator's minute tick and the daily upkeep -
	 * from the clock's CURRENT Now.
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

	UPROPERTY() TObjectPtr<UAirlineRoster> Airlines;

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
	 * ENFORCED BY: AirportOps.Present.Bus.DeadlineWakesTheBoard
	 */
	int32 JobBoardDeadlineHandle = INDEX_NONE;
	void ArmJobBoardDeadline();

	/** What FNetworkChangedEvent compares against - see UOpsRuntime::Tick. Not saved: a load swaps
	 *  the revision anyway, and a spurious first event only costs one pass. */
	TWeakObjectPtr<const URoadNetwork> SeenNetwork;
	uint32 SeenGuidelineRevision = 0;

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

	/** See GetAirlineOffers. Not a UPROPERTY: the airlines are the catalog's to hold. */
	TArray<FAirlineOffers> AirlineOffers;

	/** The catalog's airlines, flattened into airframes Model/ may read. See the .cpp. */
	TArray<FAirlineOffers> AirlineOffersFromCatalog() const;

	/** One game minute of the generator, on the clock. Bound in Attach. */
	void OfferTick();

	FDelegateHandle PhaseHandle;
	FDelegateHandle RefusalHandle;

	void Detach();
	void ApplySpeed(ESimSpeed Speed);
	void OnAgentPhase(int32 AgentId, EAgentPhase From, EAgentPhase To);
	void OnArrivalRefused(EArrivalRefusal Why);
};
