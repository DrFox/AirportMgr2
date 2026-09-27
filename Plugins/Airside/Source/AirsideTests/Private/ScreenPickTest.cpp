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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FScreenPickBoxTest,
	"Airside.Tool.ScreenPickBox",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FScreenPickBoxTest::RunTest(const FString& Parameters)
{
	using ScreenPick::FPickBox;

	// An aircraft-shaped box: 40 m long on local X, 36 m span on Y, 12 m tall, origin at the
	// NOSE GEAR (local X = 0) - which is where the old point pick looked, and the whole bug.
	const FBox Airframe(FVector(-3500.0, -1800.0, 0.0), FVector(500.0, 1800.0, 1200.0));

	// Parked at (10000, 0), turned 90 degrees: its span now runs along world X.
	FPickBox Parked;
	Parked.LocalBox = Airframe;
	Parked.LocalToWorld = FTransform(FRotator(0.0, 90.0, 0.0), FVector(10000.0, 0.0, 0.0));

	const auto Down = [](double X, double Y) { return TPair<FVector, FVector>(FVector(X, Y, 50000.0), FVector(X, Y, -1000.0)); };
	const TArray<FPickBox> One = { Parked };

	{
		// Local (-1000, 1700) is a wingtip: 1000 back from the nose gear, 17 m out. Turned 90
		// degrees that is world (10000 - 1700, -1000).
		const auto [S, E] = Down(10000.0 - 1700.0, -1000.0);
		TestEqual(TEXT("a click on the wingtip picks the aircraft - it is the whole body, not the gear"),
			ScreenPick::FirstBoxHit(One, S, E), 0);
	}
	{
		// Local (-3400, 0): the tail, 34 m from where the old pick looked.
		const auto [S, E] = Down(10000.0, -3400.0);
		TestEqual(TEXT("a click on the tail picks the aircraft"), ScreenPick::FirstBoxHit(One, S, E), 0);
	}
	{
		// Past the wingtip by a metre on the turned span axis.
		const auto [S, E] = Down(10000.0 - 1900.0, -1000.0);
		TestEqual(TEXT("a click past the wingtip misses - the box is the body, not a halo"),
			ScreenPick::FirstBoxHit(One, S, E), INDEX_NONE);
	}
	{
		// Unturned, local -X runs back along world -X; this point is behind the tail only if
		// the frame were ignored, so a pass here proves the rotation is applied.
		const auto [S, E] = Down(10000.0 - 3000.0, 0.0);
		TestEqual(TEXT("the box turns with the aircraft - an unrotated test would hit here"),
			ScreenPick::FirstBoxHit(One, S, E), INDEX_NONE);
	}

	{
		// Two aircraft on one line of sight: the ray enters the upper (airborne) one first.
		FPickBox Airborne = Parked;
		Airborne.LocalToWorld.SetLocation(FVector(10000.0, 0.0, 20000.0));
		const TArray<FPickBox> Stack = { Parked, Airborne };
		const auto [S, E] = Down(10000.0, -1000.0);
		TestEqual(TEXT("where two overlap on screen, the nearer one is picked"), ScreenPick::FirstBoxHit(Stack, S, E), 1);
	}
	{
		// A camera inside a box (the follow camera sitting in the cabin) enters it at once.
		const TArray<FPickBox> Inside = { Parked };
		TestEqual(TEXT("a segment starting inside a box hits it"),
			ScreenPick::FirstBoxHit(Inside, FVector(10000.0, -1000.0, 600.0), FVector(10000.0, -1000.0, -5000.0)), 0);
	}
	{
		// A box nothing has filled - an actor with no visible component - is never picked.
		const TArray<FPickBox> Empty = { FPickBox() };
		const auto [S, E] = Down(0.0, 0.0);
		TestEqual(TEXT("an empty box is never hit"), ScreenPick::FirstBoxHit(Empty, S, E), INDEX_NONE);
	}
	return true;
}

#endif
