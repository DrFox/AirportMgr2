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
	/** M2TrafficArrivalAirport with TWO stands beside the taxiway, both facing east so their
	 *  lead-ins cast west and meet it. Prefixed StandOcc against the unity build. */
	struct FStandOccAirport
	{
		URoadNetwork* Net = nullptr;
		FVector2D Threshold = FVector2D::ZeroVector;
		FVector2D ExitAt = FVector2D::ZeroVector;
		FVector2D StandAAt, StandBAt;
		FEntityInstanceId StandA, StandB;
	};

	FAirframe StandOccPiper()
	{
		FAirframe A;
		A.Ground = UAircraftType::PiperMeridianGround();
		A.Climb = UAircraftType::PiperMeridianClimb();
		A.Approach = UAircraftType::PiperMeridianApproach();
		A.Engine = UAircraftType::PiperMeridianEngine();
		return A;
	}

	FStandOccAirport StandOccBuild()
	{
		FStandOccAirport Out;
		Out.Net = NewObject<URoadNetwork>(GetTransientPackage());
		const FAirframe Airframe = StandOccPiper();
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

	FGuidelineNodeId StandOccPose(const FStandOccAirport& A, FEntityInstanceId Stand)
	{
		const FEntityInstance* E = A.Net->GetEntity(Stand);
		return E != nullptr ? E->PoseNode : FGuidelineNodeId();
	}

	/** Ticks until Pred() or Seconds; returns true when Pred became true. */
	template <typename P>
	bool StandOccRunUntil(UGroundTraffic& Traffic, const URoadNetwork& Net, double Seconds, P Pred, double Dt = 0.05)
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
	FStandClaimTest,
	"Airside.Model.Traffic.StandClaim",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandClaimTest::RunTest(const FString& Parameters)
{
	// THE CLAIM IS A READING OF THE GOAL. Held from dispatch (between ticks), re-asserted
	// every tick, released when the goal changes or the agent goes. Nothing on the stand.
	FStandOccAirport A = StandOccBuild();
	const FGuidelineNodeId PoseA = StandOccPose(A, A.StandA);
	const FGuidelineNodeId PoseB = StandOccPose(A, A.StandB);
	if (!TestTrue(TEXT("both stands linked"), PoseA.IsSet() && PoseB.IsSet())) { return false; }

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Id = Traffic->DispatchArrival(*A.Net, A.Threshold, StandOccPiper(), 1.0);
	if (!TestTrue(TEXT("arrival dispatched"), Id > 0)) { return false; }
	const FGuidelineNodeId Goal = Traffic->FindAgent(Id)->GoalNode;
	TestTrue(TEXT("the goal is one of the two stands"), Goal == PoseA || Goal == PoseB);

	int32 Holder = 0;
	TestTrue(TEXT("the stand is held BEFORE any tick - dispatch claims it"),
		Traffic->GetOccupancy().IsHeld(FTrafficResource::OfNode(Goal), 0, &Holder));
	TestEqual(TEXT("by this agent"), Holder, Id);

	// Still held every tick of the approach, roll and taxi; occupied once parked.
	bool bHeldThroughout = true;
	const bool bParked = StandOccRunUntil(*Traffic, *A.Net, 600.0, [&]()
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
	FRouteQuery Q; Q.Start = Goal; Q.Goal = Other; Q.Class = ETraversalClass::Aircraft;
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
