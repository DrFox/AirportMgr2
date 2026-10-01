#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Model/RoutePlanCache.h"
#include "Model/RouteSearch.h"
#include "Model/Vehicle.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoutePlanCacheHitsAfterOneFindTest,
	"Airside.Model.RoutePlanCache.HitsAfterOneFind",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoutePlanCacheHitsAfterOneFindTest::RunTest(const FString& Parameters)
{
	// LIFTED OFF ARigTestCourse's own PlanCacheKnowsItsVehicle and FitCacheDropsOnRebuild
	// (#301): the FIRST ask for a (start, goal, vehicle) is a miss the CALLER answers with a Find
	// and a Store; the SAME ask again is answered from the cache; a DIFFERENT vehicle's figures or a
	// bumped guideline revision are both misses. NO SEARCH COUNTER here: this asserted that exactly one
	// Find had run after the Store, which counted the one Find THIS TEST runs itself a few lines down -
	// 1 whatever Lookup answered. A hit is shown by the cached plan being the one Stored, below.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId A = Net->AddGuidelineNode(FVector2D(0.0, 0.0), /*bDerived=*/false);
	const FGuidelineNodeId B = Net->AddGuidelineNode(FVector2D(10000.0, 0.0), /*bDerived=*/false);
	FGuidelineEdge Edge;
	Edge.A = A;
	Edge.B = B;
	Edge.Control = FVector2D(5000.0, 0.0);
	Edge.AllowedTraffic = FTrafficMask::All();
	Edge.Direction = EGuidelineDir::Bidirectional;
	Net->AddGuidelineEdge(MoveTemp(Edge));

	FVehicle Vehicle;
	Vehicle.BodyWidth = 200.0;

	FRoutePlanCache Cache;
	Cache.EnsureFresh(*Net);
	if (!TestNull(TEXT("nothing cached before the first ask"), Cache.Lookup(A, B, Vehicle))) { return false; }

	const FRouteQuery Query = FRouteQuery::For(ERouteErrand::GraphProbe, A, B, 0.0, ETraversalClass::GroundVehicle);
	const FRoutePlan Plan = RouteSearch::Find(*Net, Query);
	if (!TestTrue(TEXT("the fixture route is found"), Plan.IsValid())) { return false; }
	Cache.Store(A, B, Vehicle, Plan, FString());

	const FCachedRoutePlan* Hit = Cache.Lookup(A, B, Vehicle);
	if (!TestNotNull(TEXT("the same ask hits"), Hit)) { return false; }
	TestTrue(TEXT("the cached plan is the one Found"), Hit->Plan.IsValid() && Hit->Plan.Length == Plan.Length);

	// A DIFFERENT VEHICLE, SAME GRAPH: VehicleIdentity is part of the key, so this misses even
	// though Start/Goal are unchanged - a route this body does not fit is a different fact.
	FVehicle Other = Vehicle;
	Other.BodyWidth += 100.0;
	TestNull(TEXT("a different vehicle's figures are a different key"), Cache.Lookup(A, B, Other));
	TestNotEqual(TEXT("because VehicleIdentity differs"), RoutePlanCache::VehicleIdentity(Vehicle), RoutePlanCache::VehicleIdentity(Other));

	// THE GRAPH CHANGES: adding a guideline node bumps GetGuidelineRevision, and EnsureFresh
	// drops everything on it - even the SAME vehicle's own hit above.
	const uint32 RevisionBefore = Net->GetGuidelineRevision();
	Net->AddGuidelineNode(FVector2D(20000.0, 0.0), /*bDerived=*/false);
	if (!TestNotEqual(TEXT("adding a node bumped the guideline revision"), Net->GetGuidelineRevision(), RevisionBefore)) { return false; }
	Cache.EnsureFresh(*Net);
	TestNull(TEXT("a new guideline revision drops the cache"), Cache.Lookup(A, B, Vehicle));

	return true;
}

#endif
