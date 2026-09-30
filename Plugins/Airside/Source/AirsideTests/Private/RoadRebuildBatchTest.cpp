#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadEditFacade.h"
#include "Present/RoadNetworkActor.h"
#include "Tool/RoadEditTarget.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------------------
// REBUILD BATCHES (FRoadRebuildBatch, URoadEditFacade's class comment): N mutations, ONE
// derived rebuild at the outermost close. Measured at the composition - a NewObject actor and
// the facade it owns, the same fixture Airside.Present.DragNotifiesGeometryOnly uses - because
// the seam under test is NotifyChanged's fold and the actor's OnChanged handler it defers,
// neither of which a model-level test could see. TopologyRebuildCountForTest stands in for
// "the guideline/anchor/plots/traffic pass ran" (see that test's banner for why one counter
// answers for all four); RebuildCountForTest for "OnChanged reached the actor at all".
//
// The world-level proof that a batched build is IDENTICAL to an unbatched one is
// AirportMgr.RigCourse.BatchedLayMatchesUnbatched, in the game module, beside the course it
// lays.
// ---------------------------------------------------------------------------------------

// NAMED, NOT ANONYMOUS: the test module is a unity build.
namespace RebuildBatchTest
{
	/** Every kind OnChanged broadcast while this is alive, in order - the kind is the contract,
	 *  and the actor's counters only say how many and whether any was Topology. */
	struct FKindRecorder
	{
		URoadEditFacade& Facade;
		TArray<EChangeKind> Kinds;
		FDelegateHandle Handle;

		explicit FKindRecorder(URoadEditFacade& InFacade) : Facade(InFacade)
		{
			Handle = Facade.OnChanged.AddLambda([this](EChangeKind Kind) { Kinds.Add(Kind); });
		}
		~FKindRecorder() { Facade.OnChanged.Remove(Handle); }
	};

	ARoadNetworkActor* MakeActor()
	{
		return NewObject<ARoadNetworkActor>(GetTransientPackage());
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRebuildBatchFoldsManyIntoOneTest,
	"Airside.Present.RebuildBatch.FoldsManyIntoOne",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRebuildBatchFoldsManyIntoOneTest::RunTest(const FString& Parameters)
{
	using namespace RebuildBatchTest;
	ARoadNetworkActor* Actor = MakeActor();
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
	URoadEditFacade* Facade = Actor->GetEditFacade();
	if (!TestNotNull(TEXT("the actor has a facade"), Facade)) { return false; }

	// THE CONTROL, unbatched: each PlaceNode/ConnectNodes runs the pass once. Without it, "one"
	// below could pass on a pipeline that had stopped running at all.
	const int32 TopologyBeforeControl = Actor->TopologyRebuildCountForTest();
	const int32 C0 = Actor->PlaceNode(FVector2D(-50000.0, 0.0));
	const int32 C1 = Actor->PlaceNode(FVector2D(-40000.0, 0.0));
	Actor->ConnectNodes(C0, C1);
	TestEqual(TEXT("unbatched, three edits run the derived pass three times - the cost a batch removes"),
		Actor->TopologyRebuildCountForTest(), TopologyBeforeControl + 3);

	const int32 RebuildsBefore = Actor->RebuildCountForTest();
	const int32 TopologyBefore = Actor->TopologyRebuildCountForTest();
	const int32 NodesBefore = Actor->Network->GetNodes().Num();
	{
		FKindRecorder Recorder(*Facade);
		{
			FRoadRebuildBatch Batch(*Actor);
			TestTrue(TEXT("the facade reports the batch open while the guard lives"), Facade->IsRebuildBatchOpen());
			int32 Previous = Actor->PlaceNode(FVector2D(0.0, 0.0));
			for (int32 Step = 1; Step <= 4; ++Step)
			{
				const int32 Next = Actor->PlaceNode(FVector2D(Step * 10000.0, (Step % 2) * 3000.0));
				TestTrue(TEXT("each connect inside the batch still succeeds - a batch defers the rebuild, never the edit"),
					Actor->ConnectNodes(Previous, Next));
				Previous = Next;
			}

			// THE MODEL IS LIVE, THE DERIVED STATE IS NOT: the nodes exist now, the rebuild has not run.
			TestEqual(TEXT("the model took all five nodes at once - only derived state waits"),
				Actor->Network->GetNodes().Num(), NodesBefore + 5);
			TestEqual(TEXT("nine edits inside an open batch reached the actor zero times"),
				Actor->RebuildCountForTest(), RebuildsBefore);
			TestEqual(TEXT("and ran the derived pass zero times"),
				Actor->TopologyRebuildCountForTest(), TopologyBefore);
		}
		TestFalse(TEXT("the batch is closed once the guard is gone"), Facade->IsRebuildBatchOpen());

		// THE MEASUREMENT - goes red if NotifyChanged broadcasts inside a batch (nine rebuilds)
		// or the close forgets its notify (zero).
		TestEqual(TEXT("nine edits in one batch reach the actor exactly once, at the close"),
			Actor->RebuildCountForTest(), RebuildsBefore + 1);
		TestEqual(TEXT("and run the derived pass exactly once"),
			Actor->TopologyRebuildCountForTest(), TopologyBefore + 1);
		TestEqual(TEXT("one broadcast, so every OnChanged listener (the runway cache too) sees one"),
			Recorder.Kinds.Num(), 1);
		TestTrue(TEXT("and it is Topology - placing and connecting changed the graph's shape"),
			Recorder.Kinds.Num() == 1 && Recorder.Kinds[0] == EChangeKind::Topology);
	}

	// AN EMPTY BATCH COSTS NOTHING, and neither does one whose every edit was refused: a
	// refusal notifies nothing unbatched, so the batch has nothing to fold.
	const int32 RebuildsBeforeEmpty = Actor->RebuildCountForTest();
	{
		FRoadRebuildBatch Batch(*Actor);
		TestFalse(TEXT("connecting a node to itself is refused"), Actor->ConnectNodes(C0, C0));
	}
	TestEqual(TEXT("a batch that folded nothing broadcasts nothing"),
		Actor->RebuildCountForTest(), RebuildsBeforeEmpty);

	// UNDO INSIDE A BATCH is a network swap (AdoptNetwork), folded like any other notify; the
	// close rebuilds from the network as it THEN is.
	const int32 NodesBeforeUndo = Actor->Network->GetNodes().Num();
	const int32 TopologyBeforeUndo = Actor->TopologyRebuildCountForTest();
	{
		FRoadRebuildBatch Batch(*Actor);
		Actor->PlaceNode(FVector2D(90000.0, 90000.0));
		TestTrue(TEXT("undo works inside a batch - history is untouched by it"), Facade->Undo());
	}
	TestEqual(TEXT("the undo inside the batch took the node back out"),
		Actor->Network->GetNodes().Num(), NodesBeforeUndo);
	TestEqual(TEXT("place + undo in one batch is still one derived pass"),
		Actor->TopologyRebuildCountForTest(), TopologyBeforeUndo + 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRebuildBatchNestedClosesOnceTest,
	"Airside.Present.RebuildBatch.NestedClosesOnce",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRebuildBatchNestedClosesOnceTest::RunTest(const FString& Parameters)
{
	using namespace RebuildBatchTest;
	ARoadNetworkActor* Actor = MakeActor();
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
	URoadEditFacade* Facade = Actor->GetEditFacade();
	if (!TestNotNull(TEXT("the actor has a facade"), Facade)) { return false; }

	const int32 TopologyBefore = Actor->TopologyRebuildCountForTest();
	{
		FRoadRebuildBatch Outer(*Actor);
		const int32 A = Actor->PlaceNode(FVector2D(0.0, 0.0));
		{
			FRoadRebuildBatch Inner(*Actor);
			const int32 B = Actor->PlaceNode(FVector2D(10000.0, 0.0));
			Actor->ConnectNodes(A, B);
		}
		// THE COUNT, NOT A BOOL: an inner close that rebuilt would show here.
		TestTrue(TEXT("the outer batch is still open after the inner one closes"), Facade->IsRebuildBatchOpen());
		TestEqual(TEXT("closing the INNER batch rebuilds nothing - only the outermost close does"),
			Actor->TopologyRebuildCountForTest(), TopologyBefore);
		Actor->PlaceNode(FVector2D(20000.0, 0.0));
	}
	TestEqual(TEXT("two nested batches, four edits, one derived pass - at the outermost close"),
		Actor->TopologyRebuildCountForTest(), TopologyBefore + 1);

	// UNMATCHED END: refused with an Error, and must not drive the depth negative - if it did,
	// the NEXT batch's Begin would leave the depth at zero and its edits would not be folded.
	AddExpectedError(TEXT("EndRebuildBatch with no batch open"), EAutomationExpectedErrorFlags::Contains, 1);
	Facade->EndRebuildBatch();
	const int32 TopologyBeforeAfterUnmatched = Actor->TopologyRebuildCountForTest();
	{
		FRoadRebuildBatch Batch(*Actor);
		Actor->PlaceNode(FVector2D(30000.0, 0.0));
		Actor->PlaceNode(FVector2D(40000.0, 0.0));
		TestEqual(TEXT("after a refused unmatched End, the next batch still folds"),
			Actor->TopologyRebuildCountForTest(), TopologyBeforeAfterUnmatched);
	}
	TestEqual(TEXT("and still closes once"),
		Actor->TopologyRebuildCountForTest(), TopologyBeforeAfterUnmatched + 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRebuildBatchKindIsCombinedTest,
	"Airside.Present.RebuildBatch.KindIsCombined",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRebuildBatchKindIsCombinedTest::RunTest(const FString& Parameters)
{
	using namespace RebuildBatchTest;

	// THE TABLE ITSELF, world-free. Geometry + Markings is the one pair that is NOT the max of
	// the enum order: each skips what the other paints, so only Topology covers both.
	TestTrue(TEXT("Geometry twice is Geometry - a drag frame's surface-only rebuild"),
		CombineChangeKinds(EChangeKind::Geometry, EChangeKind::Geometry) == EChangeKind::Geometry);
	TestTrue(TEXT("Markings twice is Markings - the paint-only rebuild that keeps guideline handles"),
		CombineChangeKinds(EChangeKind::Markings, EChangeKind::Markings) == EChangeKind::Markings);
	TestTrue(TEXT("Geometry + Markings is Topology, because neither alone repaints both"),
		CombineChangeKinds(EChangeKind::Geometry, EChangeKind::Markings) == EChangeKind::Topology
		&& CombineChangeKinds(EChangeKind::Markings, EChangeKind::Geometry) == EChangeKind::Topology);
	TestTrue(TEXT("anything with Topology is Topology, whichever side it is on"),
		CombineChangeKinds(EChangeKind::Topology, EChangeKind::Geometry) == EChangeKind::Topology
		&& CombineChangeKinds(EChangeKind::Geometry, EChangeKind::Topology) == EChangeKind::Topology
		&& CombineChangeKinds(EChangeKind::Markings, EChangeKind::Topology) == EChangeKind::Topology
		&& CombineChangeKinds(EChangeKind::Facts, EChangeKind::Topology) == EChangeKind::Topology);
	// FACTS (#446): twice is Facts - a batch of module purchases re-derives nothing - and with Geometry or Markings it is
	// Topology, because Facts neither repaints the holding bars nor reads a graph a drag has left behind the road.
	TestTrue(TEXT("Facts twice is Facts - the re-mesh that re-derives nothing"),
		CombineChangeKinds(EChangeKind::Facts, EChangeKind::Facts) == EChangeKind::Facts);
	TestTrue(TEXT("Facts with Geometry or Markings is Topology, whichever side"),
		CombineChangeKinds(EChangeKind::Facts, EChangeKind::Geometry) == EChangeKind::Topology
		&& CombineChangeKinds(EChangeKind::Markings, EChangeKind::Facts) == EChangeKind::Topology);

	// AT THE COMPOSITION. Geometry only arises from a move inside an OPEN drag (the bare-call
	// trap), and a batch opened inside a drag is the legal nesting - so that is the fixture.
	ARoadNetworkActor* Actor = MakeActor();
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
	URoadEditFacade* Facade = Actor->GetEditFacade();
	if (!TestNotNull(TEXT("the actor has a facade"), Facade)) { return false; }
	const int32 A = Actor->PlaceNode(FVector2D(0.0, 0.0));
	const int32 B = Actor->PlaceNode(FVector2D(10000.0, 0.0));
	if (!TestTrue(TEXT("the two nodes connect"), Actor->ConnectNodes(A, B))) { return false; }
	// A lone node for the drop-to-merge below: MergeNodes is the one mutator that legitimately
	// runs INSIDE an open drag (FEditTool::OnDragEnd) and notifies Topology there regardless.
	const int32 Lone = Actor->PlaceNode(FVector2D(30000.0, 20000.0));

	Facade->BeginInteractiveEdit(TEXT("drag node"));
	{
		FKindRecorder Recorder(*Facade);
		const int32 TopologyBefore = Actor->TopologyRebuildCountForTest();
		{
			FRoadRebuildBatch Batch(*Actor);
			TestTrue(TEXT("frame 1 moves"), Actor->MoveNode(B, FVector2D(10500.0, 500.0)));
			TestTrue(TEXT("frame 2 moves"), Actor->MoveNode(B, FVector2D(11000.0, 900.0)));
		}
		TestEqual(TEXT("a batch of Geometry-only notifies broadcasts once"), Recorder.Kinds.Num(), 1);
		TestTrue(TEXT("and as Geometry - a batch never strengthens a kind it did not see"),
			Recorder.Kinds.Num() == 1 && Recorder.Kinds[0] == EChangeKind::Geometry);
		TestEqual(TEXT("so the derived pass did not run for it"),
			Actor->TopologyRebuildCountForTest(), TopologyBefore);

		Recorder.Kinds.Reset();
		{
			FRoadRebuildBatch Batch(*Actor);
			TestTrue(TEXT("a move in the second batch"), Actor->MoveNode(B, FVector2D(11500.0, 1200.0)));
			TestTrue(TEXT("and a drop-to-merge, mid-drag, as FEditTool::OnDragEnd does it"), Actor->MergeNodes(B, Lone));
		}
		TestEqual(TEXT("a batch mixing Geometry with Topology broadcasts once"), Recorder.Kinds.Num(), 1);
		TestTrue(TEXT("and as Topology - MergeNodes' always-Topology rule survives the fold, and wins"),
			Recorder.Kinds.Num() == 1 && Recorder.Kinds[0] == EChangeKind::Topology);
	}
	// THE DRAG'S OWN CATCH-UP SURVIVES the batches inside it: bGeometryChangedDuringEdit was set
	// by the folded frames, so EndInteractiveEdit still owes, and fires, its Topology notify.
	const int32 TopologyBeforeEnd = Actor->TopologyRebuildCountForTest();
	Facade->EndInteractiveEdit(/*bKeep*/ true);
	TestEqual(TEXT("the drag still catches the derived graph up at its end, batches inside it or not"),
		Actor->TopologyRebuildCountForTest(), TopologyBeforeEnd + 1);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRebuildBatchRefusesDragInsideTest,
	"Airside.Present.RebuildBatch.RefusesDragInside",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRebuildBatchRefusesDragInsideTest::RunTest(const FString& Parameters)
{
	using namespace RebuildBatchTest;
	ARoadNetworkActor* Actor = MakeActor();
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
	URoadEditFacade* Facade = Actor->GetEditFacade();
	if (!TestNotNull(TEXT("the actor has a facade"), Facade)) { return false; }
	const int32 A = Actor->PlaceNode(FVector2D(0.0, 0.0));
	const int32 B = Actor->PlaceNode(FVector2D(10000.0, 0.0));
	if (!TestTrue(TEXT("the two nodes connect"), Actor->ConnectNodes(A, B))) { return false; }

	// A DRAG CANNOT OPEN INSIDE A BATCH: a batch is one synchronous call, a drag spans frames and
	// repaints each. Refused loudly; the moves that follow are bare calls (Topology each, own
	// undo step each), folded into the batch; End then no-ops on its own guard.
	AddExpectedError(TEXT("a rebuild batch is open"), EAutomationExpectedErrorFlags::Contains, 1);
	FKindRecorder Recorder(*Facade);
	const int32 TopologyBefore = Actor->TopologyRebuildCountForTest();
	{
		FRoadRebuildBatch Batch(*Actor);
		Facade->BeginInteractiveEdit(TEXT("drag inside a batch"));
		TestTrue(TEXT("the move itself still happens - only the drag bracket is refused"),
			Actor->MoveNode(B, FVector2D(10500.0, 500.0)));
		Facade->EndInteractiveEdit(/*bKeep*/ true);
		TestEqual(TEXT("nothing broadcast while the batch was open, the refused drag's End included"),
			Recorder.Kinds.Num(), 0);
	}
	TestTrue(TEXT("the batch closes as Topology: the refused bracket made the move a bare call, which "
		"must do the whole job itself (the bare-call trap) - not a Geometry nobody catches up"),
		Recorder.Kinds.Num() == 1 && Recorder.Kinds[0] == EChangeKind::Topology);
	TestEqual(TEXT("and runs the derived pass exactly once"),
		Actor->TopologyRebuildCountForTest(), TopologyBefore + 1);

	// THE REFUSAL LEAVES NO STATE BEHIND: a drag opened after the batch works as it always has.
	const int32 TopologyBeforeDrag = Actor->TopologyRebuildCountForTest();
	Facade->BeginInteractiveEdit(TEXT("drag after the batch"));
	TestTrue(TEXT("a later drag frame moves"), Actor->MoveNode(B, FVector2D(11000.0, 900.0)));
	TestEqual(TEXT("and is Geometry-only mid-drag again - the refusal did not leave the drag flag set or stuck"),
		Actor->TopologyRebuildCountForTest(), TopologyBeforeDrag);
	Facade->EndInteractiveEdit(/*bKeep*/ true);
	TestEqual(TEXT("with its one catch-up at the end"),
		Actor->TopologyRebuildCountForTest(), TopologyBeforeDrag + 1);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
