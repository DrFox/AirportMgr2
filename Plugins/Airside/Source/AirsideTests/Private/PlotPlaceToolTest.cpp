#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Present/PlotPresenter.h"
#include "Present/RoadNetworkActor.h"
#include "Profiles/RoadProfile.h"
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

		/**
		 * How deep the ghost is, measured FRONT EDGE TO BACK rather than from y = 0.
		 *
		 * An absolute Y would fold the road's half width into the answer, and it did: this
		 * asserted 800 until the frontage moved off the centreline, then failed at 1100 for
		 * a ghost that was perfectly correct. The claim is the plot's DEPTH.
		 */
		double Depth() const { return DeepestY - NearestY; }

		virtual void Marker(const FVector2D&, EPreviewStyle) override {}
		virtual void Line(const FVector2D& From, const FVector2D& To, EPreviewStyle) override
		{
			NearestY = FMath::Min3(NearestY, From.Y, To.Y);
			DeepestY = FMath::Max3(DeepestY, From.Y, To.Y);
		}
		virtual void CrossMark(const FVector2D&, const FVector2D&, EPreviewStyle) override {}
		virtual void Label(const FVector2D&, const FString&, EPreviewStyle) override {}
	};

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
		FVector2D(600.0, -1600.0));
	South.OnCommit(PlotAt(Actor, FVector2D(600.0, -1600.0)));

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
	Tool.OnCancel(PlotAt(Actor, FVector2D(600.0, 2400.0)));
	Tool.OnCancel(PlotAt(Actor, FVector2D(600.0, 2400.0)));
	Tool.OnCancel(PlotAt(Actor, FVector2D(600.0, 2400.0)));
	TestEqual(TEXT("backed all the way out"),
		static_cast<int32>(Tool.GetStage()), static_cast<int32>(EPlotStage::Idle));

	// The second gesture, anchored and dragging its WIDTH - the stage where depth has not
	// been chosen yet and the two descriptions came apart.
	Tool.OnClick(OnRoad(Actor, FVector2D(0.0, 200.0)));
	TestEqual(TEXT("anchored again"),
		static_cast<int32>(Tool.GetStage()), static_cast<int32>(EPlotStage::Width));

	const FToolContext Dragging = PlotAt(Actor, FVector2D(800.0, 200.0));

	FPlotGhostSink Sink;
	Tool.BuildPreview(Dragging, Sink);

	FToolReadoutCollector Collector;
	Tool.BuildReadout(Dragging, Collector);

	const TPair<FString, FString>* Rows = Collector.Readout.Facts.FindByPredicate(
		[](const TPair<FString, FString>& F) { return F.Key == TEXT("Rows"); });
	if (!TestNotNull(TEXT("a Rows fact"), Rows)) { return false; }
	TestEqual(TEXT("the bar says one row, depth not being chosen yet"),
		Rows->Value, FString(TEXT("1")));

	// THE GHOST IS MEASURED, not asked. Front edge to back edge is exactly
	// Rows * BayDepthUu - and a ghost still drawing the previous gesture's three rows spans
	// 2400 instead of 800, wherever the frontage happens to sit relative to the road.
	TestEqual(TEXT("and the ghost is drawn exactly that deep"),
		Sink.Depth(), PlotFit::BayDepthUu);

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

	// Three bays across and three rows deep - room to spare, so "Room for" is not trivially
	// zero and the agreement below is a real comparison rather than 0 == 0.
	FPlotPlaceTool Tool(EPlaceableEntity::FuelDepot);
	DrawPlot(Tool, Actor, FVector2D(0.0, 200.0), FVector2D(1200.0, 200.0),
		FVector2D(600.0, 2700.0));

	const FToolContext Confirming = PlotAt(Actor, FVector2D(600.0, 2700.0));
	FToolReadoutCollector Collector;
	Tool.BuildReadout(Confirming, Collector);

	// "EXPANSION SLOTS" WAS A CLAIM ABOUT BAYS, and modules no longer stand in bays. A fact
	// whose name survived its meaning is worse than one that was removed: the player reads a
	// number describing a structure the plot does not have.
	const TPair<FString, FString>* Slots = Collector.Readout.Facts.FindByPredicate(
		[](const TPair<FString, FString>& F) { return F.Key == TEXT("Expansion slots"); });
	TestNull(TEXT("the bay-slot fact is gone"), Slots);

	const TPair<FString, FString>* Room = Collector.Readout.Facts.FindByPredicate(
		[](const TPair<FString, FString>& F) { return F.Key == TEXT("Room for"); });
	if (!TestNotNull(TEXT("a Room for fact"), Room)) { return false; }

	const int32 Promised = FCString::Atoi(*Room->Value);
	TestTrue(TEXT("a plot this size reports room to grow"), Promised > 0);

	// THE AGREEMENT. Commit the very gesture that was read out, then ask the presenter what
	// the built depot actually has. A preview seeded differently from the placement would
	// pass every other assertion here and quietly promise a yard the player never gets.
	Tool.OnCommit(Confirming);
	Actor->RebuildMesh();

	TestEqual(TEXT("the room promised is the room the built depot has"),
		Actor->GetPlotPresenter()->GetRoomForMore(), Promised);

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
