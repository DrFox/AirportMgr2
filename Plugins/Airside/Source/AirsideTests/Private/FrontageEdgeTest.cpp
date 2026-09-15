#include "CoreMinimal.h"
#include "Build/AnchorLink.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** A straight guideline from West to East at Y, admitting Class. */
	void LayRoad(URoadNetwork& Net, double Y, ETraversalClass Class)
	{
		const FGuidelineNodeId West = Net.AddGuidelineNode(FVector2D(-5000.0, Y));
		const FGuidelineNodeId East = Net.AddGuidelineNode(FVector2D(5000.0, Y));

		FGuidelineEdge Edge;
		Edge.A = West;
		Edge.B = East;
		Edge.Control = FVector2D(0.0, Y);
		Edge.AllowedTraffic = FTrafficMask::Only(Class);
		Edge.AllowedTraffic.Add(ETraversalClass::Emergency);
		Edge.Direction = EGuidelineDir::Bidirectional;
		Edge.Width = 600.0;
		Edge.bDerived = true;
		Net.AddGuidelineEdge(MoveTemp(Edge));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFrontageEdgeFacesTheRoadTest,
	"Airside.Build.FrontageEdgeFacesTheRoad",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFrontageEdgeFacesTheRoadTest::RunTest(const FString& Parameters)
{
	// The plot is drawn with its NORTH edge first, so an implementation that returned
	// Outline[0]..Outline[1] would pass a "some edge was returned" assertion by luck. The
	// south edge is the one nearest the road and the one that must win.
	const TArray<FVector2D> Outline = {
		FVector2D(1200.0, 800.0), FVector2D(0.0, 800.0),
		FVector2D(0.0, 0.0),      FVector2D(1200.0, 0.0) };

	{
		URoadNetwork* Net = NewObject<URoadNetwork>();
		LayRoad(*Net, -400.0, ETraversalClass::GroundVehicle);

		FVector2D A = FVector2D::ZeroVector;
		FVector2D B = FVector2D::ZeroVector;
		const bool bFound = FAnchorLink::FindFrontageEdge(*Net, Outline,
			FAnchorLink::DefaultServiceLinkRadius, A, B);

		if (!TestTrue(TEXT("a frontage edge is found"), bFound)) { return false; }
		TestEqual(TEXT("the SOUTH edge wins, being nearest the road"), A.Y, 0.0);
		TestEqual(TEXT("both its ends, so it is an edge and not a stray point"), B.Y, 0.0);

		// IN WINDING ORDER, which PlotFit depends on to decide which side the interior is
		// on. Reversed, every bay would be laid across the road instead of into the plot.
		TestEqual(TEXT("returned in the outline's own winding order"), A.X, 0.0);
		TestEqual(TEXT("A then B, not B then A"), B.X, 1200.0);
	}

	// NO ROAD AT ALL is a refusal, not edge zero. A plot drawn in a field must say so -
	// falling back to the first edge would aim the depot at whatever the player happened
	// to click first, which is the kind of wrong that looks deliberate.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>();
		FVector2D A = FVector2D::ZeroVector;
		FVector2D B = FVector2D::ZeroVector;
		TestFalse(TEXT("no road means no frontage, not a lucky first edge"),
			FAnchorLink::FindFrontageEdge(*Net, Outline,
				FAnchorLink::DefaultServiceLinkRadius, A, B));
	}

	// A TAXIWAY IS NOT A SERVICE ROAD. A depot's trucks are ground vehicles, so a plot
	// whose only neighbour is an aircraft guideline has no frontage - otherwise the player
	// would be allowed to build a depot that dispatches onto a taxiway.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>();
		LayRoad(*Net, -400.0, ETraversalClass::Aircraft);

		FVector2D A = FVector2D::ZeroVector;
		FVector2D B = FVector2D::ZeroVector;
		TestFalse(TEXT("a taxiway is not frontage for a depot"),
			FAnchorLink::FindFrontageEdge(*Net, Outline,
				FAnchorLink::DefaultServiceLinkRadius, A, B));
	}

	// OUT OF REACH. The road is there and is the right class, but further than a service
	// link may stretch, so the plot has no usable frontage. Accepting it here and failing
	// in FAnchorLink later is the worst outcome: the depot would place and then silently
	// never join, which is the failure mode the shared radius exists to prevent.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>();
		LayRoad(*Net, -(FAnchorLink::DefaultServiceLinkRadius + 2000.0),
			ETraversalClass::GroundVehicle);

		FVector2D A = FVector2D::ZeroVector;
		FVector2D B = FVector2D::ZeroVector;
		TestFalse(TEXT("a road beyond the service link radius is not frontage"),
			FAnchorLink::FindFrontageEdge(*Net, Outline,
				FAnchorLink::DefaultServiceLinkRadius, A, B));
	}

	return true;
}

#endif
