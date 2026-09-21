#include "CoreMinimal.h"
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

		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			static const FName RoadMeshCategory(TEXT("LogRoadMesh"));
			if (Category == RoadMeshCategory && Verbosity == ELogVerbosity::Log)
			{
				++Count;
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
	TestEqual(TEXT("a Geometry (drag-frame) rebuild logs nothing at Log level on LogRoadMesh - "
		"not RoadRebuildCensus, not the Aprons/Runway markings/Runway rubber lines, not "
		"FDynamicMeshSink::Accept's own DIAG and 'Sink: built' lines (issue #178)"),
		DragSpy.Count, 0);

	// AND THE DRAG STILL ENDS IN ONE TOPOLOGY REBUILD THAT LOGS IN FULL - the census this
	// project's CLAUDE.md "Diagnosing" section depends on is silenced per-frame here, never
	// removed: it still runs, in full, the moment the drag commits.
	FLogRoadMeshLogSpy CommitSpy;
	GLog->AddOutputDevice(&CommitSpy);
	Facade->EndInteractiveEdit(/*bKeep*/ true);
	GLog->RemoveOutputDevice(&CommitSpy);

	TestTrue(TEXT("the drag's commit (a Topology rebuild) still logs RoadRebuildCensus's "
		"'Rebuilt:' line"), CommitSpy.bSawRebuiltLine);
	TestTrue(TEXT("and logs more than zero LogRoadMesh lines at Log level there - the census "
		"is silenced per-frame, not deleted"), CommitSpy.Count > 0);

	return true;
}

#endif
