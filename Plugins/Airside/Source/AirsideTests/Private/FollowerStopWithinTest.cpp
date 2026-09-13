#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RouteFollower.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFollowerStopWithinTest,
	"Airside.Model.Follower.StopWithin",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFollowerStopWithinTest::RunTest(const FString& Parameters)
{
	FRoutePlan Plan;
	Plan.Result = ERouteResult::Found;
	Plan.Polyline = { FVector2D(0.0, 0.0), FVector2D(20000.0, 0.0) };
	Plan.Length = 20000.0;

	FAirframe Airframe;   // Accel 100, Decel 200, SpeedCap 1000 - the struct defaults
	FRouteFollower Follower;
	Follower.Start(Plan, Airframe);

	// Run up to cruise with nothing ahead.
	FVector2D At; double Heading = 0.0;
	for (int32 Tick = 0; Tick < 400; ++Tick) { Follower.Advance(0.05, Airframe, At, Heading); }
	if (!TestTrue(TEXT("at cruise before the stop is asked for"), Follower.Speed > 990.0)) { return false; }

	// A stop point 3000 uu ahead, held fixed in ROUTE distance across ticks the way the
	// arbiter will hold it: the follower must come to rest short of it, never past it, and
	// never braking harder than the airframe has.
	const double StopAt = Follower.Travelled + 3000.0;
	double MaxDecelSeen = 0.0;
	double LastSpeed = Follower.Speed;
	for (int32 Tick = 0; Tick < 400; ++Tick)
	{
		const double StopWithin = FMath::Max(0.0, StopAt - Follower.Travelled);
		Follower.Advance(0.05, Airframe, StopWithin, At, Heading);
		MaxDecelSeen = FMath::Max(MaxDecelSeen, (LastSpeed - Follower.Speed) / 0.05);
		LastSpeed = Follower.Speed;
		TestTrue(TEXT("never passes the stop point"), Follower.Travelled <= StopAt + 1e-6);
	}
	TestTrue(TEXT("has stopped"), Follower.Speed < 1e-6);
	TestTrue(FString::Printf(TEXT("stopped within 1 uu of the stop point (at %.1f of %.1f)"), Follower.Travelled, StopAt),
		StopAt - Follower.Travelled < 1.0);
	TestTrue(FString::Printf(TEXT("braked no harder than Decel (%.1f <= 200)"), MaxDecelSeen), MaxDecelSeen <= 200.0 + 1e-6);

	// Released: it goes again, from rest, at the airframe's acceleration - not at cruise.
	Follower.Advance(0.05, Airframe, TNumericLimits<double>::Max(), At, Heading);
	TestTrue(TEXT("resumes from rest at Accel, not by snapping to cruise"), Follower.Speed > 0.0 && Follower.Speed <= 100.0 * 0.05 + 1e-6);
	return true;
}

#endif
