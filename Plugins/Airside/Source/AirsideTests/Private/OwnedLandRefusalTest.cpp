#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/LandGrid.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadEditFacade.h"
#include "Present/RoadNetworkActor.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** The R3 start, placed so column 0 spans X [-30000, 30000) and rows 3-4 span Y [-60000, 60000). Refusal-prefixed:
	 *  the test module is a unity build. */
	FLandGrid RefusalStart()
	{
		const FIntPoint Start[] = { FIntPoint(0, 3), FIntPoint(0, 4) };
		return FLandGrid::Make(FVector2D(-30000.0, -240000.0), 60000.0, 8, 8, Start);
	}

	TArray<FVector2D> RefusalSquare(FVector2D Min, double Side)
	{
		return { Min, Min + FVector2D(Side, 0.0), Min + FVector2D(Side, Side), Min + FVector2D(0.0, Side) };
	}
}

/**
 * EVERY BUILD ASKS THE LAND (land purchase spec R7): the preview's Why* says "Outside your land" and the commit refuses,
 * for each kind of thing that lays ground. One kind left out is a road into the void. The east cut at X = 30000 is the
 * edge every case below crosses.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOwnedLandEveryBuildAsksTheLand, "Airside.Present.OwnedLand.EveryBuildAsksTheLand",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOwnedLandEveryBuildAsksTheLand::RunTest(const FString&)
{
	FAirsideTestWorld Fixture;
	ARoadNetworkActor* Actor = Fixture.Actor;
	Actor->GetEditFacade()->AuthorOwnedLand(RefusalStart());
	const FString Outside = FLandGrid::OutsideText;

	// ROADS AND TAXIWAYS: inside builds; across the cut is refused before and at the click.
	const int32 A = Actor->PlaceNode(FVector2D(0.0, -50000.0));
	const int32 B = Actor->PlaceNode(FVector2D(0.0, 50000.0));
	TestTrue(TEXT("a taxiway on owned land builds"), Actor->ConnectNodes(A, B, ERoadKind::Taxiway));
	const int32 Segments = Actor->Network->GetSegments().Num();
	FRoadSnapResult Past;
	Past.Position = FVector2D(29500.0, 0.0);   // the centreline stays inside the cut at X = 30000; the shoulder crosses it
	TestEqual(TEXT("a taxiway whose shoulder crosses the cut is refused in the preview"),
		Actor->WhySegmentRefused(A, Past, ERoadKind::Taxiway, 0), Outside);
	TestEqual(TEXT("a node in the void is refused"), Actor->PlaceNode(FVector2D(90000.0, 0.0)), int32(INDEX_NONE));
	TestEqual(TEXT("and nothing was laid"), Actor->Network->GetSegments().Num(), Segments);

	// RUNWAYS: the strip past the north cut at Y = 60000.
	Actor->MinimumRunwayLength = 100.0;
	URoadProfile* Runway = TestProfiles::Runway();
	TestEqual(TEXT("a runway past the edge is refused in the preview"),
		Actor->WhyRunwayRefused(FVector2D(0.0, -50000.0), FVector2D(0.0, 150000.0), Runway), Outside);
	TestFalse(TEXT("and at the click"), Actor->PlaceRunway(FVector2D(0.0, -50000.0), FVector2D(0.0, 150000.0), Runway));
	TestTrue(TEXT("a runway inside builds"), Actor->WhyRunwayRefused(FVector2D(0.0, -50000.0), FVector2D(0.0, 50000.0), Runway).IsEmpty());

	// APRONS: a square straddling the east cut.
	const TArray<FVector2D> Straddle = RefusalSquare(FVector2D(25000.0, 10000.0), 10000.0);
	TestEqual(TEXT("an apron over the cut is refused in the preview"), Actor->WhyApronRefused(Straddle), Outside);
	TestEqual(TEXT("and at the click"), Actor->AddApron(Straddle), int32(INDEX_NONE));
	const TArray<FVector2D> Inside = RefusalSquare(FVector2D(5000.0, 10000.0), 10000.0);
	TestTrue(TEXT("an apron inside is not refused for land"), Actor->WhyApronRefused(Inside).IsEmpty());

	// PLOTS AND STANDS: the same straddling rectangle.
	TestEqual(TEXT("a plot over the cut says why"), Actor->WhyPlotRefused(Straddle), Outside);
	TestEqual(TEXT("a stand site over the cut says why"), Actor->WhyStandSiteRefused(Straddle), Outside);
	return true;
}

/** A MAP WITH NO LAND OWNS EVERYTHING: none of the above refuses on an airport whose grid was never authored. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOwnedLandNoGridRefusesNothing, "Airside.Present.OwnedLand.NoGridRefusesNothing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOwnedLandNoGridRefusesNothing::RunTest(const FString&)
{
	FAirsideTestWorld Fixture;
	ARoadNetworkActor* Actor = Fixture.Actor;
	TestNotEqual(TEXT("a node anywhere is placed"), Actor->PlaceNode(FVector2D(9.0e5, -9.0e5)), int32(INDEX_NONE));
	TestTrue(TEXT("an apron anywhere is not refused for land"),
		Actor->WhyApronRefused(RefusalSquare(FVector2D(9.0e5, 9.0e5), 10000.0)).IsEmpty());
	TestTrue(TEXT("a runway anywhere is not refused for land"),
		Actor->WhyRunwayRefused(FVector2D(9.0e5, 0.0), FVector2D(9.0e5, 2.0e5), TestProfiles::Runway()).IsEmpty());
	return true;
}

#endif
