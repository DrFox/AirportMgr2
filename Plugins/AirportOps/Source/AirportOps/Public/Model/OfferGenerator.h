#pragma once

#include "CoreMinimal.h"
#include "Model/ArrivalPlanner.h"
#include "Model/OpsDesignDefaults.h"
#include "Model/OpsSave.h"
#include "Model/RoadEntity.h"
#include "UObject/Object.h"

#include "OfferGenerator.generated.h"

class UPricing;

class FOpsEventBus;
class UAirlineDefinition;
class UAirport;
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

/**
 * What this airport says to ONE type of an airline's fleet: can it ever come, and if not, why.
 *
 * NOT A USTRUCT: it is a query result, never saved or reflected (controller ruling 2026-10-02, airlines panel Task 3).
 * TypeName is FText, as FOfferCandidate's is - it is the display name, already localised by whoever flattened the fleet.
 */
struct FFleetAdmission
{
	FText TypeName;
	bool bAdmitted = false;
	EArrivalRefusal Why = EArrivalRefusal::None;
	/** Empty when admitted; else the plan's own sentence with its figures (#396), else the reason's wording. */
	FString Sentence;
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
	 *
	 * Set by UOpsRuntime's constructor. TRANSIENT for Airport's reason below (#425): saved, it was a path to the
	 * runtime's subobject, which a later session's load resolved to null - and every later offer went unpriced.
	 */
	UPROPERTY(Transient) TObjectPtr<UPricing> Pricing = nullptr;

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

	/** The inbox cap (spec ruling 6). Copied from UScenario by UOpsRuntime::ApplyScenarioFigures. TRANSIENT (#449):
	 *  the scenario's, never the save's - see USimClock::RealSecondsDaylight.
	 *  ENFORCED BY: AirportOps.Model.Save.DesignFiguresAreNotSaved (not saved), AirportOps.Present.RuntimeLoad.DesignFiguresAreTheScenarios (re-applied) */
	UPROPERTY(Transient) int32 MaxPendingOffers = OpsDesignDefaults::MaxPendingOffers;

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
	 *
	 * THE REFUSAL AS THE PLAN'S OWN SENTENCE TOO - WITH its figures, which the reason alone
	 * cannot give: "not admitted to that runway" did not say whether length, width or surface
	 * failed (issue #396). For the log line that says an airline cannot come. A 4-argument
	 * overload that dropped the sentence forwarded here and had no production caller (#462);
	 * a caller that wants only the reason passes a FString it ignores.
	 */
	static bool CouldEverAdmit(const URoadNetwork& Network, const FVector2D& Focus,
		const FAirframe& Airframe, EArrivalRefusal& OutWhy, FString& OutSentence);

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

	/**
	 * AirlineFactorOf(Airline) (1.0 when it is unset) TIMES FleetShare(Airline) - the one factor every
	 * reader of the rate goes through, so the demand strip and the day banner draw the share too.
	 */
	double AirlineFactor(const UAirlineDefinition& Airline) const;

	/**
	 * The share of the airline's fleet this airport can admit, 0-1, from the last admission check -
	 * 1.0 before the first, when nothing has been judged yet.
	 *
	 * DEMAND SCALES WITH THE AIRPORT (ruled 2026-10-02, spec 2026-10-02-progression-and-fuel-supply §6).
	 * The rate used to be the airline's whole rate whatever share of its fleet qualified, so paving
	 * the runway changed WHICH aircraft Cumbria sent and not HOW MANY; the pacing model put the whole
	 * grass-to-tarmac climb at +30% income, +65% with the share. A plain proportion is the first cut:
	 * it scales the demand, NOT the floor (RateAt's rule), so the flying club still never goes quiet.
	 * ENFORCED BY: AirportOps.Model.Offers.Rate.ScalesWithAdmissibleShare
	 */
	double FleetShare(const UAirlineDefinition& Airline) const;

	/**
	 * A verdict for EVERY type in the airline's fleet, in fleet order, from the last admission check - empty
	 * before the first (nothing judged yet, which a reader must not show as "all crossed").
	 *
	 * ONE LIST: the offer pick, FleetShare, DescribeWhyNot, the cannot-come event's text and the airlines panel's
	 * tick/cross rows all read FAdmissionCache::Verdicts, so a row cannot disagree with what the generator does.
	 * ENFORCED BY: AirportOps.Model.Offers.FleetAdmission.EveryTypeHasAVerdict
	 */
	TArray<FFleetAdmission> GetFleetAdmission(FName AirlineId) const;

	/**
	 * Offers per GAME hour this airline accrues right now: RateAt at the clock's time, with DemandFactor and
	 * AirlineFactor (mood x fleet share). THE EXPRESSION TickMinute accrues, extracted - both call it, so the
	 * panel's "~N offers/h now" is the number that is actually accruing.
	 * ENFORCED BY: AirportOps.Model.Offers.Rate.CurrentRateIsWhatAccrues
	 */
	double CurrentRate(const UAirlineDefinition& Airline, const USimClock& Clock) const;

	/** AirlineFactorOf(Airline), or 1.0 when it is unset - the satisfaction half of AirlineFactor, without the fleet share. */
	double MoodFactor(const UAirlineDefinition& Airline) const;

	/**
	 * The airport whose status gates every offer - READ each minute, not subscribed to: a status is a value
	 * asked for when it is needed (spec 2026-09-29-ops-batch3 §3), like AirlineFactorOf. Set by UOpsRuntime's
	 * constructor; null in a bare NewObject, which reads as open. TRANSIENT, so the "Offers" blob never
	 * carries a path to the runtime's subobject.
	 * ENFORCED BY: AirportOps.Present.Airport.RunwayComesAndGoes
	 */
	UPROPERTY(Transient) TObjectPtr<const UAirport> Airport = nullptr;

	/**
	 * Where an airline's change of verdict is announced (FAirlineAdmissionChangedEvent, #446/#445). Owned by UOpsRuntime, like
	 * UAirlineRoster::Bus - set and cleared with the runtime's other publishers (UOpsRuntime::Publishers), null in a bare NewObject.
	 * ENFORCED BY: AirportOps.Model.Offers.AdmissionChangeIsAnnounced, AirportOps.Present.Bus.DetachUnhooksEveryPublisher
	 */
	FOpsEventBus* Bus = nullptr;

	/** Every airline back to "could come", unjudged - a reopen (UOpsRuntime::WireBus). The next TickMinute judges
	 *  afresh against the admission cache, and logs the transition if an airline still cannot come. */
	void ForgetAirlineVerdicts();

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
	 *
	 * NOTHING AT ALL unless Airport reads Open - no offer, and no airline judged (see Airport).
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

	/**
	 * Why AirlineId's fleet cannot use this airport, in the sentence TickMinute's "cannot use this airport"
	 * log line prints - the plan's own refusal with its figures, else the reason's wording. Empty when the
	 * airline has not been judged yet (no admission check has run for it). For the ops AirlineCannotCome
	 * alert, so the alert and the log say the same thing.
	 */
	FString DescribeWhyNot(FName AirlineId) const;

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
		/**
		 * One verdict per fleet type, in fleet order - THE list. What used to be Admissible (indices) and
		 * FirstRefusal/FirstRefusalSentence/FirstRefused are queries over it, so the pick, the share, the
		 * "why not" text and the panel cannot drift apart. The sentence is the plan's own, figures and all - see CouldEverAdmit.
		 */
		TArray<FFleetAdmission> Verdicts;

		int32 AdmittedCount() const
		{
			int32 Count = 0;
			for (const FFleetAdmission& Each : Verdicts)
			{
				Count += Each.bAdmitted ? 1 : 0;
			}
			return Count;
		}

		/** Fleet index of the first type refused, or INDEX_NONE when every type is admitted. */
		int32 FirstRefusedIndex() const
		{
			return Verdicts.IndexOfByPredicate([](const FFleetAdmission& Each) { return !Each.bAdmitted; });
		}
		/**
		 * The reason last ANNOUNCED for this airline (FAirlineAdmissionChangedEvent), empty while it could come. Not reset with the cache: it
		 * is what a rebuilt cache is compared with, so a change of reason is one event and a repeat of it is none.
		 */
		FString AnnouncedReason;
	};
	TMap<FName, FAdmissionCache> AdmissionCache;
	int32 AdmissionChecks = 0;
};
