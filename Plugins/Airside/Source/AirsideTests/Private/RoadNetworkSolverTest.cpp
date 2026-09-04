#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Build/RoadNetworkSolver.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"
#include "Solve/JunctionSolver.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadNetworkSolverTest,
	"Airside.Build.NetworkSolver",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadNetworkSolverTest::RunTest(const FString& Parameters)
{
	constexpr double W = 1150.0;

	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Profile = URoadProfile::MakeTransient(W * 2.0, 1500.0);

	// A 90-degree bend: centre node with an east arm and a north arm.
	const FRoadNodeId Centre = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId East   = Net->AddNode(FVector2D(40000.0, 0.0));
	const FRoadNodeId North  = Net->AddNode(FVector2D(0.0, 40000.0));

	const FRoadSegmentId ToEast  = Net->AddStraightSegment(Centre, East,  Profile);
	const FRoadSegmentId ToNorth = Net->AddStraightSegment(Centre, North, Profile);

	const FRoadSolveResult Result = FRoadNetworkSolver::SolveAll(*Net);

	TestTrue(TEXT("every node solved"), Result.FailedNodes == 0);
	TestEqual(TEXT("three nodes solved"), Result.SolvedNodes, 3);
	TestTrue(TEXT("centre node has a result"), Result.NodeResults.Contains(Centre.Index));

	// Both segments are marked solved and carry non-zero trims at the bend end.
	const FRoadSegment* SegEast = Net->GetSegment(ToEast);
	TestTrue(TEXT("east segment solved at both ends"), SegEast->bSolvedA && SegEast->bSolvedB);
	TestTrue(TEXT("east segment trimmed at the bend"), SegEast->TrimA > 0.0);

	// THE CONTRACT, carried into the model: the segment's stored cut vertices are
	// bitwise identical to the ones the junction result holds for that arm.
	const FJunctionResult& CentreResult = Result.NodeResults[Centre.Index];
	bool bFoundLeft = false;
	bool bFoundRight = false;
	for (const FJunctionArmResult& Arm : CentreResult.Arms)
	{
		if (Arm.LeftCut.X == SegEast->LeftCutA.X && Arm.LeftCut.Y == SegEast->LeftCutA.Y)
		{
			bFoundLeft = true;
		}
		if (Arm.RightCut.X == SegEast->RightCutA.X && Arm.RightCut.Y == SegEast->RightCutA.Y)
		{
			bFoundRight = true;
		}
	}
	TestTrue(TEXT("stored left cut matches the junction result exactly"), bFoundLeft);
	TestTrue(TEXT("stored right cut matches the junction result exactly"), bFoundRight);

	// A dead-end node still solves and still writes its end's cut vertices.
	const FRoadSegment* SegNorth = Net->GetSegment(ToNorth);
	TestTrue(TEXT("north segment solved at both ends"), SegNorth->bSolvedA && SegNorth->bSolvedB);
	TestFalse(TEXT("dead end wrote a real cut line"),
		SegNorth->LeftCutB.Equals(SegNorth->RightCutB, 1.0));

	// Re-solving is idempotent: same inputs, same bits.
	const FVector2D BeforeLeft = SegEast->LeftCutA;
	FRoadNetworkSolver::SolveAll(*Net);
	const FRoadSegment* Again = Net->GetSegment(ToEast);
	TestTrue(TEXT("re-solve is bitwise idempotent"),
		Again->LeftCutA.X == BeforeLeft.X && Again->LeftCutA.Y == BeforeLeft.Y);

	// The arm -> segment mapping the solver already computes internally, published so the
	// mesh builder does not have to re-derive it. Re-deriving means re-walking
	// Node.Incident and re-applying the same skip rule, which is how the two got out of
	// step before: an index into Incident is not an index into Arms.
	{
		const TArray<FRoadSegmentId>* CentreArms = Result.NodeArmSegments.Find(Centre.Index);
		if (TestNotNull(TEXT("centre node publishes its arm mapping"), CentreArms))
		{
			const FJunctionResult& CentreArmResult = Result.NodeResults[Centre.Index];
			TestEqual(TEXT("one segment id per solved arm"),
				CentreArms->Num(), CentreArmResult.Arms.Num());

			// Every entry names a live segment actually incident to this node.
			for (const FRoadSegmentId ArmSegment : *CentreArms)
			{
				const FRoadSegment* Seg = Net->GetSegment(ArmSegment);
				if (TestNotNull(TEXT("arm names a live segment"), Seg))
				{
					TestTrue(TEXT("arm's segment touches this node"),
						Seg->A == Centre || Seg->B == Centre);
				}
			}
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
