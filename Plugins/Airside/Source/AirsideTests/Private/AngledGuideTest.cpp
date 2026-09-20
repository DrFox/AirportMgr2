#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Solve/GuideArbiter.h"
#include "Tool/RoadEditTarget.h"
#include "Tool/SnapGuideChain.h"
#include "Tool/SnapGuideSettings.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** Only AngledFrom, and only the column named. Everything else is silenced so a winner can
	 *  have come from nowhere but the source under test. */
	FSnapGuideSettings OnlyAngled(bool bRoad, bool bRunway)
	{
		FSnapGuideSettings Settings;
		Settings.bExtending = false;
		Settings.bLevelWith = false;
		Settings.bParallel = false;
		Settings.bCollinear = false;
		Settings.bAngledFrom = true;
		Settings.bMatchingGap = false;
		Settings.bRoad = bRoad;
		Settings.bRunway = bRunway;
		Settings.bApron = false;
		Settings.bStand = false;
		Settings.bWorld = false;
		return Settings;
	}
}

/**
 * A ROAD'S END THROWS A SPOKE, AND THE TWO ENDS THROW DIFFERENT ONES.
 *
 * The 2026-09-20 sketch (samples/suggestion.png): "45 degrees to other road", drawn from the
 * drag to that road's near END. Nothing offered it before - FParallelGuideSource squares to a
 * road through the DRAG'S origin, never through the road's own end.
 *
 * BOTH ENDS MATTER, which is what the second leg pins: spokes off the two ends are parallel
 * lines a segment apart, so a source proposing only one would be choosing for the player and
 * every single-ended test would still pass.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAngledGuideRadiatesFromEachEndTest,
	"Airside.Tool.AngledGuideRadiatesFromEachEnd",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAngledGuideRadiatesFromEachEndTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a network actor"), Actor)) { return false; }

	// An east-west taxiway. Its ends are at x = -10000 and x = +10000, y = 0.
	IRoadEditTarget* Target = Actor;
	const int32 West = Target->PlaceNode(FVector2D(-10000.0, 0.0));
	const int32 East = Target->PlaceNode(FVector2D(10000.0, 0.0));
	Target->ConnectNodes(West, East, ERoadKind::Taxiway, INDEX_NONE);
	if (!TestTrue(TEXT("the network exists"), Actor->Network != nullptr)) { return false; }

	FGuideAnchor Anchor;
	Anchor.Origin = FVector2D(12000.0, 2000.0);
	const FSnapGuideSettings Settings = OnlyAngled(true, false);
	const FSnapGuideChain Chain;

	// ON THE 45 DEGREE SPOKE OUT OF THE EAST END: (10000,0) + t*(1,1)/sqrt(2). At (14000,4000)
	// the cursor is exactly on it, and 4000 uu clear of every other spoke this road throws -
	// well outside the 300 uu corridor, so only one candidate can be eligible.
	const SnapGuide::FResult East45 = Chain.Resolve(
		*Actor->Network, Anchor, FVector2D(14000.0, 4000.0), SnapGuide::FResult(), Settings);

	if (!TestTrue(TEXT("the east end throws a 45 degree spoke"), East45.bActive)) { return false; }
	TestEqual(TEXT("it is an AngledFrom guide"),
		static_cast<int32>(East45.Winners[0].Relation),
		static_cast<int32>(SnapGuide::ERelation::AngledFrom));
	TestEqual(TEXT("against the road"),
		static_cast<int32>(East45.Winners[0].Reference),
		static_cast<int32>(SnapGuide::EReference::Road));

	// PERPENDICULAR, NOT ANGULAR, and that is the whole of what "through the node" means: the
	// guide is judged on where the cursor ended up, not on which way the drag set off.
	TestEqual(TEXT("judged by where the cursor ended up"),
		static_cast<int32>(East45.Winners[0].Fit),
		static_cast<int32>(SnapGuide::EFit::Perpendicular));

	// THE LINE IS DRAWN TO THE END IT RADIATES FROM. Two ends throw parallel spokes and the
	// label cannot tell them apart - only the drawn line can, which is what the sketch shows.
	TestTrue(*FString::Printf(TEXT("drawn to the east end, got (%.0f, %.0f)"),
			East45.Winners[0].ReferenceAt.X, East45.Winners[0].ReferenceAt.Y),
		East45.Winners[0].ReferenceAt.Equals(FVector2D(10000.0, 0.0), 1.0));
	TestTrue(*FString::Printf(TEXT("and says the angle, got '%s'"), *East45.Winners[0].Description),
		East45.Winners[0].Description.Contains(TEXT("45 degrees")));

	// THE WEST END THROWS ITS OWN. Same bearing, a segment's length away - a source that walked
	// one end only would fail here and nowhere else.
	const SnapGuide::FResult West45 = Chain.Resolve(
		*Actor->Network, Anchor, FVector2D(-6000.0, 4000.0), SnapGuide::FResult(), Settings);

	if (!TestTrue(TEXT("the west end throws one too"), West45.bActive)) { return false; }
	TestTrue(*FString::Printf(TEXT("drawn to the west end, got (%.0f, %.0f)"),
			West45.Winners[0].ReferenceAt.X, West45.Winners[0].ReferenceAt.Y),
		West45.Winners[0].ReferenceAt.Equals(FVector2D(-10000.0, 0.0), 1.0));

	// CONTROL LEG: step off the spoke and the guide goes. Without this the assertions above
	// would pass on a source that proposed a line through every point on the map.
	const SnapGuide::FResult Off = Chain.Resolve(
		*Actor->Network, Anchor, FVector2D(14000.0, 4600.0), SnapGuide::FResult(), Settings);
	TestFalse(TEXT("424 uu off the spoke is outside the corridor, so nothing holds"), Off.bActive);

	return true;
}

/**
 * A THRESHOLD THROWS A SQUARE SPOKE, FROM ANYWHERE ON THE FIELD.
 *
 * 90 degrees is the member that nothing else could reach: FParallelGuideSource squares to a road
 * through the DRAG'S origin, so a stub leaving a threshold at right angles had no guide at all.
 *
 * UNBOUNDED, like every source in the Runway column - the drag here is 500 m clear of the strip,
 * where every reach-limited source has given up.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAngledGuideSquaresFromAThresholdTest,
	"Airside.Tool.AngledGuideSquaresFromAThreshold",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAngledGuideSquaresFromAThresholdTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a network actor"), Actor)) { return false; }

	if (!TestTrue(TEXT("the runway is laid"),
		TestGuide::LayRunway(Actor, FVector2D(-40000.0, 0.0), FVector2D(40000.0, 0.0))))
	{
		return false;
	}
	if (!TestTrue(TEXT("and the network exists"), Actor->Network != nullptr)) { return false; }

	FGuideAnchor Anchor;
	Anchor.Origin = FVector2D(40000.0, 60000.0);
	const FSnapGuideChain Chain;

	// ON THE 90 DEGREE SPOKE OUT OF THE EASTERN THRESHOLD - the line x = 40000, straight off the
	// end of the strip. 500 m north of it, far outside SearchRadiusUu.
	const SnapGuide::FResult Square = Chain.Resolve(
		*Actor->Network, Anchor, FVector2D(40000.0, 50000.0), SnapGuide::FResult(),
		OnlyAngled(false, true));

	if (!TestTrue(TEXT("the threshold squares from across the field"), Square.bActive))
	{
		return false;
	}
	TestEqual(TEXT("against the Runway column, not the Road one"),
		static_cast<int32>(Square.Winners[0].Reference),
		static_cast<int32>(SnapGuide::EReference::Runway));
	TestTrue(*FString::Printf(TEXT("named by its designators, got '%s'"),
			*Square.Winners[0].Description),
		Square.Winners[0].Description.Contains(TEXT("90 degrees"))
			&& Square.Winners[0].Description.Contains(TEXT("runway 18/36")));

	// CONTROL LEG: the Road column cannot reach a runway. Switching the columns over must leave
	// the same drag with nothing, which is what the partition promises.
	const SnapGuide::FResult AsRoad = Chain.Resolve(
		*Actor->Network, Anchor, FVector2D(40000.0, 50000.0), SnapGuide::FResult(),
		OnlyAngled(true, false));
	TestFalse(TEXT("with only the Road column on, a runway offers nothing"), AsRoad.bActive);

	return true;
}

#endif
