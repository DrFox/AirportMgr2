#pragma once

#include "CoreMinimal.h"
#include "Model/RoadEntity.h"
#include "UObject/Object.h"

#include "OfferGenerator.generated.h"

class UFlight;
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
 * Where offers come from.
 *
 * THE CHEAP HALF OF CAPABILITY, and only that. This asks FAirsideCapability - the longest
 * runway and the stands that exist - because it runs on a clock tick over every airline.
 * Whether a given offer can ACTUALLY be accepted is ArrivalPlanner::Plan's answer, asked
 * once when the player looks at the inbox: that one knows about occupancy and routes and
 * costs a search.
 *
 * Two questions, two costs, and ONE EVALUATOR EACH. A third function that re-decided "can
 * this airport take a 737" would be a second source of truth, and the two would disagree
 * the first time a stand was deleted.
 */
UCLASS()
class AIRPORTOPS_API UOfferGenerator : public UObject
{
	GENERATED_BODY()

public:
	/** How far ahead of the offer an accepted flight lands, GAME seconds. */
	UPROPERTY(EditAnywhere, Category = "Offers", meta = (ClampMin = "0.0"))
	double LeadTimeSeconds = 900.0;

	/**
	 * How long an unanswered offer stands, GAME seconds.
	 *
	 * MUST be less than LeadTimeSeconds, or an offer could lapse while its own aeroplane was
	 * already on the way - MakeOffer clamps it rather than trusting the authored pair.
	 */
	UPROPERTY(EditAnywhere, Category = "Offers", meta = (ClampMin = "0.0"))
	double OfferLifeSeconds = 600.0;

	/**
	 * Whether the airfield could take this airframe at all: runway length, and a stand wide
	 * enough to park it on.
	 *
	 * Static because it reads its two arguments and nothing else, and because the test wants
	 * to ask it without owning a generator.
	 */
	static bool AirportAdmits(const FAirsideCapability& Airport, const FAirframe& Airframe);

	/**
	 * One offer from these candidates, or nullptr if the airport can take none of them.
	 *
	 * NextId is the board's counter: the generator does not own numbering, because the board
	 * is what has to keep ids unique across a save.
	 */
	UFlight* MakeOffer(const FAirsideCapability& Airport, const TArray<FOfferCandidate>& Fleet,
		double Now, int32 NextId);
};
