#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Solve/RoadGeom.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRayToPlaneZTest,
	"Airside.Solve.RayToPlaneZ",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRayToPlaneZTest::RunTest(const FString& Parameters)
{
	FVector2D Out;
	RoadGeom::ERayToPlaneRefusal Why = RoadGeom::ERayToPlaneRefusal::None;

	// A ray edge-on to the plane has no intersection to find.
	TestFalse(TEXT("a ray parallel to the plane refuses"),
		RoadGeom::RayToPlaneZ(FVector(0.0, 0.0, 100.0), FVector(1.0, 0.0, 0.0), 0.0, 1.0e6, Out, &Why));
	TestTrue(TEXT("...and names the reason Parallel"), Why == RoadGeom::ERayToPlaneRefusal::Parallel);

	// The plane sits behind the origin along Direction: refused rather than resolving to
	// the mirror image on the far side.
	TestFalse(TEXT("a plane behind the origin refuses"),
		RoadGeom::RayToPlaneZ(FVector(0.0, 0.0, 100.0), FVector(0.0, 0.0, 1.0), 0.0, 1.0e6, Out, &Why));
	TestTrue(TEXT("...and names the reason BehindOrigin"), Why == RoadGeom::ERayToPlaneRefusal::BehindOrigin);

	// Straight down, but past the caller's MaxDistance.
	TestFalse(TEXT("a hit beyond MaxDistance refuses"),
		RoadGeom::RayToPlaneZ(FVector(0.0, 0.0, 1000.0), FVector(0.0, 0.0, -1.0), 0.0, 500.0, Out, &Why));
	TestTrue(TEXT("...and names the reason BeyondMaxDistance"), Why == RoadGeom::ERayToPlaneRefusal::BeyondMaxDistance);

	// Straight down onto Z=0 from 100 units up lands exactly at the origin - integer
	// arithmetic throughout, so no tolerance is needed.
	TestTrue(TEXT("straight down within MaxDistance succeeds"),
		RoadGeom::RayToPlaneZ(FVector(0.0, 0.0, 100.0), FVector(0.0, 0.0, -1.0), 0.0, 1000.0, Out, &Why));
	TestEqual(TEXT("straight down lands exactly at the origin, no tolerance needed"), Out, FVector2D(0.0, 0.0));
	TestTrue(TEXT("...and a success names the reason None"), Why == RoadGeom::ERayToPlaneRefusal::None);

	// A 45-degree ray from (0,0,100) toward Z=0. Direction is left UNNORMALISED - (1,0,-1)
	// rather than its unit form - because RayToPlaneZ's maths (a ratio of Direction's
	// components) is scale-invariant, and skipping the sqrt keeps every intermediate value
	// an exact double: Distance = (0 - 100) / -1 = 100 exactly, so OutXY = (100, 0) exactly
	// and needs no tolerance either.
	TestTrue(TEXT("a 45-degree ray within MaxDistance succeeds"),
		RoadGeom::RayToPlaneZ(FVector(0.0, 0.0, 100.0), FVector(1.0, 0.0, -1.0), 0.0, 1000.0, Out));
	TestEqual(TEXT("the 45-degree ray lands exactly at (100, 0)"), Out, FVector2D(100.0, 0.0));

	return true;
}

#endif
