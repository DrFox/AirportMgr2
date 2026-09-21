#include "CoreMinimal.h"
#include "Algo/Reverse.h"
#include "Misc/AutomationTest.h"
#include "Solve/PlotFit.h"
#include "Solve/RoadGeom.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadGeomTest,
	"Airside.Solve.Geom",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadGeomTest::RunTest(const FString& Parameters)
{
	// PerpCCW
	TestTrue(TEXT("perp of +X is +Y"),
		RoadGeom::PerpCCW(FVector2D(1.0, 0.0)).Equals(FVector2D(0.0, 1.0), 1e-9));

	// Rotate
	TestTrue(TEXT("rotate +X by 90deg"),
		RoadGeom::Rotate(FVector2D(1.0, 0.0), UE_DOUBLE_PI * 0.5).Equals(FVector2D(0.0, 1.0), 1e-9));

	// CcwAngleBetween is always in [0, 2PI)
	TestTrue(TEXT("east to north is 90deg"),
		FMath::IsNearlyEqual(RoadGeom::CcwAngleBetween(FVector2D(1.0, 0.0), FVector2D(0.0, 1.0)), UE_DOUBLE_PI * 0.5, 1e-9));
	TestTrue(TEXT("north to east is 270deg"),
		FMath::IsNearlyEqual(RoadGeom::CcwAngleBetween(FVector2D(0.0, 1.0), FVector2D(1.0, 0.0)), UE_DOUBLE_PI * 1.5, 1e-9));

	// LineIntersect
	FRay2D Horizontal; Horizontal.Origin = FVector2D(0.0, 5.0); Horizontal.Dir = FVector2D(1.0, 0.0);
	FRay2D Vertical;   Vertical.Origin   = FVector2D(7.0, 0.0); Vertical.Dir   = FVector2D(0.0, 1.0);
	FVector2D Hit;
	TestTrue(TEXT("lines cross"), RoadGeom::LineIntersect(Horizontal, Vertical, Hit));
	TestTrue(TEXT("crossing point"), Hit.Equals(FVector2D(7.0, 5.0), 1e-9));

	FRay2D Parallel; Parallel.Origin = FVector2D(0.0, 9.0); Parallel.Dir = FVector2D(1.0, 0.0);
	TestFalse(TEXT("parallel lines do not cross"), RoadGeom::LineIntersect(Horizontal, Parallel, Hit));

	// PolygonArea: CCW unit square has area +1
	const TArray<FVector2D> Square = {
		FVector2D(0.0, 0.0), FVector2D(1.0, 0.0), FVector2D(1.0, 1.0), FVector2D(0.0, 1.0)
	};
	TestTrue(TEXT("ccw square area"), FMath::IsNearlyEqual(RoadGeom::PolygonArea(Square), 1.0, 1e-9));

	TArray<FVector2D> Reversed = Square;
	Algo::Reverse(Reversed);
	TestTrue(TEXT("cw square area negative"), RoadGeom::PolygonArea(Reversed) < 0.0);

	// IsSimplePolygon
	TestTrue(TEXT("square is simple"), RoadGeom::IsSimplePolygon(Square));

	const TArray<FVector2D> Bowtie = {
		FVector2D(0.0, 0.0), FVector2D(1.0, 1.0), FVector2D(1.0, 0.0), FVector2D(0.0, 1.0)
	};
	TestFalse(TEXT("bowtie is not simple"), RoadGeom::IsSimplePolygon(Bowtie));

	// ClosestPointOnSegment returns a PARAMETER, clamped to the segment. Names are
	// block-local rather than reusing the shorter ones above: V7 makes shadowing an error.
	const FVector2D ChordStart(0.0, 0.0);
	const FVector2D ChordEnd(10.0, 0.0);

	TestTrue(TEXT("perpendicular foot at the midpoint"), FMath::IsNearlyEqual(
		RoadGeom::ClosestPointOnSegment(ChordStart, ChordEnd, FVector2D(5.0, 3.0)), 0.5, 1e-9));

	// A quarter along, and from the far side of the line. This is the case that separates
	// a parameter from a distance: anything returning a length gets 2.5 here, not 0.25.
	TestTrue(TEXT("a quarter along, from below"), FMath::IsNearlyEqual(
		RoadGeom::ClosestPointOnSegment(ChordStart, ChordEnd, FVector2D(2.5, -7.0)), 0.25, 1e-9));

	// Exactly 0 and exactly 1 are the clamp signalling "the closest point is an endpoint",
	// which the segment snap rule reads to stand down. Compared exactly, because a rule
	// testing `== 0.0` is only safe if the clamp really does produce that value.
	TestTrue(TEXT("past the A end clamps to exactly 0"),
		RoadGeom::ClosestPointOnSegment(ChordStart, ChordEnd, FVector2D(-40.0, 1.0)) == 0.0);
	TestTrue(TEXT("past the B end clamps to exactly 1"),
		RoadGeom::ClosestPointOnSegment(ChordStart, ChordEnd, FVector2D(90.0, 1.0)) == 1.0);
	TestTrue(TEXT("a zero-length segment returns 0 rather than dividing by it"),
		RoadGeom::ClosestPointOnSegment(ChordStart, ChordStart, FVector2D(3.0, 4.0)) == 0.0);

	return true;
}

/**
 * PointInPolygon BY WINDING NUMBER - issue #182, moved from the tests PlotFit::Contains had
 * of its own (Airside.Solve.PlotFitBays, Airside.Solve.PlotFitFacesAwayFromRoad) before that
 * file-static function was deleted in favour of this one. The cases that mattered there -
 * exactly-on-the-boundary corners, a notch, either winding - are point-in-polygon questions
 * regardless of which caller asks them, so they belong on the one function that answers them
 * now rather than on the plot solver that used to carry a private copy.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadGeomPointInPolygonTest,
	"Airside.Solve.GeomPointInPolygon",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadGeomPointInPolygonTest::RunTest(const FString& Parameters)
{
	// A NOTCH CUT OUT OF THE MIDDLE. The centre of the outer box falls in the notch, which a
	// bounding-box or centre-only test would miss - PlotFit::FitBays's own reason for testing
	// every corner rather than a bay's centre, and a fact about the outline, not about bays.
	const TArray<FVector2D> Notched = {
		FVector2D(0.0, 0.0),   FVector2D(1200.0, 0.0),
		FVector2D(1200.0, 1200.0),
		FVector2D(800.0, 1200.0), FVector2D(800.0, 300.0),  // the notch, biting down
		FVector2D(400.0, 300.0), FVector2D(400.0, 1200.0),
		FVector2D(0.0, 1200.0) };

	TestTrue(TEXT("left of the notch is inside"),
		RoadGeom::PointInPolygon(Notched, FVector2D(200.0, 600.0)));
	TestTrue(TEXT("right of the notch is inside"),
		RoadGeom::PointInPolygon(Notched, FVector2D(1000.0, 600.0)));
	TestFalse(TEXT("inside the notch itself is outside the plot"),
		RoadGeom::PointInPolygon(Notched, FVector2D(600.0, 600.0)));

	// EITHER WINDING ANSWERS THE SAME - the property the winding-number swap had to keep,
	// since callers store outlines both ways round (RoadEditFacadeSurfaces corrects to CCW
	// for the mesh builder, but PlotYard and this function are asked to work before that
	// correction happens too).
	TArray<FVector2D> Reversed = Notched;
	Algo::Reverse(Reversed);
	TestTrue(TEXT("reversed winding: left of the notch is still inside"),
		RoadGeom::PointInPolygon(Reversed, FVector2D(200.0, 600.0)));
	TestFalse(TEXT("reversed winding: the notch is still outside"),
		RoadGeom::PointInPolygon(Reversed, FVector2D(600.0, 600.0)));

	// A CORNER EXACTLY ON THE BOUNDARY is the hardest case for any point-in-polygon test, not
	// the easiest - PlotFit::FitBays's own comment named this as the reason a corner is probed
	// PlotFit::CornerInsetUu in from where it actually stands rather than tested as drawn.
	// Probed just inside and just outside a plain square, both answers must be unsurprising.
	const TArray<FVector2D> Square = {
		FVector2D(0.0, 0.0), FVector2D(1000.0, 0.0),
		FVector2D(1000.0, 1000.0), FVector2D(0.0, 1000.0) };
	TestTrue(TEXT("just inside the boundary is inside"),
		RoadGeom::PointInPolygon(Square, FVector2D(1000.0 - PlotFit::CornerInsetUu, 500.0)));
	TestFalse(TEXT("just outside the boundary is outside"),
		RoadGeom::PointInPolygon(Square, FVector2D(1000.0 + PlotFit::CornerInsetUu, 500.0)));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
