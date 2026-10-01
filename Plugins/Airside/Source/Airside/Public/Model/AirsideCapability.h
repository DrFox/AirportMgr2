#pragma once

#include "CoreMinimal.h"
#include "Model/RoadEntity.h"
#include "Model/RoadHandles.h"
// FRunwayEnd. Was reached through whatever the unity blob included first - adaptive unity
// compiled this header alone on 2026-09-27 and it did not stand on its own.
#include "Model/RunwayFacts.h"
#include "AirsideCapability.generated.h"

class URoadNetwork;
class URoadProfile;

/** One runway strip: every collinear continuous-profile segment between two thresholds. */
USTRUCT()
struct AIRSIDE_API FRunwaySummary
{
	GENERATED_BODY()

	/** The threshold this strip was found from, its direction and length, and its seed - see #88. */
	UPROPERTY() FRunwayEnd End;

	/** The pavement kind - the surface (grass/asphalt/concrete) is a fact about the profile. */
	UPROPERTY() TObjectPtr<const URoadProfile> Profile = nullptr;
};

/**
 * What the AIRFIELD can admit, as a pure function of the graph. The building half of the
 * airport's capability (which services are offered) lives in AirportOps and joins this.
 * Spec §3.1.
 */
USTRUCT()
struct AIRSIDE_API FAirsideCapability
{
	GENERATED_BODY()

	UPROPERTY() TArray<FRunwaySummary> Runways;
};

namespace AirsideCapability
{
	/**
	 * Enumerates the runways. (It enumerated stands too - FStandSummary, and a LongestRunway() beside
	 * them - until #462 deleted both: the last production reader of either went in b6926198, and
	 * only tests were keeping them.) A runway is recognised by its profile
	 * (URoadProfile::bContinuousThroughJunctions), and a strip split by exits is ONE runway:
	 * each continuous segment is asked for its extent via URoadNetwork::RunwayExtentAt, and
	 * extents that share a threshold pair (either way round) are the same strip. Deduplicated
	 * on the threshold pair rather than on segment adjacency so that the walk RunwayExtentAt
	 * already does is the only definition of "one runway" in the codebase.
	 */
	AIRSIDE_API FAirsideCapability Summarise(const URoadNetwork& Network);

	/** Summarise's runway list without the wrapper struct - the same walk. What
	 *  RunwayAdmission::CheckArrival asks, once per arrival admission. */
	AIRSIDE_API TArray<FRunwaySummary> SummariseRunways(const URoadNetwork& Network);
}
