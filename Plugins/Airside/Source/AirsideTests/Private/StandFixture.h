#pragma once

#include "CoreMinimal.h"
#include "Entities/AircraftType.h"
#include "Entities/EntityDefinition.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Solve/IcaoCode.h"
#include "Solve/StandBox.h"

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

	/**
	 * The FAR edge of a stand template, local x: EntranceSetback behind the stop mark plus the
	 * depth it measured. Every service contact sits just inside it since 2026-09-26 (user:
	 * service vehicles enter only by the edge opposite the taxiway).
	 */
	inline double FarEdgeX(const UEntityDefinition& Stand)
	{
		const TOptional<EIcaoCode> Code = IcaoCode::Parse(
			IcaoCode::LetterForStandSize(Stand.RequiredExtent.X, Stand.RequiredExtent.Y));
		check(Code.IsSet());
		return -StandBox::EntranceSetback(*Code, IcaoCode::FloorEnvelopeForLetter(*Code))
			+ Stand.RequiredExtent.Y;
	}

	/**
	 * Where a GSE road serving the shipping stand runs: 420 uu beyond its FAR edge. The fixtures
	 * that typed x = -5400 - 420 behind the old aft edge - read it here instead, so moving the
	 * edge again moves them with it. SHARED, so ServiceLinkTest and FuelDepotAnchorTest cannot
	 * lay their roads by two different rules.
	 */
	inline double FarRoadX()
	{
		return FarEdgeX(*UEntityDefinition::MakeStandTransient()) + 420.0;
	}
}

#endif
