#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "NetworkIdentity.h"
#include "Present/RoadEditFacade.h"
#include "Present/RoadEditHistory.h"
#include "Present/RoadNetworkActor.h"
#include "Model/TaxiwayStrip.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	EHoldingPositionKind M2HoldKindAt(const URoadNetwork& Net, FGuidelineNodeId Node)
	{
		const FGuidelineNode* Found = Net.GetGuidelineNode(Node);
		return Found ? Found->HoldingPosition : EHoldingPositionKind::None;
	}

	/** The flagged node realising a mark on Segment's B end, wherever the builder put it. */
	FGuidelineNodeId M2HoldRealisedAt(const URoadNetwork& Net, FRoadSegmentId Segment)
	{
		const TArray<FGuidelineNode>& Nodes = Net.GetGuidelineNodes();
		for (int32 Index = 0; Index < Nodes.Num(); ++Index)
		{
			if (Nodes[Index].bAlive && Nodes[Index].Origin.Segment == Segment && !Nodes[Index].Origin.bEndA
				&& Nodes[Index].HoldingPosition == EHoldingPositionKind::Intermediate)
			{
				return Net.GuidelineNodeIdAt(Index);
			}
		}
		return FGuidelineNodeId();
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
	URoadProfile* Runway = TestProfiles::Runway();
	URoadProfile* Taxiway = TestProfiles::Taxiway();
	const FRoadNodeId T = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId E = Net->AddNode(FVector2D(60000.0, 0.0));
	const FRoadNodeId F = Net->AddNode(FVector2D(100000.0, 0.0));
	const FRoadSegmentId R1 = Net->AddStraightSegment(T, E, Runway);
	Net->AddStraightSegment(E, F, Runway);
	const FRoadNodeId X = Net->AddNode(FVector2D(60000.0, -20000.0));
	const FRoadNodeId Y = Net->AddNode(FVector2D(80000.0, -20000.0));
	const FRoadSegmentId Tx = Net->AddStraightSegment(E, X, Taxiway);
	const FRoadSegmentId Ty = Net->AddStraightSegment(X, Y, Taxiway);
	TestGraph::Rebuild(*Net);

	// 1. THE RUNWAY END IS DERIVED and refuses the player.
	const FGuidelineNodeId RunwayEnd = TestGraph::NodeFor(*Net, Tx, /*bEndA=*/true);
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
	const FGuidelineNodeId Junction = TestGraph::NodeFor(*Net, Tx, /*bEndA=*/false);
	if (!TestTrue(TEXT("the taxiway's X-end guideline node exists"), Junction.IsSet())) { return false; }
	TestTrue(TEXT("a taxiway junction node carries nothing by itself"),
		M2HoldKindAt(*Net, Junction) == EHoldingPositionKind::None);
	TestTrue(TEXT("set"), Net->SetIntermediateHoldingPosition(Junction, true));
	TestTrue(TEXT("the node is an intermediate position"), M2HoldKindAt(*Net, Junction) == EHoldingPositionKind::Intermediate);
	TestFalse(TEXT("protecting nothing - no runway is named"), Net->GetGuidelineNode(Junction)->HoldingPositionFor.IsSet());
	TestEqual(TEXT("one mark recorded"), Net->GetHoldingPositionMarks().Num(), 1);

	// SINCE 2026-09-29 (taxiway strip stage 4) the rebuild REALISES the mark at the strip edge
	// of the taxiway X-Y that Tx joins - a node split down Tx, carrying the end's Origin - not
	// on the end itself; M2HoldRealisedAt finds it by the end it names.
	TestGraph::Rebuild(*Net);
	TestNull(TEXT("the old node handle is dead - the builder made a fresh one"), Net->GetGuidelineNode(Junction));
	const FGuidelineNodeId JunctionAfter = M2HoldRealisedAt(*Net, Tx);
	if (!TestTrue(TEXT("the fresh node exists"), JunctionAfter.IsSet())) { return false; }
	TestTrue(TEXT("and carries the intermediate position"), M2HoldKindAt(*Net, JunctionAfter) == EHoldingPositionKind::Intermediate);
	TestTrue(TEXT("and has edges - it is connected, not an orphan kept alive"), Net->GetGuidelineNode(JunctionAfter)->Incident.Num() > 0);
	TestTrue(TEXT("and the runway end is still derived after the rebuild"),
		M2HoldKindAt(*Net, TestGraph::NodeFor(*Net, Tx, true)) == EHoldingPositionKind::Runway);

	// TWICE, because one rebuild could pass on a mark the builder consumed destructively.
	TestGraph::Rebuild(*Net);
	const FGuidelineNodeId JunctionTwice = M2HoldRealisedAt(*Net, Tx);
	if (!TestTrue(TEXT("the node after a second rebuild exists"), JunctionTwice.IsSet())) { return false; }
	TestTrue(TEXT("and still carries it"), M2HoldKindAt(*Net, JunctionTwice) == EHoldingPositionKind::Intermediate);

	TestTrue(TEXT("clear"), Net->SetIntermediateHoldingPosition(JunctionTwice, false));
	TestTrue(TEXT("kind gone"), M2HoldKindAt(*Net, JunctionTwice) == EHoldingPositionKind::None);
	TestEqual(TEXT("mark gone"), Net->GetHoldingPositionMarks().Num(), 0);

	// And a cleared position stays cleared - the mark, not a stale node flag, is the source.
	TestGraph::Rebuild(*Net);
	const FGuidelineNodeId JunctionCleared = TestGraph::NodeFor(*Net, Tx, false);
	if (!TestTrue(TEXT("the node after clearing exists"), JunctionCleared.IsSet())) { return false; }
	TestTrue(TEXT("still nothing after a rebuild"), M2HoldKindAt(*Net, JunctionCleared) == EHoldingPositionKind::None);

	// 3. AN ORIGIN-LESS NODE: no mark is stored (the node is its own source), and an
	//    intermediate position there survives every rebuild by itself.
	{
		const FGuidelineNodeId Loose = Net->AddGuidelineNode(FVector2D(70000.0, -20000.0), /*bDerived=*/false);
		TestTrue(TEXT("an Origin-less node can carry an intermediate position"), Net->SetIntermediateHoldingPosition(Loose, true));
		TestEqual(TEXT("but records no mark - the node is its own source"), Net->GetHoldingPositionMarks().Num(), 0);
		TestGraph::Rebuild(*Net);
		TestTrue(TEXT("and it survives a rebuild"), M2HoldKindAt(*Net, Loose) == EHoldingPositionKind::Intermediate);

		// A RUNWAY kind on an Origin-less node (a hand-built fixture's, via the test setter)
		// IS pruned when its runway goes: the one case the mark prune cannot reach.
		const FGuidelineNodeId LooseRunway = Net->AddGuidelineNode(FVector2D(60000.0, -5000.0), /*bDerived=*/false);
		TestTrue(TEXT("a runway kind can be set on it for a test"), Net->SetRunwayHoldingPositionForTest(LooseRunway, R1));
		Net->RemoveSegment(R1);
		TestGraph::Rebuild(*Net);
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
		const FGuidelineNodeId OnTy = TestGraph::NodeFor(*Net, Ty, /*bEndA=*/true);
		if (!TestTrue(TEXT("Ty's X end exists"), OnTy.IsSet())) { return false; }
		TestTrue(TEXT("set on Ty's end"), Net->SetIntermediateHoldingPosition(OnTy, true));
		TestEqual(TEXT("one mark"), Net->GetHoldingPositionMarks().Num(), 1);
		Net->RemoveSegment(Ty);
		TestGraph::Rebuild(*Net);
		TestEqual(TEXT("mark pruned with the taxiway its node was derived for"), Net->GetHoldingPositionMarks().Num(), 0);
	}

	// 5. A TAXIWAY END THAT BECOMES A RUNWAY END: the derivation out-ranks the player's mark.
	{
		const FRoadSegmentId R2 = Net->AddStraightSegment(T, E, Runway);
		TestGraph::Rebuild(*Net);
		const FGuidelineNodeId End = TestGraph::NodeFor(*Net, Tx, true);
		if (TestTrue(TEXT("the runway is back and the end exists"), R2.IsSet() && End.IsSet()))
		{
			TestTrue(TEXT("the end is a runway-holding position again"), M2HoldKindAt(*Net, End) == EHoldingPositionKind::Runway);
		}
	}
	return true;
}

/**
 * AN INTERMEDIATE HOLD SITS AT THE STRIP EDGE OF THE TAXIWAY IT JOINS (taxiway strip spec
 * 2026-09-28, "Holds sit at the strip edge"; stage 4): where ICAO puts one, clear of a wing
 * passing on the joined taxiway. Realised by SPLITTING the stem's guideline there, not by
 * moving its end - the end is where the junction's turns attach, and moving it would change
 * every turn at the junction.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FIntermediateHoldAtStripEdgeTest,
	"Airside.Build.IntermediateHoldAtStripEdge",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FIntermediateHoldAtStripEdgeTest::RunTest(const FString& Parameters)
{
	//   W ======= J ======= E        taxiway (code E)
	//             |
	//             S                  stem taxiway, joining at J square
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Taxiway = TestProfiles::Taxiway();
	Net->DefaultProfile = Taxiway;
	const FRoadNodeId J = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadSegmentId West = Net->AddStraightSegment(Net->AddNode(FVector2D(-20000.0, 0.0)), J, Taxiway);
	Net->AddStraightSegment(J, Net->AddNode(FVector2D(20000.0, 0.0)), Taxiway);
	const FRoadSegmentId Stem = Net->AddStraightSegment(Net->AddNode(FVector2D(0.0, -20000.0)), J, Taxiway);
	TestGraph::Rebuild(*Net);

	const double StripEdge = Taxiway->GetTotalWidth() * 0.5 + TaxiwayStrip::StripWidthOf(*Net, West);
	const FGuidelineNodeId EndBefore = TestGraph::NodeFor(*Net, Stem, /*bEndA=*/false);
	if (!TestTrue(TEXT("the stem's junction end exists"), EndBefore.IsSet())) { return false; }
	const FVector2D EndAt = Net->GetGuidelineNode(EndBefore)->Position;
	TestTrue(TEXT("the end sits inside the strip - at the cut line, where the hold used to be"), FMath::Abs(EndAt.Y) < StripEdge);

	TestTrue(TEXT("set"), Net->SetIntermediateHoldingPosition(EndBefore, true));

	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		// TWICE, because the first rebuild could pass on a mark the builder consumed.
		TestGraph::Rebuild(*Net);

		TArray<FGuidelineNodeId> Holds;
		const TArray<FGuidelineNode>& Nodes = Net->GetGuidelineNodes();
		for (int32 Index = 0; Index < Nodes.Num(); ++Index)
		{
			if (Nodes[Index].bAlive && Nodes[Index].HoldingPosition == EHoldingPositionKind::Intermediate)
			{
				Holds.Add(Net->GuidelineNodeIdAt(Index));
			}
		}
		if (!TestEqual(*FString::Printf(TEXT("rebuild %d: one intermediate position"), Pass + 1), Holds.Num(), 1)) { return false; }
		const FGuidelineNode* Hold = Net->GetGuidelineNode(Holds[0]);

		// (half width + strip) / sin(90) of the JOINED taxiway, down the stem from the junction.
		TestNearlyEqual(*FString::Printf(TEXT("rebuild %d: the hold is at the joined taxiway's strip edge (%.1f)"),
			Pass + 1, -Hold->Position.Y), -Hold->Position.Y, StripEdge, 1.0);
		TestNearlyEqual(TEXT("on the stem's centreline"), Hold->Position.X, 0.0, 1.0);
		TestTrue(TEXT("naming the stem end the player marked, so a click on it clears that mark"),
			Hold->Origin.Segment == Stem && !Hold->Origin.bEndA);

		// THE END DID NOT MOVE, and the turns still attach there.
		const FGuidelineNodeId End = TestGraph::NodeFor(*Net, Stem, /*bEndA=*/false);
		const FGuidelineNode* EndNode = Net->GetGuidelineNode(End);
		if (!TestNotNull(TEXT("the stem end still exists"), EndNode)) { return false; }
		TestTrue(TEXT("and is not the hold"), End != Holds[0]);
		TestTrue(TEXT("and has not moved"), EndNode->Position.Equals(EndAt, 0.01));
		int32 Turns = 0;
		for (const FGuidelineEdgeId Id : EndNode->Incident)
		{
			const FGuidelineEdge* Edge = Net->GetGuidelineEdge(Id);
			Turns += (Edge != nullptr && !Edge->DerivedFrom.IsSet()) ? 1 : 0;
		}
		TestTrue(TEXT("and the junction's turns still attach to it"), Turns > 0);
	}
	TestEqual(TEXT("still one mark, on the end - where it is realised moved, what it names did not"),
		Net->GetHoldingPositionMarks().Num(), 1);
	return true;
}

// ---------------------------------------------------------------------------------------
// Issue #437: THE FACADE NO LONGER RE-TYPES THE MODEL'S TWO REFUSALS. URoadEditFacade::
// SetIntermediateHoldingPosition used to check "runway-holding" and "a road's stop line at a taxiway
// crossing" itself, ABOVE its edit scope, because a scope could not roll back and a refusal inside
// one had to be a refusal that had written nothing. The scope can roll back now, so the facade asks
// the model once and undoes whatever it wrote on a no. This is the pin for what the two hoisted
// checks did: a derived position refuses the player, from either world, changing nothing and
// leaving no undo step - and one that IS the player's still lands, so the refusals are not simply a
// dead path.
namespace
{
	/** The first live guideline node carrying Kind, or unset. */
	FGuidelineNodeId FirstHoldingOfKind(const URoadNetwork& Net, EHoldingPositionKind Kind)
	{
		const TArray<FGuidelineNode>& Nodes = Net.GetGuidelineNodes();
		for (int32 Index = 0; Index < Nodes.Num(); ++Index)
		{
			if (Nodes[Index].bAlive && Nodes[Index].HoldingPosition == Kind)
			{
				return Net.GuidelineNodeIdAt(Index);
			}
		}
		return FGuidelineNodeId();
	}

	bool RunDerivedHoldingRefusedThroughTheFacade(FAutomationTestBase& T, EWorldType::Type WorldType)
	{
		FAirsideTestWorld World(/*bSpawnActor*/ true, WorldType);
		if (!T.TestNotNull(TEXT("a world"), World.Actor)) { return false; }
		ARoadNetworkActor* Actor = World.Actor;

		// One case per derived kind: the runway's, and a road's stop line at a taxiway crossing.
		for (const EHoldingPositionKind Derived : { EHoldingPositionKind::Runway, EHoldingPositionKind::TaxiwayCrossing })
		{
			Actor->ClearNetwork();
			URoadNetwork& Net = *Actor->Network;
			if (Derived == EHoldingPositionKind::Runway)
			{
				//   T ======= E ======= F        runway;  E - X taxiway: its E end is runway-holding
				URoadProfile* Runway = TestProfiles::Runway();
				URoadProfile* Taxiway = TestProfiles::Taxiway();
				const FRoadNodeId Threshold = Net.AddNode(FVector2D(0.0, 0.0));
				const FRoadNodeId E = Net.AddNode(FVector2D(60000.0, 0.0));
				const FRoadNodeId Far = Net.AddNode(FVector2D(100000.0, 0.0));
				Net.AddStraightSegment(Threshold, E, Runway);
				Net.AddStraightSegment(E, Far, Runway);
				const FRoadNodeId X = Net.AddNode(FVector2D(60000.0, -20000.0));
				Net.AddStraightSegment(E, X, Taxiway);
				TestGraph::Rebuild(Net);
			}
			else
			{
				FRoadCrossingFixture::Lay(Net, /*bFarSide*/ true);
				TestGraph::Derive(Net);
			}
			const FGuidelineNodeId Node = FirstHoldingOfKind(Net, Derived);
			if (!T.TestTrue(FString::Printf(TEXT("the layout has a derived holding position (kind %d)"), static_cast<int32>(Derived)),
				Node.IsSet())) { return false; }

			URoadNetwork* Before = DuplicateObject<URoadNetwork>(&Net, GetTransientPackage());
			const int32 DepthBefore = Actor->History != nullptr ? Actor->History->UndoDepth() : 0;
			T.TestFalse(TEXT("the player cannot set over a derived position"), Actor->SetIntermediateHoldingPosition(Node.Index, true));
			T.TestFalse(TEXT("nor clear it"), Actor->SetIntermediateHoldingPosition(Node.Index, false));
			const TArray<FString> Differing = NetworkIdentity::DifferingProperties(*Actor->Network, *Before);
			T.TestEqual(FString::Printf(TEXT("the model is BITWISE what it was (differs in: %s)"),
				*FString::Join(Differing, TEXT(", "))), Differing.Num(), 0);
			T.TestEqual(TEXT("and no undo step was pushed"), Actor->History != nullptr ? Actor->History->UndoDepth() : 0, DepthBefore);
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDerivedHoldingRefusedThroughFacadeTest,
	"Airside.Present.DerivedHoldingRefusedThroughFacade",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDerivedHoldingRefusedThroughFacadeTest::RunTest(const FString& Parameters)
{
	return RunDerivedHoldingRefusedThroughTheFacade(*this, EWorldType::Game)
		&& RunDerivedHoldingRefusedThroughTheFacade(*this, EWorldType::Editor);
}

#endif
