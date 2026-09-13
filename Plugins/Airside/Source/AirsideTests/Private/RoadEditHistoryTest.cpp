#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Build/RoadNetworkSolver.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Model/RoadSlotMap.h"
#include "Profiles/RoadProfile.h"
#include "Tool/RoadEditHistory.h"

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

#endif // WITH_DEV_AUTOMATION_TESTS
