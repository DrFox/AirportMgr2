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

/**
 * #166: the ghost preview solves only the two nodes it draws, through
 * FRoadNetworkSolver::SolveNodeInto, instead of SolveAll's whole-graph walk. This is the
 * regression guard that the two paths write IDENTICAL bits for the nodes they share -
 * built both ways against two separate copies of the same fixture, compared bitwise,
 * exactly as the surface model's own weld contract is measured (see the project's
 * "welds BITWISE" rule) rather than merely asserted.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadNetworkSolverPartialTest,
	"Airside.Build.NetworkSolverPartial",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadNetworkSolverPartialTest::RunTest(const FString& Parameters)
{
	constexpr double W = 1150.0;
	URoadProfile* Profile = URoadProfile::MakeTransient(W * 2.0, 1500.0);

	// A THREE-ARM JUNCTION, so the untouched arm (East) proves the partial solve did not
	// quietly re-solve a node nobody asked for, the same failure mode SolveAll's own
	// per-node body exists to prevent by being the ONE thing both callers run.
	URoadNetwork* NetFull = NewObject<URoadNetwork>(GetTransientPackage());
	const FRoadNodeId FullCentre = NetFull->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId FullEast   = NetFull->AddNode(FVector2D(40000.0, 0.0));
	const FRoadNodeId FullNorth  = NetFull->AddNode(FVector2D(0.0, 40000.0));
	const FRoadNodeId FullWest   = NetFull->AddNode(FVector2D(-40000.0, 0.0));
	NetFull->AddStraightSegment(FullCentre, FullEast, Profile);
	NetFull->AddStraightSegment(FullCentre, FullNorth, Profile);
	const FRoadSegmentId FullToWest = NetFull->AddStraightSegment(FullCentre, FullWest, Profile);

	// A SEPARATE, IDENTICAL network (same fixture, same code, same handles by construction
	// - both are built the same way in the same order), solved the two different ways, so
	// one solve's writes cannot contaminate the other's inputs.
	URoadNetwork* NetPartial = NewObject<URoadNetwork>(GetTransientPackage());
	const FRoadNodeId PartialCentre = NetPartial->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId PartialEast   = NetPartial->AddNode(FVector2D(40000.0, 0.0));
	const FRoadNodeId PartialNorth  = NetPartial->AddNode(FVector2D(0.0, 40000.0));
	const FRoadNodeId PartialWest   = NetPartial->AddNode(FVector2D(-40000.0, 0.0));
	NetPartial->AddStraightSegment(PartialCentre, PartialEast, Profile);
	NetPartial->AddStraightSegment(PartialCentre, PartialNorth, Profile);
	const FRoadSegmentId PartialToWest = NetPartial->AddStraightSegment(PartialCentre, PartialWest, Profile);

	const FRoadSolveResult Full = FRoadNetworkSolver::SolveAll(*NetFull);

	FRoadSolveResult Partial;
	FRoadNetworkSolver::SolveNodeInto(*NetPartial, PartialCentre.Index, 12, Partial);
	FRoadNetworkSolver::SolveNodeInto(*NetPartial, PartialWest.Index, 12, Partial);

	TestEqual(TEXT("the partial solve reports exactly the two nodes asked for"),
		Partial.SolvedNodes, 2);
	TestFalse(TEXT("the untouched east node has no partial result - nothing solved it"),
		Partial.NodeResults.Contains(PartialEast.Index));

	// THE GHOSTED ARM'S CUTS, at both ends: bitwise identical whichever path wrote them.
	// The centre end carries the whole junction's fan (all three arms), so this is also
	// the check that a two-node solve writes every OTHER live arm at a solved node, not
	// only the one arm a caller happens to care about - AddGhostJunction's fan needs them.
	const FRoadSegment* FullSeg = NetFull->GetSegment(FullToWest);
	const FRoadSegment* PartialSeg = NetPartial->GetSegment(PartialToWest);
	TestTrue(TEXT("west segment solved both ends on the partial path"),
		PartialSeg->bSolvedA && PartialSeg->bSolvedB);
	TestTrue(TEXT("centre-end trim matches the full solve bitwise"),
		PartialSeg->TrimA == FullSeg->TrimA);
	TestTrue(TEXT("centre-end left cut matches the full solve bitwise"),
		PartialSeg->LeftCutA == FullSeg->LeftCutA);
	TestTrue(TEXT("centre-end right cut matches the full solve bitwise"),
		PartialSeg->RightCutA == FullSeg->RightCutA);
	TestTrue(TEXT("west-end trim matches the full solve bitwise"),
		PartialSeg->TrimB == FullSeg->TrimB);
	TestTrue(TEXT("west-end left cut matches the full solve bitwise"),
		PartialSeg->LeftCutB == FullSeg->LeftCutB);
	TestTrue(TEXT("west-end right cut matches the full solve bitwise"),
		PartialSeg->RightCutB == FullSeg->RightCutB);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
