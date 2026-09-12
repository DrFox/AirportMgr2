#pragma once

#include "CoreMinimal.h"
#include "Model/ArrivalPlanner.h"
#include "Model/RoadEntity.h"
#include "UObject/Object.h"

#include "OfferGenerator.generated.h"

class UFlight;
class URoadNetwork;
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
 * The cost is one route search per candidate per offer tick, a handful of each, minutes
 * apart. That is not a price worth a second source of truth.
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
	 * Whether this field could EVER take this airframe, and why not when it could not.
	 *
	 * PERMANENT refusals only. RunwayOccupied and NoFreeStand clear on their own, so an
	 * aeroplane refused for those is still worth offering - the player answers the offer
	 * minutes before it lands, and the row shows the live reason meanwhile. RunwayTooShort,
	 * NotAdmitted, NoExit and NoRouteToStand do not clear without the player building
	 * something, so offering them is offering a button that can never be pressed.
	 *
	 * Static because it reads its arguments and nothing else, and because the tests want to
	 * ask it without owning a generator.
	 */
	static bool CouldEverAdmit(const URoadNetwork& Network, const FVector2D& Focus,
		const FAirframe& Airframe, EArrivalRefusal& OutWhy);

	/** True for a refusal no amount of waiting will clear. See CouldEverAdmit. */
	static bool IsPermanentRefusal(EArrivalRefusal Why);

	/**
	 * One offer from these candidates, or nullptr if the airport can take none of them.
	 *
	 * NextId is the board's counter: the generator does not own numbering, because the board
	 * is what has to keep ids unique across a save.
	 */
	UFlight* MakeOffer(const URoadNetwork& Network, const FVector2D& Focus,
		const TArray<FOfferCandidate>& Fleet, double Now, int32 NextId);
};
