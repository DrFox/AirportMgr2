#include "CoreMinimal.h"
#include "Build/AnchorLink.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Entities/AircraftType.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/ArrivalPlanner.h"
#include "Model/GroundTraffic.h"
#include "Model/LandingRun.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteSearch.h"
#include "Model/TrafficOccupancy.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// The StandClaimTest fixture, repeated: the tests module is a unity build, so an
	// anonymous-namespace helper cannot be shared between files. Prefixed StandOcc2.
	struct FStandOcc2Airport
	{
		URoadNetwork* Net = nullptr;
		FVector2D Threshold = FVector2D::ZeroVector;
		FVector2D ExitAt = FVector2D::ZeroVector;
		FVector2D StandAAt, StandBAt;
		FEntityInstanceId StandA, StandB;
	};

	FAirframe StandOcc2Piper()
	{
		FAirframe A;
		A.Ground = UAircraftType::PiperMeridianGround();
		A.Climb = UAircraftType::PiperMeridianClimb();
		A.Approach = UAircraftType::PiperMeridianApproach();
		A.Engine = UAircraftType::PiperMeridianEngine();
		return A;
	}

	/** Solve, derive guidelines and re-link every stand: what the facade's RebuildMesh does. */
	void StandOcc2Rebuild(URoadNetwork& Net)
	{
		const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(Net);
		FRoadGuidelineBuilder::Build(Net, Solved);
		FAnchorLink::Build(Net);
	}

	FStandOcc2Airport StandOcc2Build()
	{
		FStandOcc2Airport Out;
		Out.Net = NewObject<URoadNetwork>(GetTransientPackage());
		const FAirframe Airframe = StandOcc2Piper();
		const double Needed = FLandingRun::RequiredLandingDistance(
			Airframe.Ground, Airframe.Climb, Airframe.Approach) * FLandingRun::LandingMargin;
		Out.ExitAt = FVector2D(Needed * 1.2, 0.0);
		const FVector2D FarAt(Needed * 3.0, 0.0);

		URoadProfile* Runway = URoadProfile::MakeTransient(4500.0, 1500.0, 450.0);
		Runway->bContinuousThroughJunctions = true;
		URoadProfile* Taxiway = URoadProfile::MakeTransient(2300.0, 1500.0, 230.0);

		const FRoadNodeId T = Out.Net->AddNode(Out.Threshold);
		const FRoadNodeId X = Out.Net->AddNode(Out.ExitAt);
		const FRoadNodeId F = Out.Net->AddNode(FarAt);
		Out.Net->AddStraightSegment(T, X, Runway);
		Out.Net->AddStraightSegment(X, F, Runway);
		const FRoadNodeId TaxiEnd = Out.Net->AddNode(Out.ExitAt + FVector2D(0.0, -20000.0));
		Out.Net->AddStraightSegment(X, TaxiEnd, Taxiway);

		const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Out.Net);
		FRoadGuidelineBuilder::Build(*Out.Net, Solved);

		UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
		Out.StandAAt = Out.ExitAt + FVector2D(9000.0, -10000.0);
		Out.StandBAt = Out.ExitAt + FVector2D(9000.0, -16000.0);
		Out.StandA = Out.Net->PlaceEntity(Stand, Stand->Anchors, Out.StandAAt, 0.0);
		Out.StandB = Out.Net->PlaceEntity(Stand, Stand->Anchors, Out.StandBAt, 0.0);
		FAnchorLink::Build(*Out.Net);
		return Out;
	}

	FGuidelineNodeId StandOcc2Pose(const FStandOcc2Airport& A, FEntityInstanceId Stand)
	{
		const FEntityInstance* E = A.Net->GetEntity(Stand);
		return E != nullptr ? E->PoseNode : FGuidelineNodeId();
	}

	template <typename P>
	bool StandOcc2RunUntil(UGroundTraffic& Traffic, const URoadNetwork& Net, double Seconds, P Pred, double Dt = 0.05)
	{
		for (double Clock = 0.0; Clock < Seconds; Clock += Dt)
		{
			Traffic.Advance(Dt, &Net);
			if (Pred()) { return true; }
		}
		return Pred();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandChoiceTest,
	"Airside.Model.ArrivalPlanner.SkipsHeldStand",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandChoiceTest::RunTest(const FString& Parameters)
{
	FStandOcc2Airport A = StandOcc2Build();
	const FGuidelineNodeId PoseA = StandOcc2Pose(A, A.StandA);
	const FGuidelineNodeId PoseB = StandOcc2Pose(A, A.StandB);
	if (!TestTrue(TEXT("both stands linked"), PoseA.IsSet() && PoseB.IsSet())) { return false; }
	const FAirframe Piper = StandOcc2Piper();

	// Planner level: with the first choice held by agent 7, the plan goes to the other; with
	// both held, NoFreeStand.
	FTrafficOccupancy Occ;
	const FArrivalPlan Free = ArrivalPlanner::Plan(*A.Net, A.Threshold, Piper, &Occ);
	if (!TestTrue(TEXT("plans with both free"), Free.IsValid())) { return false; }
	const FGuidelineNodeId First = Free.TaxiIn.Steps.Last().To;

	auto Hold = [&](FGuidelineNodeId Node, int32 Agent)
	{
		FTrafficClaim C; C.AgentId = Agent; C.Resource = FTrafficResource::OfNode(Node); C.Rank = 10;
		FTrafficClaim B; Occ.TryClaim(C, B);
	};
	Hold(First, 7);
	const FArrivalPlan Other = ArrivalPlanner::Plan(*A.Net, A.Threshold, Piper, &Occ);
	if (!TestTrue(TEXT("plans with one held"), Other.IsValid())) { return false; }
	TestTrue(TEXT("and goes to the OTHER stand"), Other.TaxiIn.Steps.Last().To != First);
	Hold(Other.TaxiIn.Steps.Last().To, 8);
	const FArrivalPlan None = ArrivalPlanner::Plan(*A.Net, A.Threshold, Piper, &Occ);
	TestEqual(TEXT("both held is NoFreeStand"), None.Why, EArrivalRefusal::NoFreeStand);
	TestTrue(TEXT("and it has words"), !ArrivalPlanner::DescribeRefusal(None).IsEmpty());

	// Unreachable is still NoRouteToStand: remove both stands.
	A.Net->RemoveEntity(A.StandA);
	A.Net->RemoveEntity(A.StandB);
	StandOcc2Rebuild(*A.Net);
	TestEqual(TEXT("no stands at all is NoRouteToStand, not NoFreeStand"),
		ArrivalPlanner::Plan(*A.Net, A.Threshold, Piper, &Occ).Why, EArrivalRefusal::NoRouteToStand);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandChoiceTwoArrivalsTest,
	"Airside.Model.Traffic.TwoArrivalsTwoStands",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandChoiceTwoArrivalsTest::RunTest(const FString& Parameters)
{
	// THE REPORT: "I called in 2 aircraft, they both went to the same stand." Through the
	// model, end to end: the second is dispatched once the first has vacated the runway (the
	// runway claim would refuse it earlier, for its own reason), and gets the other stand.
	// A third, once the second has vacated too, is refused for want of a stand.
	FStandOcc2Airport A = StandOcc2Build();
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const FAirframe Piper = StandOcc2Piper();

	const int32 First = Traffic->DispatchArrival(*A.Net, A.Threshold, Piper, 1.0);
	if (!TestTrue(TEXT("first dispatched"), First > 0)) { return false; }
	// UNTIL THE RUNWAY IS CLEAR, not merely until Taxiing: an aircraft that has just vacated
	// still holds the strip under the crossing rule until its tail is off it, and a landing
	// asked in that window is refused as RunwayOccupied - its own reason, not this test's.
	auto RunwayFree = [&]()
	{
		for (const FTrafficClaim& C : Traffic->GetOccupancy().GetClaims())
		{
			if (C.Resource.Kind == ETrafficResourceKind::Surface) { return false; }
		}
		return true;
	};
	if (!TestTrue(TEXT("first vacates and clears the runway"), StandOcc2RunUntil(*Traffic, *A.Net, 300.0,
		[&]() { const FRoadAgent* P = Traffic->FindAgent(First); return P && P->Phase == EAgentPhase::Taxiing && RunwayFree(); }))) { return false; }

	TArray<EArrivalRefusal> Refusals;
	Traffic->OnArrivalRefused.AddLambda([&](EArrivalRefusal Why) { Refusals.Add(Why); });
	const int32 Second = Traffic->DispatchArrival(*A.Net, A.Threshold, Piper, 1.0);
	if (!TestTrue(FString::Printf(TEXT("second dispatched (refusals: %d)"), Refusals.Num()), Second > 0)) { return false; }
	TestTrue(TEXT("two aircraft, two stands"),
		Traffic->FindAgent(First)->GoalNode != Traffic->FindAgent(Second)->GoalNode);

	if (!TestTrue(TEXT("second vacates and clears the runway"), StandOcc2RunUntil(*Traffic, *A.Net, 300.0,
		[&]() { const FRoadAgent* P = Traffic->FindAgent(Second); return P && P->Phase == EAgentPhase::Taxiing && RunwayFree(); }))) { return false; }
	const int32 Third = Traffic->DispatchArrival(*A.Net, A.Threshold, Piper, 1.0);
	TestEqual(TEXT("a third is refused"), Third, 0);
	TestTrue(TEXT("for want of a free stand"), Refusals.Num() > 0 && Refusals.Last() == EArrivalRefusal::NoFreeStand);
	return true;
}

#endif
