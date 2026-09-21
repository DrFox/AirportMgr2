#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Present/RoadEditFacade.h"
#include "Present/RoadNetworkActor.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------------------
// Issue #165: every committed edit AND every drag frame used to re-solve and re-derive the
// WHOLE airport - FRoadGuidelineBuilder::Build, FAnchorLink::Build, Plots->RebuildFrom,
// Traffic->OnGraphRebuilt, all of it, every single MoveNode call a drag makes. This measures
// the fix at the level of the composition (the actor and the facade it owns), not the model
// struct alone - a delegate rewired to the wrong overload, or a Kind that defaulted the
// wrong way, would show here and nowhere lower.
//
// TopologyRebuildCountForTest stands in for "the guideline builder / anchor links / plots /
// traffic pass ran": ARoadNetworkActor::RebuildMeshForChange gates all four behind the same
// `if (Kind == EChangeKind::Topology)`, so one counter answers for all of them without this
// test reaching into UGroundTraffic or UPlotPresenter to ask each separately.
// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDragNotifiesGeometryOnlyTest,
	"Airside.Present.DragNotifiesGeometryOnly",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDragNotifiesGeometryOnlyTest::RunTest(const FString& Parameters)
{
	// NewObject, no world: matching Airside.Present.MeshRebuildsOnFacadeChange rather than a
	// spawned FAirsideTestWorld actor - what is under test is the delegate's Kind and the
	// counters it drives, neither of which needs PostRegisterAllComponents.
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

	// THE CONTROL: PlaceNode and ConnectNodes are ordinary topology-changing mutators, and
	// each already ran the derived-graph pass once - without this, "exactly once" below
	// could pass on a pass that had stopped running at all.
	TestTrue(TEXT("placing and connecting nodes each ran the derived-graph pass"),
		Actor->TopologyRebuildCountForTest() >= 2);

	URoadEditFacade* Facade = Actor->GetEditFacade();
	if (!TestNotNull(TEXT("the actor has a facade"), Facade))
	{
		return false;
	}

	const int32 RebuildsBeforeDrag = Actor->RebuildCountForTest();
	const int32 TopologyBeforeDrag = Actor->TopologyRebuildCountForTest();

	// THREE FRAMES OF ONE DRAG. The count is arbitrary and deliberately more than one: issue
	// #165 is specifically that every frame re-ran the whole pipeline, not merely that the
	// first one did.
	Facade->BeginInteractiveEdit(TEXT("drag node"));
	TestTrue(TEXT("drag frame 1 moves"), Actor->MoveNode(B, FVector2D(10500.0, 500.0)));
	TestTrue(TEXT("drag frame 2 moves"), Actor->MoveNode(B, FVector2D(11000.0, 900.0)));
	TestTrue(TEXT("drag frame 3 moves"), Actor->MoveNode(B, FVector2D(11500.0, 1200.0)));
	Facade->EndInteractiveEdit(/*bKeep*/ true);

	// THE SURFACE STILL TRACKS EVERY FRAME - three moves plus the commit notify
	// EndInteractiveEdit fires at the end, four rebuilds in total. A drag that stopped
	// repainting the mesh per frame would be a different, worse bug (a cursor the pavement
	// does not follow).
	TestEqual(TEXT("every drag frame still rebuilt the surface, plus one for the commit"),
		Actor->RebuildCountForTest(), RebuildsBeforeDrag + 4);

	// THE MEASUREMENT: the derived-graph pass ran exactly once for the whole drag, at
	// EndInteractiveEdit, not once per MoveNode call. This is the line that fails on
	// unpatched main, where every MoveNode notify ran the full pipeline.
	TestEqual(TEXT("but the guideline/anchor/plots/traffic pass ran exactly once, at the "
		"drag's end - not once per frame"),
		Actor->TopologyRebuildCountForTest(), TopologyBeforeDrag + 1);

	// AND A PLAIN MUTATOR STILL RUNS IT, afterwards - the control that closes the loop: the
	// pass is skipped for a drag frame specifically, not broken outright.
	const int32 TopologyBeforePlace = Actor->TopologyRebuildCountForTest();
	Actor->PlaceNode(FVector2D(90000.0, 90000.0));
	TestEqual(TEXT("and PlaceNode still runs the derived-graph pass"),
		Actor->TopologyRebuildCountForTest(), TopologyBeforePlace + 1);

	return true;
}

#endif
