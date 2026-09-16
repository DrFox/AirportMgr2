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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FShiftDeflectionRoundTripsToItsRadiusTest,
    "Airside.Solve.ShiftDeflectionRoundTripsToItsRadius",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FShiftDeflectionRoundTripsToItsRadiusTest::RunTest(const FString& Parameters)
{
    // THE SAME ARGUMENT AS THE TEST ABOVE, one construction over. ShiftDeflectionFor is
    // CornerRunFor solved for its SECOND argument, and it is what sizes the connector from a
    // service road onto a stand's lane - so a truck's ability to drive onto a stand at all rests
    // on this inverse being exact rather than nearly right. Reachable only through
    // Airside.Build.StandLinkClearsTheTruckLock until now, which is an integration fixture at
    // two discrete gaps: it would not have caught an inverse that was wrong in between.
    //
    // MEASURED ON THE GEOMETRY, never against a restatement of the formula. The S is built here
    // exactly as FAnchorLink::Join builds it - the first curve from the line it leaves, the
    // second onto the line it joins - and both are handed to TightestRadius. A helper shared
    // with production could be wrong in one place and agree with itself, which is the reason
    // FAirframe::TightestFollowableRadius gives at its own copy.
    for (const double Radius : { 250.0, 699.4, 2500.0 })
    {
        for (const double Shift : { 120.0, 400.0, 1000.0, 1978.0, 5400.0, 20000.0 })
        {
            double Run = 0.0;
            const double Deflect = GuidelineGeom::ShiftDeflectionFor(Radius, Shift, Run);
            const FString Where = FString::Printf(
                TEXT("R %.0f across %.0f: %.2f deg on a %.0f uu run"),
                Radius, Shift, FMath::RadiansToDegrees(Deflect), Run);

            // BOTH CURVES CARRY HALF THE SHIFT BETWEEN THEM, which is the relation the run
            // comes from: 2 s sin(b) = Shift. Restated rather than taken from the header.
            TestTrue(*FString::Printf(TEXT("the run is the shift over twice the sine - %s"), *Where),
                FMath::IsNearlyEqual(Run, Shift / (2.0 * FMath::Sin(Deflect)), 0.01));

            // The S as Join lays it: leave along +X, deflect by b, cross, and deflect back onto
            // the parallel line Shift away. Control points one run out on each side, exactly as
            // the builder places them.
            const FVector2D Leaves = FVector2D::ZeroVector;
            const FVector2D Control = Leaves + FVector2D(Run, 0.0);
            const FVector2D Turned(FMath::Cos(Deflect), FMath::Sin(Deflect));
            const FVector2D Meets = Control + Turned * (2.0 * Run);
            const FVector2D Middle = Control + Turned * Run;
            const FVector2D Joins = Meets + FVector2D(Run, 0.0);

            TestTrue(*FString::Printf(TEXT("and the two curves cross the whole shift - %s"), *Where),
                FMath::IsNearlyEqual(Meets.Y, Shift, 0.01));

            const double Off = GuidelineGeom::TightestRadius(Leaves, Control, Middle);
            const double On = GuidelineGeom::TightestRadius(Middle, Meets, Joins);

            // AT THE CAP THE ANSWER IS "MORE THAN ASKED", not "exactly asked": past a shift of
            // 2 sqrt(2) R a right angle already clears the radius, and deflecting further would
            // be leaving the line rather than shifting off it. Below the cap the inverse is
            // exact, and that is the half that has to be.
            const bool bCapped = Shift > Radius * 2.0 * UE_DOUBLE_SQRT_2;
            TestTrue(
                *FString::Printf(TEXT("the curve off the line delivers %.1f, wanted %.1f - %s"),
                    Off, Radius, *Where),
                bCapped ? Off >= Radius - 0.5 : FMath::IsNearlyEqual(Off, Radius, 0.5));
            TestTrue(
                *FString::Printf(TEXT("the curve onto it delivers %.1f, wanted %.1f - %s"),
                    On, Radius, *Where),
                bCapped ? On >= Radius - 0.5 : FMath::IsNearlyEqual(On, Radius, 0.5));

            // AND THE ROUND TRIP THROUGH ITS SIBLING, which is the identity the header claims
            // and the reason the two live in one file: R = CornerRunFor(Shift/4, b).
            if (!bCapped)
            {
                TestTrue(
                    *FString::Printf(TEXT("CornerRunFor(Shift/4, b) is the radius again - %s"), *Where),
                    FMath::IsNearlyEqual(
                        GuidelineGeom::CornerRunFor(Shift * 0.25, Deflect), Radius, 0.5));
            }
        }
    }

    // THE CAP BOUNDARY ITSELF, pinned by name because the accepted bound on a stand's turn-back
    // sweep rests on it: at a shift of 2 sqrt(2) R the deflection is EXACTLY a right angle, and
    // the run is exactly half the shift. Below it the deflection is shallower; above it the cap
    // holds and the run stays at half.
    {
        constexpr double Radius = 699.4;
        const double AtCap = Radius * 2.0 * UE_DOUBLE_SQRT_2;

        double Run = 0.0;
        TestTrue(TEXT("a shift of 2 sqrt(2) R deflects exactly a right angle"),
            FMath::IsNearlyEqual(
                GuidelineGeom::ShiftDeflectionFor(Radius, AtCap, Run), UE_DOUBLE_HALF_PI, 1.0e-6));
        TestTrue(TEXT("and takes half the shift as its run"),
            FMath::IsNearlyEqual(Run, AtCap * 0.5, 0.01));

        TestTrue(TEXT("a shift just short of it deflects less than a right angle"),
            GuidelineGeom::ShiftDeflectionFor(Radius, AtCap * 0.9, Run) < UE_DOUBLE_HALF_PI);
        TestTrue(TEXT("and one well past it is held at the right angle"),
            FMath::IsNearlyEqual(
                GuidelineGeom::ShiftDeflectionFor(Radius, AtCap * 4.0, Run), UE_DOUBLE_HALF_PI,
                1.0e-6));
    }

    // A SHIFT OF NOTHING IS NOT A TRANSITION. Zero run with it, because a caller that laid a
    // curve on a zero run would lay a degenerate one - see the header.
    {
        double Run = -1.0;
        TestEqual(TEXT("two lines already on top of each other need no transition"),
            GuidelineGeom::ShiftDeflectionFor(699.4, 0.0, Run), 0.0);
        TestEqual(TEXT("and no run to make one on"), Run, 0.0);
    }

    // AND NO RADIUS TO CLEAR IS NO CONSTRAINT, which is what an airframe with no measured axles
    // reports - zero means "nothing to clear", not "clears nothing".
    {
        double Run = 0.0;
        TestTrue(TEXT("an airframe with no lock is not constrained to a slant"),
            FMath::IsNearlyEqual(
                GuidelineGeom::ShiftDeflectionFor(0.0, 4000.0, Run), UE_DOUBLE_HALF_PI, 1.0e-6));
        TestTrue(TEXT("and crosses on half the shift"), FMath::IsNearlyEqual(Run, 2000.0, 0.01));
    }

    return true;
}


#endif
