#pragma once

#include "CoreMinimal.h"
#include "Model/OpsDefinition.h"

#include "AirlineDefinition.generated.h"

class UAircraftType;

/**
 * One airline: who offers flights, in what, and how often.
 *
 * A DEFINITION ASSET rather than a table on the scenario, because UOpsDefinition's own
 * header already names airlines as an intended subclass, and because the fleet is a list of
 * UAircraftType assets - a content reference, which is what a data asset is for. A scenario
 * table would have to name types by string and would drift from the assets themselves.
 *
 * NEEDS ITS OWN PrimaryAssetTypesToScan LINE in DefaultGame.ini. UOpsDefinition::
 * GetPrimaryAssetId derives the type from the class name minus its prefix, so without that
 * line the catalog scans nothing, loads nothing, and reports "0 AirlineDefinition(s)" - with
 * no error anywhere to say the assets exist.
 */
UCLASS(BlueprintType)
class AIRPORTOPS_API UAirlineDefinition : public UOpsDefinition
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "Airline") FText DisplayName;

	/**
	 * Every type this airline may send. An offer picks one of these that the airport can
	 * actually take - see UOfferGenerator::CouldEverAdmit.
	 *
	 * SOFT, not a TObjectPtr (issue #191). UAircraftType lives in Airside's Entities/, and
	 * this is Model/ - the layer that may not own an Entities/ type, per CLAUDE.md, the same
	 * rule UAirsideContent::DefaultAircraft already follows for the same class. A bare forward
	 * declaration used to let a hard TObjectPtr compile with no #include, which passed
	 * Check-Architecture's include-direction lint while this header still owned an Entities/
	 * reference - the lint greps #include lines, and there was none to catch. A soft pointer
	 * means this header owns only a PATH, which Model/ is allowed to; UOpsRuntime::
	 * AirlineOffersFromCatalog (Present/, where Entities/ is legal - see its own comment) is the
	 * one place that resolves it to the real type.
	 */
	UPROPERTY(EditAnywhere, Category = "Airline") TArray<TSoftObjectPtr<UAircraftType>> Fleet;

	/**
	 * How much this airline wants to come at each hour of the day: 24 weights, 0..1, one per
	 * hour from 00:00, interpolated linearly between hours and wrapping at midnight. Empty is
	 * flat 1.0 all day.
	 *
	 * TIME OF DAY IS WHAT MAKES A BUSY PERIOD (spec 2026-09-28 ruling 1): a scheduled carrier
	 * peaks morning and evening, a flying club flies in daylight. It is predictable on
	 * purpose, so a player can see the morning coming on the demand strip and build for it.
	 *
	 * REPLACED OffersPerDay AND OfferWeight. OfferWeight was read by nothing - the old pick
	 * was uniform over every airline's types - and OffersPerDay's comment claimed a scaling by
	 * capability the code never did.
	 */
	UPROPERTY(EditAnywhere, Category = "Demand") TArray<double> DemandCurve;

	/** Offers per GAME hour at curve weight 1.0 and a fee multiplier of 1.0. */
	UPROPERTY(EditAnywhere, Category = "Demand", meta = (ClampMin = "0.0"))
	double PeakOffersPerHour = 1.0;

	/**
	 * The rate never falls below this in DAYLIGHT, whatever the curve and the fee say.
	 *
	 * NON-ZERO ONLY ON THE FLOOR AIRLINE - "you should never be in a place where no one wants
	 * to use you". Daylight only: at night the curve rules, so the night is a real lull.
	 */
	UPROPERTY(EditAnywhere, Category = "Demand", meta = (ClampMin = "0.0"))
	double FloorOffersPerHour = 0.0;

	/**
	 * The flying club: the airline that is always there.
	 *
	 * C READS THIS - reputation never stops it offering, and a lapsed offer of its never costs
	 * the player anything (spec 2026-09-28 rulings 7 and 8). Carried onto each flight as
	 * UFlight::bFloorAirline so C need not find the airline again.
	 */
	UPROPERTY(EditAnywhere, Category = "Demand") bool bIsFloor = false;

	/**
	 * REAL seconds an offer of this airline's stands in the inbox. Pause stops it; the speed
	 * setting does not change it - see UFlightBoard::TickOffers.
	 *
	 * PER AIRLINE, because "some airlines are more demanding than others" is one personality,
	 * and it lives with the contract length below rather than in two places.
	 */
	UPROPERTY(EditAnywhere, Category = "Offer", meta = (ClampMin = "1.0"))
	double OfferWindowSeconds = 60.0;

	/** GAME seconds from the accept to the aircraft appearing on approach. */
	UPROPERTY(EditAnywhere, Category = "Offer", meta = (ClampMin = "0.0"))
	double LeadTimeSeconds = 900.0;

	/**
	 * The turnaround contract: GAME seconds from the accept to airborne again. See
	 * UFlight::ContractSeconds, and C, which scores AirborneAt against it.
	 *
	 * ONE FIGURE PER AIRLINE (2026-09-28, from play) - it replaced lead + a 10-minute taxi
	 * allowance + the airframe's turnaround x a slack multiplier, which gave an SR22 40 game
	 * minutes and saw it reach its stand with one to spare. Aircraft MOVE in real seconds while
	 * the clock runs ~21x in daylight (USimClock), so landing and taxiing in alone cost ~28 game
	 * minutes measured, a fuel loop ~30 and taxiing out ~25 - about 95 before any queueing. How
	 * demanding an airline is, is simply how long it gives: two hours is strict, three relaxed.
	 * ENFORCED BY: AirportOps.Content.AirlineDefinition.TheAssetManagerScansThem (>= 2 h)
	 */
	UPROPERTY(EditAnywhere, Category = "Offer", meta = (ClampMin = "0.0"))
	double ContractSeconds = 7200.0;

	/**
	 * What the row prints as the flight's name. A flight-number prefix ("CU" -> "CU 204"), or
	 * a tail pattern where every '?' becomes a letter ("G-????" -> "G-ABCD"), which is how
	 * private owners read. See UOfferGenerator::MakeCallsign.
	 */
	UPROPERTY(EditAnywhere, Category = "Offer") FString CallsignPrefix = TEXT("XX");

	/** DemandCurve at a time of day, 0..1. Flat 1.0 when no curve is authored. */
	double CurveAt(double TimeOfDaySeconds) const;
};
