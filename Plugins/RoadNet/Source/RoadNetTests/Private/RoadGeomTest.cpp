#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Solve/RoadGeom.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadGeomTest,
	"RoadNet.Solve.Geom",
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

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
