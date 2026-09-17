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
	// WHAT CHANGED 2026-09-15. The two numbers above no longer exist as two numbers. The
	// service road's fillet is DERIVED from the largest vehicle admitted, the way a painted
	// taxi line is swept for the largest aircraft admitted (IcaoCode::RadiusForLetter), so
	// there is nothing left to drift and this test asserts the derivation holds rather than
	// that two transcriptions still match.
	//
	// It still MEASURES rather than trusting: ResolvedFilletRadius could be wired to the
	// wrong resolver, or the margin dropped, and the arithmetic below catches both.
	const FAirframe Largest = UAirsideSettings::ResolveLargestServiceVehicle();

	// Guard rather than assume: a largest vehicle that pivots would divide by zero below, and
	// would also mean the resolver had stopped describing a steered vehicle at all.
	if (!TestEqual(TEXT("the largest service vehicle steers geometrically"),
			Largest.EffectiveSteerLaw(), ESteerLaw::RollingSteer))
	{
		return false;
	}
	const double Lock = FMath::Sin(FMath::DegreesToRadians(
		FMath::Clamp(Largest.Ground.MaxSteerDegrees, 0.0, 90.0)));
	if (!TestTrue(TEXT("and has a steering lock above zero"), Lock > KINDA_SMALL_NUMBER))
	{
		return false;
	}

	// The same expression FSpeedProfile::Build uses. Written out rather than shared because a
	// helper both sides called could be wrong in one place and agree with itself.
	const double TightestFollowable = Largest.Wheelbase() / Lock;

	const URoadProfile* Service = URoadProfile::MakeServiceRoadTransient();
	if (!TestNotNull(TEXT("the service road profile is buildable"), Service))
	{
		return false;
	}

	// HEADROOM, AND THE REASON IS NOT THE ONE THIS TEST USED TO GIVE. It blamed
	// RoadNetworkSolver for scaling the preferred radius down to fit a junction's arms; it
	// does not - RoadGeom::SolveFillet passes the radius through verbatim. What actually loses
	// 30% is that a vehicle drives the GUIDELINE through the corner, which is a QUADRATIC, not
	// the pavement fillet: R = d/sqrt(2) at a right angle. Naming the wrong mechanism is how
	// the margin came to be 1.25, below the 1.414 the arithmetic demands - it could not have
	// been right for any vehicle, and survived only because the truck was small enough that
	// 750 * 0.707 still cleared its 471 uu lock.
	//
	// Airside.Solve.TurnPathIsTighterThanItsFillet pins that arithmetic and measures the
	// DELIVERED radius. This test keeps the coarser check - that the REQUEST clears the lock
	// with margin - because it is the one a reader of the profile can verify by eye.
	//
	// RESTATED rather than read from URoadProfile::JunctionScalingMargin, for exactly the
	// reason the radius is restated: a test that imported the constant would still pass if
	// someone set it to 1.0.
	const double Required = TightestFollowable * 1.6;

	TestTrue(
		FString::Printf(
			TEXT("the service road's derived %.0f uu fillet clears the %.0f uu the largest "
			     "service vehicle's steering needs, with margin for the junction solver "
			     "scaling it down (wheelbase %.1f, lock %.1f deg, needs >= %.0f)"),
			Service->ResolvedFilletRadius(), TightestFollowable,
			Largest.Wheelbase(), Largest.Ground.MaxSteerDegrees, Required),
		Service->ResolvedFilletRadius() >= Required);

	// AND THE PROFILE CARRIES NO NUMBER TO DRIFT. A stored radius would be stale the moment a
	// larger vehicle joined the fleet, which is precisely the failure this change removes - so
	// asserting the sentinel is asserting that the derivation is actually reached, rather than
	// that some authored figure happens to be big enough today.
	TestEqual(
		TEXT("the service road profile stores the derive sentinel rather than a radius"),
		Service->PreferredFilletRadius, 0.0);

	// A TAXIWAY IS UNAFFECTED, which is what says the sentinel narrowed the change to the
	// roads it was meant for. Its corner is swept for the largest AIRCRAFT admitted and stays
	// authored.
	const URoadProfile* Taxiway = URoadProfile::MakeTransient(2300.0, 1530.0);
	if (Taxiway != nullptr)
	{
		TestEqual(
			TEXT("a taxiway still turns on its own authored radius, not a vehicle's"),
			Taxiway->ResolvedFilletRadius(), 1530.0);
	}

	return true;
}

#endif
