#pragma once

#include "CoreMinimal.h"
#include "Model/ArrivalPlanner.h"
#include "Model/OpsSave.h"
#include "Model/RoadEntity.h"
#include "UObject/Object.h"

#include "OfferGenerator.generated.h"

class UPricing;

class UAirlineDefinition;
class UFlight;
class URoadNetwork;
class USimClock;
struct FAirsideCapability;

/**
 * One thing an airline could send, flattened out of its definition assets.
 *
 * FLATTENED BY THE CALLER, because Model/ may not see Entities/ and the fleet is a list of
 * UAircraftType assets. UOpsRuntime reads UAircraftType::Airframe() once per candidate, the
 * same division of labour URoadNetwork::PlaceEntity already uses for a design wingspan.
 */
USTRUCT()
struct AIRPORTOPS_API FOfferCandidate
{
	GENERATED_BODY()

	UPROPERTY() FAirframe Airframe;
	UPROPERTY() FText AirlineName;
	UPROPERTY() FText TypeName;
};

/**
 * One airline and what of its fleet the caller could resolve, handed to TickMinute.
 *
 * NOT A USTRUCT: it is an argument, never saved or reflected, and the airline it points at is
 * held by UOpsCatalog's own UPROPERTY for as long as the runtime is attached.
 */
struct FAirlineOffers
{
	const UAirlineDefinition* Airline = nullptr;
	TArray<FOfferCandidate> Fleet;
};

/** One airline's running total toward its next offer. Saved, so a reload continues it. */
USTRUCT()
struct AIRPORTOPS_API FAirlineOfferState
{
	GENERATED_BODY()

	/** Offers owed so far, in units of offers. Emits one each time it reaches Threshold. */
	UPROPERTY() double Accumulated = 0.0;

	/** What Accumulated must reach for the next offer; drawn from the stream, 0 = not yet. */
	UPROPERTY() double Threshold = 0.0;

	/** Whether any of the fleet could use the airport last minute. For the transition log. */
	UPROPERTY() bool bCouldCome = true;
};

/**
 * Where offers come from.
 *
 * ONE EVALUATOR, NOT TWO. This asks ArrivalPlanner::Plan - the same question the inbox asks
 * when the player looks at a row - with the occupancy left out, so it answers "could this
 * field EVER take this aeroplane" rather than "is it free right now".
 *
 * It used to filter on FAirsideCapability alone (longest runway, widest stand) on the
 * grounds that generation is cheap and acceptance is dear. That shipped an inbox in which
 * every single Accept was greyed out on 2026-09-11: the cheap filter knew nothing about
 * RunwayAdmission, so it happily offered A320s to a 15 m-wide GA strip that admits a 15 m
 * wingspan. A filter that disagrees with the gate behind it is worse than no filter - it
 * fills the inbox with decisions the player is not allowed to make.
 *
 * The cost is one route search per fleet type per airline per game minute. N was ~10 types
 * across 2 airlines on 2026-09-28; a fleet of hundreds would want the answer cached on the
 * guideline revision, the way the board caches its verdict.
 */
UCLASS()
class AIRPORTOPS_API UOfferGenerator : public UObject, public IOpsPersistent
{
	GENERATED_BODY()

public:
	// --- IOpsPersistent ---------------------------------------------------------------
	/**
	 * "Offers". NEW WITH SNAPSHOT VERSION 5: this class's Stream comment said "SAVED" for a
	 * month while the generator was in no Persistents() list, so a reload restarted the
	 * sequence. Now the stream, the running totals and the drop count all travel.
	 */
	virtual FName SaveBlobName() const override { return TEXT("Offers"); }
	virtual UObject& AsPersistentObject() override { return *this; }

	/** Game seconds between TickMinute calls. The runtime books Clock->Every with this. */
	static constexpr double TickSeconds = 60.0;

	/**
	 * What a landing is worth, or null for a test that does not care.
	 *
	 * THE OFFER IS PRICED, NOT THE LANDING. See MakeOffer - the fee is fixed here so the inbox
	 * row can show what accepting it is worth, and so the player's lever moves NEW offers only.
	 * Its DemandFactor is also read every tick - see RateAt.
	 */
	UPROPERTY() TObjectPtr<UPricing> Pricing = nullptr;

	/**
	 * Which aeroplane this generator picks, next, and every other draw it makes (thresholds,
	 * callsigns).
	 *
	 * A SEEDED STREAM AND NOT FMath::RandHelper, which is what this used. The global RNG is
	 * shared with everything else in the process and is advanced by anything that draws from
	 * it, so the same save reloaded twice offered different aeroplanes - and the determinism
	 * the systems map asks for ("same seed, same inputs, same ledger") could not be written as
	 * a test at all, which is why M1 deferred it.
	 *
	 * SAVED, so a reload continues the same sequence rather than restarting it: a player who
	 * reloads to dodge an offer they did not like should get the same one back.
	 * ENFORCED BY: AirportOps.Save.GeneratorSurvivesASave
	 */
	UPROPERTY() FRandomStream Stream;

	/** Per airline (keyed by its asset FName), how close it is to its next offer. */
	UPROPERTY() TMap<FName, FAirlineOfferState> States;

	/**
	 * Offers the airport was owed and could not take because the inbox was full.
	 *
	 * C READS IT as unmet demand. Saved; never reset here - C decides what a count means.
	 */
	UPROPERTY() int32 DroppedOffers = 0;

	/** The inbox cap (spec ruling 6). Copied from UScenario at attach. */
	UPROPERTY() int32 MaxPendingOffers = 8;

	/**
	 * Whether this field could EVER take this airframe, and why not when it could not.
	 *
	 * PERMANENT refusals only. RunwayOccupied and NoFreeStand clear on their own, so an
	 * aeroplane refused for those is still worth offering - the player answers the offer
	 * minutes before it lands, and the row shows the live reason meanwhile. RunwayTooShort,
	 * NotAdmitted, NoExit, NoRouteToStand and NoStandBigEnough do not clear without the
	 * player building something, so offering them is offering a button that can never be pressed.
	 *
	 * Static because it reads its arguments and nothing else, and because the tests want to
	 * ask it without owning a generator.
	 */
	static bool CouldEverAdmit(const URoadNetwork& Network, const FVector2D& Focus,
		const FAirframe& Airframe, EArrivalRefusal& OutWhy);

	/**
	 * The same, and the refusal as the plan's own sentence - WITH its figures, which the reason
	 * alone cannot give: "not admitted to that runway" did not say whether length, width or
	 * surface failed (issue #396). For the log line that says an airline cannot come.
	 */
	static bool CouldEverAdmit(const URoadNetwork& Network, const FVector2D& Focus,
		const FAirframe& Airframe, EArrivalRefusal& OutWhy, FString& OutSentence);

	/** True for a refusal no amount of waiting will clear. See CouldEverAdmit. */
	static bool IsPermanentRefusal(EArrivalRefusal Why);

	/**
	 * One airline's offer rate, offers per GAME hour, at this time of day:
	 * Peak x CurveAt x DemandFactor, never below the airline's floor in daylight.
	 *
	 * THE FEE'S ONLY COST, AND IT IS PAID HERE. A higher landing fee scales DemandFactor down,
	 * so the player earns more per aeroplane and sees fewer of them. See UPricing::Elasticity
	 * for why that trade is deliberately even until the airport is capacity-bound: the lever
	 * is meant to pose "am I full?", not to have a best setting.
	 *
	 * READ EVERY TICK, not once at Attach - which is what OfferIntervalSeconds, the function
	 * this replaced, was reduced to, and why raising the fee used to be pure profit.
	 * ENFORCED BY: AirportOps.Model.Offers.Generate.FeeStepMovesCadence
	 *
	 * A negative factor is clamped to zero rather than trusted: it would otherwise make a
	 * negative rate, which reads exactly like an airport nobody flies to.
	 *
	 * ONE FUNCTION, which the demand strip samples too, so the strip cannot draw a curve the
	 * generator does not follow.
	 *
	 * AND THE AIRLINE'S OWN FACTOR - its satisfaction, through UAirlineRoster::RateMultiplier (spec
	 * 2026-09-29-ops-event-bus section 3) - scales the demand but NOT the floor, for the fee's reason:
	 * the floor is what keeps the airport from going silent, whatever the airline thinks of it.
	 * ENFORCED BY: AirportOps.Model.Offers.Rate.AirlineFactorSparesTheFloor
	 */
	static double RateAt(const UAirlineDefinition& Airline, double TimeOfDaySeconds, bool bDaylight,
		double DemandFactor, double AirlineFactor = 1.0);

	/**
	 * RateAt summed over every airline, each at its own factor - what the demand strip draws, given the
	 * generator's AirlineFactor so it draws the rate the generator follows. ONE SIGNATURE: a
	 * factor-less overload would be a second way in that silently ignored satisfaction (stage 2 review).
	 */
	static double TotalRateAt(TArrayView<const FAirlineOffers> Airlines, double TimeOfDaySeconds,
		bool bDaylight, double DemandFactor, TFunctionRef<double(const UAirlineDefinition&)> AirlineFactorOf);

	/** The demand factor RateAt is given: Pricing's, or 1.0 when there is none. */
	double DemandFactor() const;

	/**
	 * How much this airline's own demand is scaled - its satisfaction. Set by UOpsRuntime::Attach to
	 * read UAirlineRoster; unset in a bare NewObject, and AirlineFactor then answers 1.0. READ every
	 * minute, never cached here: it is the roster's value, and a copy would be a second one.
	 */
	TFunction<double(const UAirlineDefinition&)> AirlineFactorOf;

	/** AirlineFactorOf(Airline), or 1.0 when it is unset. */
	double AirlineFactor(const UAirlineDefinition& Airline) const;

	/**
	 * One game minute of demand. Returns the offers it made (0..n), never more than the
	 * inbox has room for: MaxPendingOffers - PendingNow.
	 *
	 * AN ACCUMULATOR, NOT A TIMER (spec 2026-09-28). Each airline adds RateAt/60 to its own
	 * running total and emits an offer each time the total reaches a threshold drawn from
	 * [0.6, 1.4] - so the mean rate is exact, the spacing is not a metronome, and a peak hour
	 * is reliably busy. Poisson arrivals were rejected: a peak can go randomly dead, which a
	 * player reads as a bug.
	 *
	 * NO BANKING. An airline none of whose fleet can use the airport accrues nothing, so the
	 * minute the player widens a runway does not release hours of queued demand at once.
	 *
	 * NextId is the board's counter: the generator does not own numbering, because the board
	 * is what has to keep ids unique across a save.
	 */
	TArray<UFlight*> TickMinute(const URoadNetwork& Network, const FVector2D& Focus,
		TArrayView<const FAirlineOffers> Airlines, const USimClock& Clock, int32 PendingNow,
		TFunctionRef<int32()> NextId);

	/**
	 * Build the offer for one chosen candidate. Admissibility and the pick are TickMinute's;
	 * this only fills the flight in, which is why it takes no network.
	 */
	UFlight* MakeOffer(const FVector2D& Focus, const UAirlineDefinition& Airline,
		const FOfferCandidate& Chosen, double Now, int32 Id);

	/** "CU 204" from "CU", or "G-ABCD" from "G-????" - see UAirlineDefinition::CallsignPrefix. */
	static FString MakeCallsign(const FString& Prefix, FRandomStream& Stream);

	/** How many CouldEverAdmit route searches TickMinute has actually run. See AdmissionCache. */
	int32 AdmissionChecksForTest() const { return AdmissionChecks; }

private:
	/**
	 * Which of an airline's fleet this field could ever take, remembered per airline.
	 *
	 * CACHED ON THE GRAPH (review I2, 2026-09-28): CouldEverAdmit is a full ArrivalPlanner::Plan
	 * per fleet type with NO occupancy, so its answer moves only when the network, its guideline
	 * revision, the approach focus or the fleet does - and it used to run every game minute,
	 * ~40 times a real second at x32. Not saved: a load recomputes on its first minute.
	 */
	struct FAdmissionCache
	{
		TWeakObjectPtr<const URoadNetwork> Network;
		uint32 GuidelineRevision = 0;
		FVector2D Focus = FVector2D::ZeroVector;
		int32 FleetSize = INDEX_NONE;
		TArray<int32> Admissible;
		EArrivalRefusal FirstRefusal = EArrivalRefusal::None;
		/** FirstRefusal as the plan described it, figures and all - see CouldEverAdmit. */
		FString FirstRefusalSentence;
		int32 FirstRefused = INDEX_NONE;
	};
	TMap<FName, FAdmissionCache> AdmissionCache;
	int32 AdmissionChecks = 0;
};
