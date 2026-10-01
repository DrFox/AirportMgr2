#pragma once

#include "CoreMinimal.h"
#include "Model/RunwayFacts.h"

class UFlight;
class UFlightBoard;
class UGroundTraffic;
class URoadNetwork;

/**
 * WHICH RUNWAY A FLIGHT IS USING, for the ARRIVALS row (2026-10-01: "show what runway has been chosen"). One function over
 * the phases, so the row cannot name a runway the model is not using:
 *   - holding: the runways it WAITS for (FArrivalQueue::UsableRunwaysFor) - none is chosen until it is cleared, and the
 *     queue clears it onto whichever of these is free first;
 *   - landing and taxiing in: the end it landed on (its agent's FLandingRun::End);
 *   - taxiing out: the end its departure is armed for, once armed; departing: the end it rolls on (FTakeoffRun::End);
 *   - otherwise none - accepted and not yet due, on stand, manoeuvring, and every finished phase.
 */
namespace FlightRunway
{
	AIRPORTOPS_API TArray<FRunwayEnd> For(const UFlightBoard& Board, const UGroundTraffic& Traffic, const UFlight& Flight);

	/** The ends' names (RunwayQuery::EndName - "09L" beside a parallel), duplicates dropped, joined " or ". Empty for none. */
	AIRPORTOPS_API FString Names(const URoadNetwork& Network, TConstArrayView<FRunwayEnd> Ends);
}
