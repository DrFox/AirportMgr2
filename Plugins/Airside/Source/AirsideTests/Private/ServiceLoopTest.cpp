#include "CoreMinimal.h"
#include "Entities/AircraftType.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadEntity.h"
#include "Solve/RoadGeom.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/**
	 * Nearest point on the implicitly-closed loop to Query - the spur a service anchor gets.
	 *
	 * The same rule FServiceLoopBuild follows in the graph, restated here rather than called,
	 * because this file has to be able to judge a HAND-BUILT definition that the graph builder
	 * would never be handed - see the far-side fixture below.
	 */
	FVector2D NearestOnLoop(const TArray<FVector2D>& Loop, const FVector2D& Query)
	{
		FVector2D Best = FVector2D::ZeroVector;
		double BestDistance = TNumericLimits<double>::Max();
		for (int32 At = 0; At < Loop.Num(); ++At)
		{
			const FVector2D& A = Loop[At];
			const FVector2D& B = Loop[(At + 1) % Loop.Num()];
			const double T = RoadGeom::ClosestPointOnSegment(A, B, Query);
			const FVector2D Point = FMath::Lerp(A, B, T);
			const double Distance = FVector2D::Distance(Point, Query);
			if (Distance < BestDistance)
			{
				BestDistance = Distance;
				Best = Point;
			}
		}
		return Best;
	}

	/**
	 * How many loop segments and anchor spurs cross the fuselage LENGTHWISE - the design's
	 * section 5 invariant, MEASURED.
	 *
	 * The centreline as a finite segment from TailX to NoseX, never the footprint outline:
	 * forbidding the footprint would forbid passing UNDER A WING, which is normal and which
	 * the hydrant requires - HydrantPit is under the starboard wing root because that is
	 * where a hydrant pit is.
	 *
	 * A COUNT rather than a bool, so the test can say a passing case is not a vacuous one.
	 */
	int32 CountCentrelineCrossings(const UEntityDefinition& Definition)
	{
		if (Definition.DesignAircraft == nullptr || Definition.ServiceLoop.Num() < 3)
		{
			return 0;
		}

		const FEntityFootprint& Footprint = Definition.DesignAircraft->Footprint;
		const FVector2D Tail(Footprint.TailX, 0.0);
		const FVector2D Nose(Footprint.NoseX, 0.0);

		int32 Crossings = 0;
		for (int32 At = 0; At < Definition.ServiceLoop.Num(); ++At)
		{
			const FVector2D& A = Definition.ServiceLoop[At];
			const FVector2D& B = Definition.ServiceLoop[(At + 1) % Definition.ServiceLoop.Num()];
			Crossings += RoadGeom::SegmentsCross(A, B, Tail, Nose) ? 1 : 0;
		}
		for (const FEntityAnchor& Anchor : Definition.Anchors)
		{
			if (TraversalForRole(Anchor.Role) == ETraversalClass::Aircraft)
			{
				continue;
			}
			const FVector2D Spur = NearestOnLoop(Definition.ServiceLoop, Anchor.LocalPosition);
			Crossings += RoadGeom::SegmentsCross(Anchor.LocalPosition, Spur, Tail, Nose) ? 1 : 0;
		}
		return Crossings;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FServiceLoopEnclosesTheStandTest,
	"Airside.Entities.ServiceLoopEnclosesTheStand",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FServiceLoopEnclosesTheStandTest::RunTest(const FString& Parameters)
{
	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
	if (!TestNotNull(TEXT("a stand definition"), Stand)) { return false; }
	if (!TestNotNull(TEXT("with a design aircraft"), Stand->DesignAircraft.Get())) { return false; }

	// FOUR POINTS, NOT FIVE. The loop is closed implicitly and the array does not repeat its
	// first point - storing the repeat would be a value that must agree with another value
	// in the same array.
	if (!TestEqual(TEXT("the loop is a four-sided box"), Stand->ServiceLoop.Num(), 4))
	{
		return false;
	}

	const FEntityFootprint& Footprint = Stand->DesignAircraft->Footprint;
	const double HalfSpan = Footprint.Wingspan * 0.5;

	// EVERY WINGTIP CORNER. PointInPolygon rather than a bounds comparison, because what the
	// graph will drive round is the POLYGON, and a bounds test would still pass if the
	// builder emitted the corners in an order that folds the box through itself.
	const FVector2D Corners[4] = {
		FVector2D(Footprint.NoseX,  HalfSpan), FVector2D(Footprint.NoseX, -HalfSpan),
		FVector2D(Footprint.TailX,  HalfSpan), FVector2D(Footprint.TailX, -HalfSpan) };
	for (const FVector2D& Corner : Corners)
	{
		TestTrue(TEXT("the aircraft's footprint is inside the loop"),
			RoadGeom::PointInPolygon(Stand->ServiceLoop, Corner));
	}

	// EVERY ANCHOR, which is why the box is a UNION and not the footprint alone: TugStand
	// waits at +1400, nine metres ahead of a nose that stops at +507.
	for (const FEntityAnchor& Anchor : Stand->Anchors)
	{
		TestTrue(TEXT("every anchor is inside the loop"),
			RoadGeom::PointInPolygon(Stand->ServiceLoop, Anchor.LocalPosition));
	}

	// THE CLEARANCE ITSELF, so a loop that merely touched the wingtips would fail here. The
	// figures are arithmetic from the A320's footprint and the anchors, not dialled in.
	double MinX = TNumericLimits<double>::Max(), MaxX = -TNumericLimits<double>::Max();
	double MinY = TNumericLimits<double>::Max(), MaxY = -TNumericLimits<double>::Max();
	for (const FVector2D& Point : Stand->ServiceLoop)
	{
		MinX = FMath::Min(MinX, Point.X); MaxX = FMath::Max(MaxX, Point.X);
		MinY = FMath::Min(MinY, Point.Y); MaxY = FMath::Max(MaxY, Point.Y);
	}
	TestEqual(TEXT("3 m behind the tail"), MinX, -3550.0);
	TestEqual(TEXT("3 m ahead of the tug's box"), MaxX, 1700.0);
	TestEqual(TEXT("3 m outboard of the port wingtip"), MinY, -2090.0);
	TestEqual(TEXT("3 m outboard of the starboard wingtip"), MaxY, 2090.0);

	// A DEPOT HAS NONE, and empty is a supported state rather than an unfinished one.
	UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
	if (TestNotNull(TEXT("a depot definition"), Depot))
	{
		TestEqual(TEXT("a depot has no service loop - one pose, no anchors"),
			Depot->ServiceLoop.Num(), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FServiceLoopClearsTheAircraftTest,
	"Airside.Entities.ServiceLoopClearsTheAircraft",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FServiceLoopClearsTheAircraftTest::RunTest(const FString& Parameters)
{
	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
	if (!TestNotNull(TEXT("a stand definition"), Stand)) { return false; }

	TestEqual(TEXT("no loop segment and no spur crosses the fuselage lengthwise"),
		CountCentrelineCrossings(*Stand), 0);

	// THE VACUITY CHECK. A counter that could never report anything would pass the assertion
	// above on a stand whose lane ran straight through the aeroplane, so the same counter is
	// pointed at a layout that genuinely does cross.
	//
	// A ONE-SIDED lane, hand-built: the anchors are the real ones, but the lane runs only up
	// the starboard side, so the port-side anchors have nowhere to spur but across the
	// fuselage. This is the arrangement the design rejected - joining anchors to whatever is
	// nearest without a lane round the aeroplane.
	{
		UEntityDefinition* Bad = NewObject<UEntityDefinition>(GetTransientPackage());
		UEntityDefinition::BuildCodeCStand(Bad, Stand->DesignAircraft);
		Bad->ServiceLoop = {
			FVector2D(-3550.0,  500.0), FVector2D( 1700.0,  500.0),
			FVector2D( 1700.0, 2090.0), FVector2D(-3550.0, 2090.0) };

		TestTrue(TEXT("a lane only up the starboard side makes the port anchors cross"),
			CountCentrelineCrossings(*Bad) > 0);
	}
	return true;
}

#endif
