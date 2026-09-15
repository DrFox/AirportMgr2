#include "CoreMinimal.h"
#include "Content/AirsideSettings.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadEntity.h"
#include "Profiles/RoadProfile.h"
#include "Solve/GuidelineGeom.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTurnPathIsTighterThanItsFilletTest,
	"Airside.Solve.TurnPathIsTighterThanItsFillet",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTurnPathIsTighterThanItsFilletTest::RunTest(const FString& Parameters)
{
	// THE THING A VEHICLE ACTUALLY DRIVES AT A JUNCTION IS NOT THE FILLET.
	//
	// URoadProfile::PreferredFilletRadius rounds the PAVEMENT. The guideline through the
	// corner is something else: FRoadGuidelineBuilder lays a QUADRATIC whose control point is
	// the node and whose ends are the two trimmed arm ends. Those two curves do not have the
	// same radius, and the quadratic is the tighter of them - which is what a truck has to
	// steer round.
	//
	// THIS COST A PIE SESSION ON 2026-09-15. The service road's fillet was derived at 874 uu
	// against a truck needing 699, Airside.Model.ServiceRoadFilletClearsTheTruckLock passed,
	// and the route log then read:
	//
	//   Route asks for R=580 uu at 0, but the steering lock allows only R>=699
	//
	// 580 / 874 = 0.66. The test was checking the radius that was REQUESTED and the truck was
	// driving the radius that was DELIVERED - CLAUDE.md's "check where a list is CONSUMED,
	// not where it is declared", in geometry rather than in a list.
	//
	// The arithmetic, so the margin is a derivation rather than a dialled-in number. For a
	// symmetric quadratic with legs d meeting at a right angle, at the midpoint
	//
	//   |B'| = d*sqrt(2),  B' x B'' = 4 d^2,  so  kappa = 4d^2 / (d*sqrt(2))^3 = sqrt(2)/d
	//
	// giving R = d / sqrt(2) = 0.707 d. A circular fillet of radius R has tangent length
	// R/tan(theta/2), which at a right angle is R itself - so the legs ARE the fillet radius
	// and the guideline comes out at 0.707 of it. Corners sharper than square come out
	// tighter still, which is the 0.66 measured in play.
	constexpr double Legs = 1000.0;

	const FVector2D A(-Legs, 0.0);
	const FVector2D Control(0.0, 0.0);
	const FVector2D B(0.0, Legs);

	const double Delivered = GuidelineGeom::TightestRadius(A, Control, B);
	const double Predicted = Legs / FMath::Sqrt(2.0);

	TestEqual(
		TEXT("a right-angle turn path's tightest radius is its leg length over root two"),
		Delivered, Predicted, Predicted * 0.02);

	// AND IT IS STRICTLY TIGHTER THAN THE FILLET THAT PRODUCED IT. This is the assertion that
	// matters: anything sizing a corner from the fillet alone is sizing it 30% too generously.
	TestTrue(
		FString::Printf(
			TEXT("the turn path (%.0f uu) is tighter than the %.0f uu fillet whose tangents "
			     "placed its ends"),
			Delivered, Legs),
		Delivered < Legs);

	// WHAT THE SERVICE ROAD MUST THEREFORE ASK FOR. URoadProfile::JunctionScalingMargin
	// exists to cover exactly this, and until 2026-09-15 it was 1.25 - below the 1.414 the
	// arithmetic above demands before any allowance for a corner sharper than square. It
	// passed only because the truck was small enough that 750 * 0.707 still cleared its 471.
	//
	// RESTATED, not read from the constant, for the reason every geometry test here restates
	// its arithmetic: a test that imported the margin would pass if someone set it to 1.0.
	const FAirframe Largest = UAirsideSettings::ResolveLargestServiceVehicle();
	const double Lock = FMath::Sin(FMath::DegreesToRadians(
		FMath::Clamp(Largest.Ground.MaxSteerDegrees, 0.0, 90.0)));
	if (!TestTrue(TEXT("the largest service vehicle has a steering lock to clear"),
			Lock > KINDA_SMALL_NUMBER && Largest.Wheelbase() > KINDA_SMALL_NUMBER))
	{
		return false;
	}
	const double Needed = Largest.Wheelbase() / Lock;

	const URoadProfile* Service = URoadProfile::MakeServiceRoadTransient();
	if (!TestNotNull(TEXT("the service road profile is buildable"), Service))
	{
		return false;
	}

	// The corner the road ACTUALLY hands the truck: the fillet, through the quadratic.
	const FVector2D CornerA(-Service->ResolvedFilletRadius(), 0.0);
	const FVector2D CornerB(0.0, Service->ResolvedFilletRadius());
	const double OnTheRoad = GuidelineGeom::TightestRadius(CornerA, Control, CornerB);

	TestTrue(
		FString::Printf(
			TEXT("the turn path a service road junction delivers (%.0f uu, from a %.0f uu "
			     "fillet) clears the %.0f uu the largest service vehicle's steering needs"),
			OnTheRoad, Service->ResolvedFilletRadius(), Needed),
		OnTheRoad >= Needed);

	return true;
}

#endif
