#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Tool/ScreenPick.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FScreenPickTest,
	"Airside.Tool.ScreenPick",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FScreenPickTest::RunTest(const FString& Parameters)
{
	// The pixel rule the controller applies to projected agents. World-free so the one
	// judgement in aircraft picking is tested without a camera.
	const TArray<FVector2D> Points = { FVector2D(100.0, 100.0), FVector2D(110.0, 100.0), FVector2D(500.0, 500.0) };

	TestEqual(TEXT("nearest of two within radius wins"), ScreenPick::NearestWithin(Points, FVector2D(108.0, 100.0), 24.0), 1);
	TestEqual(TEXT("exactly on a point picks it"), ScreenPick::NearestWithin(Points, FVector2D(500.0, 500.0), 24.0), 2);
	TestEqual(TEXT("outside the radius of everything is none"), ScreenPick::NearestWithin(Points, FVector2D(300.0, 300.0), 24.0), INDEX_NONE);
	TestEqual(TEXT("on the radius counts as within"), ScreenPick::NearestWithin(Points, FVector2D(524.0, 500.0), 24.0), 2);
	TestEqual(TEXT("empty is none"), ScreenPick::NearestWithin(TConstArrayView<FVector2D>(), FVector2D::ZeroVector, 24.0), INDEX_NONE);
	return true;
}

#endif
