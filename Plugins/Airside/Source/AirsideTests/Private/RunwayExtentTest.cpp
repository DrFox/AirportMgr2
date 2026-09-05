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

	// 4. NEAREST-THRESHOLD IS THE SAME SEARCH WITHOUT THE PROXIMITY TEST, and that is the
	//    whole difference between the two questions.
	//
	//    "Am I on a runway" must say no across the airport - point 2 above is that rule, and
	//    it exists because without it every dispatched route armed a departure. "Which runway
	//    would I land on" is asked of a click that is deliberately nowhere near one, so the
	//    same refusal would make an arrival impossible to order.
	{
		TestFalse(TEXT("the far taxiway is not ON a runway"),
			Net->RunwayExtentAt(FarAway, OutThreshold, OutDirection, OutLength));

		if (TestTrue(TEXT("but it still has a nearest runway to land on"),
			Net->NearestRunwayThreshold(FarAway, OutThreshold, OutDirection, OutLength)))
		{
			TestEqual(TEXT("and it is the threshold nearer the query"), OutThreshold, Threshold);
			TestTrue(TEXT("landing away from that threshold"), OutDirection.X > 0.99);
			TestEqual(TEXT("down the full length"), OutLength, 100000.0);
		}
	}

	// 5. THE EXITS. A runway is continuous through junctions, so anything joining it already
	//    puts a guideline node on the centreline - which means the exits can be FOUND rather
	//    than declared, and a taxiway drawn onto the runway later is an exit immediately.
	{
		constexpr double HalfWidth = 2250.0;

		// On the centreline, at 10 km, 40 km and 70 km - ADDED OUT OF ORDER ON PURPOSE.
		//
		// The guideline node array is in creation order, so adding them near-to-far would
		// make the ordering assertion below pass on an implementation that did no sorting at
		// all. This project has shipped that shape of assertion before; see the note in
		// CLAUDE.md about ones that merely name a contract.
		const FGuidelineNodeId Late = Net->AddGuidelineNode(FVector2D(70000.0, 0.0), false);
		const FGuidelineNodeId Early = Net->AddGuidelineNode(FVector2D(10000.0, 0.0), false);
		const FGuidelineNodeId Middle = Net->AddGuidelineNode(FVector2D(40000.0, 0.0), false);

		// On a PARALLEL taxiway abeam the middle one. The reason the lateral bound exists:
		// every airport has a taxiway running alongside its runway, and taking a node on it
		// for an exit would turn an aircraft off into the grass.
		Net->AddGuidelineNode(FVector2D(40000.0, HalfWidth * 4.0), false);

		// Beyond the far threshold, which is not on the runway at all.
		Net->AddGuidelineNode(FVector2D(130000.0, 0.0), false);

		const TArray<FGuidelineNodeId> Exits =
			Net->RunwayExitNodes(Threshold, FVector2D(1.0, 0.0), 100000.0, HalfWidth, 0.0);

		TestEqual(TEXT("three nodes lie on the runway, and only three"), Exits.Num(), 3);
		if (Exits.Num() == 3)
		{
			// ORDERED BY DISTANCE, which is what makes "the first exit I can take" a
			// question the caller can answer by taking the first element. The guideline node
			// array is in creation order, which has nothing to do with geometry.
			TestEqual(TEXT("nearest the threshold first"), Exits[0], Early);
			TestEqual(TEXT("then the middle one"), Exits[1], Middle);
			TestEqual(TEXT("then the far one"), Exits[2], Late);
		}

		// 6. THE MEASUREMENT AN ARRIVAL DEPENDS ON. An exit before the aircraft could have
		//    slowed to taxi speed is not an exit it can take, and offering one would turn a
		//    landing aircraft off the runway at approach speed.
		const TArray<FGuidelineNodeId> Usable =
			Net->RunwayExitNodes(Threshold, FVector2D(1.0, 0.0), 100000.0, HalfWidth, 30000.0);

		TestEqual(TEXT("an exit inside the landing distance is not offered"), Usable.Num(), 2);
		if (Usable.Num() == 2)
		{
			TestEqual(TEXT("the first usable one is the middle exit"), Usable[0], Middle);
		}
	}

	// 7. A NETWORK WITH NO RUNWAY never reports one, whatever is asked of it - the state
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
