#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Solve/RoadGeom.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrailPointTest,
	"Airside.Solve.TrailPoint",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrailPointTest::RunTest(const FString& Parameters)
{
	// Facing +X: a point 100 uu FORWARD along the body is +100 in X, and nothing in Y.
	const FVector2D Ahead = RoadGeom::TrailPoint(FVector2D(1000.0, 2000.0), 0.0, 100.0);
	TestEqual(TEXT("forward along +X moves X"), Ahead.X, 1100.0, 0.01);
	TestEqual(TEXT("and leaves Y alone"), Ahead.Y, 2000.0, 0.01);

	// Facing +Y (90 degrees): the same forward offset moves Y. This is the one that catches
	// a sin/cos swap, which looks correct at every multiple of 45 degrees.
	const FVector2D Turned = RoadGeom::TrailPoint(
		FVector2D(0.0, 0.0), FMath::DegreesToRadians(90.0), 100.0);
	TestEqual(TEXT("facing +Y, forward moves Y"), Turned.Y, 100.0, 0.01);
	TestEqual(TEXT("and not X"), Turned.X, 0.0, 0.01);

	// A NEGATIVE offset trails BEHIND, which is the direction every caller actually uses:
	// the origin is aft of the steered axle on a conforming airframe.
	const FVector2D Behind = RoadGeom::TrailPoint(
		FVector2D(0.0, 0.0), FMath::DegreesToRadians(90.0), -454.3);
	TestEqual(TEXT("a negative offset trails behind"), Behind.Y, -454.3, 0.01);

	// Zero offset is the identity - the conforming case, and it must be EXACT rather than
	// nearly so: every unmeasured airframe in the game goes through this path and must land
	// where it does today, bit for bit.
	const FVector2D Same = RoadGeom::TrailPoint(FVector2D(123.0, -456.0), 1.234, 0.0);
	TestTrue(TEXT("zero offset is exactly the identity"),
		Same.X == 123.0 && Same.Y == -456.0);

	return true;
}

#endif
