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
	 *
	 * SOFT, not a TObjectPtr (issue #191). UAircraftType lives in Airside's Entities/, and
	 * this is Model/ - the layer that may not own an Entities/ type, per CLAUDE.md, the same
	 * rule UAirsideContent::DefaultAircraft already follows for the same class. A bare forward
	 * declaration used to let a hard TObjectPtr compile with no #include, which passed
	 * Check-Architecture's include-direction lint while this header still owned an Entities/
	 * reference - the lint greps #include lines, and there was none to catch. A soft pointer
	 * means this header owns only a PATH, which Model/ is allowed to; UOpsRuntime::
	 * CandidatesFromCatalog (Present/, where Entities/ is legal - see its own comment) is the
	 * one place that resolves it to the real type.
	 */
	UPROPERTY(EditAnywhere, Category = "Airline") TArray<TSoftObjectPtr<UAircraftType>> Fleet;

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
