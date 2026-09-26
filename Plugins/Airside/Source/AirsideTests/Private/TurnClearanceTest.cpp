#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Content/AirsideSettings.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

// Spec 2026-09-23 §6: a turn's radius and its clearance to the pavement are MEASURED by the
// builder, on the samples the follower walks, so route search can ask whether a vehicle's
// swept path fits without re-deriving any geometry.

namespace TurnClearance
{
	/** A T: hub at the origin, arms west, east and north, all FROM the hub. */
	URoadNetwork* Tee(URoadProfile* Profile)
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		const FRoadNodeId Hub = Net->AddNode(FVector2D(0.0, 0.0));
		Net->AddStraightSegment(Hub, Net->AddNode(FVector2D(-30000.0, 0.0)), Profile);
		Net->AddStraightSegment(Hub, Net->AddNode(FVector2D(30000.0, 0.0)), Profile);
		Net->AddStraightSegment(Hub, Net->AddNode(FVector2D(0.0, 30000.0)), Profile);
		TestGraph::Derive(*Net);
		return Net;
	}

	/** The junction's own turns: no DerivedFrom and a control near the hub (not a balloon). */
	template <typename F>
	void ForEachTurn(const URoadNetwork& Net, F&& Visit)
	{
		for (const FGuidelineEdge& Edge : Net.GetGuidelineEdges())
		{
			if (Edge.bAlive && Edge.bDerived && !Edge.DerivedFrom.IsSet() && Edge.Control.Size() < 10000.0)
			{
				Visit(Edge);
			}
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTurnClearanceTest, "Airside.Build.TurnClearance",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTurnClearanceTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Narrow = TurnClearance::Tee(URoadProfile::MakeServiceRoadTransient(300.0, 60.0, 800.0));

	int32 Turns = 0, Curved = 0;
	TurnClearance::ForEachTurn(*Narrow, [&](const FGuidelineEdge& Turn)
	{
		++Turns;
		const FVector2D A = Narrow->GetGuidelineNode(Turn.A)->Position;
		const FVector2D B = Narrow->GetGuidelineNode(Turn.B)->Position;
		if (FMath::IsNearlyZero(FVector2D::CrossProduct(Turn.Control - A, B - Turn.Control), 1.0))
		{
			// Straight through the T: no inside to a straight line, and the lane width gates it.
			TestEqual(TEXT("a straight-through turn carries no radius"), Turn.MinRadius, 0.0);
			return;
		}
		++Curved;
		TestTrue(TEXT("a turn carries the radius it actually delivers"), Turn.MinRadius > 0.0);
		// At the very least the lane the turn starts in is pavement, half a lane either side.
		TestTrue(TEXT("its inner clearance is measured and at least half a lane"), Turn.ClearInner >= 150.0 - 10.0);
		TestTrue(TEXT("and so is its outer"), Turn.ClearOuter >= 150.0 - 10.0);
	});
	TestEqual(TEXT("six turns at a two-lane T"), Turns, 6);
	TestEqual(TEXT("four of them curved - two left, two right"), Curved, 4);

	int32 Lanes = 0, Balloons = 0;
	for (const FGuidelineEdge& Edge : Narrow->GetGuidelineEdges())
	{
		if (!Edge.bAlive) { continue; }
		if (Edge.DerivedFrom.IsSet())
		{
			++Lanes;
			TestEqual(TEXT("a straight lane carries no radius"), Edge.MinRadius, 0.0);
			TestEqual(TEXT("and no clearance - the lane width gates it"), Edge.ClearInner, -1.0);
		}
		else if (Edge.Control.Size() >= 10000.0)
		{
			++Balloons;
			TestTrue(TEXT("a dead-end balloon carries its radius, so the lock is checked"), Edge.MinRadius > 0.0);
			TestEqual(TEXT("but no clearance: it lies over grass by ruling"), Edge.ClearInner, -1.0);
		}
	}
	TestTrue(TEXT("lanes and balloons were both seen"), Lanes > 0 && Balloons > 0);

	// A BIGGER CORNER WIDENS THE TURN ITSELF, which is what shrinks a vehicle's sweep: the
	// tightest curved turn must deliver a larger radius. NOT more clearance inside it: this
	// test first asserted that, and on 2026-09-24 the summed inner clearance at a 25 m fillet
	// was not larger than at 8 m - the line follows the fillet out rather than away from it.
	auto TightestCurve = [](const URoadNetwork& Net)
	{
		double Tightest = TNumericLimits<double>::Max();
		TurnClearance::ForEachTurn(Net, [&](const FGuidelineEdge& Turn)
		{
			if (Turn.MinRadius > 0.0) { Tightest = FMath::Min(Tightest, Turn.MinRadius); }
		});
		return Tightest;
	};
	URoadNetwork* Generous = TurnClearance::Tee(URoadProfile::MakeServiceRoadTransient(300.0, 60.0, 2500.0));
	TestTrue(TEXT("a wider fillet delivers a wider tightest turn"), TightestCurve(*Generous) > TightestCurve(*Narrow));
	return true;
}

#endif
