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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHoldingPositionSurvivesRebuildTest,
	"Airside.Build.HoldingPositionSurvivesRebuild",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FHoldingPositionSurvivesRebuildTest::RunTest(const FString& Parameters)
{
	// A runway with a taxiway joining it at E. The taxiway's end node at E is the holding-position
	// candidate. The claim under test is spec §6: the flag is stored by identity and the
	// builder re-applies it, so a rebuild - which allocates FRESH nodes - keeps the bar.
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
	const FRoadSegmentId Tx = Net->AddStraightSegment(E, X, Taxiway);
	M2HoldRebuild(*Net);

	const FGuidelineNodeId Bar = M2HoldNodeFor(*Net, Tx, /*bEndA=*/true);
	if (!TestTrue(TEXT("the taxiway's E-end guideline node exists"), Bar.IsSet())) { return false; }

	// A CHAIN MEMBER, not one named segment: E joins two continuous runway segments, so
	// which of them the one-hop walk meets first is decided by the junction's arm order -
	// and both are the same runway, which is the only thing a bar can protect.
	const FRoadSegmentId Near = Net->RunwayNearGuidelineNode(Bar);
	TestTrue(TEXT("the runway near that node is the one it joins"), Net->RunwayChain(R1).Contains(Near));
	TestFalse(TEXT("the far end of the taxiway is near no runway"), Net->RunwayNearGuidelineNode(M2HoldNodeFor(*Net, Tx, false)).IsSet());

	TestTrue(TEXT("set"), Net->SetIntermediateHoldingPosition(Bar, R1));
	TestTrue(TEXT("the flag is on the node"), Net->GetGuidelineNode(Bar)->HoldingPositionFor == R1);
	TestEqual(TEXT("one mark recorded"), Net->GetHoldingPositionMarks().Num(), 1);

	M2HoldRebuild(*Net);
	TestNull(TEXT("the old node handle is dead - the builder made a fresh one"), Net->GetGuidelineNode(Bar));
	const FGuidelineNodeId BarAfter = M2HoldNodeFor(*Net, Tx, true);
	if (!TestTrue(TEXT("the fresh node exists"), BarAfter.IsSet())) { return false; }
	TestTrue(TEXT("and carries the flag"), Net->GetGuidelineNode(BarAfter)->HoldingPositionFor == R1);
	TestTrue(TEXT("and has edges - it is connected, not an orphan kept alive"), Net->GetGuidelineNode(BarAfter)->Incident.Num() > 0);

	// TWICE, because one rebuild could pass on a mark the builder consumed destructively.
	M2HoldRebuild(*Net);
	const FGuidelineNodeId BarTwice = M2HoldNodeFor(*Net, Tx, true);
	if (!TestTrue(TEXT("the node after a second rebuild exists"), BarTwice.IsSet())) { return false; }
	TestTrue(TEXT("and still carries the flag"), Net->GetGuidelineNode(BarTwice)->HoldingPositionFor == R1);

	TestTrue(TEXT("clear"), Net->SetIntermediateHoldingPosition(BarTwice, FRoadSegmentId()));
	TestFalse(TEXT("flag gone"), Net->GetGuidelineNode(BarTwice)->HoldingPositionFor.IsSet());
	TestEqual(TEXT("mark gone"), Net->GetHoldingPositionMarks().Num(), 0);

	// And a cleared bar stays cleared - the mark, not a stale node flag, is the source.
	M2HoldRebuild(*Net);
	const FGuidelineNodeId BarCleared = M2HoldNodeFor(*Net, Tx, true);
	if (!TestTrue(TEXT("the node after clearing exists"), BarCleared.IsSet())) { return false; }
	TestFalse(TEXT("still no flag after a rebuild"),
		Net->GetGuidelineNode(BarCleared)->HoldingPositionFor.IsSet());

	// A bar may only name a runway. A taxiway is not one, and refusing here is what keeps
	// the arbiter from expanding a chain that is not a strip.
	TestFalse(TEXT("a taxiway cannot be held short of"), Net->SetIntermediateHoldingPosition(BarCleared, Tx));

	// AN ORIGIN-LESS NODE IS NOT EXEMPT FROM THE PRUNE. No mark is stored for one (see
	// URoadNetwork::SetIntermediateHoldingPosition: the flag itself is the source there), so PruneHoldingPositionMarks
	// cannot reach it - and without the builder clearing it, the node would go on naming a
	// runway that has been deleted, which is a bar guarding a strip that is not there.
	{
		const FGuidelineNodeId Loose = Net->AddGuidelineNode(FVector2D(60000.0, -5000.0), /*bDerived=*/false);
		TestTrue(TEXT("an Origin-less node can be flagged"), Net->SetIntermediateHoldingPosition(Loose, R1));
		TestEqual(TEXT("but records no mark - the flag is its own source"),
			Net->GetHoldingPositionMarks().Num(), 0);

		M2HoldRebuild(*Net);
		TestTrue(TEXT("and survives a rebuild while its runway lives"),
			Net->GetGuidelineNode(Loose) != nullptr && Net->GetGuidelineNode(Loose)->HoldingPositionFor == R1);

		Net->RemoveSegment(R1);
		M2HoldRebuild(*Net);
		if (!TestNotNull(TEXT("the node itself outlives the runway - it is never swept"),
			Net->GetGuidelineNode(Loose)))
		{
			return false;
		}
		TestFalse(TEXT("but its flag is cleared with the runway it named"),
			Net->GetGuidelineNode(Loose)->HoldingPositionFor.IsSet());
	}

	// A mark whose runway was deleted is pruned rather than left pointing at nothing.
	const FRoadSegmentId R2 = Net->AddStraightSegment(T, E, Runway);
	M2HoldRebuild(*Net);
	const FGuidelineNodeId BarAgain = M2HoldNodeFor(*Net, Tx, true);
	if (!TestTrue(TEXT("the taxiway end node exists again"), BarAgain.IsSet())) { return false; }
	Net->SetIntermediateHoldingPosition(BarAgain, R2);
	Net->RemoveSegment(R2);
	M2HoldRebuild(*Net);
	TestEqual(TEXT("mark pruned with its runway"), Net->GetHoldingPositionMarks().Num(), 0);
	TestFalse(TEXT("and the flag went with it"),
		M2HoldNodeFor(*Net, Tx, true).IsSet()
			&& Net->GetGuidelineNode(M2HoldNodeFor(*Net, Tx, true))->HoldingPositionFor.IsSet());

	// A mark whose NODE identity is gone - At.Segment deleted - goes too. The taxiway is
	// what carries the flagged end, so removing it must not leave a mark keyed on a segment
	// slot that a later road can be handed.
	const FRoadSegmentId R3 = Net->AddStraightSegment(T, E, Runway);
	M2HoldRebuild(*Net);
	const FGuidelineNodeId BarOnTx = M2HoldNodeFor(*Net, Tx, true);
	if (!TestTrue(TEXT("a flagged taxiway end once more"), BarOnTx.IsSet())) { return false; }
	TestTrue(TEXT("flagged"), Net->SetIntermediateHoldingPosition(BarOnTx, R3));
	TestEqual(TEXT("one mark again"), Net->GetHoldingPositionMarks().Num(), 1);
	Net->RemoveSegment(Tx);
	M2HoldRebuild(*Net);
	TestEqual(TEXT("mark pruned with the taxiway its node was derived for"),
		Net->GetHoldingPositionMarks().Num(), 0);
	return true;
}

#endif
