#include "CoreMinimal.h"
#include "Content/AirsideSettings.h"
#include "Misc/AutomationTest.h"
#include "Model/ArrivalPlanner.h"
#include "Model/RoadNetwork.h"
#include "Model/TrafficOccupancy.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRunwayChainTest,
	"Airside.Model.RunwayChain",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayChainTest::RunTest(const FString& Parameters)
{
	// A runway split at an exit into two segments, a taxiway off the exit, and a SECOND
	// runway elsewhere that must not be swept into the chain.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Runway = URoadProfile::MakeTransient(4500.0, 1500.0, 450.0);
	Runway->bContinuousThroughJunctions = true;
	URoadProfile* Taxiway = URoadProfile::MakeTransient(2300.0, 1500.0, 230.0);

	const FRoadNodeId T = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId E = Net->AddNode(FVector2D(60000.0, 0.0));
	const FRoadNodeId F = Net->AddNode(FVector2D(100000.0, 0.0));
	const FRoadSegmentId R1 = Net->AddStraightSegment(T, E, Runway);
	const FRoadSegmentId R2 = Net->AddStraightSegment(E, F, Runway);
	const FRoadNodeId X = Net->AddNode(FVector2D(60000.0, -20000.0));
	const FRoadSegmentId Tx = Net->AddStraightSegment(E, X, Taxiway);

	const FRoadNodeId T2 = Net->AddNode(FVector2D(0.0, 500000.0));
	const FRoadNodeId F2 = Net->AddNode(FVector2D(100000.0, 500000.0));
	const FRoadSegmentId Other = Net->AddStraightSegment(T2, F2, Runway);

	TestTrue(TEXT("a runway-profiled segment is a runway"), Net->IsRunwaySegment(R1));
	TestFalse(TEXT("a taxiway is not"), Net->IsRunwaySegment(Tx));

	const TArray<FRoadSegmentId> Chain = Net->RunwayChain(R2);
	TestEqual(TEXT("the chain crosses the exit split: two segments"), Chain.Num(), 2);
	TestTrue(TEXT("it contains both halves"), Chain.Contains(R1) && Chain.Contains(R2));
	TestFalse(TEXT("and not the other runway"), Chain.Contains(Other));
	TestFalse(TEXT("and not the taxiway"), Chain.Contains(Tx));
	TestEqual(TEXT("seeded from a taxiway the chain is empty"), Net->RunwayChain(Tx).Num(), 0);

	FVector2D Threshold, Direction; double Length = 0.0; FRoadSegmentId Seed;
	TestTrue(TEXT("extent query answers near the far end"), Net->RunwayExtentAt(FVector2D(99000.0, 100.0), Threshold, Direction, Length, &Seed));
	TestEqual(TEXT("and names the segment whose end was nearest"), Seed, R2);
	TestEqual(TEXT("length is the whole strip, not the seed"), Length, 100000.0, 1.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FArrivalPlannerRunwayOccupiedTest,
	"Airside.Model.ArrivalPlanner.RunwayOccupied",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FArrivalPlannerRunwayOccupiedTest::RunTest(const FString& Parameters)
{
	// Only the runway step is under test, so the plan is allowed to fail LATER (no exits,
	// no stands): the assertion is that a held runway is refused BEFORE any of that, with
	// its own reason, and that the same graph unheld gets past the runway step.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Runway = URoadProfile::MakeTransient(4500.0, 1500.0, 450.0);
	Runway->bContinuousThroughJunctions = true;
	const FRoadNodeId T = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId E = Net->AddNode(FVector2D(60000.0, 0.0));
	const FRoadNodeId F = Net->AddNode(FVector2D(120000.0, 0.0));
	const FRoadSegmentId R1 = Net->AddStraightSegment(T, E, Runway);
	Net->AddStraightSegment(E, F, Runway);

	const FAirframe Airframe = UAirsideSettings::ResolveDefaultAirframe();

	FTrafficOccupancy Table;
	FTrafficClaim Hold; Hold.AgentId = 42; Hold.Resource = FTrafficResource::OfSurface(R1); Hold.bOccupied = true;
	FTrafficClaim Blocker;
	Table.TryClaim(Hold, Blocker);

	const FArrivalPlan Held = ArrivalPlanner::Plan(*Net, FVector2D(-1000.0, 0.0), Airframe, &Table);
	TestEqual(TEXT("a runway held on ANY segment of its chain refuses the arrival"), Held.Why, EArrivalRefusal::RunwayOccupied);
	TestEqual(TEXT("the plan still names the chain it was refused for"), Held.RunwayChain.Num(), 2);
	TestTrue(TEXT("DescribeRefusal has words for it"), ArrivalPlanner::DescribeRefusal(Held).Contains(TEXT("in use")));

	Table.Clear();
	const FArrivalPlan Free = ArrivalPlanner::Plan(*Net, FVector2D(-1000.0, 0.0), Airframe, &Table);
	TestNotEqual(TEXT("unheld, the refusal (if any) is a later step's"), Free.Why, EArrivalRefusal::RunwayOccupied);
	TestNotEqual(TEXT("and the runway was found"), Free.Why, EArrivalRefusal::NoRunway);
	return true;
}

#endif
