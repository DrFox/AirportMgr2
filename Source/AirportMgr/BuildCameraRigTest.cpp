#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "BuildCameraRig.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Named under "Airside." although the rig lives in the game module: Run-AirsideTests.ps1
 * filters on that prefix by default, and a test the pre-commit run does not pick up is a
 * test that will be found failing by the next person to touch the camera.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildCameraRigInFrameTest,
	"Airside.View.BuildCameraRig.InFrame",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildCameraRigInFrameTest::RunTest(const FString& Parameters)
{
	// FAutomationTestBase has no FVector2D overload; comparing the components keeps the
	// failure message naming which axis went wrong rather than printing two structs.
	const auto TestEqual2D = [this](const TCHAR* What, const FVector2D& Actual, const FVector2D& Expected)
	{
		TestEqual(*FString::Printf(TEXT("%s (X)"), What), Actual.X, Expected.X, 1e-9);
		TestEqual(*FString::Printf(TEXT("%s (Y)"), What), Actual.Y, Expected.Y, 1e-9);
	};

	// A rig whose focus and yaw are RELATIVE TO AN AIRCRAFT: the watch camera keeps its
	// state in the aircraft's frame so that easing and panning ride along through a turn,
	// and asks InFrame for the world-space rig to place the camera from.
	FBuildCameraRig Relative;
	Relative.Distance = 1500.0;
	Relative.MinDistance = 800.0;
	Relative.MaxDistance = 20000.0;
	Relative.MinPitch = 10.0;
	Relative.MaxPitch = 60.0;

	const FVector2D Aircraft(1000.0, 2000.0);
	const double Heading = 90.0; // nose along +Y

	// 1. YAW IS RELATIVE TO THE HEADING. Looking along heading - 90 is looking across the
	// aircraft from its right-hand side; with the nose on +Y that is looking along +X.
	Relative.Yaw = -90.0;
	Relative.Focus = FVector2D::ZeroVector;
	{
		const FBuildCameraRig World = Relative.InFrame(Aircraft, Heading);

		TestEqual(TEXT("world yaw is heading plus relative yaw"), World.Yaw, 0.0, 1e-9);
		TestEqual2D(TEXT("zero offset focuses on the aircraft itself"), World.Focus, Aircraft);
		TestEqual(TEXT("limits travel with the rig, so the derived pitch is the same one the "
			"player tuned"), World.PitchDegrees(), Relative.PitchDegrees(), 1e-9);

		// The camera sits Distance away along the reversed look direction: behind the
		// aircraft on -X, level with it on Y, above it by the pitch. This is what "from the
		// side, Distance away" means once heading has been folded in.
		const double Pitch = FMath::DegreesToRadians(World.PitchDegrees());
		const FVector Eye = World.CameraLocation(0.0);
		TestEqual(TEXT("camera backs off along -X"), Eye.X, 1000.0 - 1500.0 * FMath::Cos(Pitch), 1e-6);
		TestEqual(TEXT("camera stays on the aircraft's Y"), Eye.Y, 2000.0, 1e-6);
		TestEqual(TEXT("camera rises by the pitch"), Eye.Z, 1500.0 * FMath::Sin(Pitch), 1e-6);
		TestEqual(TEXT("camera looks along world yaw"), World.CameraRotation().Yaw, 0.0, 1e-9);
	}

	// 2. THE FOCUS OFFSET IS IN THE AIRCRAFT'S FRAME. X is ahead of the nose, Y is to the
	// right of it - Unreal is left-handed, +Y right of +X - so with the nose on +Y, "ahead"
	// is +Y and "right" is -X. A WASD nudge towards the tail must stay towards the tail
	// after the aircraft turns, which is the whole reason the offset is not stored in world.
	Relative.Focus = FVector2D(500.0, 0.0);
	TestEqual2D(TEXT("5 m ahead of the nose lands on +Y for a +Y heading"),
		Relative.InFrame(Aircraft, Heading).Focus, FVector2D(1000.0, 2500.0));

	Relative.Focus = FVector2D(0.0, 300.0);
	TestEqual2D(TEXT("3 m off the right wing lands on -X for a +Y heading"),
		Relative.InFrame(Aircraft, Heading).Focus, FVector2D(700.0, 2000.0));

	// 3. THE RELATIVE RIG IS NOT TOUCHED. InFrame is a projection, not a mode switch; the
	// watch state must survive the call or the next frame would compound the heading.
	TestEqual2D(TEXT("relative focus untouched"), Relative.Focus, FVector2D(0.0, 300.0));
	TestEqual(TEXT("relative yaw untouched"), Relative.Yaw, -90.0, 1e-9);

	return true;
}

#endif
