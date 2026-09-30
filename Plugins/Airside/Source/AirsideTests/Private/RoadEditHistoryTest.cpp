#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "NetworkIdentity.h"
#include "Build/RoadNetworkSolver.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Model/RoadSlotMap.h"
#include "Profiles/RoadProfile.h"
#include "Present/RoadEditHistory.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadEditHistoryTest,
	"Airside.Tool.EditHistory",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadEditHistoryTest::RunTest(const FString& Parameters)
{
	URoadEditHistory* History = NewObject<URoadEditHistory>(GetTransientPackage());
	URoadNetwork* Live = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Profile = URoadProfile::MakeTransient(200.0, 100.0, 20.0);
	if (!TestNotNull(TEXT("history constructed"), History) || Live == nullptr)
	{
		return false;
	}

	TestFalse(TEXT("nothing to undo before any edit"), History->CanUndo());
	TestFalse(TEXT("nothing to redo either"), History->CanRedo());

	// An edit that refuses must leave no trace. Otherwise the player gets an undo step
	// that visibly does nothing and has to press it twice to get past.
	{
		History->BeginEdit(*Live, TEXT("refused"));
		TestTrue(TEXT("an edit is open"), History->IsEditing());
		History->AbandonEdit();
		TestFalse(TEXT("abandoning closes the edit"), History->IsEditing());
		TestEqual(TEXT("an abandoned edit pushes nothing"), History->UndoDepth(), 0);
	}

	// A SUCCESSFUL MUTATION LEFT UNCOMMITTED IS STILL ABANDONED (#125). FRoadEditScope's own
	// header says it "cannot forget to end an edit" - meaning every path out of scope ends
	// the edit one way or the other, not that it defaults to keeping it. This is exactly the
	// shape of the #125 bug: URoadEditFacade::ConnectGuidelines and DisconnectGuideline
	// mutated the live graph successfully and then fell off the end of the function with the
	// scope's Commit() never called, so the destructor took the AbandonEdit branch on a
	// change that had actually happened - no undo step, no OnChanged. Pinned here at the
	// scope's own level, world-free, rather than only through the two callers that had the
	// bug (Airside.Present.MeshRebuildsOnFacadeChange covers those directly).
	{
		// A SCRATCH network and a fresh History of its own, so this block leaves no trace on
		// Live/History for the sections below - they go on to assert exact undo/redo depths
		// and node counts that this uncommitted mutation would otherwise throw off.
		URoadNetwork* Scratch = NewObject<URoadNetwork>(GetTransientPackage());
		URoadEditHistory* ScratchHistory = NewObject<URoadEditHistory>(GetTransientPackage());
		const int32 NodesBeforeScope = Scratch->GetNodes().Num();
		{
			FRoadEditScope Edit(ScratchHistory, Scratch, TEXT("uncommitted"));
			Scratch->AddNode(FVector2D(1234.0, 5678.0));
			// Edit.Commit() DELIBERATELY NOT CALLED - the scope destructs at the closing
			// brace with the mutation already applied to Scratch, same as a caller that
			// forgot the line.
		}
		TestEqual(TEXT("an uncommitted scope pushes no undo step, even over a real mutation"),
			ScratchHistory->UndoDepth(), 0);
		TestEqual(TEXT("and the live graph is NOT rolled back - AbandonEdit discards the "
			"snapshot, it is not a rollback (see FRoadEditScope's own header)"),
			Scratch->GetNodes().Num(), NodesBeforeScope + 1);
	}

	// A committed edit, and the round trip through it.
	{
		History->BeginEdit(*Live, TEXT("place node"));
		Live->AddNode(FVector2D(100.0, 200.0));
		History->CommitEdit();

		TestEqual(TEXT("a committed edit pushes one entry"), History->UndoDepth(), 1);
		TestEqual(TEXT("the entry carries its label"),
			History->PeekUndoLabel(), FString(TEXT("place node")));

		URoadNetwork* Back = History->Undo(*Live);
		if (!TestNotNull(TEXT("undo returns a graph"), Back))
		{
			return false;
		}
		TestEqual(TEXT("the restored graph is the one from before the edit"),
			Back->GetNodes().Num(), 0);
		TestEqual(TEXT("undo empties the undo stack"), History->UndoDepth(), 0);
		TestEqual(TEXT("and fills the redo stack"), History->RedoDepth(), 1);

		// The caller adopts what undo returned, so redo is asked about THAT graph.
		Live = Back;

		URoadNetwork* Forward = History->Redo(*Live);
		if (!TestNotNull(TEXT("redo returns a graph"), Forward))
		{
			return false;
		}
		TestEqual(TEXT("redo brings the node back"), Forward->GetNodes().Num(), 1);
		TestEqual(TEXT("redo refills the undo stack"), History->UndoDepth(), 1);
		Live = Forward;
	}

	// Editing after an undo abandons the future. Keeping it would let a redo graft a
	// state onto a graph that is no longer underneath it.
	{
		URoadNetwork* Back = History->Undo(*Live);
		Live = Back != nullptr ? Back : Live;
		TestEqual(TEXT("a redo is waiting"), History->RedoDepth(), 1);

		History->BeginEdit(*Live, TEXT("a different edit"));
		Live->AddNode(FVector2D(-500.0, 0.0));
		History->CommitEdit();

		TestEqual(TEXT("a new edit discards the redo future"), History->RedoDepth(), 0);
	}

	// The solver's own output must survive a snapshot VERBATIM. Asserted here rather than
	// through the actor, because adopting a restored graph there ends in a rebuild that
	// would recompute these and hide whether the snapshot ever carried them.
	{
		History->Clear();

		URoadNetwork* Solved = NewObject<URoadNetwork>(GetTransientPackage());
		const FRoadNodeId Left = Solved->AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId Right = Solved->AddNode(FVector2D(5000.0, 0.0));
		const FRoadSegmentId Span = Solved->AddStraightSegment(Left, Right, Profile);
		FRoadNetworkSolver::SolveAll(*Solved);

		const FRoadSegment Before = Solved->GetSegments()[0];
		TestTrue(TEXT("the fixture really was solved"), Before.bSolvedA && Before.bSolvedB);

		History->BeginEdit(*Solved, TEXT("delete segment"));
		TestTrue(TEXT("the fixture segment removes"), Solved->RemoveSegment(Span));
		History->CommitEdit();

		URoadNetwork* Back = History->Undo(*Solved);
		if (!TestNotNull(TEXT("undo returns the solved graph"), Back))
		{
			return false;
		}

		// Bitwise, the same discipline the weld contract uses. These cut vertices are
		// shared bit for bit with the junction mesh, so an undo that re-derived them
		// instead of restoring them would reopen the seam by a hair - not by a mile - and
		// a tolerance here would report success on exactly that.
		const FRoadSegment& After = Back->GetSegments()[0];
		TestTrue(TEXT("the A cut vertices come back bitwise"),
			After.LeftCutA == Before.LeftCutA && After.RightCutA == Before.RightCutA);
		TestTrue(TEXT("the B cut vertices come back bitwise"),
			After.LeftCutB == Before.LeftCutB && After.RightCutB == Before.RightCutB);
		TestTrue(TEXT("the trims come back"),
			After.TrimA == Before.TrimA && After.TrimB == Before.TrimB);

		// Generation identity, which is the property spec 7.3 warns about and the command
		// layer built to it broke twice: its create command reverted by removing, which
		// bumps the counter, so every outstanding handle silently went stale.
		TestEqual(TEXT("the segment's generation survives"), After.Generation, Before.Generation);
		TestTrue(TEXT("the ORIGINAL segment handle resolves against the restored graph"),
			RoadSlot::IsValid<FRoadSegmentId, FRoadSegment>(Back->GetSegments(), Span));
	}

	// The stack is capped, and drops the OLDEST - the states nobody is coming back to.
	{
		History->Clear();
		History->MaxDepth = 3;

		URoadNetwork* Counting = NewObject<URoadNetwork>(GetTransientPackage());
		for (int32 Round = 0; Round < 6; ++Round)
		{
			History->BeginEdit(*Counting, FString::Printf(TEXT("edit %d"), Round));
			Counting->AddNode(FVector2D(Round * 100.0, 0.0));
			History->CommitEdit();
		}

		TestEqual(TEXT("the stack is capped at MaxDepth"), History->UndoDepth(), 3);
		TestEqual(TEXT("and the newest edit is the one on top"),
			History->PeekUndoLabel(), FString(TEXT("edit 5")));
	}

	return true;
}

// ---------------------------------------------------------------------------------------
// Issue #437: THE SCOPE CAN ROLL BACK, AND DOES NOT NEED AN UNDO HISTORY TO.
namespace
{
	/** Roads, a derived guideline graph and a drive side that is not the default: every group of
	 *  fields a restore has to bring back is holding something. */
	URoadNetwork* MakeRollbackSubject()
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		FRoadCrossingFixture::Lay(*Net, /*bFarSide*/ true);
		TestGraph::Derive(*Net);
		Net->SetDriveSide(EDriveSide::Left);
		return Net;
	}

	/** What a mutator does to a network before it refuses: a node added, a segment torn out, a
	 *  guideline edge dropped, the drive side flipped - four groups, none of them notified. */
	void WriteThenRefuse(URoadNetwork& Net)
	{
		Net.AddNode(FVector2D(123.0, 456.0));
		for (int32 Index = 0; Index < Net.GetSegments().Num(); ++Index)
		{
			const FRoadSegmentId Segment = Net.SegmentIdAt(Index);
			if (Segment.IsSet()) { Net.RemoveSegment(Segment); break; }
		}
		for (int32 Index = 0; Index < Net.GetGuidelineEdges().Num(); ++Index)
		{
			const FGuidelineEdgeId Edge = Net.GuidelineEdgeIdAt(Index);
			if (Edge.IsSet()) { Net.RemoveGuidelineEdge(Edge); break; }
		}
		Net.SetDriveSide(EDriveSide::Right);
	}
}

/**
 * A ROLLED-BACK SCOPE PUTS EVERY FIELD BACK, BITWISE, WITH OR WITHOUT AN UNDO HISTORY. Run both
 * ways because the two hold their snapshot in different places (the history's pending one, the
 * scope's own) and the editor world - no history - is the one that had no way back at all.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEditScopeRollbackTest,
	"Airside.Present.EditScopeRollsBackBitwise",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FEditScopeRollbackTest::RunTest(const FString& Parameters)
{
	for (const bool bWithHistory : { true, false })
	{
		const FString Way = bWithHistory ? TEXT("with a history") : TEXT("with no history");
		URoadNetwork* Net = MakeRollbackSubject();
		URoadNetwork* Reference = DuplicateObject<URoadNetwork>(Net, GetTransientPackage());
		URoadEditHistory* History = bWithHistory ? NewObject<URoadEditHistory>(GetTransientPackage()) : nullptr;

		// THE CONTROL: the writes below really do change every group, so an assertion that the
		// model came back is not one a scope that restored nothing would pass.
		{
			URoadNetwork* Written = DuplicateObject<URoadNetwork>(Net, GetTransientPackage());
			WriteThenRefuse(*Written);
			TestTrue(FString::Printf(TEXT("control (%s): the writes change the model"), *Way),
				NetworkIdentity::DifferingProperties(*Written, *Reference).Num() >= 3);
		}

		const uint32 EditRevisionBefore = Net->GetEditRevision();
		{
			FRoadEditScope Edit(History, Net, TEXT("refused after it wrote"));
			WriteThenRefuse(*Net);
			TestTrue(FString::Printf(TEXT("%s: the scope reports it rolled back"), *Way), Edit.Rollback());
		}

		const TArray<FString> Differing = NetworkIdentity::DifferingProperties(*Net, *Reference);
		TestEqual(FString::Printf(TEXT("%s: the model is BITWISE what it was (differs in: %s)"), *Way,
			*FString::Join(Differing, TEXT(", "))), Differing.Num(), 0);
		if (History != nullptr)
		{
			TestEqual(TEXT("with a history: a rolled-back edit is not an undo step"), History->UndoDepth(), 0);
			TestFalse(TEXT("and the history's edit is over - a new one may begin"), History->IsEditing());
		}
		TestTrue(FString::Printf(TEXT("%s: the revision clock moved FORWARD, so nothing stamped during the failed edit can match"), *Way),
			Net->GetEditRevision() > EditRevisionBefore);
		TestFalse(FString::Printf(TEXT("%s: the restored guideline graph is not behind the restored road"), *Way),
			Net->AreGuidelinesBehindRoad());
	}
	return true;
}

/**
 * A SCOPE THAT IS NOT ROLLED BACK IS UNCHANGED, and one with nothing to roll back says so: the
 * explicit-Rollback contract RoadEditHistoryTest's "uncommitted scope restores nothing" pins from the
 * other side, an inert scope, and a second call after the edit ended.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEditScopeRollbackEdgesTest,
	"Airside.Present.EditScopeRollbackEdges",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FEditScopeRollbackEdgesTest::RunTest(const FString& Parameters)
{
	{
		FRoadEditScope Inert(nullptr, nullptr, TEXT("nothing to edit"));
		TestFalse(TEXT("a scope with no network has nothing to roll back"), Inert.Rollback());
	}
	{
		URoadNetwork* Net = MakeRollbackSubject();
		URoadEditHistory* History = NewObject<URoadEditHistory>(GetTransientPackage());
		FRoadEditScope Edit(History, Net, TEXT("twice"));
		WriteThenRefuse(*Net);
		TestTrue(TEXT("the first Rollback restores"), Edit.Rollback());
		TestFalse(TEXT("the second has no edit left to roll back"), Edit.Rollback());
	}
	{
		// A COMMITTED scope keeps its write: Rollback is the failure path, not a default.
		URoadNetwork* Net = MakeRollbackSubject();
		URoadEditHistory* History = NewObject<URoadEditHistory>(GetTransientPackage());
		const int32 NodesBefore = Net->GetNodes().Num();
		{
			FRoadEditScope Edit(History, Net, TEXT("kept"));
			Net->AddNode(FVector2D(1.0, 2.0));
			Edit.Commit();
		}
		TestEqual(TEXT("a committed scope keeps its node"), Net->GetNodes().Num(), NodesBefore + 1);
		TestEqual(TEXT("and pushes its undo step"), History->UndoDepth(), 1);
	}
	return true;
}

/**
 * RestoreFrom MOVES THE CLOCKS FORWARD (issue #318's "revision clocks restart at zero", closed for
 * the restore path). A snapshot is a DuplicateObject clone, so its non-UPROPERTY clocks read zero; a
 * cache that stamped itself at revision R during the failed edit must not meet R again afterwards.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRestoreFromClocksTest,
	"Airside.Model.RestoreFromMovesClocksForward",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRestoreFromClocksTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = MakeRollbackSubject();
	URoadNetwork* Snapshot = DuplicateObject<URoadNetwork>(Net, GetTransientPackage());
	TestEqual(TEXT("control: a duplicate starts its clocks at zero"), Snapshot->GetEditRevision(), 0u);

	WriteThenRefuse(*Net);
	const uint32 EditSeen = Net->GetEditRevision();
	const uint32 GuidelineSeen = Net->GetGuidelineRevision();
	Net->RestoreFrom(*Snapshot);

	TestTrue(TEXT("the road clock is past everything seen during the failed edit"), Net->GetEditRevision() > EditSeen);
	TestTrue(TEXT("and so is the guideline clock"), Net->GetGuidelineRevision() > GuidelineSeen);

	// Restoring AGAIN, from the same snapshot, still moves them: the clocks never repeat.
	const uint32 EditAfterFirst = Net->GetEditRevision();
	Net->RestoreFrom(*Snapshot);
	TestTrue(TEXT("a second restore moves the road clock again"), Net->GetEditRevision() > EditAfterFirst);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
