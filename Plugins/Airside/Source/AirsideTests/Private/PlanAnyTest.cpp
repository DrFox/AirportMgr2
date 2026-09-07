#include "CoreMinimal.h"
#include "Build/AnchorLink.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Content/AirsideSettings.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/DeparturePlanner.h"
#include "Model/RoadNetwork.h"
#include "Model/RunwayFacts.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

DEFINE_LOG_CATEGORY_STATIC(LogPlanAnyTest, Log, All);

namespace
{
	/**
	 * TWO runways: a long W-E strip at Y=0 with a 45-degree taxiway to a stand, and a short
	 * N-S strip far to the east joined by a long taxiway. The stand's nearest departure is
	 * the W-E strip; the N-S one is reachable but a longer taxi.
	 */
	struct FPlanAnyAirport
	{
		URoadNetwork* Net = nullptr;
		FRoadSegmentId LongSeed;
		FRoadSegmentId ShortSeed;
		FGuidelineNodeId StandNode;
	};

	FPlanAnyAirport PlanAnyBuild(UObject* Outer)
	{
		FPlanAnyAirport Out;
		Out.Net = NewObject<URoadNetwork>(Outer);
		URoadProfile* Runway = URoadProfile::MakeTransient(1800.0, 1500.0, 180.0);
		Runway->bContinuousThroughJunctions = true;
		URoadProfile* Taxiway = URoadProfile::MakeTransient(2300.0, 1500.0, 230.0);

		const FRoadNodeId W = Out.Net->AddNode(FVector2D(-40000.0, 0.0));
		const FRoadNodeId X = Out.Net->AddNode(FVector2D(20000.0, 0.0));
		const FRoadNodeId E = Out.Net->AddNode(FVector2D(60000.0, 0.0));
		const FRoadNodeId T = Out.Net->AddNode(FVector2D(40000.0, -20000.0));
		Out.LongSeed = Out.Net->AddStraightSegment(W, X, Runway);
		Out.Net->AddStraightSegment(X, E, Runway);
		Out.Net->AddStraightSegment(X, T, Taxiway);

		// The short strip, 60000 uu east of T, joined at its midpoint.
		const FRoadNodeId S = Out.Net->AddNode(FVector2D(100000.0, -50000.0));
		const FRoadNodeId M = Out.Net->AddNode(FVector2D(100000.0, -20000.0));
		const FRoadNodeId N = Out.Net->AddNode(FVector2D(100000.0, 10000.0));
		Out.ShortSeed = Out.Net->AddStraightSegment(S, M, Runway);
		Out.Net->AddStraightSegment(M, N, Runway);
		Out.Net->AddStraightSegment(T, M, Taxiway);

		const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Out.Net);
		FRoadGuidelineBuilder::Build(*Out.Net, Solved);
		UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
		Out.Net->PlaceEntity(Stand, Stand->Anchors, FVector2D(45000.0, -14000.0), 0.0);
		FAnchorLink::Build(*Out.Net);
		for (const FEntityInstance& I : Out.Net->GetEntities())
		{
			if (I.bAlive && I.PoseNode.IsSet()) { Out.StandNode = I.PoseNode; }
		}
		return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlanAnyShortestTest,
	"Airside.Model.DeparturePlanner.PlanAny.Shortest",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlanAnyShortestTest::RunTest(const FString& Parameters)
{
	FPlanAnyAirport A = PlanAnyBuild(GetTransientPackage());
	if (!TestTrue(TEXT("the stand is linked"), A.StandNode.IsSet())) { return false; }
	const FAirframe Airframe = UAirsideSettings::ResolveDefaultAirframe();

	const FDeparturePlan Plan = DeparturePlanner::PlanAny(*A.Net, A.StandNode, Airframe, ETraversalClass::Aircraft);
	UE_LOG(LogPlanAnyTest, Log, TEXT("PlanAny: %s"), *DeparturePlanner::Describe(Plan));
	if (!TestTrue(FString::Printf(TEXT("planned: %s"), *DeparturePlanner::Describe(Plan)), Plan.IsValid())) { return false; }

	// Both strips admit the Piper; the W-E one is the shorter taxi from this stand.
	TestTrue(TEXT("chose the W-E strip (threshold on Y=0)"), FMath::Abs(Plan.Threshold.Y) < 1.0);

	// The alternative, priced: any plan to the N-S strip is longer.
	FVector2D Th, Dir; double Len = 0.0;
	FRoadSegmentId Seed;
	A.Net->RunwayExtentAt(FVector2D(100000.0, -49990.0), Th, Dir, Len, &Seed);
	const FDeparturePlan Other = DeparturePlanner::Plan(*A.Net, A.StandNode, Th + Dir * 10.0, Airframe, ETraversalClass::Aircraft);
	UE_LOG(LogPlanAnyTest, Log, TEXT("N-S alternative: %s"), *DeparturePlanner::Describe(Other));
	if (Other.IsValid())
	{
		TestTrue(TEXT("the chosen taxi is no longer than the other strip's"), Plan.Route.Length <= Other.Route.Length);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlanAnyAdmissionTest,
	"Airside.Model.DeparturePlanner.PlanAny.SkipsRefusedRunway",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlanAnyAdmissionTest::RunTest(const FString& Parameters)
{
	FPlanAnyAirport A = PlanAnyBuild(GetTransientPackage());
	if (!TestTrue(TEXT("the stand is linked"), A.StandNode.IsSet())) { return false; }

	// Make the near strip grass and demand tarmac: PlanAny must go to the far one.
	FRunwayFacts Grass;
	Grass.Surface = ERunwaySurface::Grass;
	A.Net->SetRunwayFacts(A.LongSeed, Grass);
	FRunwayFacts Tarmac;
	Tarmac.Surface = ERunwaySurface::Tarmac;
	A.Net->SetRunwayFacts(A.ShortSeed, Tarmac);
	FAirframe Airframe = UAirsideSettings::ResolveDefaultAirframe();
	Airframe.Requirements.MinimumSurface = ERunwaySurface::Tarmac;

	const FDeparturePlan Plan = DeparturePlanner::PlanAny(*A.Net, A.StandNode, Airframe, ETraversalClass::Aircraft);
	UE_LOG(LogPlanAnyTest, Log, TEXT("PlanAny (near strip refused): %s"), *DeparturePlanner::Describe(Plan));
	if (!TestTrue(FString::Printf(TEXT("planned: %s"), *DeparturePlanner::Describe(Plan)), Plan.IsValid())) { return false; }
	TestTrue(TEXT("chose the N-S strip (threshold on X=100000)"), FMath::Abs(Plan.Threshold.X - 100000.0) < 1.0);

	// Both refused: the refusal names admission, not "no runway".
	A.Net->SetRunwayFacts(A.ShortSeed, Grass);
	const FDeparturePlan None = DeparturePlanner::PlanAny(*A.Net, A.StandNode, Airframe, ETraversalClass::Aircraft);
	TestFalse(TEXT("no plan when every strip refuses"), None.IsValid());
	TestEqual(TEXT("the reason is the admission, so the log says why"), None.Why, EDepartureRefusal::NotAdmitted);

	// An empty network: NoRunway.
	URoadNetwork* Empty = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId Lone = Empty->AddGuidelineNode(FVector2D::ZeroVector, false);
	TestEqual(TEXT("no runways at all is NoRunway"),
		DeparturePlanner::PlanAny(*Empty, Lone, Airframe, ETraversalClass::Aircraft).Why, EDepartureRefusal::NoRunway);
	return true;
}

#endif
