#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "BuildCameraRig.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildCameraRigInFrameTest,
	"AirportMgr.View.BuildCameraRig.InFrame",
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

/**
 * FCameraRigLimits::ApplyLimits/Reset - the seam issue #94 introduced to replace
 * ApplyViewLimits/ApplyWatchLimits (two near-identical copiers) and the two near-identical
 * resets in CreateBuildCamera and ToggleWatchAgent. One function each, exercised here
 * without a world - UBuildCameraComponent's own use of them needs a spawned camera actor
 * to observe past this point, which is what Airside.View.BuildCameraComponent.ToggleWatchAgent
 * covers instead.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildCameraRigLimitsTest,
	"Airside.View.BuildCameraRig.ApplyLimitsAndReset",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildCameraRigLimitsTest::RunTest(const FString& Parameters)
{
	FCameraRigLimits Limits;
	Limits.MinDistance = 111.0;
	Limits.MaxDistance = 22222.0;
	Limits.MinPitch = 12.0;
	Limits.MaxPitch = 65.0;
	Limits.StartDistance = 4000.0;
	Limits.StartYaw = 30.0;

	// 1. ApplyLimits copies the four clamp fields and NOTHING else - Focus/Distance/Yaw are
	// left exactly where the rig had them, which is what lets a details-panel edit take
	// effect on a LIVE rig without resetting its pose (see EaseToward's own use of this
	// shape for Target's limits).
	{
		FBuildCameraRig Rig;
		Rig.Focus = FVector2D(500.0, -250.0);
		Rig.Distance = 9999.0;
		Rig.Yaw = 123.0;

		Rig.ApplyLimits(Limits);

		TestEqual(TEXT("MinDistance copied"), Rig.MinDistance, 111.0);
		TestEqual(TEXT("MaxDistance copied"), Rig.MaxDistance, 22222.0);
		TestEqual(TEXT("MinPitch copied"), Rig.MinPitch, 12.0);
		TestEqual(TEXT("MaxPitch copied"), Rig.MaxPitch, 65.0);
		TestTrue(TEXT("Focus untouched"), Rig.Focus.Equals(FVector2D(500.0, -250.0), 1e-9));
		TestEqual(TEXT("Distance untouched"), Rig.Distance, 9999.0, 1e-9);
		TestEqual(TEXT("Yaw untouched"), Rig.Yaw, 123.0, 1e-9);
	}

	// 2. Reset does ApplyLimits AND snaps the pose to the limits' own start values - what
	// CreateBuildCamera's setup and ToggleWatchAgent's "reset on every entry" block each
	// used to spell out by hand.
	{
		FBuildCameraRig Rig;
		Rig.Focus = FVector2D(500.0, -250.0);
		Rig.Distance = 9999.0;
		Rig.Yaw = 123.0;

		Rig.Reset(Limits);

		TestEqual(TEXT("Reset also copies the limits"), Rig.MinDistance, 111.0);
		TestTrue(TEXT("Focus snaps to the origin"), Rig.Focus.Equals(FVector2D::ZeroVector, 1e-9));
		TestEqual(TEXT("Distance snaps to StartDistance"), Rig.Distance, 4000.0, 1e-9);
		TestEqual(TEXT("Yaw snaps to StartYaw"), Rig.Yaw, 30.0, 1e-9);
	}

	// 3. StartDistance is clamped into [MinDistance, MaxDistance] - an author who widens
	// MaxDistance without raising StartDistance, or the reverse, must not reset to a pose
	// the same Reset call would then refuse to hold.
	{
		FCameraRigLimits Narrow;
		Narrow.MinDistance = 1000.0;
		Narrow.MaxDistance = 2000.0;
		Narrow.StartDistance = 50.0;   // below MinDistance

		FBuildCameraRig Rig;
		Rig.Reset(Narrow);
		TestEqual(TEXT("an out-of-range StartDistance is clamped to MinDistance"), Rig.Distance, 1000.0, 1e-9);
	}

	return true;
}

#endif
