#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "UI/UiSparkline.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE SPARKLINE'S GEOMETRY, measured without painting: y = 0 is the TOP so value 1 draws highest,
 * x spreads evenly, out-of-range values clamp (a satisfaction that overshoots must not leave the
 * widget), a lone value sits mid-width and an empty series draws nothing.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiSparklineLayOutTest, "AirportMgr.UI.Sparkline.LayOut",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUiSparklineLayOutTest::RunTest(const FString& Parameters)
{
	TArray<FVector2D> P;
	const FVector2D Size(100.0, 20.0);

	const double Three[] = { 0.0, 0.5, 1.0 };
	UUiSparkline::LayOut(Three, Size, P);
	if (!TestEqual(TEXT("three values, three points"), P.Num(), 3)) { return false; }
	TestEqual(TEXT("0 is the bottom, left"), P[0], FVector2D(0.0, 20.0));
	TestEqual(TEXT("0.5 is the middle"), P[1], FVector2D(50.0, 10.0));
	TestEqual(TEXT("1 is the top, right"), P[2], FVector2D(100.0, 0.0));

	const double One[] = { 0.25 };
	UUiSparkline::LayOut(One, Size, P);
	if (!TestEqual(TEXT("one value, one point"), P.Num(), 1)) { return false; }
	TestEqual(TEXT("a lone value sits at mid x"), P[0], FVector2D(50.0, 15.0));

	const double Wild[] = { -3.0, 7.0 };
	UUiSparkline::LayOut(Wild, Size, P);
	if (!TestEqual(TEXT("two points"), P.Num(), 2)) { return false; }
	TestEqual(TEXT("below 0 clamps to the bottom"), P[0].Y, 20.0);
	TestEqual(TEXT("above 1 clamps to the top"), P[1].Y, 0.0);

	P.Add(FVector2D(1.0, 1.0));
	UUiSparkline::LayOut(TArrayView<const double>(), Size, P);
	TestEqual(TEXT("no values, no points (and stale points are cleared)"), P.Num(), 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
