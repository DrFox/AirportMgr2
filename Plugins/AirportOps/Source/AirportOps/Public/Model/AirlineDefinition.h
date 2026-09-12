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
	 * actually take - see UOfferGenerator::AirportAdmits.
	 */
	UPROPERTY(EditAnywhere, Category = "Airline") TArray<TObjectPtr<UAircraftType>> Fleet;

	/** Relative weight against other airlines when an offer is generated. */
	UPROPERTY(EditAnywhere, Category = "Airline", meta = (ClampMin = "0.0"))
	double OfferWeight = 1.0;

	/**
	 * Offers per GAME day when the airport can take this airline's whole fleet.
	 *
	 * The generator scales it down as capability falls, which is what makes GROWING the
	 * airport busy the inbox rather than a timer doing it regardless.
	 */
	UPROPERTY(EditAnywhere, Category = "Airline", meta = (ClampMin = "0.0"))
	double OffersPerDay = 6.0;
};
