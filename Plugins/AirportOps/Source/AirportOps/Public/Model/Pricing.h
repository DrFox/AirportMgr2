#pragma once

#include "CoreMinimal.h"
#include "Model/OpsSave.h"
#include "UObject/Object.h"

#include "Pricing.generated.h"

struct FAirframe;

/**
 * The ONE place a base figure becomes a number anyone spends.
 *
 * WHY A RESOLVER AND NOT CONSTANTS ON THE ASSETS. Prices are not constants in this game:
 * research will make roads cheaper, a fuel contract will make fuel cheaper, and the player
 * chooses what to charge for a landing. If call sites read a rate off an asset and spent it,
 * each of those would become a hunt for call sites - which is exactly how the Piper's
 * performance figures came to be typed at seven places. So Airside quotes what is THERE
 * (BuildCost), and this decides what it COSTS.
 *
 * ONE LIVE MODIFIER TODAY: LandingFeeMultiplier. Research and contract modifiers become
 * further inputs to these same functions in M4, with no call site touched. A generic stacked-
 * modifier engine with named sources and an ordering rule is deliberately NOT here - nothing
 * but the lever could push one, and scaffolding with no publisher is the bug CLAUDE.md names
 * three times.
 *
 * THE FEE TABLE IS THE ICAO LETTER TABLE. Solve/IcaoCode.h is already this codebase's one
 * table of code letters, so a new aircraft type is priced the moment it has a wingspan and
 * there is no per-type fee to author anywhere. The letter-to-MONEY row lives here rather than
 * there because it is money, not aerodrome geometry, and Solve/ stays dependency-free.
 */
UCLASS()
class AIRPORTOPS_API UPricing : public UObject, public IOpsPersistent
{
	GENERATED_BODY()

public:
	// --- IOpsPersistent ---------------------------------------------------------------
	virtual FName SaveBlobName() const override { return TEXT("Pricing"); }
	virtual UObject& AsPersistentObject() override { return *this; }

	/**
	 * What the player charges, as a multiple of the authored fee. THE one decision this class
	 * exposes to them, and saved because it is theirs rather than the scenario's.
	 */
	UPROPERTY() double LandingFeeMultiplier = 1.0;

	/**
	 * How hard demand answers the fee: offers scale by Multiplier^-Elasticity, constant-
	 * elasticity demand, the textbook form.
	 *
	 * ONE BY DEFAULT, AND THAT IS A DESIGN DECISION, NOT A PLACEHOLDER. At 1.0, fee times
	 * demand is flat: raising the fee earns more per flight and loses exactly enough flights
	 * to cancel it. So the lever pays NOTHING while stands stand empty, and pays real money
	 * only once the airport is full and turning away offers it could not have served anyway.
	 * The decision becomes "am I full?", a question about the airport, rather than a slider
	 * with one correct position. Below 1.0 raising fees would always be right; above 1.0,
	 * always wrong. Both are traps, and a player would find either in an afternoon.
	 */
	UPROPERTY() double Elasticity = 1.0;

	/** What tearing something out gives back, as a fraction of today's price. */
	UPROPERTY() double RefundFraction = 0.5;

	/**
	 * U+00A4, the Unicode GENERIC currency sign - the glyph whose whole purpose is to stand in
	 * for an unspecified currency. No real country is implied, and unlike an invented glyph it
	 * is present in every font the UI might fall back to.
	 */
	UPROPERTY() FString CurrencySymbol = TEXT("¤");

	/** What this aeroplane pays to land, the player's lever included. */
	double LandingFee(const FAirframe& Airframe) const;

	/** Per GAME hour on a stand. A tenth of the landing fee - see the class comment. */
	double ParkingFeePerHour(const FAirframe& Airframe) const;

	/**
	 * What a completed fuelling earns. Half the landing fee.
	 *
	 * NOT SCALED BY THE LANDING-FEE LEVER: the player is charging for a service they actually
	 * performed, not for permission to land. One lever moving both would make the fee decision
	 * unreadable - a rise meant to price landings would quietly reprice fuelling too.
	 */
	double FuelServiceFee(const FAirframe& Airframe) const;

	/** Offers per day scale by this. See Elasticity. */
	double DemandFactor() const;

	/**
	 * What Airside's quoted base amount actually costs.
	 *
	 * Source is the URoadProfile or UEntityDefinition being placed, and is UNUSED TODAY - it is
	 * how an M4 research discount aimed at taxiways will key on the asset itself rather than on
	 * a parallel enum of build kinds that would have to be kept in agreement with
	 * EPlaceableEntity. Named now because the call sites that must pass it are being written
	 * now, and threading an argument back through them later is the churn this avoids.
	 */
	double PriceOfBuild(double BaseAmount, const UObject* Source) const;

	/** What tearing it out gives back: RefundFraction of today's price. */
	double ScrapValue(double BaseAmount, const UObject* Source) const;

	/** The ONLY place money becomes text, so no currency symbol ever reaches Airside. */
	FText Format(double Amount) const;

private:
	/** The authored landing fee for an ICAO code letter, before the lever. */
	double BaseLandingFeeForLetter(const FString& Letter) const;
};
