#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Solve/PlotFit.h"
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

		// The one segment laid above, running from x = -10000 to x = +10000.
		Context.Snap.Segment = Actor->Network->SegmentIdAt(0);
		Context.Snap.SegmentT = FMath::Clamp((Where.X + 10000.0) / 20000.0, 0.0, 1.0);
		return Context;
	}

	/** Anchor, width east, depth north, in the stages the gesture expects. */
	void DrawPlot(FPlotPlaceTool& Tool, ARoadNetworkActor* Actor,
		const FVector2D& AnchorAt, const FVector2D& WidthAt, const FVector2D& DepthAt)
	{
		// ONLY THE ANCHOR NEEDS THE ROAD. The width and depth clicks are read off the cursor
		// against the anchored frame, so their snap kind is irrelevant - which is also why a
		// plot can be dragged out over open ground.
		Tool.OnClick(OnRoad(Actor, AnchorAt));
		Tool.OnClick(PlotAt(Actor, WidthAt));
		Tool.OnClick(PlotAt(Actor, DepthAt));
	}
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
	TestEqual(TEXT("the first click anchors and asks for width"),
		static_cast<int32>(Tool.GetStage()), static_cast<int32>(EPlotStage::Width));

	Tool.OnClick(PlotAt(Actor, FVector2D(1200.0, 200.0)));
	TestEqual(TEXT("the second locks width and asks for depth"),
		static_cast<int32>(Tool.GetStage()), static_cast<int32>(EPlotStage::Depth));

	Tool.OnClick(PlotAt(Actor, FVector2D(600.0, 1600.0)));
	TestEqual(TEXT("the third locks depth and asks for confirmation"),
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
	TestEqual(TEXT("cancel from Confirm goes back to Depth"),
		static_cast<int32>(Tool.GetStage()), static_cast<int32>(EPlotStage::Depth));

	Tool.OnCancel(Anywhere);
	TestEqual(TEXT("and again to Width"),
		static_cast<int32>(Tool.GetStage()), static_cast<int32>(EPlotStage::Width));

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
	// bar is on screen. Every stage but the last must ignore it - committing from Width
	// would build a plot with no depth at all.
	FPlotPlaceTool Tool(EPlaceableEntity::FuelDepot);

	Tool.OnCommit(PlotAt(Actor, FVector2D(0.0, 200.0)));
	TestEqual(TEXT("Build in Idle builds nothing"), LiveEntities(Actor), 0);

	Tool.OnClick(OnRoad(Actor, FVector2D(0.0, 200.0)));
	Tool.OnCommit(PlotAt(Actor, FVector2D(0.0, 200.0)));
	TestEqual(TEXT("Build in Width builds nothing"), LiveEntities(Actor), 0);

	Tool.OnClick(PlotAt(Actor, FVector2D(1200.0, 200.0)));
	Tool.OnCommit(PlotAt(Actor, FVector2D(1200.0, 200.0)));
	TestEqual(TEXT("Build in Depth builds nothing"), LiveEntities(Actor), 0);

	Tool.OnClick(PlotAt(Actor, FVector2D(600.0, 1600.0)));
	Tool.OnCommit(PlotAt(Actor, FVector2D(600.0, 1600.0)));
	TestEqual(TEXT("and only from Confirm does it build"), LiveEntities(Actor), 1);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotWidthRunsBothWaysTest,
	"Airside.Tool.PlotWidthRunsBothWays",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotWidthRunsBothWaysTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	Actor->ClearNetwork();
	Actor->FuelDepotDefinition = UEntityDefinition::MakeFuelDepotTransient();
	LayServiceRoad(Actor, 0.0);

	// WEST of the anchor. Dragging back past it must run the plot the other way rather than
	// refusing or collapsing - a player who anchors and then changes their mind about which
	// way to go should not have to cancel and start again.
	FPlotPlaceTool Tool(EPlaceableEntity::FuelDepot);
	DrawPlot(Tool, Actor, FVector2D(0.0, 200.0), FVector2D(-1200.0, 200.0),
		FVector2D(-600.0, 1600.0));
	Tool.OnCommit(PlotAt(Actor, FVector2D(-600.0, 1600.0)));

	if (!TestEqual(TEXT("a westward plot is built"), LiveEntities(Actor), 1)) { return false; }

	const FEntityInstance& Depot = Actor->Network->GetEntities()[0];

	double MinX = TNumericLimits<double>::Max();
	for (const FVector2D& Corner : Depot.Outline)
	{
		MinX = FMath::Min(MinX, Corner.X);
	}
	TestTrue(TEXT("and it lies west of the anchor"), MinX < -100.0);

	// STILL NORTH OF THE ROAD. Flipping the along-direction without flipping the side with
	// it would put the whole plot across the road - a much worse outcome than refusing, and
	// invisible in a test that only checked which way it ran.
	for (const FVector2D& Corner : Depot.Outline)
	{
		TestTrue(TEXT("and still on the side the cursor was"), Corner.Y > -1.0);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotAnchorsSnapToBaysTest,
	"Airside.Tool.PlotAnchorsSnapToBays",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotAnchorsSnapToBaysTest::RunTest(const FString& Parameters)
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
		FVector2D(1100.0, 1600.0));
	Tool.OnCommit(PlotAt(Actor, FVector2D(1100.0, 1600.0)));

	if (!TestEqual(TEXT("a depot is built"), LiveEntities(Actor), 1)) { return false; }

	// The road runs from x = -10000, so a bay multiple measured from its A end is
	// -10000 + N * 400 - which is itself a multiple of 400.
	double MinX = TNumericLimits<double>::Max();
	for (const FVector2D& Corner : Actor->Network->GetEntities()[0].Outline)
	{
		MinX = FMath::Min(MinX, Corner.X);
	}

	const double Remainder = FMath::Fmod(FMath::Abs(MinX), PlotFit::BayWidthUu);
	TestTrue(TEXT("the plot starts on a bay multiple, not where the cursor was"),
		Remainder < 1.0 || Remainder > PlotFit::BayWidthUu - 1.0);

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

	// Three bays wide, ONE row deep.
	DrawPlot(Tool, Actor, FVector2D(0.0, 200.0), FVector2D(1200.0, 200.0),
		FVector2D(600.0, 800.0));

	FToolReadoutCollector Collector;
	Tool.BuildReadout(PlotAt(Actor, FVector2D(600.0, 800.0)), Collector);

	TestTrue(TEXT("confirm is committable"), Collector.Readout.bCommittable);

	const TPair<FString, FString>* Bays = Collector.Readout.Facts.FindByPredicate(
		[](const TPair<FString, FString>& F) { return F.Key == TEXT("Bays"); });
	if (!TestNotNull(TEXT("a Bays fact"), Bays)) { return false; }

	// THE NUMBER THE PLAYER READS IS THE WIDTH THAT WAS DRAGGED. That agreement is the whole
	// reason facts are emitted from a const per-frame call beside the preview rather than
	// held as state the bar polls.
	TestEqual(TEXT("three bays, the width that was dragged"),
		Bays->Value, FString(TEXT("3")));

	// ONE ROW DEEP WARNS, and does not refuse - a one-row depot works perfectly well and
	// may be exactly what the player wants. The analogue of Manor Lords' "Plots without
	// Extension Space".
	TestEqual(TEXT("and it warns there is no room to grow"),
		Collector.Readout.Warnings.Num(), 1);

	return true;
}

#endif
