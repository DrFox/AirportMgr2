#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Tool/PlotDrawTool.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** Free-snap, 150uu radius - see TestTool::ContextAt, as ApronDrawToolTest uses. */
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

	/** A service road along y = Y, so a plot's south edge has a frontage to find. */
	void LayServiceRoad(URoadNetwork& Net, double Y)
	{
		const FGuidelineNodeId West = Net.AddGuidelineNode(FVector2D(-10000.0, Y));
		const FGuidelineNodeId East = Net.AddGuidelineNode(FVector2D(10000.0, Y));

		FGuidelineEdge Edge;
		Edge.A = West;
		Edge.B = East;
		Edge.Control = FVector2D(0.0, Y);
		Edge.AllowedTraffic = FTrafficMask::Only(ETraversalClass::GroundVehicle);
		Edge.AllowedTraffic.Add(ETraversalClass::Emergency);
		Edge.Direction = EGuidelineDir::Bidirectional;
		Edge.Width = 600.0;
		Edge.bDerived = true;
		Net.AddGuidelineEdge(MoveTemp(Edge));
	}

	/** Draw a closed 12 m x 8 m plot with its south edge on y = 0. */
	void DrawThreeBayPlot(FPlotDrawTool& Tool, ARoadNetworkActor* Actor)
	{
		Tool.OnClick(PlotAt(Actor, FVector2D(0.0, 0.0)));
		Tool.OnClick(PlotAt(Actor, FVector2D(1200.0, 0.0)));
		Tool.OnClick(PlotAt(Actor, FVector2D(1200.0, 800.0)));
		Tool.OnClick(PlotAt(Actor, FVector2D(0.0, 800.0)));

		// Near the first corner, not on it - the cursor never lands on a stored coordinate.
		Tool.OnClick(PlotAt(Actor, FVector2D(60.0, 40.0)));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotDrawToolPlacesADepotTest,
	"Airside.Tool.PlotDrawToolPlacesADepot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotDrawToolPlacesADepotTest::RunTest(const FString& Parameters)
{
	// A REAL WORLD, for the reason ApronDrawToolTest records at its own top: this drives an
	// actor whose components must be registered, and a bare NewObject is a half-built actor
	// masquerading as a working one.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	Actor->ClearNetwork();
	LayServiceRoad(*Actor->Network, -400.0);

	FPlotDrawTool Tool(EPlaceableEntity::FuelDepot);
	Tool.SetModules({ EDepotModule::Shed, EDepotModule::Shed, EDepotModule::Tank });

	TestTrue(TEXT("a fresh tool has nothing part-drawn"), Tool.IsIdle());

	Tool.OnClick(PlotAt(Actor, FVector2D(0.0, 0.0)));
	TestFalse(TEXT("the first click starts an outline"), Tool.IsIdle());
	Tool.OnClick(PlotAt(Actor, FVector2D(1200.0, 0.0)));
	Tool.OnClick(PlotAt(Actor, FVector2D(1200.0, 800.0)));
	Tool.OnClick(PlotAt(Actor, FVector2D(0.0, 800.0)));
	TestEqual(TEXT("four corners placed"), Tool.GetCorners().Num(), 4);
	TestEqual(TEXT("and nothing committed yet"), LiveEntities(Actor), 0);

	Tool.OnClick(PlotAt(Actor, FVector2D(60.0, 40.0)));
	TestTrue(TEXT("closing on the first corner ends the outline"), Tool.IsIdle());
	TestEqual(TEXT("and commits exactly one depot"), LiveEntities(Actor), 1);

	const TArray<FEntityInstance>& Entities = Actor->Network->GetEntities();
	if (!TestTrue(TEXT("an entity to read"), Entities.Num() > 0)) { return false; }
	const FEntityInstance& Depot = Entities[0];

	TestEqual(TEXT("the four corners drawn, not the closing click"), Depot.Outline.Num(), 4);
	TestEqual(TEXT("two sheds is two trucks, through the whole gesture"), Depot.Trucks, 2);
	TestEqual(TEXT("and the mix survives the commit"), Depot.Modules.Num(), 3);

	// THE GATE IS ON THE FRONTAGE. Its pose must sit on the road-facing edge, not at the
	// plot's centre - a truck dispatched from the middle of the yard would have no lead-in.
	TestTrue(TEXT("the pose sits on the frontage edge, not in the middle of the plot"),
		FMath::IsNearlyEqual(Depot.Position.Y, 0.0, 1.0));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotDrawToolRefusesAPlotWithNoRoadTest,
	"Airside.Tool.PlotDrawToolRefusesAPlotWithNoRoad",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotDrawToolRefusesAPlotWithNoRoadTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	// NO ROAD AT ALL. The same gesture, on a field.
	Actor->ClearNetwork();

	FPlotDrawTool Tool(EPlaceableEntity::FuelDepot);
	AddExpectedError(TEXT("no edge of the plot is within"), EAutomationExpectedErrorFlags::Contains, 0);

	DrawThreeBayPlot(Tool, Actor);

	TestEqual(TEXT("nothing is placed off a road"), LiveEntities(Actor), 0);

	// AND THE CORNERS SURVIVE. A refusal that binned four corners would punish the player
	// for the tool's own rule - they should be able to cancel back or close somewhere else
	// without redrawing from scratch.
	TestFalse(TEXT("the outline is still in progress, not thrown away"), Tool.IsIdle());
	TestEqual(TEXT("with every corner still placed"), Tool.GetCorners().Num(), 4);

	return true;
}

#endif
