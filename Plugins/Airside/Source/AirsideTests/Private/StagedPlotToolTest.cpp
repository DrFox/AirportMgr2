#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Present/RoadNetworkActor.h"
#include "Solve/PlotYard.h"
#include "Solve/RoadGeom.h"
#include "Tool/PlotGesture.h"
#include "Tool/PlotPlaceTool.h"
#include "Tool/RoadEditTarget.h"
#include "Tool/StagedPlotTool.h"
#include "Tool/StandPlotTool.h"
#include "Tool/ToolReadout.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE SEAM ISSUE #302 INTRODUCED: FPlotPlaceTool and FStandPlotTool no longer implement
 * OnClick/OnCancel/OnCommit/OnDeactivate/BuildPreview/BuildReadout at all - FStagedPlotTool's
 * are `override final`, so these tests exercise ONE body for both tools, reached only through
 * FStagedPlotTool& so a future fork of the skeleton (a tool that re-overrode one of the final
 * methods) could not compile, let alone pass. Named StagedPlotToolFixture, NOT the anonymous
 * or per-file named namespaces PlotPlaceToolTest.cpp / StandPlotToolTest.cpp already use - the
 * tests module is a UNITY build, and a second `namespace StandPlotToolFixture` here would
 * redefine every helper it already declares the moment both land in one translation unit.
 */
namespace StagedPlotSeamFixture
{
	void LayRoad(ARoadNetworkActor* Actor, double Y, ERoadKind Kind)
	{
		IRoadEditTarget* Target = Actor;
		const int32 West = Target->PlaceNode(FVector2D(-10000.0, Y));
		const int32 East = Target->PlaceNode(FVector2D(10000.0, Y));
		Target->ConnectNodes(West, East, Kind, INDEX_NONE);
	}

	FToolContext At(IRoadEditTarget& Target, const FVector2D& Where)
	{
		return TestTool::ContextAt(Target, Where);
	}

	/**
	 * A context whose snap names the road SEGMENT, which is what the anchor click reads - the
	 * same construction PlotPlaceToolTest.cpp's OnRoad and StandPlotToolTest.cpp's AnchorCursor
	 * plumbing each do for their own tool, written once more here because neither is reachable
	 * from this file (see this namespace's own top comment).
	 */
	FToolContext OnRoad(IRoadEditTarget& Target, const URoadNetwork& Network, const FVector2D& Where)
	{
		FToolContext Context = TestTool::ContextAt(Target, Where, ERoadSnapKind::Segment);
		Context.Snap.Segment = Network.SegmentIdAt(0);
		const FRoadSegment* Segment = Network.GetSegment(Context.Snap.Segment);
		const FRoadNode* A = Segment != nullptr ? Network.GetNode(Segment->A) : nullptr;
		const FRoadNode* B = Segment != nullptr ? Network.GetNode(Segment->B) : nullptr;
		if (A != nullptr && B != nullptr)
		{
			Context.Snap.SegmentT = RoadGeom::ClosestPointOnSegment(A->Position, B->Position, Where);
		}
		return Context;
	}

	int32 Pinned(FStagedPlotTool& Tool) { return Tool.PinnedCount(); }

	bool CommittableOf(FStagedPlotTool& Tool, const FToolContext& Context)
	{
		FToolReadoutCollector Collector;
		Tool.BuildReadout(Context, Collector);
		return Collector.Readout.bCommittable;
	}

	bool WarnsOf(FStagedPlotTool& Tool, const FToolContext& Context, const TCHAR* Fragment)
	{
		FToolReadoutCollector Collector;
		Tool.BuildReadout(Context, Collector);
		return Collector.Readout.Warnings.ContainsByPredicate(
			[Fragment](const FString& W) { return W.Contains(Fragment); });
	}

	/** Draws nothing; BuildPreview still has to be CALLED for #302's memo to be exercised. */
	struct FNullPreviewSink : IToolPreviewSink
	{
		virtual void Marker(const FVector2D&, EPreviewStyle) override {}
		virtual void Line(const FVector2D&, const FVector2D&, EPreviewStyle) override {}
		virtual void CrossMark(const FVector2D&, const FVector2D&, EPreviewStyle) override {}
		virtual void Label(const FVector2D&, const FString&, EPreviewStyle) override {}
	};

	/**
	 * A target that lays no road of its own but REFUSES EVERY COMMIT - FNullEditTarget's own
	 * PlaceEntityInPlot/PlaceStandInPlot defaults are already INDEX_NONE, so the only overrides
	 * needed are the ones that make the shape otherwise Committable: a real network to anchor
	 * against, a definition/empty-refusal answer, and (the depot only) a kit that actually
	 * fits the drawn ground. This is deliberately the "should be unreachable" case
	 * bLastCommitRefused exists for - both evaluators say yes, and Place() still says no.
	 */
	struct FRefusingDepotTarget : FNullEditTarget
	{
		const URoadNetwork* NetworkPtr = nullptr;
		UEntityDefinition* Definition = nullptr;

		virtual const URoadNetwork* GetNetwork() const override { return NetworkPtr; }
		virtual const UEntityDefinition* GetEntityDefinition(EPlaceableEntity) const override { return Definition; }

		/** A kit small enough to fit the DrawPlot-shaped quad this test drags out, so Total > 0
		 *  and Committable agrees with the shape right up until Place() itself refuses it. */
		virtual TArray<PlotYard::FKitSpec> ResolveDepotKits() const override
		{
			PlotYard::FKitSpec Kit;
			Kit.Footprint.LengthUu = 300.0;
			Kit.Footprint.WidthUu = 300.0;
			Kit.ReserveWeight = 1;
			Kit.RunCap = 10;
			return { Kit };
		}
	};

	struct FRefusingStandTarget : FNullEditTarget
	{
		const URoadNetwork* NetworkPtr = nullptr;

		virtual const URoadNetwork* GetNetwork() const override { return NetworkPtr; }

		/** FNullEditTarget's own default is "no target" - a refusal reason that would make
		 *  Committable false for an unrelated reason before Place() is ever asked. Empty here
		 *  says the shape itself is fine, so the forced PlaceStandInPlot refusal below is the
		 *  ONLY reason the commit fails. */
		virtual FString WhyStandRefused(TArrayView<const FVector2D>) const override { return FString(); }
	};
}

/**
 * ONE SKELETON, TWO TOOLS - issue #302's own fix, measured rather than asserted by name.
 * Both tools are driven through FStagedPlotTool& only, so this could not compile against a
 * tool that re-overrode OnClick/OnCancel/OnCommit/BuildReadout (all `override final` on the
 * base) - the fork the issue found FStandPlotTool to be.
 *
 * THE SAME FIVE GESTURES, IN THE SAME ORDER, PRODUCE THE SAME SHAPE OF TRANSITIONS: a click
 * with nothing under the cursor for Remove pins nothing; the anchor pins exactly one corner;
 * every further click pins exactly one more, up to each tool's own MaxPinned; cancelling from
 * Confirm steps back exactly one; and a commit the target refuses leaves the gesture exactly
 * where it was, with a warning naming the refusal, while Committable itself does not move -
 * see bLastCommitRefused's own comment on why that is the deliberately unreachable case.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStagedPlotToolsShareOneSkeletonTest,
	"Airside.Tool.StagedPlot.BothToolsShareOneSkeleton",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStagedPlotToolsShareOneSkeletonTest::RunTest(const FString& Parameters)
{
	using namespace StagedPlotSeamFixture;

	// --- THE DEPOT, through FStagedPlotTool's own methods only ---------------------------
	FAirsideTestWorld DepotWorld;
	if (!TestNotNull(TEXT("a depot world"), DepotWorld.World)) { return false; }
	ARoadNetworkActor* DepotActor = DepotWorld.Actor;
	if (!TestNotNull(TEXT("depot actor"), DepotActor)) { return false; }
	DepotActor->ClearNetwork();
	LayRoad(DepotActor, 0.0, ERoadKind::ServiceRoad);

	FRefusingDepotTarget DepotTarget;
	DepotTarget.NetworkPtr = DepotActor->Network;
	DepotTarget.Definition = UEntityDefinition::MakeFuelDepotTransient();

	FPlotPlaceTool DepotTool(EPlaceableEntity::FuelDepot);
	FStagedPlotTool& DepotSeam = DepotTool;

	TArray<int32> DepotTrace;

	// REMOVE ON NOTHING: nothing under the cursor, so no gesture starts or advances.
	{
		FToolContext Remove = At(DepotTarget, FVector2D(8000.0, 8000.0));
		Remove.bRemoveModifier = true;
		DepotSeam.OnClick(Remove);
		DepotTrace.Add(Pinned(DepotSeam));
	}

	// THE ANCHOR, then the depot's own three remaining clicks - DrawPlot's own coordinates
	// (PlotPlaceToolTest.cpp), known to reach Confirm with a simple, uncrossed quad.
	DepotSeam.OnClick(OnRoad(DepotTarget, *DepotActor->Network, FVector2D(0.0, 200.0)));
	DepotTrace.Add(Pinned(DepotSeam));
	DepotSeam.OnClick(At(DepotTarget, FVector2D(1200.0, 200.0)));
	DepotTrace.Add(Pinned(DepotSeam));
	DepotSeam.OnClick(At(DepotTarget, FVector2D(600.0, 1600.0)));
	DepotTrace.Add(Pinned(DepotSeam));
	const FVector2D DepotLastCorner(0.0, 1500.0);
	DepotSeam.OnClick(At(DepotTarget, DepotLastCorner));
	DepotTrace.Add(Pinned(DepotSeam));

	if (!TestEqual(TEXT("the depot reached Confirm"), Pinned(DepotSeam), 4)) { return false; }

	const FToolContext DepotAnywhere = At(DepotTarget, FVector2D(600.0, 1600.0));

	// CANCEL FROM CONFIRM: exactly one corner back.
	DepotSeam.OnCancel(DepotAnywhere);
	DepotTrace.Add(Pinned(DepotSeam));

	// RE-CLICK THE SAME CORNER to re-reach Confirm for the refused commit below.
	DepotSeam.OnClick(At(DepotTarget, DepotLastCorner));
	if (!TestEqual(TEXT("depot back to Confirm"), Pinned(DepotSeam), 4)) { return false; }

	const bool DepotCommittableBefore = CommittableOf(DepotSeam, DepotAnywhere);
	const bool DepotWarnedBefore = WarnsOf(DepotSeam, DepotAnywhere, TEXT("Build failed"));
	DepotSeam.OnCommit(DepotAnywhere);
	DepotTrace.Add(Pinned(DepotSeam));
	const bool DepotCommittableAfter = CommittableOf(DepotSeam, DepotAnywhere);
	const bool DepotWarnedAfter = WarnsOf(DepotSeam, DepotAnywhere, TEXT("Build failed"));

	// --- THE STAND, through the SAME base methods -----------------------------------------
	FAirsideTestWorld StandWorld;
	if (!TestNotNull(TEXT("a stand world"), StandWorld.World)) { return false; }
	ARoadNetworkActor* StandActor = StandWorld.Actor;
	if (!TestNotNull(TEXT("stand actor"), StandActor)) { return false; }
	StandActor->ClearNetwork();
	LayRoad(StandActor, 0.0, ERoadKind::Taxiway);

	FRefusingStandTarget StandTarget;
	StandTarget.NetworkPtr = StandActor->Network;

	FStandPlotTool StandTool;
	FStagedPlotTool& StandSeam = StandTool;

	TArray<int32> StandTrace;

	{
		FToolContext Remove = At(StandTarget, FVector2D(8000.0, 8000.0));
		Remove.bRemoveModifier = true;
		StandSeam.OnClick(Remove);
		StandTrace.Add(Pinned(StandSeam));
	}

	StandSeam.OnClick(OnRoad(StandTarget, *StandActor->Network, FVector2D(0.0, 1000.0)));
	StandTrace.Add(Pinned(StandSeam));

	// READ THE ANCHOR BACK rather than assumed - the same reason every other test in this
	// module queries Rect()/Quad() instead of predicting where the snap landed.
	TArray<FVector2D> Anchored;
	StandTool.Rect(At(StandTarget, FVector2D(0.0, 1000.0)), Anchored);
	if (!TestTrue(TEXT("the stand anchored"), Anchored.Num() > 0)) { return false; }
	const FVector2D StandAnchor = Anchored[0];

	StandSeam.OnClick(At(StandTarget, StandAnchor + FVector2D(3000.0, 0.0)));
	StandTrace.Add(Pinned(StandSeam));
	const FVector2D StandLastCorner = StandAnchor + FVector2D(3000.0, 3000.0);
	StandSeam.OnClick(At(StandTarget, StandLastCorner));
	StandTrace.Add(Pinned(StandSeam));

	if (!TestEqual(TEXT("the stand reached Confirm"), Pinned(StandSeam), 3)) { return false; }

	const FToolContext StandAnywhere = At(StandTarget, StandLastCorner);

	StandSeam.OnCancel(StandAnywhere);
	StandTrace.Add(Pinned(StandSeam));

	StandSeam.OnClick(At(StandTarget, StandLastCorner));
	if (!TestEqual(TEXT("stand back to Confirm"), Pinned(StandSeam), 3)) { return false; }

	const bool StandCommittableBefore = CommittableOf(StandSeam, StandAnywhere);
	const bool StandWarnedBefore = WarnsOf(StandSeam, StandAnywhere, TEXT("Build failed"));
	StandSeam.OnCommit(StandAnywhere);
	StandTrace.Add(Pinned(StandSeam));
	const bool StandCommittableAfter = CommittableOf(StandSeam, StandAnywhere);
	const bool StandWarnedAfter = WarnsOf(StandSeam, StandAnywhere, TEXT("Build failed"));

	// --- THE SAME SHAPE OF TRANSITIONS, whatever MaxPinned each tool's own gesture needs ---
	if (!TestEqual(TEXT("depot: remove-on-nothing pins nothing"), DepotTrace[0], 0)) { return false; }
	if (!TestEqual(TEXT("stand: remove-on-nothing pins nothing"), StandTrace[0], 0)) { return false; }

	if (!TestEqual(TEXT("depot: the anchor pins exactly one corner"), DepotTrace[1], 1)) { return false; }
	if (!TestEqual(TEXT("stand: the anchor pins exactly one corner"), StandTrace[1], 1)) { return false; }

	// EVERY CLICK BETWEEN THE ANCHOR AND CONFIRM PINS EXACTLY ONE MORE. Both traces have the
	// anchor at index 1 and Confirm two entries before the end (cancel, then the refused
	// commit) - the only place the two counts differ at all is how many such clicks there are,
	// four corners against three.
	for (int32 Index = 1; Index < DepotTrace.Num() - 3; ++Index)
	{
		if (!TestEqual(*FString::Printf(TEXT("depot: click %d pins exactly one more"), Index),
			DepotTrace[Index + 1], DepotTrace[Index] + 1))
		{
			return false;
		}
	}
	for (int32 Index = 1; Index < StandTrace.Num() - 3; ++Index)
	{
		if (!TestEqual(*FString::Printf(TEXT("stand: click %d pins exactly one more"), Index),
			StandTrace[Index + 1], StandTrace[Index] + 1))
		{
			return false;
		}
	}

	// CANCEL FROM CONFIRM STEPS BACK EXACTLY ONE.
	TestEqual(TEXT("depot: cancel from Confirm steps back one"), DepotTrace[DepotTrace.Num() - 2], 3);
	TestEqual(TEXT("stand: cancel from Confirm steps back one"), StandTrace[StandTrace.Num() - 2], 2);

	// A REFUSED COMMIT LEAVES THE GESTURE EXACTLY WHERE IT WAS - Confirm, not Idle.
	TestEqual(TEXT("depot: a refused commit stays at Confirm"), DepotTrace.Last(), 4);
	TestEqual(TEXT("stand: a refused commit stays at Confirm"), StandTrace.Last(), 3);

	// BOTH EVALUATORS AGREED BEFORE THE COMMIT - this really is bLastCommitRefused's own
	// "should be unreachable" case, not a shape either tool's normal Committable rule already
	// refused.
	TestTrue(TEXT("depot: was committable before the forced refusal"), DepotCommittableBefore);
	TestTrue(TEXT("stand: was committable before the forced refusal"), StandCommittableBefore);
	TestFalse(TEXT("depot: no refusal warning before the commit"), DepotWarnedBefore);
	TestFalse(TEXT("stand: no refusal warning before the commit"), StandWarnedBefore);

	// AFTER THE FORCED REFUSAL: the warning appears on both, and Committable itself does NOT
	// move - it is computed from the same evaluator the commit itself is judged against, and a
	// refusal here is the safety net, not a second opinion that changes the button's own light.
	TestTrue(TEXT("depot: warns of the refused build"), DepotWarnedAfter);
	TestTrue(TEXT("stand: warns of the refused build"), StandWarnedAfter);
	TestTrue(TEXT("depot: Committable is unmoved by the forced refusal"), DepotCommittableAfter);
	TestTrue(TEXT("stand: Committable is unmoved by the forced refusal"), StandCommittableAfter);

	return true;
}

/**
 * THE REFUSAL MEMO IS SHARED ACROSS CALLERS - issue #302's own named fix. Before it,
 * FStandPlotTool asked IRoadEditTarget::WhyStandRefused for the SAME Shown outline once from
 * BuildPreview (DescribeLetter's own label) and again from BuildReadout, on every hover frame -
 * exactly the "solve done twice a frame" #180 had already named and fixed for the depot's own
 * packer call, missed here only because this tool was copied before that memo existed.
 *
 * ONE CONTEXT, BOTH CALLS: the same shape ARoadBuildController::PlayerTick and
 * RoadBuildHUD::DrawHUD would describe on one real frame.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandPlotRefusalMemoIsSharedAcrossCallersTest,
	"Airside.Tool.StagedPlot.RefusalMemoIsSharedAcrossCallers",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandPlotRefusalMemoIsSharedAcrossCallersTest::RunTest(const FString& Parameters)
{
	using namespace StagedPlotSeamFixture;

	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	Actor->ClearNetwork();
	Actor->StandDefinition = UEntityDefinition::MakeStandTransient();
	LayRoad(Actor, 0.0, ERoadKind::Taxiway);

	FStandPlotTool Tool;
	Tool.OnClick(OnRoad(*Actor, *Actor->Network, FVector2D(0.0, 1000.0)));

	TArray<FVector2D> Anchored;
	Tool.Rect(At(*Actor, FVector2D(0.0, 1000.0)), Anchored);
	if (!TestTrue(TEXT("anchored"), Anchored.Num() > 0)) { return false; }
	const FVector2D Anchor = Anchored[0];

	// A GENEROUS RECTANGLE, well clear of every refusal WhyStandRefused could name - too
	// small, an unfit letter, an overlap - so BOTH calls below ask the same real question and
	// the memo, not an early "no target" or "too small" return, is what this test measures.
	Tool.OnClick(At(*Actor, Anchor + FVector2D(4000.0, 0.0)));
	Tool.OnClick(At(*Actor, Anchor + FVector2D(4000.0, 3000.0)));
	if (!TestEqual(TEXT("reached Confirm"), static_cast<int32>(Tool.GetStage()),
		static_cast<int32>(EStandStage::Confirm)))
	{
		return false;
	}

	const FToolContext Frame = At(*Actor, Anchor + FVector2D(4000.0, 3000.0));

	const int32 Before = Tool.GetRefusalCountForTest();

	FNullPreviewSink Sink;
	Tool.BuildPreview(Frame, Sink);
	FToolReadoutCollector Collector;
	Tool.BuildReadout(Frame, Collector);

	const int32 After = Tool.GetRefusalCountForTest();

	// ONE ASK, NOT TWO: BuildPreview's own Describe() hook asks first and stores the memo;
	// BuildReadout's DescribeReadout hook, asking for the SAME Shown outline this same frame,
	// must read the memo back rather than asking the facade again.
	TestEqual(TEXT("BuildPreview and BuildReadout together ask WhyStandRefused exactly once"),
		After - Before, 1);

	// AND A SECOND FRAME OF THE SAME SHAPE ASKS NOTHING FURTHER - the memo survives across
	// calls, not just within one, exactly as FPlotPlaceTool::ReservationFor's own memo does.
	FNullPreviewSink SecondSink;
	Tool.BuildPreview(Frame, SecondSink);
	FToolReadoutCollector SecondCollector;
	Tool.BuildReadout(Frame, SecondCollector);
	TestEqual(TEXT("an unchanged shape runs no further ask"),
		Tool.GetRefusalCountForTest() - After, 0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
