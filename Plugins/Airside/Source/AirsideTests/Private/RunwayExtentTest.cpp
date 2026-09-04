#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// Prefixed against the UNITY build - these test files share one translation unit.

	/** A 45 m runway lying east-west, plus a taxiway far away from it. */
	URoadNetwork* RunwayExtentFixture(FVector2D& OutThresholdWest, FVector2D& OutFarTaxiway)
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());

		URoadProfile* Runway = URoadProfile::MakeTransient(4500.0, 1500.0, 450.0);
		Runway->bContinuousThroughJunctions = true;

		URoadProfile* Taxiway = URoadProfile::MakeTransient(2300.0, 1500.0, 230.0);

		OutThresholdWest = FVector2D(0.0, 0.0);
		const FRoadNodeId West = Net->AddNode(OutThresholdWest);
		const FRoadNodeId East = Net->AddNode(FVector2D(100000.0, 0.0));
		Net->AddStraightSegment(West, East, Runway);

		// A taxiway a long way off - a stand on the other side of the airport.
		OutFarTaxiway = FVector2D(-60000.0, 40000.0);
		const FRoadNodeId A = Net->AddNode(OutFarTaxiway);
		const FRoadNodeId B = Net->AddNode(OutFarTaxiway + FVector2D(5000.0, 0.0));
		Net->AddStraightSegment(A, B, Taxiway);

		return Net;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRunwayExtentTest,
	"Airside.Model.RunwayExtent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayExtentTest::RunTest(const FString& Parameters)
{
	FVector2D Threshold;
	FVector2D FarAway;
	URoadNetwork* Net = RunwayExtentFixture(Threshold, FarAway);

	FVector2D OutThreshold;
	FVector2D OutDirection;
	double OutLength = 0.0;

	// 1. AT A THRESHOLD, it is a runway, and the departure runs the length of it.
	{
		const bool bFound = Net->RunwayExtentAt(Threshold, OutThreshold, OutDirection, OutLength);
		if (TestTrue(TEXT("a query at the threshold finds the runway"), bFound))
		{
			TestEqual(TEXT("and reports that threshold, not the far one"), OutThreshold, Threshold);
			TestTrue(TEXT("pointing down the runway"), OutDirection.X > 0.99);
			TestEqual(TEXT("for its whole length"), OutLength, 100000.0);
		}
	}

	// 2. THE MEASUREMENT. A query nowhere near a runway is NOT on one.
	//
	//    This returned true for any point at all as long as one runway existed anywhere:
	//    the search kept the nearest threshold and never asked how near. Every route
	//    dispatched after a runway was placed therefore armed a departure at it, and the
	//    aircraft - having taxied correctly to a stand on the far side of the airport -
	//    jumped to the runway and rolled. It read as the routing tool being broken.
	{
		const bool bFound = Net->RunwayExtentAt(FarAway, OutThreshold, OutDirection, OutLength);
		TestFalse(FString::Printf(
			TEXT("a taxiway %.0f uu from the runway is not a departure point"),
			FVector2D::Distance(FarAway, Threshold)),
			bFound);
	}

	// 3. THE EDGE OF THE RULE, so the tolerance is a stated distance rather than whatever
	//    the implementation happens to do. A runway admits a query within its own WIDTH of a
	//    threshold - derived from the runway, so a wider one is correspondingly forgiving and
	//    nothing here is a magic number.
	{
		const double Width = 4500.0;

		TestTrue(TEXT("just inside a runway's width of the threshold still counts"),
			Net->RunwayExtentAt(Threshold + FVector2D(0.0, Width * 0.9),
				OutThreshold, OutDirection, OutLength));

		TestFalse(TEXT("and well outside it does not"),
			Net->RunwayExtentAt(Threshold + FVector2D(0.0, Width * 3.0),
				OutThreshold, OutDirection, OutLength));
	}

	// 4. A NETWORK WITH NO RUNWAY never reports one, whatever is asked of it - the state
	//    every airport is in before a runway is laid.
	{
		URoadNetwork* Bare = NewObject<URoadNetwork>(GetTransientPackage());
		URoadProfile* Taxiway = URoadProfile::MakeTransient(2300.0, 1500.0, 230.0);
		const FRoadNodeId A = Bare->AddNode(FVector2D::ZeroVector);
		const FRoadNodeId B = Bare->AddNode(FVector2D(5000.0, 0.0));
		Bare->AddStraightSegment(A, B, Taxiway);

		TestFalse(TEXT("a network with no runway offers no departure"),
			Bare->RunwayExtentAt(FVector2D::ZeroVector, OutThreshold, OutDirection, OutLength));
	}

	return true;
}

#endif
