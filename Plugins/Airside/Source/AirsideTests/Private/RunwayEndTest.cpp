#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Model/RunwayFacts.h"
#include "Model/RunwayQuery.h"
#include "Profiles/RoadProfile.h"
#include "Testing/AirsideTestGraph.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRunwayEndTest,
	"Airside.Model.RunwayEnd",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayEndTest::RunTest(const FString& Parameters)
{
	// A 1000 uu strip on a diagonal, so X-only arithmetic could not pass by accident.
	FRunwayEnd End;
	End.Threshold = FVector2D(1000.0, 2000.0);
	End.Direction = FVector2D(0.6, 0.8);   // unit length: 3-4-5 triangle scaled
	End.Length = 1000.0;
	End.Seed.Index = 7;
	End.Seed.Generation = 3;

	// 1. FarEnd is the threshold walked the whole length along the direction.
	{
		const FVector2D Expected = End.Threshold + End.Direction * End.Length;
		TestEqual(TEXT("FarEnd is Threshold + Direction * Length"), End.FarEnd(), Expected);
	}

	// 2. OffsetOf projects a point onto the strip's own axis, signed - negative short of
	//    the threshold, exactly what a query on final approach needs.
	{
		const FVector2D OnAxisPastThreshold = End.Threshold + End.Direction * 250.0;
		TestEqual(TEXT("a point on-axis, ahead, offsets positively"),
			End.OffsetOf(OnAxisPastThreshold), 250.0, 1.0e-6);

		const FVector2D OnAxisBeforeThreshold = End.Threshold - End.Direction * 400.0;
		TestEqual(TEXT("a point behind the threshold offsets negatively"),
			End.OffsetOf(OnAxisBeforeThreshold), -400.0, 1.0e-6);

		// Off-axis: only the component ALONG Direction counts, never the lateral one -
		// the same rule IsPointOnRunway uses for its own Distance.
		const FVector2D Lateral(-End.Direction.Y, End.Direction.X);   // perpendicular, unit length
		const FVector2D OffAxis = End.Threshold + End.Direction * 100.0 + Lateral * 5000.0;
		TestEqual(TEXT("a lateral offset does not change the along-strip distance"),
			End.OffsetOf(OffAxis), 100.0, 1.0e-6);
	}

	// 3. Reversed describes the SAME strip from its other end: Threshold becomes FarEnd,
	//    Direction flips, Length is unchanged, and Seed still names the one segment this
	//    was measured from - Reversed renames the end, not the segment.
	{
		const FRunwayEnd Back = End.Reversed();
		TestEqual(TEXT("Reversed threshold is the original FarEnd"), Back.Threshold, End.FarEnd());
		TestEqual(TEXT("Reversed direction points the other way"), Back.Direction, -End.Direction);
		TestEqual(TEXT("length is unchanged"), Back.Length, End.Length);
		TestEqual(TEXT("seed is unchanged - it names the segment, not the end"), Back.Seed, End.Seed);

		// Reversing twice returns to the original end - the operation is its own inverse,
		// which is what "the same strip, the other way round" has to mean.
		const FRunwayEnd ThereAndBack = Back.Reversed();
		TestEqual(TEXT("reversed twice is the threshold again"), ThereAndBack.Threshold, End.Threshold);
		TestEqual(TEXT("and the direction again"), ThereAndBack.Direction, End.Direction);
	}

	return true;
}

/**
 * A QUERIED END'S DIRECTION IS A UNIT VECTOR, and so is its Reversed() (issue #444). FLandingRun::Start and FTakeoffRun::Start
 * re-normalised the end they were handed; #444 dropped both copies on FRunwayEnd's own contract ("Unit vector from
 * Threshold toward the far end"), which RunwayQuery writes as Along / Length. This holds the producers to it: a strip laid
 * on a 3-4-5 diagonal of 50 000 uu, so a raw, unnormalised Along could not pass by accident.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRunwayEndQueriedDirectionIsUnitTest,
	"Airside.Model.RunwayQueryEndIsUnit",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayEndQueriedDirectionIsUnitTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FRoadNodeId A = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId B = Net->AddNode(FVector2D(30000.0, 40000.0));
	Net->AddStraightSegment(A, B, TestProfiles::Runway());

	// FVector2D has no IsNormalized; unit length to a tolerance far tighter than any figure the poses are read at.
	auto IsUnit = [](const FVector2D& V) { return FMath::IsNearlyEqual(V.SizeSquared(), 1.0, 1.0e-9); };
	FRunwayEnd End;
	if (!TestTrue(TEXT("the strip is found from beside its threshold"),
		RunwayQuery::RunwayExtentAt(*Net, FVector2D(300.0, 400.0), End))) { return false; }
	TestTrue(FString::Printf(TEXT("its Direction is unit length (%.9f)"), End.Direction.Size()), IsUnit(End.Direction));
	TestTrue(TEXT("and so is its Reversed()'s"), IsUnit(End.Reversed().Direction));
	TestTrue(TEXT("and PointAt walks it in uu: its far end is the strip's length away"),
		FMath::IsNearlyEqual(FVector2D::Distance(End.Threshold, End.PointAt(End.Length)), End.Length, 1.0));

	const TArray<FRunwayEnd> Departures = RunwayQuery::DepartureRunways(*Net);
	if (!TestEqual(TEXT("one runway takes departures"), Departures.Num(), 1)) { return false; }
	TestTrue(TEXT("the departure enumeration's end is unit too"), IsUnit(Departures[0].Direction));
	TestTrue(TEXT("and its Reversed()'s"), IsUnit(Departures[0].Reversed().Direction));
	return true;
}

#endif
