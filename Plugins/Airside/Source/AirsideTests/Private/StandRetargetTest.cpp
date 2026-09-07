#include "CoreMinimal.h"
#include "Build/AnchorLink.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Entities/AircraftType.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/ArrivalPlanner.h"
#include "Model/GroundTraffic.h"
#include "Model/InspectFacts.h"
#include "Model/LandingRun.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteSearch.h"
#include "Model/TrafficOccupancy.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// The StandClaimTest fixture, repeated against the unity build. Prefixed StandOcc3.
	struct FStandOcc3Airport
	{
		URoadNetwork* Net = nullptr;
		FVector2D Threshold = FVector2D::ZeroVector;
		FVector2D ExitAt = FVector2D::ZeroVector;
		FVector2D StandAAt, StandBAt;
		FEntityInstanceId StandA, StandB;
	};

	FAirframe StandOcc3Piper()
	{
		FAirframe A;
		A.Ground = UAircraftType::PiperMeridianGround();
		A.Climb = UAircraftType::PiperMeridianClimb();
		A.Approach = UAircraftType::PiperMeridianApproach();
		A.Engine = UAircraftType::PiperMeridianEngine();
		return A;
	}

	void StandOcc3Rebuild(URoadNetwork& Net)
	{
		const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(Net);
		FRoadGuidelineBuilder::Build(Net, Solved);
		FAnchorLink::Build(Net);
	}

	FStandOcc3Airport StandOcc3Build()
	{
		FStandOcc3Airport Out;
		Out.Net = NewObject<URoadNetwork>(GetTransientPackage());
		const FAirframe Airframe = StandOcc3Piper();
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

	FGuidelineNodeId StandOcc3Pose(const FStandOcc3Airport& A, FEntityInstanceId Stand)
	{
		const FEntityInstance* E = A.Net->GetEntity(Stand);
		return E != nullptr ? E->PoseNode : FGuidelineNodeId();
	}

	template <typename P>
	bool StandOcc3RunUntil(UGroundTraffic& Traffic, const URoadNetwork& Net, double Seconds, P Pred, double Dt = 0.05)
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
	FStandRetargetTest,
	"Airside.Model.Traffic.StandRetarget",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandRetargetTest::RunTest(const FString& Parameters)
{
	// THE SECOND QUESTION OF 2026-09-07: "what happens if the reserved stand gets deleted as
	// the aircraft is coming in to land". Retarget to a free stand; failing that wait, not
	// strand; and go the moment a stand appears.
	FStandOcc3Airport A = StandOcc3Build();
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const FAirframe Piper = StandOcc3Piper();

	const int32 Id = Traffic->DispatchArrival(*A.Net, A.Threshold, Piper, 1.0);
	if (!TestTrue(TEXT("dispatched"), Id > 0)) { return false; }
	Traffic->Advance(0.05, A.Net);   // one tick: on final
	const FGuidelineNodeId Goal0 = Traffic->FindAgent(Id)->GoalNode;
	const FEntityInstanceId Target = (Goal0 == StandOcc3Pose(A, A.StandA)) ? A.StandA : A.StandB;
	const FEntityInstanceId Spare = (Target == A.StandA) ? A.StandB : A.StandA;

	// 1. DELETE THE STAND IT IS HEADING FOR while it is on final. It retargets to the other.
	A.Net->RemoveEntity(Target);
	StandOcc3Rebuild(*A.Net);
	Traffic->OnGraphRebuilt(*A.Net);
	{
		const FRoadAgent* P = Traffic->FindAgent(Id);
		TestEqual(TEXT("still arriving"), P->Phase, EAgentPhase::Arriving);
		TestTrue(TEXT("its goal is now the spare stand"), P->GoalNode == StandOcc3Pose(A, Spare));
		TestFalse(TEXT("and it is not waiting"), P->bAwaitingStand);
		TestTrue(TEXT("the spare stand is held for it, between ticks"),
			Traffic->GetOccupancy().IsHeld(FTrafficResource::OfNode(P->GoalNode), 0));
	}

	// 2. DELETE THE SPARE TOO. Nothing to retarget to: it waits, and lands anyway (v1).
	A.Net->RemoveEntity(Spare);
	StandOcc3Rebuild(*A.Net);
	Traffic->OnGraphRebuilt(*A.Net);
	{
		const FRoadAgent* P = Traffic->FindAgent(Id);
		TestTrue(TEXT("awaiting a stand"), P->bAwaitingStand);
		TestTrue(TEXT("with a live node to wait at"), A.Net->GetGuidelineNode(P->GoalNode) != nullptr);
		TestEqual(TEXT("status says so"), InspectFacts::StatusOf(*P), FString(TEXT("No stand - waiting")));
	}
	if (!TestTrue(TEXT("it lands and stops"), StandOcc3RunUntil(*Traffic, *A.Net, 600.0,
		[&]() { const FRoadAgent* P = Traffic->FindAgent(Id); return P && P->Phase == EAgentPhase::Parked; }))) { return false; }
	TestTrue(TEXT("a parked waiter occupies the node it stopped at"),
		Traffic->GetOccupancy().IsHeld(FTrafficResource::OfNode(Traffic->FindAgent(Id)->GoalNode), 0));

	// 3. BUILD A STAND. The rebuild re-offers it; the aircraft goes.
	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
	const FEntityInstanceId NewStand = A.Net->PlaceEntity(Stand, Stand->Anchors, A.StandAAt, 0.0);
	StandOcc3Rebuild(*A.Net);
	Traffic->OnGraphRebuilt(*A.Net);
	Traffic->Advance(0.05, A.Net);   // the re-offer runs at the end of a tick
	{
		const FRoadAgent* P = Traffic->FindAgent(Id);
		TestFalse(TEXT("no longer waiting"), P->bAwaitingStand);
		TestTrue(TEXT("heading for the new stand"), P->GoalNode == StandOcc3Pose(A, NewStand));
		TestEqual(TEXT("taxiing again"), P->Phase, EAgentPhase::Taxiing);
	}
	TestTrue(TEXT("and it parks there"), StandOcc3RunUntil(*Traffic, *A.Net, 600.0,
		[&]() { const FRoadAgent* P = Traffic->FindAgent(Id); return P && P->Phase == EAgentPhase::Parked && P->GoalNode == StandOcc3Pose(A, NewStand); }));
	return true;
}

#endif
