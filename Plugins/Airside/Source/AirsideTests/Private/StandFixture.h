#pragma once

#include "CoreMinimal.h"
#include "Entities/AircraftType.h"
#include "Entities/EntityDefinition.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Placing a stand the way the game places one, for the tests that need one on a graph.
 *
 * SHARED RATHER THAN COPIED, since 2026-09-16. It lived in ServiceLinkTest.cpp, where it was
 * the only caller; StandLaneTest.cpp now needs the same placement to measure the lane the
 * BUILDER laid rather than the one the definition describes. A second copy of this call would
 * be a second statement of which of URoadNetwork::PlaceEntity's seven arguments come off the
 * definition - and the one nobody updated is how an entity gets placed with a zero wingspan
 * and every span rule silently stops binding.
 *
 * STILL NAMESPACE ServiceLinkFixture, deliberately: renaming it would touch every assertion in
 * a file this task is already rewriting, for no gain a reader could name.
 */
namespace ServiceLinkFixture
{
	inline FEntityInstanceId PlaceStand(URoadNetwork& Net, UEntityDefinition& Stand,
		const FVector2D& At, double Heading)
	{
		return Net.PlaceEntity(&Stand, Stand.Anchors, At, Heading,
			Stand.DesignAircraft != nullptr ? Stand.DesignAircraft->Footprint.Wingspan : 0.0,
			Stand.PoseRole, Stand.Trucks);
	}
}

#endif
