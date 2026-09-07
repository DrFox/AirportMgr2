#include "CoreMinimal.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** The guideline node derived for one end of Segment, or unset. */
	FGuidelineNodeId M2HoldNodeFor(const URoadNetwork& Net, FRoadSegmentId Segment, bool bEndA)
	{
		const TArray<FGuidelineNode>& Nodes = Net.GetGuidelineNodes();
		for (int32 Index = 0; Index < Nodes.Num(); ++Index)
		{
			if (Nodes[Index].bAlive && Nodes[Index].Origin.Segment == Segment && Nodes[Index].Origin.bEndA == bEndA)
			{
				return Net.GuidelineNodeIdAt(Index);
			}
		}
		return FGuidelineNodeId();
	}

	void M2HoldRebuild(URoadNetwork& Net)
	{
		const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(Net);
		FRoadGuidelineBuilder::Build(Net, Solved);
	}

	EHoldingPositionKind M2HoldKindAt(const URoadNetwork& Net, FGuidelineNodeId Node)
	{
		const FGuidelineNode* Found = Net.GetGuidelineNode(Node);
		return Found ? Found->HoldingPosition : EHoldingPositionKind::None;
	}
}

/**
 * AN INTERMEDIATE HOLDING POSITION IS THE PLAYER'S AND SURVIVES A REBUILD (spec 2026-09-07,
 * inheriting M2 §6's rule for the old hold-short mark): stored by identity, re-applied by
 * the builder onto the FRESH node each rebuild allocates. A runway-holding position at the
 * same airport is the junction's, not the player's, and refuses the tool.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHoldingPositionSurvivesRebuildTest,
	"Airside.Build.HoldingPositionSurvivesRebuild",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FHoldingPositionSurvivesRebuildTest::RunTest(const FString& Parameters)
{
	//   T ======= E ======= F        runway
	//             |
	//             X ----- Y          taxiway E-X, taxiway X-Y: X is a taxiway junction
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Runway = URoadProfile::MakeTransient(4500.0, 1500.0, 450.0);
	Runway->bContinuousThroughJunctions = true;
	URoadProfile* Taxiway = URoadProfile::MakeTransient(2300.0, 1500.0, 230.0);
	const FRoadNodeId T = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId E = Net->AddNode(FVector2D(60000.0, 0.0));
	const FRoadNodeId F = Net->AddNode(FVector2D(100000.0, 0.0));
	const FRoadSegmentId R1 = Net->AddStraightSegment(T, E, Runway);
	Net->AddStraightSegment(E, F, Runway);
	const FRoadNodeId X = Net->AddNode(FVector2D(60000.0, -20000.0));
	const FRoadNodeId Y = Net->AddNode(FVector2D(80000.0, -20000.0));
	const FRoadSegmentId Tx = Net->AddStraightSegment(E, X, Taxiway);
	const FRoadSegmentId Ty = Net->AddStraightSegment(X, Y, Taxiway);
	M2HoldRebuild(*Net);

	// 1. THE RUNWAY END IS DERIVED and refuses the player.
	const FGuidelineNodeId RunwayEnd = M2HoldNodeFor(*Net, Tx, /*bEndA=*/true);
	if (!TestTrue(TEXT("the taxiway's E-end guideline node exists"), RunwayEnd.IsSet())) { return false; }
	TestTrue(TEXT("the taxiway end at the runway is a runway-holding position, derived"),
		M2HoldKindAt(*Net, RunwayEnd) == EHoldingPositionKind::Runway);
	// A CHAIN MEMBER, not one named segment: E joins two continuous runway segments, and
	// both ARE the runway, which is the only thing such a position can protect.
	TestTrue(TEXT("protecting the strip it joins"),
		Net->RunwayChain(R1).Contains(Net->GetGuidelineNode(RunwayEnd)->HoldingPositionFor));
	TestFalse(TEXT("the player cannot clear it"), Net->SetIntermediateHoldingPosition(RunwayEnd, false));
	TestEqual(TEXT("no mark is recorded for a derived position"), Net->GetHoldingPositionMarks().Num(), 0);

	// 2. THE PLAYER PLACES AN INTERMEDIATE ONE at the taxiway junction X, on Tx's X end.
	const FGuidelineNodeId Junction = M2HoldNodeFor(*Net, Tx, /*bEndA=*/false);
	if (!TestTrue(TEXT("the taxiway's X-end guideline node exists"), Junction.IsSet())) { return false; }
	TestTrue(TEXT("a taxiway junction node carries nothing by itself"),
		M2HoldKindAt(*Net, Junction) == EHoldingPositionKind::None);
	TestTrue(TEXT("set"), Net->SetIntermediateHoldingPosition(Junction, true));
	TestTrue(TEXT("the node is an intermediate position"), M2HoldKindAt(*Net, Junction) == EHoldingPositionKind::Intermediate);
	TestFalse(TEXT("protecting nothing - no runway is named"), Net->GetGuidelineNode(Junction)->HoldingPositionFor.IsSet());
	TestEqual(TEXT("one mark recorded"), Net->GetHoldingPositionMarks().Num(), 1);

	M2HoldRebuild(*Net);
	TestNull(TEXT("the old node handle is dead - the builder made a fresh one"), Net->GetGuidelineNode(Junction));
	const FGuidelineNodeId JunctionAfter = M2HoldNodeFor(*Net, Tx, false);
	if (!TestTrue(TEXT("the fresh node exists"), JunctionAfter.IsSet())) { return false; }
	TestTrue(TEXT("and carries the intermediate position"), M2HoldKindAt(*Net, JunctionAfter) == EHoldingPositionKind::Intermediate);
	TestTrue(TEXT("and has edges - it is connected, not an orphan kept alive"), Net->GetGuidelineNode(JunctionAfter)->Incident.Num() > 0);
	TestTrue(TEXT("and the runway end is still derived after the rebuild"),
		M2HoldKindAt(*Net, M2HoldNodeFor(*Net, Tx, true)) == EHoldingPositionKind::Runway);

	// TWICE, because one rebuild could pass on a mark the builder consumed destructively.
	M2HoldRebuild(*Net);
	const FGuidelineNodeId JunctionTwice = M2HoldNodeFor(*Net, Tx, false);
	if (!TestTrue(TEXT("the node after a second rebuild exists"), JunctionTwice.IsSet())) { return false; }
	TestTrue(TEXT("and still carries it"), M2HoldKindAt(*Net, JunctionTwice) == EHoldingPositionKind::Intermediate);

	TestTrue(TEXT("clear"), Net->SetIntermediateHoldingPosition(JunctionTwice, false));
	TestTrue(TEXT("kind gone"), M2HoldKindAt(*Net, JunctionTwice) == EHoldingPositionKind::None);
	TestEqual(TEXT("mark gone"), Net->GetHoldingPositionMarks().Num(), 0);

	// And a cleared position stays cleared - the mark, not a stale node flag, is the source.
	M2HoldRebuild(*Net);
	const FGuidelineNodeId JunctionCleared = M2HoldNodeFor(*Net, Tx, false);
	if (!TestTrue(TEXT("the node after clearing exists"), JunctionCleared.IsSet())) { return false; }
	TestTrue(TEXT("still nothing after a rebuild"), M2HoldKindAt(*Net, JunctionCleared) == EHoldingPositionKind::None);

	// 3. AN ORIGIN-LESS NODE: no mark is stored (the node is its own source), and an
	//    intermediate position there survives every rebuild by itself.
	{
		const FGuidelineNodeId Loose = Net->AddGuidelineNode(FVector2D(70000.0, -20000.0), /*bDerived=*/false);
		TestTrue(TEXT("an Origin-less node can carry an intermediate position"), Net->SetIntermediateHoldingPosition(Loose, true));
		TestEqual(TEXT("but records no mark - the node is its own source"), Net->GetHoldingPositionMarks().Num(), 0);
		M2HoldRebuild(*Net);
		TestTrue(TEXT("and it survives a rebuild"), M2HoldKindAt(*Net, Loose) == EHoldingPositionKind::Intermediate);

		// A RUNWAY kind on an Origin-less node (a hand-built fixture's, via the test setter)
		// IS pruned when its runway goes: the one case the mark prune cannot reach.
		const FGuidelineNodeId LooseRunway = Net->AddGuidelineNode(FVector2D(60000.0, -5000.0), /*bDerived=*/false);
		TestTrue(TEXT("a runway kind can be set on it for a test"), Net->SetRunwayHoldingPositionForTest(LooseRunway, R1));
		Net->RemoveSegment(R1);
		M2HoldRebuild(*Net);
		if (TestNotNull(TEXT("the node itself outlives the runway - it is never swept"), Net->GetGuidelineNode(LooseRunway)))
		{
			TestTrue(TEXT("but its runway kind is cleared with the runway it named"),
				M2HoldKindAt(*Net, LooseRunway) == EHoldingPositionKind::None
				&& !Net->GetGuidelineNode(LooseRunway)->HoldingPositionFor.IsSet());
		}
	}

	// 4. A MARK WHOSE NODE IDENTITY IS GONE goes too. The taxiway carries the marked end, so
	//    removing it must not leave a mark keyed on a segment slot a later road can be handed.
	{
		const FGuidelineNodeId OnTy = M2HoldNodeFor(*Net, Ty, /*bEndA=*/true);
		if (!TestTrue(TEXT("Ty's X end exists"), OnTy.IsSet())) { return false; }
		TestTrue(TEXT("set on Ty's end"), Net->SetIntermediateHoldingPosition(OnTy, true));
		TestEqual(TEXT("one mark"), Net->GetHoldingPositionMarks().Num(), 1);
		Net->RemoveSegment(Ty);
		M2HoldRebuild(*Net);
		TestEqual(TEXT("mark pruned with the taxiway its node was derived for"), Net->GetHoldingPositionMarks().Num(), 0);
	}

	// 5. A TAXIWAY END THAT BECOMES A RUNWAY END: the derivation out-ranks the player's mark.
	{
		const FRoadSegmentId R2 = Net->AddStraightSegment(T, E, Runway);
		M2HoldRebuild(*Net);
		const FGuidelineNodeId End = M2HoldNodeFor(*Net, Tx, true);
		if (TestTrue(TEXT("the runway is back and the end exists"), R2.IsSet() && End.IsSet()))
		{
			TestTrue(TEXT("the end is a runway-holding position again"), M2HoldKindAt(*Net, End) == EHoldingPositionKind::Runway);
		}
	}
	return true;
}

#endif
