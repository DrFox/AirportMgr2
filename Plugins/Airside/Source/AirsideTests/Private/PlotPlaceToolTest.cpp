#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Build/DepotKit.h"
#include "Build/PlotLayoutStrategy.h"
#include "Content/AirsideSettings.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Present/PlotPresenter.h"
#include "Present/RoadNetworkActor.h"
#include "Profiles/RoadProfile.h"
#include "Solve/PlotFit.h"
#include "Solve/PlotYard.h"
#include "Solve/RoadGeom.h"
#include "Tool/PlotPlaceTool.h"
#include "Tool/RoadEditTarget.h"
#include "Tool/ToolReadout.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FToolContext PlotAt(ARoadNetworkActor* Actor, const FVector2D& Where)
	{
		return TestTool::ContextAt(*Actor, Where);
	}

	int32 LiveEntities(const ARoadNetworkActor* Actor)
	{
		int32 Alive = 0;
		for (const FEntityInstance& Entity : Actor->Network->GetEntities())
		{
			if (Entity.bAlive) { ++Alive; }
		}
		return Alive;
	}

	/**
	 * A real service-road SEGMENT, not a guideline edge.
	 *
	 * The gesture anchors off FToolContext::Snap, which the driver resolves against the ROAD
	 * graph - so a test that laid only guidelines would find nothing to snap to and every
	 * click would be ignored, which looks exactly like a broken tool.
	 */
	void LayRoad(ARoadNetworkActor* Actor, double Y, ERoadKind Kind)
	{
		IRoadEditTarget* Target = Actor;
		const int32 West = Target->PlaceNode(FVector2D(-10000.0, Y));
		const int32 East = Target->PlaceNode(FVector2D(10000.0, Y));
		Target->ConnectNodes(West, East, Kind, INDEX_NONE);
	}

	void LayServiceRoad(ARoadNetworkActor* Actor, double Y)
	{
		LayRoad(Actor, Y, ERoadKind::ServiceRoad);
	}

	/**
	 * A context whose snap names the road SEGMENT, which is what the anchor click reads.
	 *
	 * TestTool::ContextAt fills only what every snap kind has in common and says so at its
	 * own declaration - a Segment kind still needs its handle and its parameter from the
	 * caller. Left at the default the snap is Free, the tool correctly refuses to anchor,
	 * and every stage assertion fails for a reason that has nothing to do with the tool.
	 */
	FToolContext OnRoad(ARoadNetworkActor* Actor, const FVector2D& Where)
	{
		FToolContext Context = TestTool::ContextAt(*Actor, Where, ERoadSnapKind::Segment);
		Context.Snap.Segment = Actor->Network->SegmentIdAt(0);

		// T FROM THE SEGMENT'S REAL ENDS, not from the coordinates LayRoad was asked for.
		// This assumed the road ran -10000 to +10000 and it does not, so every anchor landed
		// somewhere other than under the cursor - harmless while nothing measured absolute
		// positions, and the cause of three baffling failures the moment something did.
		const FRoadSegment* Segment = Actor->Network->GetSegment(Context.Snap.Segment);
		const FRoadNode* A = Segment != nullptr ? Actor->Network->GetNode(Segment->A) : nullptr;
		const FRoadNode* B = Segment != nullptr ? Actor->Network->GetNode(Segment->B) : nullptr;
		if (A != nullptr && B != nullptr)
		{
			Context.Snap.SegmentT =
				RoadGeom::ClosestPointOnSegment(A->Position, B->Position, Where);
		}
		return Context;
	}

	/**
	 * Captures how far from the road the preview actually reaches.
	 *
	 * ONLY Line MATTERS: IToolPreviewSink::Polygon is non-virtual and lands here as four
	 * Lines, while the bay marks arrive as CrossMark and Marker and contribute no geometry
	 * to measure. So the deepest Y IS the plot's far edge.
	 */
	struct FPlotGhostSink : IToolPreviewSink
	{
		double NearestY = TNumericLimits<double>::Max();
		double DeepestY = -TNumericLimits<double>::Max();

		int32 Lines = 0;
		int32 CrossMarks = 0;
		TArray<EPreviewStyle> MarkerStyles;
		TArray<EPreviewStyle> LineStyles;

		/** Lines drawn in one style. What "only what is pinned" is actually counted in. */
		int32 LinesOf(EPreviewStyle Style) const
		{
			int32 Count = 0;
			for (const EPreviewStyle S : LineStyles)
			{
				if (S == Style) { ++Count; }
			}
			return Count;
		}

		/** Markers drawn in one style. The chosen anchor is the only Pending one. */
		int32 MarkersOf(EPreviewStyle Style) const
		{
			int32 Count = 0;
			for (const EPreviewStyle S : MarkerStyles)
			{
				if (S == Style) { ++Count; }
			}
			return Count;
		}

		/**
		 * How deep the ghost is, measured FRONT EDGE TO BACK rather than from y = 0.
		 *
		 * An absolute Y would fold the road's half width into the answer, and it did: this
		 * asserted 800 until the frontage moved off the centreline, then failed at 1100 for
		 * a ghost that was perfectly correct. The claim is the plot's DEPTH.
		 */
		double Depth() const { return DeepestY - NearestY; }

		virtual void Marker(const FVector2D&, EPreviewStyle Style) override
		{
			MarkerStyles.Add(Style);
		}
		virtual void Line(const FVector2D& From, const FVector2D& To, EPreviewStyle Style) override
		{
			++Lines;
			LineStyles.Add(Style);
			NearestY = FMath::Min3(NearestY, From.Y, To.Y);
			DeepestY = FMath::Max3(DeepestY, From.Y, To.Y);
		}
		virtual void CrossMark(const FVector2D&, const FVector2D&, EPreviewStyle) override
		{
			++CrossMarks;
		}
		virtual void Label(const FVector2D&, const FString&, EPreviewStyle) override {}
	};

	/**
	 * Anchor, frontage east, then BOTH back corners: the four points the gesture expects.
	 *
	 * The fourth click is placed directly above the anchor, so a caller that asks for a
	 * rectangle gets one - callers that want a trapezoid place their own corners.
	 *
	 * ONLY THE ANCHOR NEEDS THE ROAD. Every later click is read off the cursor against the
	 * anchored frame, so its snap kind is irrelevant - which is also why a plot can be
	 * dragged out over open ground.
	 */
	void DrawPlot(FPlotPlaceTool& Tool, ARoadNetworkActor* Actor,
		const FVector2D& AnchorAt, const FVector2D& FrontageAt, const FVector2D& DepthAt)
	{
		Tool.OnClick(OnRoad(Actor, AnchorAt));
		Tool.OnClick(PlotAt(Actor, FrontageAt));
		Tool.OnClick(PlotAt(Actor, DepthAt));
		Tool.OnClick(PlotAt(Actor, FVector2D(AnchorAt.X, DepthAt.Y)));
	}
}

/**
 * THE PLOT IS STARTED FROM WHERE IT GOES, not from the carriageway.
 *
 * Every other test in this file anchors with PlotAt/OnRoad putting the cursor ON the road,
 * which is exactly why none of them noticed that moving off it made the anchors vanish:
 * "as soon as you mouse move off the road the option to start the plot process disappears
 * which doesn't feel very natural, as it seems as if you are placing it on the road not next
 * to it" (PIE, 2026-09-17).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotStartsFromOffTheRoadTest,
	"Airside.Tool.PlotStartsFromOffTheRoad",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotStartsFromOffTheRoadTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	Actor->ClearNetwork();
	Actor->FuelDepotDefinition = UEntityDefinition::MakeFuelDepotTransient();
	LayServiceRoad(Actor, 0.0);

	// TEN METRES OUT, IN THE GRASS, and with a FREE snap - PlotAt names no segment at all,
	// which is what the cursor actually carries once it leaves the carriageway.
	{
		FPlotPlaceTool Tool(EPlaceableEntity::FuelDepot);
		Tool.OnClick(PlotAt(Actor, FVector2D(0.0, 1000.0)));
		TestEqual(TEXT("a click 10 m off the road still anchors"), Tool.PinnedCount(), 1);

		// AND ON THE RIGHT SIDE. The side comes from where the cursor is, so standing north
		// of the road must put the plot north of it.
		TArray<FVector2D> Quad;
		Tool.Quad(PlotAt(Actor, FVector2D(2000.0, 1000.0)), Quad);
		if (!TestTrue(TEXT("a frontage to measure"), Quad.Num() >= 2)) { return false; }
		TestTrue(*FString::Printf(TEXT("and the plot is on the cursor's side, got y %.0f"),
			Quad[0].Y), Quad[0].Y > 0.0);
	}

	// THE OTHER SIDE TOO, or the side would be a constant that happened to match.
	{
		FPlotPlaceTool Tool(EPlaceableEntity::FuelDepot);
		Tool.OnClick(PlotAt(Actor, FVector2D(0.0, -1000.0)));
		if (!TestEqual(TEXT("anchors from the south as well"), Tool.PinnedCount(), 1))
		{
			return false;
		}
		TArray<FVector2D> Quad;
		Tool.Quad(PlotAt(Actor, FVector2D(2000.0, -1000.0)), Quad);
		if (!TestTrue(TEXT("a frontage to measure"), Quad.Num() >= 2)) { return false; }
		TestTrue(*FString::Printf(TEXT("and it goes south, got y %.0f"), Quad[0].Y),
			Quad[0].Y < 0.0);
	}

	// BUT THE ROAD STILL HAS TO BE NEAR. Without a limit the whole map would offer anchors
	// against whatever road happened to be closest, which is no guidance at all.
	{
		FPlotPlaceTool Tool(EPlaceableEntity::FuelDepot);
		Tool.OnClick(PlotAt(Actor, FVector2D(0.0, PlotGesture::AnchorReachUu + 500.0)));
		TestEqual(TEXT("a click well beyond the reach anchors nothing"), Tool.PinnedCount(), 0);
	}

	// AND A TAXIWAY IS STILL NOT A SERVICE ROAD. The search skips anything without a ground
	// vehicle guideline, so widening WHERE you can stand did not widen WHAT you can stand by.
	{
		Actor->ClearNetwork();
		LayRoad(Actor, 0.0, ERoadKind::Taxiway);

		FPlotPlaceTool Tool(EPlaceableEntity::FuelDepot);
		Tool.OnClick(PlotAt(Actor, FVector2D(0.0, 1000.0)));
		TestEqual(TEXT("standing beside a taxiway anchors nothing"), Tool.PinnedCount(), 0);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotPinsOneCornerAtATimeTest,
	"Airside.Tool.PlotPinsOneCornerAtATime",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotPinsOneCornerAtATimeTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	Actor->ClearNetwork();
	Actor->FuelDepotDefinition = UEntityDefinition::MakeFuelDepotTransient();
	LayServiceRoad(Actor, 0.0);

	FPlotPlaceTool Tool(EPlaceableEntity::FuelDepot);
	TestEqual(TEXT("a fresh tool has pinned nothing"), Tool.PinnedCount(), 0);

	Tool.OnClick(OnRoad(Actor, FVector2D(0.0, 200.0)));
	TestEqual(TEXT("the first click pins the anchor"), Tool.PinnedCount(), 1);

	Tool.OnClick(PlotAt(Actor, FVector2D(2000.0, 200.0)));
	TestEqual(TEXT("the second pins the frontage"), Tool.PinnedCount(), 2);

	Tool.OnClick(PlotAt(Actor, FVector2D(2000.0, 1800.0)));
	TestEqual(TEXT("the third pins a back corner"), Tool.PinnedCount(), 3);

	Tool.OnClick(PlotAt(Actor, FVector2D(0.0, 1500.0)));
	TestEqual(TEXT("the fourth pins the last corner"), Tool.PinnedCount(), 4);
	TestEqual(TEXT("and the gesture is ready to commit"),
		static_cast<int32>(Tool.GetStage()), static_cast<int32>(EPlotStage::Confirm));

	// THE LAST CLICK LOCKS, IT DOES NOT BUILD. The review beat is the whole point of a
	// staged gesture; a click that committed would delete it.
	TestEqual(TEXT("and nothing was built by pinning it"), LiveEntities(Actor), 0);

	// AND CANCEL WALKS BACK ONE AT A TIME, which is the answer a misclick deserves.
	Tool.OnCancel(PlotAt(Actor, FVector2D(0.0, 1500.0)));
	TestEqual(TEXT("cancel unpins the last corner"), Tool.PinnedCount(), 3);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotFrontageSnapsInFiveMetreStepsTest,
	"Airside.Tool.PlotFrontageSnapsInFiveMetreSteps",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotFrontageSnapsInFiveMetreStepsTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	Actor->ClearNetwork();
	Actor->FuelDepotDefinition = UEntityDefinition::MakeFuelDepotTransient();
	LayServiceRoad(Actor, 0.0);

	// MEASURED FROM THE ANCHOR THE TOOL ACTUALLY CHOSE, not from an assumed one. OnRoad
	// computes SegmentT as if the road ran from -10000 to +10000 and it does not, so the
	// anchor lands somewhere this test has no business predicting - and an earlier version
	// of it silently measured every frontage from the wrong origin.
	auto FrontageFor = [&](double Reach)
	{
		FPlotPlaceTool Tool(EPlaceableEntity::FuelDepot);
		Tool.OnClick(OnRoad(Actor, FVector2D(0.0, 200.0)));

		TArray<FVector2D> Anchored;
		Tool.Quad(PlotAt(Actor, FVector2D(0.0, 200.0)), Anchored);
		if (Anchored.Num() < 1)
		{
			return 0.0;
		}

		TArray<FVector2D> Quad;
		Tool.Quad(PlotAt(Actor, Anchored[0] + FVector2D(Reach, 0.0)), Quad);
		return Quad.Num() >= 2 ? FVector2D::Distance(Quad[0], Quad[1]) : 0.0;
	};

	// ROUNDED, NOT TRUNCATED, and not left at 17. Rounding is the difference between a grid
	// that feels magnetic and one that feels grudging.
	TestEqual(TEXT("a 17 m drag locks at 15 m"), FrontageFor(1700.0), 1500.0);
	TestEqual(TEXT("an 18 m drag locks at 20 m"), FrontageFor(1800.0), 2000.0);
	TestEqual(TEXT("a 23 m drag locks at 25 m"), FrontageFor(2300.0), 2500.0);

	// THE FLOOR IS 15 m AND IT IS A FLOOR, not a step. A yard narrower is not a yard, so a
	// cursor 3 m along asks for the smallest plot there is rather than one nothing fits in.
	TestEqual(TEXT("a 3 m drag still asks for the 15 m minimum"), FrontageFor(300.0), 1500.0);

	// DRAGGED BACK PAST THE ANCHOR RUNS THE PLOT THE OTHER WAY rather than refusing or
	// collapsing. A player who anchors then changes their mind about direction should not
	// have to cancel and start again. This is what FPlotWidthRunsBothWaysTest used to pin.
	TestEqual(TEXT("dragging west of the anchor still gives a 20 m frontage"),
		FrontageFor(-1800.0), 2000.0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotBackCornersAreFreeTest,
	"Airside.Tool.PlotBackCornersAreFree",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotBackCornersAreFreeTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	Actor->ClearNetwork();
	Actor->FuelDepotDefinition = UEntityDefinition::MakeFuelDepotTransient();
	LayServiceRoad(Actor, 0.0);

	FPlotPlaceTool Tool(EPlaceableEntity::FuelDepot);
	Tool.OnClick(OnRoad(Actor, FVector2D(0.0, 200.0)));
	Tool.OnClick(PlotAt(Actor, FVector2D(2000.0, 200.0)));

	// AN ARBITRARY DEPTH, deliberately not a multiple of anything. The frontage is quantised
	// because it must tile with the plot next door; a back corner is shared with nothing, and
	// snapping it would only refuse shapes the ground calls for.
	TArray<FVector2D> Quad;
	Tool.Quad(PlotAt(Actor, FVector2D(2000.0, 1737.0)), Quad);
	if (!TestEqual(TEXT("a quad has four corners"), Quad.Num(), 4)) { return false; }

	TestTrue(*FString::Printf(TEXT("the back corner keeps the depth asked for, got %.0f"),
		Quad[2].Y), FMath::IsNearlyEqual(Quad[2].Y, 1737.0, 1.0));

	// AND THE TWO RULES ARE DEMONSTRABLY DIFFERENT, not accidentally the same: the frontage
	// in the very same quad did snap.
	TestEqual(TEXT("while the frontage in the same quad snapped to 20 m"),
		FVector2D::Distance(Quad[0], Quad[1]), 2000.0);

	// AND THE UNREACHED CORNER COMPLETES A PARALLELOGRAM, so two pinned corners read as a
	// finished shape the player adjusts rather than one trailing off.
	TestTrue(TEXT("the unreached corner completes a parallelogram"),
		(Quad[3] - Quad[0]).Equals(Quad[2] - Quad[1], 1.0));

	// SIDEWAYS, NOT JUST DEEPER. The back corners rode the frontage's normal until
	// 2026-09-17, which made every plot a trapezoid with perpendicular sides; a corner
	// dragged past the frontage's far end must actually go there.
	TArray<FVector2D> Leaning;
	Tool.Quad(PlotAt(Actor, FVector2D(3400.0, 1737.0)), Leaning);
	if (!TestEqual(TEXT("still four corners"), Leaning.Num(), 4)) { return false; }
	TestTrue(*FString::Printf(TEXT("a back corner goes where the cursor is, got x %.0f"),
		Leaning[2].X), FMath::IsNearlyEqual(Leaning[2].X, 3400.0, 1.0));

	// BUT NEVER BEHIND THE FRONTAGE. The plot goes on the side of the road the anchor chose;
	// a corner across that line would lay concrete on the carriageway.
	TArray<FVector2D> Behind;
	Tool.Quad(PlotAt(Actor, FVector2D(1000.0, -500.0)), Behind);
	if (!TestEqual(TEXT("four corners again"), Behind.Num(), 4)) { return false; }
	TestTrue(*FString::Printf(TEXT("a corner dragged onto the road slides to the frontage, "
		"got y %.0f"), Behind[2].Y), Behind[2].Y >= Behind[0].Y - 1.0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotStagesAdvanceTest,
	"Airside.Tool.PlotStagesAdvance",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotStagesAdvanceTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	Actor->ClearNetwork();
	Actor->FuelDepotDefinition = UEntityDefinition::MakeFuelDepotTransient();
	LayServiceRoad(Actor, 0.0);

	FPlotPlaceTool Tool(EPlaceableEntity::FuelDepot);
	TestEqual(TEXT("a fresh tool is idle"),
		static_cast<int32>(Tool.GetStage()), static_cast<int32>(EPlotStage::Idle));

	Tool.OnClick(OnRoad(Actor, FVector2D(0.0, 200.0)));
	TestEqual(TEXT("the first click anchors and asks for the frontage"),
		static_cast<int32>(Tool.GetStage()), static_cast<int32>(EPlotStage::Frontage));

	Tool.OnClick(PlotAt(Actor, FVector2D(1200.0, 200.0)));
	TestEqual(TEXT("the second pins the frontage and asks for a back corner"),
		static_cast<int32>(Tool.GetStage()), static_cast<int32>(EPlotStage::CornerA));

	Tool.OnClick(PlotAt(Actor, FVector2D(600.0, 1600.0)));
	TestEqual(TEXT("the third pins that corner and asks for the last"),
		static_cast<int32>(Tool.GetStage()), static_cast<int32>(EPlotStage::CornerB));

	Tool.OnClick(PlotAt(Actor, FVector2D(0.0, 1500.0)));
	TestEqual(TEXT("the fourth pins the last corner and asks for confirmation"),
		static_cast<int32>(Tool.GetStage()), static_cast<int32>(EPlotStage::Confirm));

	// THE LAST CLICK BUILDS NOTHING. The review beat is the whole point of the Build button;
	// a click that committed would delete it.
	TestEqual(TEXT("and nothing is built yet"), LiveEntities(Actor), 0);

	Tool.OnCommit(PlotAt(Actor, FVector2D(600.0, 1600.0)));
	TestEqual(TEXT("Build commits exactly one depot"), LiveEntities(Actor), 1);
	TestEqual(TEXT("and the tool returns to idle for the next one"),
		static_cast<int32>(Tool.GetStage()), static_cast<int32>(EPlotStage::Idle));

	const TArray<FEntityInstance>& Entities = Actor->Network->GetEntities();
	if (!TestTrue(TEXT("an entity to read"), Entities.Num() > 0)) { return false; }
	TestEqual(TEXT("the plot is a rectangle"), Entities[0].Outline.Num(), 4);

	// AND IT SITS ON THE SIDE OF THE ROAD THE CURSOR WAS. Every corner north of the road,
	// because that is where the player pointed - a fixed side would be wrong half the time.
	for (const FVector2D& Corner : Entities[0].Outline)
	{
		TestTrue(TEXT("the plot is north of the road, where the cursor was"), Corner.Y > -1.0);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotCancelStepsBackOneStageTest,
	"Airside.Tool.PlotCancelStepsBackOneStage",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotCancelStepsBackOneStageTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	Actor->ClearNetwork();
	LayServiceRoad(Actor, 0.0);

	FPlotPlaceTool Tool(EPlaceableEntity::FuelDepot);
	DrawPlot(Tool, Actor, FVector2D(0.0, 200.0), FVector2D(1200.0, 200.0),
		FVector2D(600.0, 1600.0));

	// ONE STAGE AT A TIME, the same answer the outline tool gives a misclick: binning the
	// whole gesture is harsher than the mistake deserves.
	const FToolContext Anywhere = PlotAt(Actor, FVector2D(600.0, 1600.0));

	Tool.OnCancel(Anywhere);
	TestEqual(TEXT("cancel from Confirm goes back to the last corner"),
		static_cast<int32>(Tool.GetStage()), static_cast<int32>(EPlotStage::CornerB));

	Tool.OnCancel(Anywhere);
	TestEqual(TEXT("and again to the first back corner"),
		static_cast<int32>(Tool.GetStage()), static_cast<int32>(EPlotStage::CornerA));

	Tool.OnCancel(Anywhere);
	TestEqual(TEXT("and again to the frontage"),
		static_cast<int32>(Tool.GetStage()), static_cast<int32>(EPlotStage::Frontage));

	Tool.OnCancel(Anywhere);
	TestEqual(TEXT("and again to Idle"),
		static_cast<int32>(Tool.GetStage()), static_cast<int32>(EPlotStage::Idle));

	Tool.OnCancel(Anywhere);
	TestEqual(TEXT("and cancelling an idle tool is harmless"),
		static_cast<int32>(Tool.GetStage()), static_cast<int32>(EPlotStage::Idle));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotCommitsOnlyFromConfirmTest,
	"Airside.Tool.PlotCommitsOnlyFromConfirm",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotCommitsOnlyFromConfirmTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	Actor->ClearNetwork();
	Actor->FuelDepotDefinition = UEntityDefinition::MakeFuelDepotTransient();
	LayServiceRoad(Actor, 0.0);

	// Build is a WIDGET, not a stage of the gesture, so OnCommit is reachable whenever the
	// bar is on screen. EVERY stage but the last must ignore it - committing from a half
	// drawn quad would build a plot the player never finished describing.
	FPlotPlaceTool Tool(EPlaceableEntity::FuelDepot);

	Tool.OnCommit(PlotAt(Actor, FVector2D(0.0, 200.0)));
	TestEqual(TEXT("Build with nothing pinned builds nothing"), LiveEntities(Actor), 0);

	Tool.OnClick(OnRoad(Actor, FVector2D(0.0, 200.0)));
	Tool.OnCommit(PlotAt(Actor, FVector2D(0.0, 200.0)));
	TestEqual(TEXT("Build with one corner builds nothing"), LiveEntities(Actor), 0);

	Tool.OnClick(PlotAt(Actor, FVector2D(2000.0, 200.0)));
	Tool.OnCommit(PlotAt(Actor, FVector2D(2000.0, 200.0)));
	TestEqual(TEXT("Build with the frontage alone builds nothing"), LiveEntities(Actor), 0);

	Tool.OnClick(PlotAt(Actor, FVector2D(2000.0, 1800.0)));
	Tool.OnCommit(PlotAt(Actor, FVector2D(2000.0, 1800.0)));
	TestEqual(TEXT("Build with three corners builds nothing"), LiveEntities(Actor), 0);

	Tool.OnClick(PlotAt(Actor, FVector2D(0.0, 1800.0)));
	Tool.OnCommit(PlotAt(Actor, FVector2D(0.0, 1800.0)));
	TestEqual(TEXT("and only with four does it build"), LiveEntities(Actor), 1);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotAnchorsSnapToTheFrontageStepTest,
	"Airside.Tool.PlotAnchorsSnapToTheFrontageStep",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotAnchorsSnapToTheFrontageStepTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	Actor->ClearNetwork();
	Actor->FuelDepotDefinition = UEntityDefinition::MakeFuelDepotTransient();
	LayServiceRoad(Actor, 0.0);

	// A cursor BETWEEN two bay multiples anchors on one of them, not where it was clicked.
	// Without this a plot starts at an arbitrary offset and two depots on one road can never
	// sit flush - which is what the snap dots promise the player.
	FPlotPlaceTool Tool(EPlaceableEntity::FuelDepot);
	DrawPlot(Tool, Actor, FVector2D(517.0, 200.0), FVector2D(1717.0, 200.0),
		FVector2D(1100.0, 2200.0));
	Tool.OnCommit(PlotAt(Actor, FVector2D(1100.0, 2200.0)));

	if (!TestEqual(TEXT("a depot is built"), LiveEntities(Actor), 1)) { return false; }

	// The road runs from x = -10000, so an anchor measured from its A end is -10000 + N * the
	// frontage step - itself a multiple of that step. NOT a bay multiple any more: 4 m is the
	// width of a SHED, and a plot strides in 5 m.
	double MinX = TNumericLimits<double>::Max();
	for (const FVector2D& Corner : Actor->Network->GetEntities()[0].Outline)
	{
		MinX = FMath::Min(MinX, Corner.X);
	}

	const double Remainder = FMath::Fmod(FMath::Abs(MinX), PlotGesture::FrontageStepUu);
	TestTrue(TEXT("the plot starts on a frontage step, not where the cursor was"),
		Remainder < 1.0 || Remainder > PlotGesture::FrontageStepUu - 1.0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotIgnoresATaxiwayTest,
	"Airside.Tool.PlotIgnoresATaxiway",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotIgnoresATaxiwayTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	Actor->ClearNetwork();

	// A TAXIWAY, not a service road. A depot anchored here would dispatch its trucks onto
	// one, so the gesture must refuse to start at all rather than build something that looks
	// right and routes an aeroplane into a fuel truck.
	LayRoad(Actor, 0.0, ERoadKind::Taxiway);

	// SNAPPED TO IT PROPERLY, which is the whole point of this test. An earlier version left
	// the context at ContextAt's default Free kind, so the tool refused for want of ANY
	// segment and this passed without ever reaching the service-road filter - green for a
	// reason that had nothing to do with what it claims to check.
	FPlotPlaceTool Tool(EPlaceableEntity::FuelDepot);
	Tool.OnClick(OnRoad(Actor, FVector2D(0.0, 200.0)));

	TestEqual(TEXT("a taxiway does not anchor a depot plot"),
		static_cast<int32>(Tool.GetStage()), static_cast<int32>(EPlotStage::Idle));

	// AND THE READOUT SAYS WHY, rather than the tool silently doing nothing - a click that
	// is ignored with no explanation reads as a broken tool.
	FToolReadoutCollector Collector;
	Tool.BuildReadout(OnRoad(Actor, FVector2D(0.0, 200.0)), Collector);
	TestEqual(TEXT("and the readout says to move near a service road"),
		Collector.Readout.Warnings.Num(), 1);
	TestFalse(TEXT("and nothing is committable"), Collector.Readout.bCommittable);

	return true;
}

/**
 * THE PREVIEW AND THE READOUT, MEASURED AGAINST EACH OTHER - which the test named
 * PlotReadoutMatchesPreview does not actually do: it reads facts at the Confirm stage and
 * never looks at a line the tool drew.
 *
 * The case is the SECOND plot of a session. Stage returns to Idle but Width and Depth keep
 * what the last gesture locked, so a stale depth is there to be drawn; the first plot of a
 * session cannot catch this because Depth still holds its initial 1.
 */
/**
 * THE PAD DOES NOT LIE ON THE ROAD.
 *
 * A segment's ends are NODE positions, so every frame of this gesture is derived from the
 * centreline; anchoring there built the depot over half the carriageway. PIE, 2026-09-16:
 * "the plot needs to be built on the side of the road, not overlapping".
 *
 * Measured against the profile's own half width rather than a typed number, so widening the
 * service road cannot quietly re-break this.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotClearsTheCarriagewayTest,
	"Airside.Tool.PlotClearsTheCarriageway",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotClearsTheCarriagewayTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	Actor->ClearNetwork();
	Actor->FuelDepotDefinition = UEntityDefinition::MakeFuelDepotTransient();
	LayServiceRoad(Actor, 0.0);

	const FRoadSegment* Road = Actor->Network->GetSegment(Actor->Network->SegmentIdAt(0));
	if (!TestNotNull(TEXT("a road segment"), Road)) { return false; }
	if (!TestNotNull(TEXT("with a profile"), Road->Profile.Get())) { return false; }
	const double HalfWidth = Road->Profile->GetMaxHalfWidth();
	if (!TestTrue(TEXT("the road has width to clear"), HalfWidth > 0.0)) { return false; }

	// Drawn on the +Y side: cursor north of the road at every stage.
	FPlotPlaceTool Tool(EPlaceableEntity::FuelDepot);
	DrawPlot(Tool, Actor, FVector2D(0.0, 200.0), FVector2D(1200.0, 200.0),
		FVector2D(600.0, 1600.0));
	Tool.OnCommit(PlotAt(Actor, FVector2D(600.0, 1600.0)));

	const FEntityInstance* Placed = nullptr;
	for (const FEntityInstance& Entity : Actor->Network->GetEntities())
	{
		if (Entity.bAlive) { Placed = &Entity; break; }
	}
	if (!TestNotNull(TEXT("a depot was placed"), Placed)) { return false; }

	double NearestY = TNumericLimits<double>::Max();
	for (const FVector2D& Point : Placed->Outline)
	{
		NearestY = FMath::Min(NearestY, Point.Y);
	}

	// NOT "greater than zero" - that would pass with the frontage one millimetre off the
	// centreline, still buried in the tarmac. It has to clear the kerb.
	TestTrue(*FString::Printf(
		TEXT("the pad starts at the kerb (%.0f) rather than on the centreline, got %.0f"),
		HalfWidth, NearestY), NearestY >= HalfWidth - 1.0);

	// AND THE OTHER SIDE, because the offset travels with Inward and a sign error would
	// clear the road going north and drive the plot straight through it going south.
	Actor->ClearNetwork();
	LayServiceRoad(Actor, 0.0);
	FPlotPlaceTool South(EPlaceableEntity::FuelDepot);
	DrawPlot(South, Actor, FVector2D(0.0, -200.0), FVector2D(1200.0, -200.0),
		FVector2D(600.0, -2200.0));
	South.OnCommit(PlotAt(Actor, FVector2D(600.0, -2200.0)));

	const FEntityInstance* Below = nullptr;
	for (const FEntityInstance& Entity : Actor->Network->GetEntities())
	{
		if (Entity.bAlive) { Below = &Entity; break; }
	}
	if (!TestNotNull(TEXT("a depot was placed south of the road"), Below)) { return false; }

	double FurthestY = -TNumericLimits<double>::Max();
	for (const FVector2D& Point : Below->Outline)
	{
		FurthestY = FMath::Max(FurthestY, Point.Y);
	}
	TestTrue(*FString::Printf(
		TEXT("a plot drawn south clears the kerb too, got %.0f"), FurthestY),
		FurthestY <= -HalfWidth + 1.0);

	return true;
}

/**
 * THE GHOST DRAWS THE MODULES, not marks about a grid that no longer exists.
 *
 * BuildPreview used to cross-mark "which way each bay faces" and ring "slots behind row 1",
 * both from a bay grid nothing built any more, since deleted outright. The
 * player's verdict on marks describing a deleted structure: "I'm not actually sure what they
 * are supposed to be telling me" (PIE, 2026-09-16).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotDrawsOnlyWhatIsPinnedTest,
	"Airside.Tool.PlotDrawsOnlyWhatIsPinned",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotDrawsOnlyWhatIsPinnedTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	Actor->ClearNetwork();
	Actor->FuelDepotDefinition = UEntityDefinition::MakeFuelDepotTransient();
	LayServiceRoad(Actor, 0.0);

	FPlotPlaceTool Tool(EPlaceableEntity::FuelDepot);

	// ONE POINT: the frontage, and NOTHING ELSE. At one corner the shape is not decided, so
	// a boundary drawn here is a promise the next click breaks - which is the whole reason
	// this test, and the document behind it, exist.
	Tool.OnClick(OnRoad(Actor, FVector2D(0.0, 200.0)));
	{
		FPlotGhostSink Sink;
		Tool.BuildPreview(PlotAt(Actor, FVector2D(3000.0, 200.0)), Sink);

		TestEqual(TEXT("one pinned corner draws one frontage edge and no more"),
			Sink.Lines, 1);
		TestEqual(TEXT("and no module footprints"),
			Sink.LinesOf(EPreviewStyle::Pending), 0);
	}

	// TWO POINTS: a boundary appears, and the edges still moving are provisional.
	Tool.OnClick(PlotAt(Actor, FVector2D(3000.0, 200.0)));
	{
		FPlotGhostSink Sink;
		Tool.BuildPreview(PlotAt(Actor, FVector2D(3000.0, 1800.0)), Sink);

		TestEqual(TEXT("the pinned frontage is solid"),
			Sink.LinesOf(EPreviewStyle::Pinned), 1);
		TestTrue(TEXT("and the rest of the boundary is provisional"),
			Sink.LinesOf(EPreviewStyle::Provisional) > 0);

		// STILL NO CONTENTS. Both back corners are unknown, so the plot has no settled depth
		// anywhere and anything drawn inside it would move on the next two clicks.
		TestEqual(TEXT("two corners is too early to promise what fits"),
			Sink.LinesOf(EPreviewStyle::Pending), 0);
	}

	// THREE POINTS: only the last corner moves, so what is drawn inside is a promise the
	// gesture can keep.
	Tool.OnClick(PlotAt(Actor, FVector2D(3000.0, 1800.0)));
	{
		FPlotGhostSink Sink;
		Tool.BuildPreview(PlotAt(Actor, FVector2D(0.0, 1800.0)), Sink);

		TestTrue(TEXT("three corners is when the contents appear"),
			Sink.LinesOf(EPreviewStyle::Pending) > 0);

		// WHOLE FOOTPRINTS, four lines apiece - the same rectangles the readout counts.
		TestEqual(TEXT("and they are whole footprints, four lines each"),
			Sink.LinesOf(EPreviewStyle::Pending) % 4, 0);

		// TWO EDGES PINNED BY NOW: the frontage, and the one running back from its far end.
		TestEqual(TEXT("and two edges have stopped moving"),
			Sink.LinesOf(EPreviewStyle::Pinned), 2);
	}

	// A DOT PER PINNED CORNER, so "Plot Points: 3/4" has something on the ground to count
	// against rather than being a number the player has to take on trust.
	{
		FPlotGhostSink Sink;
		Tool.BuildPreview(PlotAt(Actor, FVector2D(0.0, 1800.0)), Sink);
		TestEqual(TEXT("three pinned corners draw three dots"),
			Sink.MarkersOf(EPreviewStyle::Pinned), 3);
	}

	return true;
}

/**
 * A FOLDED QUAD IS REFUSED AT THE CLICK THAT WOULD MAKE IT.
 *
 * This began life asserting the same thing on 2026-09-16, pinned a perfectly legal plot, and
 * was replaced by an invariant - because while the back corners rode the frontage's normal
 * no cursor position could fold the shape. Freeing them to move sideways on 2026-09-17
 * brought the fold back, and the refusal with it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotRefusesACrossedQuadTest,
	"Airside.Tool.PlotRefusesACrossedQuad",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotRefusesACrossedQuadTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	Actor->ClearNetwork();
	Actor->FuelDepotDefinition = UEntityDefinition::MakeFuelDepotTransient();
	LayServiceRoad(Actor, 0.0);

	FPlotPlaceTool Tool(EPlaceableEntity::FuelDepot);
	Tool.OnClick(OnRoad(Actor, FVector2D(0.0, 200.0)));
	Tool.OnClick(PlotAt(Actor, FVector2D(2000.0, 200.0)));
	Tool.OnClick(PlotAt(Actor, FVector2D(2000.0, 1800.0)));
	if (!TestEqual(TEXT("three corners are down"), Tool.PinnedCount(), 3)) { return false; }

	// PAST THE CORNER BEFORE IT. The quad is (0,300) (2000,300) (2000,1800) and a last corner
	// at (3000,1800) closes back across the edge running out to corner 2 - the shape folds
	// through itself, and the ear-clipper downstream would make overlapping faces of it
	// rather than refusing.
	const FVector2D Folding(3000.0, 1800.0);
	{
		TArray<FVector2D> Shown;
		Tool.Quad(PlotAt(Actor, Folding), Shown);
		if (!TestEqual(TEXT("four corners to judge"), Shown.Num(), 4)) { return false; }

		// ASSERTED, not assumed: if this shape is simple then the test below proves nothing,
		// which is exactly how its predecessor passed while checking the wrong thing.
		TestFalse(TEXT("that really is a crossed quad"), RoadGeom::IsSimplePolygon(Shown));
	}

	Tool.OnClick(PlotAt(Actor, Folding));
	TestEqual(TEXT("a crossed quad does not pin"), Tool.PinnedCount(), 3);

	// AND A LEGAL LAST CORNER STILL PINS, or this would pass on a tool that refused
	// everything - the shape a refusal test fails in.
	Tool.OnClick(PlotAt(Actor, FVector2D(0.0, 1500.0)));
	TestEqual(TEXT("but a legal one is accepted"), Tool.PinnedCount(), 4);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotGhostDrawsTheModulesTest,
	"Airside.Tool.PlotGhostDrawsTheModules",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotGhostDrawsTheModulesTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	Actor->ClearNetwork();
	Actor->FuelDepotDefinition = UEntityDefinition::MakeFuelDepotTransient();
	LayServiceRoad(Actor, 0.0);

	// 30 m of frontage, 24 m deep - big enough that every module of the mix stands, so the
	// footprint count below is a real number rather than whatever survived a cramped plot.
	FPlotPlaceTool Tool(EPlaceableEntity::FuelDepot);
	DrawPlot(Tool, Actor, FVector2D(0.0, 200.0), FVector2D(3000.0, 200.0),
		FVector2D(1500.0, 2700.0));

	const FToolContext Confirming = PlotAt(Actor, FVector2D(1500.0, 2700.0));

	FPlotGhostSink Sink;
	Tool.BuildPreview(Confirming, Sink);

	// NOT A CROSS-MARK IN SIGHT. They were the grid's own vocabulary and the grid is gone;
	// leaving them would be drawing a claim about the plot that is no longer true.
	TestEqual(TEXT("no bay cross-marks survive"), Sink.CrossMarks, 0);

	// FOUR LINES FOR THE PLOT, FOUR FOR EACH STAND. A rectangle is four Line calls through
	// IToolPreviewSink::Polygon, so the count says exactly how many footprints were drawn -
	// and it would not move at all if BuildPreview stopped drawing modules entirely.
	//
	// STANDS, NOT BAYS, and the two parted company when runs arrived: a three-bay shed run is
	// ONE outline. Summing the readout's ceilings here would expect three rectangles where
	// the ghost honestly draws one, so the count comes from the reservation itself.
	const TArray<PlotYard::FKitSpec> Specs = DepotKitSpecs(UAirsideSettings::GetContent());
	TArray<FVector2D> Shown;
	Tool.Quad(Confirming, Shown);
	if (!TestEqual(TEXT("a confirmed plot has four corners"), Shown.Num(), 4))
	{
		return false;
	}
	// THROUGH THE SEAM, because the tool goes through it. Calling PlotYard::Reserve here would
	// measure the ghost of a band yard against the stand count of a scattered one.
	const FVector2D Pose = (Shown[0] + Shown[1]) * 0.5;
	FPlotSite Site;
	Site.Outline = Shown;
	Site.FrontageA = Shown[0];
	Site.FrontageB = Shown[1];
	Site.Gate = Pose;
	Site.Seed = DepotYardSeed(Pose);

	const PlotYard::FReservation Reservation =
		PlotLayoutFor(EPlotLayout::FuelYardBands)->Solve(Site, Specs);

	if (!TestTrue(TEXT("a plot this size reserves something"),
		Reservation.Stands.Num() > 0))
	{
		return false;
	}
	TestEqual(TEXT("the ghost outlines the plot and every stand reserved in it"),
		Sink.Lines, 4 + 4 * Reservation.Stands.Num());

	return true;
}

/**
 * THE ANCHOR A CLICK WOULD TAKE IS VISIBLE BEFORE THE CLICK.
 *
 * A row of identical dots says where anchors exist; it does not say which one the cursor
 * has. "I would prefer a mouse snap to a point to select the initial point" (PIE,
 * 2026-09-16) - the snap was always there, it just could not be seen.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotShowsTheAnchorItWouldTakeTest,
	"Airside.Tool.PlotShowsTheAnchorItWouldTake",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotShowsTheAnchorItWouldTakeTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	Actor->ClearNetwork();
	Actor->FuelDepotDefinition = UEntityDefinition::MakeFuelDepotTransient();
	LayServiceRoad(Actor, 0.0);

	// Idle, hovering the road: the stage where the anchor dots are offered.
	FPlotPlaceTool Tool(EPlaceableEntity::FuelDepot);
	FPlotGhostSink Sink;
	Tool.BuildPreview(OnRoad(Actor, FVector2D(0.0, 200.0)), Sink);

	TestTrue(TEXT("anchor dots are offered along the road"),
		Sink.MarkerStyles.Num() > 1);

	// EXACTLY ONE, or the highlight means nothing: none and the cursor is unreadable, more
	// than one and it is pointing at several places at once.
	TestEqual(TEXT("exactly one of them is the anchor the click would take"),
		Sink.MarkersOf(EPreviewStyle::Pending), 1);

	// AND IT MOVES WITH THE CURSOR. A highlight nailed to the segment's A end would satisfy
	// the count above on every frame while telling the player nothing.
	FPlotGhostSink Far;
	Tool.BuildPreview(OnRoad(Actor, FVector2D(4000.0, 200.0)), Far);
	TestEqual(TEXT("still exactly one, further down the road"),
		Far.MarkersOf(EPreviewStyle::Pending), 1);

	int32 NearIndex = INDEX_NONE;
	int32 FarIndex = INDEX_NONE;
	for (int32 I = 0; I < Sink.MarkerStyles.Num(); ++I)
	{
		if (Sink.MarkerStyles[I] == EPreviewStyle::Pending) { NearIndex = I; }
	}
	for (int32 I = 0; I < Far.MarkerStyles.Num(); ++I)
	{
		if (Far.MarkerStyles[I] == EPreviewStyle::Pending) { FarIndex = I; }
	}
	TestTrue(TEXT("and it is a different dot for a cursor 40 m along"), NearIndex != FarIndex);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotGhostAgreesWithTheBarTest,
	"Airside.Tool.PlotGhostAgreesWithTheBar",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotGhostAgreesWithTheBarTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	Actor->ClearNetwork();
	Actor->FuelDepotDefinition = UEntityDefinition::MakeFuelDepotTransient();
	LayServiceRoad(Actor, 0.0);

	FPlotPlaceTool Tool(EPlaceableEntity::FuelDepot);

	// A first plot THREE rows deep, then backed out of stage by stage. Committing would do
	// as well; what matters is only that Depth is left holding 3.
	DrawPlot(Tool, Actor, FVector2D(0.0, 200.0), FVector2D(1200.0, 200.0),
		FVector2D(600.0, 2400.0));
	for (int32 I = 0; I < 4; ++I)
	{
		Tool.OnCancel(PlotAt(Actor, FVector2D(600.0, 2400.0)));
	}
	TestEqual(TEXT("backed all the way out"),
		static_cast<int32>(Tool.GetStage()), static_cast<int32>(EPlotStage::Idle));

	// The second gesture, mid-frontage: the stage where a stale corner would show.
	Tool.OnClick(OnRoad(Actor, FVector2D(0.0, 200.0)));
	TestEqual(TEXT("anchored again"),
		static_cast<int32>(Tool.GetStage()), static_cast<int32>(EPlotStage::Frontage));

	const FToolContext Dragging = PlotAt(Actor, FVector2D(1600.0, 200.0));

	FPlotGhostSink Sink;
	Tool.BuildPreview(Dragging, Sink);

	FToolReadoutCollector Collector;
	Tool.BuildReadout(Dragging, Collector);

	const TPair<FString, FString>* Frontage = Collector.Readout.Facts.FindByPredicate(
		[](const TPair<FString, FString>& F) { return F.Key == TEXT("Frontage"); });
	if (!TestNotNull(TEXT("a Frontage fact"), Frontage)) { return false; }

	// 16 m ASKED FOR ROUNDS TO 15 m, and the readout must say what the line on the ground
	// says. The predecessor of this assertion caught a ghost drawing the PREVIOUS gesture's
	// depth, which is why the first gesture above is drawn and backed out of: on a session's
	// FIRST gesture every member still holds its initial value, where stale and correct agree.
	TestEqual(TEXT("the readout reports the frontage that actually snapped"),
		Frontage->Value, FString(TEXT("15 m")));

	return true;
}

/**
 * THE ROOM THE READOUT PROMISES IS THE ROOM THE DEPOT HAS.
 *
 * "Expansion slots" was width * depth - placed: arithmetic over a bay grid that nothing
 * builds any more. The replacement runs the SAME solver UPlotPresenter runs, seeded off the
 * frontage midpoint - which is the Position URoadEditFacade::PlaceEntityInPlot stores - so
 * the two are not merely consistent by inspection, they are the same computation.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotReadoutCountsRoomNotSlotsTest,
	"Airside.Tool.PlotReadoutCountsRoomNotSlots",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotReadoutCountsRoomNotSlotsTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
	if (!TestNotNull(TEXT("a plot presenter"), Actor->GetPlotPresenter())) { return false; }

	Actor->ClearNetwork();
	Actor->FuelDepotDefinition = UEntityDefinition::MakeFuelDepotTransient();
	LayServiceRoad(Actor, 0.0);

	// 30 m OF FRONTAGE, 24 m DEEP, and the size is the point. Drawn 12 m wide this passed
	// only because a broken anchor put the plot's origin 20 m up the road and made it
	// accidentally 30 m after all; at a true 15 m there is no room left once the mix is
	// standing, and "reports room to grow" would have been asserting the opposite of itself.
	FPlotPlaceTool Tool(EPlaceableEntity::FuelDepot);
	DrawPlot(Tool, Actor, FVector2D(0.0, 200.0), FVector2D(3000.0, 200.0),
		FVector2D(1500.0, 2700.0));

	const FToolContext Confirming = PlotAt(Actor, FVector2D(1500.0, 2700.0));
	FToolReadoutCollector Collector;
	Tool.BuildReadout(Confirming, Collector);

	// "EXPANSION SLOTS" WAS A CLAIM ABOUT BAYS, and modules no longer stand in bays. A fact
	// whose name survived its meaning is worse than one that was removed: the player reads a
	// number describing a structure the plot does not have.
	const TPair<FString, FString>* Slots = Collector.Readout.Facts.FindByPredicate(
		[](const TPair<FString, FString>& F) { return F.Key == TEXT("Expansion slots"); });
	TestNull(TEXT("the bay-slot fact is gone"), Slots);

	// "ROOM FOR N" IS GONE TOO, and for the same reason the slot fact went. It could only
	// ever mean "N of the sample footprint" - a number about a phantom tank rather than
	// about anything the player can buy. A line per kit says what the plot will HOLD.
	const TPair<FString, FString>* Room = Collector.Readout.Facts.FindByPredicate(
		[](const TPair<FString, FString>& F) { return F.Key == TEXT("Room for"); });
	TestNull(TEXT("the sampled room fact is gone"), Room);

	const TPair<FString, FString>* Sheds = Collector.Readout.Facts.FindByPredicate(
		[](const TPair<FString, FString>& F) { return F.Key == TEXT("Sheds"); });
	if (!TestNotNull(TEXT("a Sheds fact"), Sheds)) { return false; }

	int32 Promised = 0;
	for (const TCHAR* Label : { TEXT("Sheds"), TEXT("Tanks"), TEXT("Pumps") })
	{
		const FString Wanted(Label);
		const TPair<FString, FString>* Fact = Collector.Readout.Facts.FindByPredicate(
			[&Wanted](const TPair<FString, FString>& F) { return F.Key == Wanted; });
		if (TestNotNull(*FString::Printf(TEXT("a %s fact"), Label), Fact))
		{
			Promised += FCString::Atoi(*Fact->Value);
		}
	}
	TestTrue(TEXT("a plot this size holds something"), Promised > 0);

	// THE AGREEMENT. Commit the very gesture that was read out, then ask the presenter what
	// the built depot actually has. A preview seeded differently from the placement would
	// pass every other assertion here and quietly promise a yard the player never gets.
	//
	// BUILT PLUS GHOSTED, because the ceilings count every bay the plot holds and the depot
	// arrives with its starter mix already lit. Comparing against the ghosts alone would be
	// comparing capacity with capacity-minus-what-was-bought.
	Tool.OnCommit(Confirming);
	Actor->RebuildMesh();

	const UPlotPresenter* Plots = Actor->GetPlotPresenter();
	TestEqual(TEXT("the bays promised are the bays the built depot has"),
		Plots->GetModuleCount() + Plots->GetGhostCount(), Promised);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotReadoutMatchesPreviewTest,
	"Airside.Tool.PlotReadoutMatchesPreview",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotReadoutMatchesPreviewTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	Actor->ClearNetwork();
	Actor->FuelDepotDefinition = UEntityDefinition::MakeFuelDepotTransient();
	LayServiceRoad(Actor, 0.0);

	FPlotPlaceTool Tool(EPlaceableEntity::FuelDepot);

	// IDLE IS NOT COMMITTABLE. Every stage but the last must say so, or the Build button
	// lights before there is anything to build.
	{
		FToolReadoutCollector Collector;
		Tool.BuildReadout(PlotAt(Actor, FVector2D(0.0, 200.0)), Collector);
		TestFalse(TEXT("idle is not committable"), Collector.Readout.bCommittable);
	}

	// Three bays wide, and DEEP ENOUGH TO HOLD SOMETHING. It was 6 m deep and held nothing
	// once PlotFit::BayDepthUu went to 12 m - a plot that holds nothing warns, which is what
	// the warning assertion below is about.
	DrawPlot(Tool, Actor, FVector2D(0.0, 200.0), FVector2D(1200.0, 200.0),
		FVector2D(600.0, 2000.0));

	FToolReadoutCollector Collector;
	Tool.BuildReadout(PlotAt(Actor, FVector2D(600.0, 2000.0)), Collector);

	TestTrue(TEXT("confirm is committable"), Collector.Readout.bCommittable);

	const TPair<FString, FString>* Bays = Collector.Readout.Facts.FindByPredicate(
		[](const TPair<FString, FString>& F) { return F.Key == TEXT("Frontage"); });
	if (!TestNotNull(TEXT("a Frontage fact"), Bays)) { return false; }

	// THE NUMBER THE PLAYER READS IS THE WIDTH THAT WAS DRAGGED. That agreement is the whole
	// reason facts are emitted from a const per-frame call beside the preview rather than
	// held as state the bar polls.
	TestEqual(TEXT("three bays, the width that was dragged"),
		Bays->Value, FString(TEXT("15 m")));

	// A FULL PLOT NO LONGER WARNS. "No room to grow" fired whenever a depot had no spare bay,
	// which under reservation is the NORMAL end state of a well-drawn plot - the warning
	// would cry wolf on every one of them. A plot that holds NOTHING is the case worth
	// naming, and 12 x 6 m is not that.
	//
	// The analogue of Manor Lords' "Plots without Extension Space" is now the ghost itself:
	// the player sees the empty slots rather than reading that they exist.
	TestEqual(TEXT("a plot that holds something does not warn"),
		Collector.Readout.Warnings.Num(), 0);

	// AND THE READOUT NAMES WHAT IT HOLDS, per kit. This is the fact that replaced both
	// "Modules N of M" and "Room for N".
	const TPair<FString, FString>* Sheds = Collector.Readout.Facts.FindByPredicate(
		[](const TPair<FString, FString>& F) { return F.Key == TEXT("Sheds"); });
	TestNotNull(TEXT("a Sheds fact"), Sheds);

	return true;
}

#endif
