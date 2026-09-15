#include "CoreMinimal.h"
#include "Content/AirsideSettings.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadEntity.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FServiceRoadFilletClearsTheTruckLockTest,
	"Airside.Model.ServiceRoadFilletClearsTheTruckLock",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FServiceRoadFilletClearsTheTruckLockTest::RunTest(const FString& Parameters)
{
	// TWO NUMBERS AUTHORED IN DIFFERENT FILES THAT HAVE TO AGREE, and until this test they
	// did not know about each other:
	//
	//   URoadProfile::MakeServiceRoadTransient  - the junction fillet a service road turns on
	//   UAirsideSettings::ResolveDefaultVehicle - the truck's wheelbase and steering lock
	//
	// A rigid vehicle cannot follow an arc tighter than Wheelbase / sin(lock) AT ANY SPEED -
	// FSpeedProfile calls that TightestFollowable, and on a corner below it drops the agent
	// to MinSteeringSpeed and wears the crab. So a service road whose own corners are tighter
	// than its own trucks can steer is a road the traffic crawls round for ever.
	//
	// THAT IS NOT HYPOTHETICAL. Reported from play 2026-09-14: the fuel truck crawled every
	// corner between the depot and the stand. The profile log read
	//
	//   Tightest R=418 uu at 3254 -> 50 uu/s (TIGHTER THAN THE STEERING LOCK).
	//   lock allows R>=510 (wheelbase 361, lock 45 deg)
	//
	// The authored fillet was 500 uu against a 510 uu requirement - short by TEN
	// CENTIMETRES, and 418 once RoadNetworkSolver scaled it down to fit the junction. Two
	// figures a couple of percent apart, on opposite sides of a cliff, neither wrong on its
	// own. Exactly the drift a test pins and review does not.
	const FAirframe Van = UAirsideSettings::ResolveDefaultVehicle();

	// Guard rather than assume: a Van with no axles would divide by zero below and would
	// also mean ResolveDefaultVehicle had stopped describing a steered vehicle at all.
	if (!TestTrue(TEXT("the default vehicle has measured axles, or it is not steering"),
			Van.HasAxles()))
	{
		return false;
	}
	const double Lock = FMath::Sin(FMath::DegreesToRadians(
		FMath::Clamp(Van.Ground.MaxSteerDegrees, 0.0, 90.0)));
	if (!TestTrue(TEXT("and a steering lock above zero"), Lock > KINDA_SMALL_NUMBER))
	{
		return false;
	}

	// The same expression FSpeedProfile::Build uses. Written out rather than shared because
	// a helper both sides called could be wrong in one place and agree with itself.
	const double TightestFollowable = Van.Wheelbase() / Lock;

	const URoadProfile* Service = URoadProfile::MakeServiceRoadTransient();
	if (!TestNotNull(TEXT("the service road profile is buildable"), Service))
	{
		return false;
	}

	// HEADROOM, NOT MERE SUFFICIENCY. An exact match would pass while sitting on the cliff
	// edge, and the junction solver does not lay the preferred radius unconditionally:
	// RoadNetworkSolver scales it down when the arms cannot fit it, which is how 500 became
	// 418 on the reported route. A fillet that only just clears the lock in the profile is
	// therefore one that does NOT clear it on a real junction. 1.25 covers the scaling seen
	// in play with room to spare, without demanding a motorway sweep on an apron road.
	const double Required = TightestFollowable * 1.25;

	TestTrue(
		FString::Printf(
			TEXT("the service road's %.0f uu fillet clears the %.0f uu the default vehicle's "
			     "steering needs, with margin for the junction solver scaling it down "
			     "(wheelbase %.1f, lock %.1f deg, needs >= %.0f)"),
			Service->PreferredFilletRadius, TightestFollowable,
			Van.Wheelbase(), Van.Ground.MaxSteerDegrees, Required),
		Service->PreferredFilletRadius >= Required);

	return true;
}

#endif
