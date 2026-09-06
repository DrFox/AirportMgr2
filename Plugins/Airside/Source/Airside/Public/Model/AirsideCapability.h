#pragma once

#include "CoreMinimal.h"
#include "Model/RoadEntity.h"
#include "Model/RoadHandles.h"
#include "AirsideCapability.generated.h"

class URoadNetwork;
class URoadProfile;

/** One runway strip: every collinear continuous-profile segment between two thresholds. */
USTRUCT()
struct AIRSIDE_API FRunwaySummary
{
	GENERATED_BODY()

	UPROPERTY() FVector2D Threshold = FVector2D::ZeroVector;
	UPROPERTY() FVector2D Direction = FVector2D(1.0, 0.0);
	UPROPERTY() double Length = 0.0;
	/** The pavement kind - the surface (grass/asphalt/concrete) is a fact about the profile. */
	UPROPERTY() TObjectPtr<const URoadProfile> Profile = nullptr;
};

/** One stand: what it was sized for and which service roles can reach it. */
USTRUCT()
struct AIRSIDE_API FStandSummary
{
	GENERATED_BODY()

	UPROPERTY() FEntityInstanceId Entity;
	UPROPERTY() double DesignWingspan = 0.0;
	UPROPERTY() TArray<EServiceRole> AnchorRoles;
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
	UPROPERTY() TArray<FStandSummary> Stands;

	double LongestRunway() const;
};

namespace AirsideCapability
{
	/**
	 * Enumerates runways and stands. A runway is recognised by its profile
	 * (URoadProfile::bContinuousThroughJunctions), and a strip split by exits is ONE runway:
	 * each continuous segment is asked for its extent via URoadNetwork::RunwayExtentAt, and
	 * extents that share a threshold pair (either way round) are the same strip. Deduplicated
	 * on the threshold pair rather than on segment adjacency so that the walk RunwayExtentAt
	 * already does is the only definition of "one runway" in the codebase.
	 */
	AIRSIDE_API FAirsideCapability Summarise(const URoadNetwork& Network);
}
