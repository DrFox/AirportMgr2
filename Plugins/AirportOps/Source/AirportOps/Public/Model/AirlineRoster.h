#pragma once

#include "CoreMinimal.h"
#include "Model/OpsDefinition.h"
#include "Model/OpsSave.h"
#include "UObject/Object.h"
#include "AirlineRoster.generated.h"

class FOpsEventBus;
struct FDayEndedEvent;
struct FFlightAirborneEvent;
struct FOfferDeclinedEvent;
struct FOfferExpiredEvent;

/** One change to an airline's satisfaction and what caused it - the "why" the inbox row shows. */
USTRUCT()
struct AIRPORTOPS_API FAirlineSatisfactionChange
{
	GENERATED_BODY()

	/** New minus old, after the clamp - so a change that hit the ceiling records what it really moved. */
	UPROPERTY() double Delta = 0.0;

	/** "on time", "late departure (25 min)", "offer ignored", ... */
	UPROPERTY() FString Cause;
};

/**
 * How one airline feels about this airport. Spec 2026-09-29-ops-event-bus §3.
 *
 * A ROW, NOT A UObject PER AIRLINE - the spec named a UAirline object; this is the FServiceVehicle-in-
 * UJobBoard shape instead: one object to save and one to wire, and nothing needs a UObject handle per
 * airline yet (no Blueprint, no list view binds one). Revisit when an airline card needs one.
 */
USTRUCT()
struct AIRPORTOPS_API FAirlineStanding
{
	GENERATED_BODY()

	/** The UAirlineDefinition's object name - the same key UFlight::AirlineId carries. */
	UPROPERTY() FName AirlineId;

	/** 0..1. Starts at FAirlineSatisfactionTuning::Start. */
	UPROPERTY() double Satisfaction = 0.5;

	/** The last few changes, oldest first, capped at UAirlineRoster::RecentCap. */
	UPROPERTY() TArray<FAirlineSatisfactionChange> Recent;
};

/**
 * Every airline's satisfaction, and the rules that move it: the proving consumer of the ops event bus
 * (spec 2026-09-29 §3) and the first airline state that exists at runtime - before it an airline was a
 * UAirlineDefinition plus an offer accumulator.
 *
 * A REACTION, IN THE BUS'S SENSE. It hears what the Sim tier has already settled - a flight airborne
 * and how late, an offer that lapsed or was declined, a day ending - and changes only its own state.
 * UOpsRuntime::WireBus is where it is subscribed; this class never sees the bus's subscribe side.
 *
 * READ, NOT SUBSCRIBED TO, by the offer generator: RateMultiplier is a value asked for when an offer
 * rate is computed, which is what a value is for. See UOfferGenerator::AirlineFactorOf.
 *
 * SEEDED FROM THE CATALOG, never from an event. An event naming an airline the roster does not have is
 * the debug flight (key 7, NAME_None) or an airline since removed from content, and neither should grow
 * a row - so every handler ignores an unknown id rather than Ensure-ing it.
 * ENFORCED BY: AirportOps.Model.Airlines.UnknownAirlineIgnored
 */
UCLASS()
class AIRPORTOPS_API UAirlineRoster : public UObject, public IOpsPersistent
{
	GENERATED_BODY()

public:
	static constexpr int32 RecentCap = 5;

	// --- IOpsPersistent ---------------------------------------------------------------
	virtual FName SaveBlobName() const override { return TEXT("Airlines"); }
	virtual UObject& AsPersistentObject() override { return *this; }

	/** Rows are the save's; a snapshot with no "Airlines" blob restores none, and UOpsRuntime re-seeds
	 *  every catalog airline at Tuning.Start after the load. */
	virtual void OnBeforeRestore() override { Standings.Reset(); }

	/**
	 * The figures the rules use - the scenario's, copied at attach.
	 *
	 * TRANSIENT, so never the save's: a save made before a retune must not carry the old tuning
	 * forward, and the scenario asset is the one place a designer edits them.
	 */
	UPROPERTY(Transient) FAirlineSatisfactionTuning Tuning;

	/** Where changes are announced. Set by UOpsRuntime::Attach; null in a bare NewObject, and every
	 *  publish checks. Raw: the runtime owns both this and the bus. */
	FOpsEventBus* Bus = nullptr;

	/** Add AirlineId at Tuning.Start if it has no row. The catalog's airlines, at attach and after a load. */
	void Ensure(FName AirlineId);

	/** The row, or null for an airline the roster was never seeded with. */
	const FAirlineStanding* Find(FName AirlineId) const;

	/**
	 * How much this airline's demand is scaled: Lerp(MinRateMultiplier, MaxRateMultiplier, Satisfaction),
	 * never below 1.0 for a floor airline - bIsFloor exists so the airport is never empty, and an unhappy
	 * floor airline must not undo that. 1.0 for an airline with no row.
	 * ENFORCED BY: AirportOps.Model.Airlines.FloorNeverBelowOne
	 */
	double RateMultiplier(FName AirlineId, bool bIsFloor) const;

	// --- Reaction-tier handlers (subscribed in UOpsRuntime::WireBus) ---------------------
	void OnFlightAirborne(const FFlightAirborneEvent& Event);
	void OnOfferExpired(const FOfferExpiredEvent& Event);

	/** DELIBERATELY FREE: declining is a legitimate choice (spec §3), and an airline that punished it
	 *  would make the inbox a trap. Heard so the decision is visible in the log, and nothing more. */
	void OnOfferDeclined(const FOfferDeclinedEvent& Event);

	/** Every airline forgives a little each day: Tuning.DailyDriftFraction of the way back to Start. */
	void OnDayEnded(const FDayEndedEvent& Event);

	const TArray<FAirlineStanding>& GetStandings() const { return Standings; }

private:
	/** Saved: the rows ARE this object's state. */
	UPROPERTY() TArray<FAirlineStanding> Standings;

	FAirlineStanding* FindMutable(FName AirlineId);

	/** Clamp to 0..1; if it moved, record the change, trim Recent, log it and publish it. */
	void Apply(FAirlineStanding& Standing, double Delta, const FString& Cause);
};
