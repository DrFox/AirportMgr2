#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoutePolicy.h"
#include "Model/RouteSearch.h"
#include "Model/TrafficOccupancy.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandClaimTest,
	"Airside.Model.Traffic.StandClaim",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandClaimTest::RunTest(const FString& Parameters)
{
	// THE CLAIM IS A READING OF THE GOAL. Held from dispatch (between ticks), re-asserted
	// every tick, released when the goal changes or the agent goes. Nothing on the stand.
	const FTestAirport A = FTestAirport::Build(TestAirframes::Piper(), { .StandCount = 2 });
	const FGuidelineNodeId PoseA = A.Pose(A.Stands[0]);
	const FGuidelineNodeId PoseB = A.Pose(A.Stands[1]);
	if (!TestTrue(TEXT("both stands linked"), PoseA.IsSet() && PoseB.IsSet())) { return false; }

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Id = Traffic->DispatchArrival(*A.Net, A.Threshold, TestAirframes::Piper(), 1.0);
	if (!TestTrue(TEXT("arrival dispatched"), Id > 0)) { return false; }
	const FGuidelineNodeId Goal = Traffic->FindAgent(Id)->GoalNode;
	TestTrue(TEXT("the goal is one of the two stands"), Goal == PoseA || Goal == PoseB);

	int32 Holder = 0;
	TestTrue(TEXT("the stand is held BEFORE any tick - dispatch claims it"),
		Traffic->GetOccupancy().IsHeld(FTrafficResource::OfNode(Goal), 0, &Holder));
	TestEqual(TEXT("by this agent"), Holder, Id);

	// Still held every tick of the approach, roll and taxi; occupied once parked.
	bool bHeldThroughout = true;
	const bool bParked = RunUntil(*Traffic, *A.Net, 600.0, [&]()
	{
		bHeldThroughout = bHeldThroughout && Traffic->GetOccupancy().IsHeld(FTrafficResource::OfNode(Goal), 0);
		const FRoadAgent* P = Traffic->FindAgent(Id);
		return P == nullptr || P->Phase == EAgentPhase::Parked;
	});
	if (!TestTrue(TEXT("parked"), bParked && Traffic->FindAgent(Id) != nullptr)) { return false; }
	TestTrue(TEXT("held on every tick from dispatch to parking"), bHeldThroughout);
	{
		const FTrafficClaim* Mine = Traffic->GetOccupancy().GetClaims().FindByPredicate(
			[&](const FTrafficClaim& C) { return C.AgentId == Id && C.Resource == FTrafficResource::OfNode(Goal); });
		TestTrue(TEXT("a parked aircraft OCCUPIES its stand node"), Mine != nullptr && Mine->bOccupied);
	}

	// Redirect elsewhere: the stand frees between ticks, and the model says stands may have freed.
	const FGuidelineNodeId Other = (Goal == PoseA) ? PoseB : PoseA;
	FRouteQuery Q; Q.Errand = ERouteErrand::GraphProbe; Q.Policy = FRoutePolicy::For(Q.Errand); Q.Start = Goal; Q.Goal = Other; Q.Class = ETraversalClass::Aircraft;
	const FRoutePlan ToOther = RouteSearch::Find(*A.Net, Q);
	if (!TestTrue(TEXT("a route between the stands exists"), ToOther.IsValid())) { return false; }
	if (!TestTrue(TEXT("redirected"), Traffic->RedirectAgent(Id, A.Net, ToOther))) { return false; }
	TestFalse(TEXT("the old stand is released at the redirect, not a tick later"),
		Traffic->GetOccupancy().IsHeld(FTrafficResource::OfNode(Goal), 0));
	TestTrue(TEXT("the new stand is held at the redirect"),
		Traffic->GetOccupancy().IsHeld(FTrafficResource::OfNode(Other), 0));
	TestTrue(TEXT("the model flags that a stand may have freed"), Traffic->StandsMayHaveFreedForTest());

	// Retire: everything goes.
	Traffic->RetireAgent(Id);
	TestFalse(TEXT("retired agents hold nothing"), Traffic->GetOccupancy().IsHeld(FTrafficResource::OfNode(Other), 0));
	return true;
}

#endif
