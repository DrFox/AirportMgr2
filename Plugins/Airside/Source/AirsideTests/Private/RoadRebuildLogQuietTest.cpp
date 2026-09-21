#include "CoreMinimal.h"
#include "AirsideLog.h"
#include "Logging/LogVerbosity.h"
#include "Misc/AutomationTest.h"
#include "Present/RoadEditFacade.h"
#include "Present/RoadNetworkActor.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/**
	 * Counts LogRoadMesh lines at EXACTLY Log verbosity - not Warning, not Verbose - during
	 * whatever it is registered across. Issue #178: a Geometry (drag-frame) rebuild must emit
	 * ZERO of these, and the point is the COST, not the print - RoadRebuildCensus::Log builds
	 * an FString and two TSets, and FDynamicMeshSink::Accept walks every normal element,
	 * before either one reaches a UE_LOG call this spy could otherwise miss by watching only
	 * what got past LogRoadMesh's own verbosity filter. Scoped to one measured window per use
	 * (AddOutputDevice/RemoveOutputDevice bracketing a single call), the same shape
	 * FAutomationTestBase::AddExpectedMessage's own scoping uses.
	 */
	class FLogRoadMeshLogSpy : public FOutputDevice
	{
	public:
		int32 Count = 0;
		bool bSawRebuiltLine = false;

		// ISSUE #216: the count alone cannot say WHICH line survived, and that is exactly
		// the question an order-dependent failure needs answered - a line captured here that
		// this test's own code never emits (the census, the sink's DIAG, the markings count)
		// means the spy's window is catching something unrelated, not a leak in the quiet
		// path itself. Captured verbatim rather than re-derived from Count, so the failure
		// message names the actual line instead of asking a human to reproduce it under a
		// debugger to find out.
		TArray<FString> CapturedLines;

		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			static const FName RoadMeshCategory(TEXT("LogRoadMesh"));
			if (Category == RoadMeshCategory && Verbosity == ELogVerbosity::Log)
			{
				++Count;
				CapturedLines.Add(FString(V));
				// RoadRebuildCensus::Log's own final line - see its header. The one line
				// CLAUDE.md's "Diagnosing" section names by name, so this test does too rather
				// than trusting the count alone to say WHICH line survived.
				if (FCString::Strstr(V, TEXT("Rebuilt:")) != nullptr)
				{
					bSawRebuiltLine = true;
				}
			}
		}
	};
}

// ---------------------------------------------------------------------------------------
// Issue #178: MoveNode's per-frame Geometry notify (RoadEditFacade.cpp, mid-drag) reaches
// URoadSurfacePresenter::RebuildSurfaceOnly up to 60 times a second while a node is held.
// Before this fix that rebuild still paid for and printed ~17 LogRoadMesh lines and an O(V)
// scan over every mesh normal, on every one of those frames - see FDynamicMeshSink::Accept,
// RoadRebuildCensus::Log, and RoadSurfacePresenter.cpp's own Aprons/Runway census lines. This
// measures the fix at the level of the composition (the actor and the facade it owns, exactly
// as RebuildKindTest.cpp's FDragNotifiesGeometryOnlyTest measures the REBUILD side of the same
// lifecycle), not FSurfaceSettings::bQuiet in isolation - a mistranslated Kind or a missed
// call site would show here and nowhere lower.
// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRebuildLogQuietOnDragTest,
	"Airside.Present.RebuildLogQuietOnDrag",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRebuildLogQuietOnDragTest::RunTest(const FString& Parameters)
{
	// ISSUE #216 (third sighting): read BEFORE anything else runs, not just before the commit
	// spy, because the question this answers is whether an EARLIER test in the suite left
	// LogRoadMesh's runtime verbosity somewhere other than its Log default (a SetVerbosity
	// call that never restored it would silently drop every Log-level line this test depends
	// on, on the affected side of Log, while an unrelated diagnostic logged at Warning or
	// Display would still get through - exactly the "some lines survive, one specific line
	// does not" shape the third sighting reported).
	const ELogVerbosity::Type StartVerbosity = GetLogRoadMeshVerbosityForTest();

	// NewObject, no world: matching Airside.Present.DragNotifiesGeometryOnly rather than a
	// spawned FAirsideTestWorld actor - what is under test is what gets logged, which needs
	// neither PostRegisterAllComponents nor a real World.
	ARoadNetworkActor* Actor = NewObject<ARoadNetworkActor>(GetTransientPackage());
	if (!TestNotNull(TEXT("actor constructed"), Actor))
	{
		return false;
	}

	const int32 A = Actor->PlaceNode(FVector2D(0.0, 0.0));
	const int32 B = Actor->PlaceNode(FVector2D(10000.0, 0.0));
	if (!TestTrue(TEXT("the two nodes connect"), Actor->ConnectNodes(A, B)))
	{
		return false;
	}

	URoadEditFacade* Facade = Actor->GetEditFacade();
	if (!TestNotNull(TEXT("the actor has a facade"), Facade))
	{
		return false;
	}

	// A DRAG FRAME. MoveNode called while an interactive edit is open notifies Geometry, not
	// Topology - see RoadEditFacade.cpp's own comment on the "bare-call trap" for why that
	// distinction exists at all.
	FLogRoadMeshLogSpy DragSpy;
	GLog->AddOutputDevice(&DragSpy);
	Facade->BeginInteractiveEdit(TEXT("drag node"));
	const bool bMoved = Actor->MoveNode(B, FVector2D(10500.0, 500.0));
	GLog->RemoveOutputDevice(&DragSpy);

	if (!TestTrue(TEXT("the drag frame moves the node"), bMoved))
	{
		Facade->EndInteractiveEdit(/*bKeep*/ false);
		return false;
	}

	// THE MEASUREMENT: this is the line that fails on unpatched main, where the census and
	// the sink's own diagnostics ran and logged at Log level on every frame regardless of
	// whether the drag had moved anything interesting.
	//
	// ISSUE #216: on a failure the message NAMES every captured line verbatim, not just the
	// count - a full-suite run reported this as its only failure while it passed alone, and
	// a bare count cannot say whether the extra line is a real leak on the quiet path or
	// something else entirely landing in the spy's window. Joined with '; ' rather than left
	// as an array so it prints on one line the automation report does not truncate away.
	if (DragSpy.Count != 0)
	{
		AddError(FString::Printf(
			TEXT("a Geometry (drag-frame) rebuild logs nothing at Log level on LogRoadMesh - ")
			TEXT("not RoadRebuildCensus, not the Aprons/Runway markings/Runway rubber lines, not ")
			TEXT("FDynamicMeshSink::Accept's own DIAG and 'Sink: built' lines (issue #178). ")
			TEXT("Expected 0, got %d. Captured line(s): %s"),
			DragSpy.Count, *FString::Join(DragSpy.CapturedLines, TEXT("; "))));
	}

	// AND THE DRAG STILL ENDS IN ONE TOPOLOGY REBUILD THAT LOGS IN FULL - the census this
	// project's CLAUDE.md "Diagnosing" section depends on is silenced per-frame here, never
	// removed: it still runs, in full, the moment the drag commits.
	//
	// ISSUE #216: RebuildCountForTest/TopologyRebuildCountForTest bracket the commit too, not
	// just the spy - Count and bSawRebuiltLine alone cannot tell "the Topology rebuild ran and
	// logged something else" apart from "the Topology rebuild never ran at all", and the third
	// sighting needs exactly that distinction: a 0 TopologyRebuildCountForTest delta means
	// EndInteractiveEdit never reached RebuildMeshForChange(Topology), which points at
	// bGeometryChangedDuringEdit or the notify itself; a nonzero delta with no 'Rebuilt:' line
	// means the rebuild ran but RoadRebuildCensus::Log's own path is what dropped it.
	const int32 RebuildCountBeforeCommit = Actor->RebuildCountForTest();
	const int32 TopologyRebuildCountBeforeCommit = Actor->TopologyRebuildCountForTest();

	FLogRoadMeshLogSpy CommitSpy;
	GLog->AddOutputDevice(&CommitSpy);
	Facade->EndInteractiveEdit(/*bKeep*/ true);
	GLog->RemoveOutputDevice(&CommitSpy);

	const int32 RebuildCountAfterCommit = Actor->RebuildCountForTest();
	const int32 TopologyRebuildCountAfterCommit = Actor->TopologyRebuildCountForTest();

	// ISSUE #216: the SAME shape of dump the drag half got in PR #219 - verbatim captured
	// lines rather than a bare count, plus the two facts a captured line alone cannot state:
	// whether the Topology rebuild ran at all (the RebuildCountForTest deltas), and whether
	// LogRoadMesh's own verbosity was ever anything but Log at the start of this test (a
	// leaked SetVerbosity from an earlier test in the suite would explain a Log-level line
	// vanishing while nothing else in this test's assertions would catch it).
	if (!CommitSpy.bSawRebuiltLine || CommitSpy.Count == 0)
	{
		AddError(FString::Printf(
			TEXT("the drag's commit (a Topology rebuild) must still log RoadRebuildCensus's ")
			TEXT("'Rebuilt:' line in full - silenced only on a drag frame (issue #178), never ")
			TEXT("on the commit that ends one. Saw bSawRebuiltLine=%s, Count=%d. LogRoadMesh ")
			TEXT("verbosity at this test's start was %s (Log is the built-in default - ")
			TEXT("anything else leaked from an earlier test). RebuildCountForTest %d -> %d, ")
			TEXT("TopologyRebuildCountForTest %d -> %d (a zero TopologyRebuildCountForTest ")
			TEXT("delta means the Topology rebuild never ran at all). Captured line(s): %s"),
			CommitSpy.bSawRebuiltLine ? TEXT("true") : TEXT("false"), CommitSpy.Count,
			::ToString(StartVerbosity),
			RebuildCountBeforeCommit, RebuildCountAfterCommit,
			TopologyRebuildCountBeforeCommit, TopologyRebuildCountAfterCommit,
			*FString::Join(CommitSpy.CapturedLines, TEXT("; "))));
	}

	return true;
}

#endif
