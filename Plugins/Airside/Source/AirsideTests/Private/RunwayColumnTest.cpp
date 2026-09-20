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

/**
 * THE EXTENDED CENTRELINE SURVIVES THE PARTITION.
 *
 * FCollinearGuideSource used to offer "in line with runway 18/36" by accident - it walked every
 * segment, runways included - so the line existed but answered to the Collinear toggle rather
 * than the Runway one. Skipping runways there without moving it here would have DELETED it,
 * which is the loss this test exists to make loud.
 *
 * UNBOUNDED, unlike Collinear's own: a runway's extended centreline is the approach path, and
 * it is meaningful from anywhere on the field.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRunwayGuideOffersItsOwnLineTest,
	"Airside.Tool.RunwayGuideOffersItsOwnLine",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayGuideOffersItsOwnLineTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a network actor"), Actor)) { return false; }

	// A runway along +X through Y=0. NORTH IS +X in this project, so this strip is 18/36.
	if (!TestTrue(TEXT("the runway is laid"),
		TestGuide::LayRunway(Actor, FVector2D(-40000.0, 0.0), FVector2D(40000.0, 0.0))))
	{
		return false;
	}
	if (!TestTrue(TEXT("and the network exists"), Actor->Network != nullptr)) { return false; }

	// THE ORIGIN IS 500 m OUT, far beyond SearchRadiusUu, where every reach-limited source has
	// given up. The CURSOR is on the runway's own line - which is what a perpendicular fit
	// measures, however far along it the drag has gone.
	FGuideAnchor Anchor;
	Anchor.Origin = FVector2D(60000.0, 50000.0);
	const FVector2D Cursor(60000.0, 0.0);

	FSnapGuideSettings Settings;
	Settings.bExtending = false;
	Settings.bLevelWith = false;
	Settings.bParallel = false;
	Settings.bCollinear = true;
	Settings.bMatchingGap = false;
	Settings.bTaxiway = false;
	Settings.bServiceRoad = false;
	Settings.bRunway = true;
	Settings.bApron = false;
	Settings.bStand = false;
	Settings.bWorld = false;

	const FSnapGuideChain Chain;
	const SnapGuide::FResult Result = Chain.Resolve(
		*Actor->Network, Anchor, Cursor, SnapGuide::FResult(), Settings);

	if (!TestTrue(TEXT("the runway's own line reaches the whole field"), Result.bActive))
	{
		return false;
	}
	const SnapGuide::FCandidate* Winner = Result.Of(SnapGuide::EFit::Perpendicular);
	if (!TestNotNull(TEXT("and it is a perpendicular fit, not an angular one"), Winner))
	{
		return false;
	}
	TestEqual(TEXT("it is a Collinear guide"),
		static_cast<int32>(Winner->Relation),
		static_cast<int32>(SnapGuide::ERelation::Collinear));
	TestEqual(TEXT("against the Runway column, not the Road one"),
		static_cast<int32>(Winner->Reference),
		static_cast<int32>(SnapGuide::EReference::Runway));

	// CONTROL LEG: switch the Runway column off and the same drag is offered nothing. Without
	// this, a Resolve answering from some other source would pass the assertions above.
	Settings.bRunway = false;
	const SnapGuide::FResult Off = Chain.Resolve(
		*Actor->Network, Anchor, Cursor, SnapGuide::FResult(), Settings);
	TestFalse(TEXT("with the Runway column off, nothing answers"), Off.bActive);

	return true;
}

/**
 * OFFSET AND PARALLEL MUST NAME ONE ROAD BETWEEN THEM.
 *
 * FOffsetGuideSource's header records that it deliberately picks the same nearest road
 * FParallelGuideSource does, so "parallel to the taxiway" and "the same gap as its neighbour"
 * compose into one answer rather than two unrelated ones. Once Parallel excludes runways,
 * Offset must too - otherwise the two disagree about which road they are talking about, and
 * nothing else in the suite would say so.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOffsetAndParallelNameOneRoadTest,
	"Airside.Tool.OffsetAndParallelNameOneRoad",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOffsetAndParallelNameOneRoadTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a network actor"), Actor)) { return false; }

	// A RUNWAY AND TWO TAXIWAYS, ALL PARALLEL. The runway is laid first so it holds the lowest
	// segment index - a source that picked by index rather than by distance would choose it,
	// and this test would catch that too.
	if (!TestTrue(TEXT("the runway is laid"),
		TestGuide::LayRunway(Actor, FVector2D(-40000.0, 12000.0), FVector2D(40000.0, 12000.0))))
	{
		return false;
	}

	IRoadEditTarget* Target = Actor;
	const int32 NearWest = Target->PlaceNode(FVector2D(-20000.0, 0.0));
	const int32 NearEast = Target->PlaceNode(FVector2D(20000.0, 0.0));
	Target->ConnectNodes(NearWest, NearEast, ERoadKind::Taxiway, INDEX_NONE);

	// The neighbour, 4000 uu south of the near taxiway - the gap Offset has to copy.
	const int32 FarWest = Target->PlaceNode(FVector2D(-20000.0, -4000.0));
	const int32 FarEast = Target->PlaceNode(FVector2D(20000.0, -4000.0));
	Target->ConnectNodes(FarWest, FarEast, ERoadKind::Taxiway, INDEX_NONE);
	if (!TestTrue(TEXT("the network exists"), Actor->Network != nullptr)) { return false; }

	// THE ORIGIN IS NEAREST THE FIRST TAXIWAY. Each relation is asked SEPARATELY, and the
	// point is that both name the same road.
	//
	// NOT BOTH AT ONCE, and the reason is the arbiter rather than the sources: Parallel's line
	// runs along the road THROUGH the origin, and MatchingGap's runs along it one gap away.
	// They are parallel, so SnapGuide::Arbitrate's pull guard drops the perpendicular one -
	// "a line parallel to the angular winner pins nothing it has not already pinned". The
	// first draft of this test asked for two winners and got one for exactly that reason.
	FGuideAnchor Anchor;
	Anchor.Origin = FVector2D(0.0, 600.0);
	const FSnapGuideChain Chain;

	FSnapGuideSettings Settings;
	Settings.bExtending = false;
	Settings.bLevelWith = false;
	Settings.bCollinear = false;
	Settings.bTaxiway = true;
	Settings.bServiceRoad = true;

	// THE RUNWAY COLUMN IS ON THROUGHOUT, so a source that had not been partitioned is free to
	// name it - a runway is a segment like any other until IsRunwaySegment is asked.
	Settings.bRunway = true;
	Settings.bApron = false;
	Settings.bStand = false;
	Settings.bWorld = false;

	// PARALLEL: the cursor's BEARING from the origin is what an angular fit measures, so it
	// sits nearly due east of it - 6 degrees, inside the 7 degree tolerance.
	Settings.bParallel = true;
	Settings.bMatchingGap = false;
	const SnapGuide::FResult Along = Chain.Resolve(
		*Actor->Network, Anchor, FVector2D(30000.0, 3800.0), SnapGuide::FResult(), Settings);
	if (!TestTrue(TEXT("the nearest road offers its direction"), Along.bActive))
	{
		return false;
	}
	TestEqual(TEXT("and it is the taxiway, not the runway"),
		static_cast<int32>(Along.Winners[0].Reference),
		static_cast<int32>(SnapGuide::EReference::Taxiway));

	// MATCHING GAP: the cursor is on the line one gap NORTH of the reference - away from the
	// neighbour, which is the only side this source ever proposes.
	Settings.bParallel = false;
	Settings.bMatchingGap = true;
	const SnapGuide::FResult Gap = Chain.Resolve(
		*Actor->Network, Anchor, FVector2D(8000.0, 4000.0), SnapGuide::FResult(), Settings);
	if (!TestTrue(TEXT("and the same road offers its neighbour's gap"), Gap.bActive))
	{
		return false;
	}
	TestEqual(TEXT("named as the taxiway too, so the two describe one road between them"),
		static_cast<int32>(Gap.Winners[0].Reference),
		static_cast<int32>(SnapGuide::EReference::Taxiway));

	return true;
}

#endif
