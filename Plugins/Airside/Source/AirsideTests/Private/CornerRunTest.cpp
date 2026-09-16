#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Solve/GuidelineGeom.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCornerRunRoundTripsToItsRadiusTest,
    "Airside.Solve.CornerRunRoundTripsToItsRadius",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FCornerRunRoundTripsToItsRadiusTest::RunTest(const FString& Parameters)
{
    // THE WHOLE POINT OF PUTTING IT HERE. CornerRunFor is the inverse of TightestRadius, and
    // nothing said so while they lived in different files - which is how the stand spec came
    // to cost a quadratic corner as if it were a circular fillet and lost 40% of the run it
    // needed at a right angle. The two are now measured against each other.
    const double Radius = 699.4;

    for (const double InteriorDegrees : { 30.0, 60.0, 90.0, 120.0, 137.25, 160.0 })
    {
        const double Interior = FMath::DegreesToRadians(InteriorDegrees);
        const double Run = GuidelineGeom::CornerRunFor(Radius, Interior);

        // The corner as the builder lays it: control ON the corner, ends Run back along each
        // leg. Put the corner at the origin with its legs symmetric about +X.
        const double Half = Interior * 0.5;
        const FVector2D Corner = FVector2D::ZeroVector;
        const FVector2D Back(FMath::Cos(Half), FMath::Sin(Half));
        const FVector2D Onward(FMath::Cos(Half), -FMath::Sin(Half));

        const double Delivered = GuidelineGeom::TightestRadius(
            Corner + Back * Run, Corner, Corner + Onward * Run);

        TestTrue(
            *FString::Printf(
                TEXT("a %.2f deg corner cut back %.1f uu delivers %.1f, wanted %.1f"),
                InteriorDegrees, Run, Delivered, Radius),
            FMath::IsNearlyEqual(Delivered, Radius, 0.5));
    }

    // AND THE FIGURE THE SPEC TURNS ON, pinned by name so a change to it is deliberate. A
    // right-angle corner costs 1.414 R, not R - the circular fillet's tangent length, which
    // is what the first draft used.
    TestTrue(
        TEXT("a right-angle corner costs sqrt(2) times its radius, not one times"),
        FMath::IsNearlyEqual(
            GuidelineGeom::CornerRunFor(Radius, UE_DOUBLE_HALF_PI), Radius * UE_DOUBLE_SQRT_2,
            0.5));

    // A hairpin has no cut that gives it any radius. The caller's clamp wants a number it can
    // scale, not an infinity that would scale both corners of a leg to nothing.
    TestTrue(
        TEXT("a hairpin reports the maximum rather than an infinity"),
        GuidelineGeom::CornerRunFor(Radius, 0.0) >= TNumericLimits<double>::Max() * 0.5);

    return true;
}

#endif
