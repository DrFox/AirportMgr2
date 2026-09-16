#include "CoreMinimal.h"
#include "Content/AirsideSettings.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadEntity.h"
#include "Model/ReverseRun.h"
#include "Model/SpeedProfile.h"
#include "Solve/GuidelineGeom.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FReverseTurnsTighterThanForwardTest,
	"Airside.Model.ReverseTurnsTighterThanForward",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FReverseTurnsTighterThanForwardTest::RunTest(const FString& Parameters)
{
	// WHY A BAY IS AFFORDABLE AT ALL. Going forward, a rigid vehicle pivots about its STEERED
	// axle and the tightest arc it can hold is L/sin(lock). Going backward it pivots about its
	// FIXED axle, and the tightest becomes L/tan(lock) - strictly smaller for any lock under
	// 90 degrees, because tan exceeds sin there.
	//
	// That is the whole argument for backing into a bay rather than driving through one: the
	// manoeuvre that needs 699 uu of room forwards needs 495 in reverse, about 30% less, and a
	// drive-through bay would have needed TWO forward corners where this needs none.
	const FAirframe Truck = UAirsideSettings::ResolveLargestServiceVehicle();
	if (!TestTrue(TEXT("the service vehicle steers on measured axles"), Truck.HasAxles()))
	{
		return false;
	}

	const double Forward = Truck.TightestFollowableRadius();
	const double Reverse = Truck.TightestReversibleRadius();

	AddInfo(FString::Printf(TEXT("wheelbase %.1f, lock %.1f deg: forward %.1f uu, reverse %.1f uu"),
		Truck.Wheelbase(), Truck.Ground.MaxSteerDegrees, Forward, Reverse));

	TestTrue(TEXT("a reversing vehicle turns tighter than a forward one"), Reverse < Forward);

	// THE ARITHMETIC, RESTATED rather than shared with the accessor - the reason
	// FAirframe::TightestFollowableRadius gives at its own copy. A helper both sides called
	// could be wrong in one place and agree with itself.
	const double Lock = FMath::DegreesToRadians(FMath::Clamp(Truck.Ground.MaxSteerDegrees, 0.0, 90.0));
	TestTrue(
		*FString::Printf(TEXT("reverse is Wheelbase/tan(lock) - got %.2f, expected %.2f"),
			Reverse, Truck.Wheelbase() / FMath::Tan(Lock)),
		FMath::IsNearlyEqual(Reverse, Truck.Wheelbase() / FMath::Tan(Lock), 0.01));

	// SAME GUARDS AS ITS FORWARD SIBLING, and zero means "this airframe does not steer
	// geometrically" rather than "it can turn on a sixpence". A pivot-law vehicle turns about
	// itself and has no such limit; reporting 0 for it would read as an infinitely tight
	// requirement to any caller comparing a radius against it.
	FAirframe Pivoting = Truck;
	Pivoting.SteerLaw = ESteerLaw::Pivot;
	TestEqual(TEXT("a pivot-law vehicle reports no reverse limit"),
		Pivoting.TightestReversibleRadius(), 0.0, UE_DOUBLE_KINDA_SMALL_NUMBER);

	FAirframe Locked = Truck;
	Locked.Ground.MaxSteerDegrees = 0.0;
	TestEqual(TEXT("a vehicle with no steering lock reports no reverse limit"),
		Locked.TightestReversibleRadius(), 0.0, UE_DOUBLE_KINDA_SMALL_NUMBER);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpeedProfileJudgesReverseByTheReverseLimitTest,
	"Airside.Model.SpeedProfileJudgesReverseByTheReverseLimit",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSpeedProfileJudgesReverseByTheReverseLimitTest::RunTest(const FString& Parameters)
{
	// THE ONE ARC THAT DISCRIMINATES. Between the reverse limit (495) and the forward one
	// (699) sits a band of curves a vehicle can back along and cannot drive along. An arc in
	// that band is the only shape that tells the two rules apart: judged forwards it must be
	// refused, judged backwards it must be accepted.
	//
	// WITHOUT THIS, a reverse leg would be checked against the forward limit and rejected for
	// being legal, or - far worse - not checked at all, which is precisely how four attempts
	// at stand routing shipped green and crabbed. The oracle learns direction BEFORE anything
	// reverses, rather than after something does.
	const FAirframe Truck = UAirsideSettings::ResolveLargestServiceVehicle();
	const double Forward = Truck.TightestFollowableRadius();
	const double Reverse = Truck.TightestReversibleRadius();
	if (!TestTrue(TEXT("there is a band between the two limits to aim at"), Reverse < Forward))
	{
		return false;
	}

	// Midway between them, so neither limit is being grazed and a small change in either
	// direction cannot flip the result by accident.
	const double Radius = (Forward + Reverse) * 0.5;

	// A quarter-circle of that radius, sampled. An arc rather than a quadratic because the
	// radius has to BE the figure under test, not approximately it.
	TArray<FVector2D> Arc;
	constexpr int32 Steps = 24;
	for (int32 At = 0; At <= Steps; ++At)
	{
		const double Theta = (UE_DOUBLE_HALF_PI * At) / Steps;
		Arc.Add(FVector2D(Radius * FMath::Sin(Theta), Radius * (1.0 - FMath::Cos(Theta))));
	}

	FSpeedProfile Forwards;
	Forwards.Build(Arc, Truck, EDriveDirection::Forward);

	FSpeedProfile Backwards;
	Backwards.Build(Arc, Truck, EDriveDirection::Reverse);

	AddInfo(FString::Printf(
		TEXT("arc R=%.0f uu between reverse %.0f and forward %.0f; forwards says tightest %.0f, ")
		TEXT("backwards says tightest %.0f"),
		Radius, Reverse, Forward, Forwards.GetTightestRadius(), Backwards.GetTightestRadius()));

	TestTrue(
		*FString::Printf(TEXT("an arc of %.0f uu is refused going forwards (limit %.0f)"),
			Radius, Forward),
		Forwards.WasTighterThanLock());

	TestFalse(
		*FString::Printf(TEXT("an arc of %.0f uu is allowed going backwards (limit %.0f)"),
			Radius, Reverse),
		Backwards.WasTighterThanLock());

	// AND THE OTHER RULE IS UNCHANGED BY DIRECTION. A vertex whose heading changes instantly is
	// untakeable whichever way the vehicle is pointing - nothing about reversing lets a body
	// rotate without moving. Asserted so a future change to the direction switch cannot quietly
	// exempt reverse from the sharp-vertex rule.
	TArray<FVector2D> Hairpin = { FVector2D(0.0, 0.0), FVector2D(1000.0, 0.0), FVector2D(0.0, 10.0) };
	FSpeedProfile Sharp;
	Sharp.Build(Hairpin, Truck, EDriveDirection::Reverse);
	TestTrue(TEXT("a sharp vertex is still sharp in reverse"), Sharp.HasSharpVertex());

	return true;
}

namespace ReverseFixture
{
	/**
	 * EVERY GROUND VEHICLE THE GAME HAS, asked of the resolvers rather than typed here.
	 *
	 * The fleet is one airframe today - ResolveLargestServiceVehicle's own comment says it
	 * returns the same one as ResolveDefaultVehicle - and a hand-written list would stay a
	 * list of one after somebody adds the second. Enumerating from the resolvers means a new
	 * vehicle widens every test below without anyone remembering to.
	 */
	inline TArray<TPair<FString, FAirframe>> GroundFleet()
	{
		TArray<TPair<FString, FAirframe>> Fleet;
		Fleet.Emplace(TEXT("default vehicle"), UAirsideSettings::ResolveDefaultVehicle());

		const FAirframe Largest = UAirsideSettings::ResolveLargestServiceVehicle();
		if (!FMath::IsNearlyEqual(Largest.Wheelbase(),
				Fleet[0].Value.Wheelbase(), UE_DOUBLE_KINDA_SMALL_NUMBER))
		{
			Fleet.Emplace(TEXT("largest service vehicle"), Largest);
		}
		return Fleet;
	}

	/** An arc of the given radius through a quarter turn, as a bay's approach would be. */
	inline FRoutePlan ArcPlan(double Radius)
	{
		FRoutePlan Plan;
		constexpr int32 Steps = 24;
		for (int32 At = 0; At <= Steps; ++At)
		{
			const double Theta = (UE_DOUBLE_HALF_PI * At) / Steps;
			Plan.Polyline.Add(
				FVector2D(Radius * FMath::Sin(Theta), Radius * (1.0 - FMath::Cos(Theta))));
		}
		Plan.Length = GuidelineGeom::PolylineLength(Plan.Polyline);

		// FRoutePlan::IsValid() is Result == Found, not "has a polyline" - a fixture that only
		// fills the geometry is refused at FReverseRun::Start's first guard, before the check
		// it was written to exercise, and reads as the manoeuvre rejecting a legal arc.
		Plan.Result = ERouteResult::Found;
		return Plan;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEveryGroundVehicleBacksIntoItsBayWithoutCrabbingTest,
	"Airside.Model.EveryGroundVehicleBacksIntoItsBayWithoutCrabbing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FEveryGroundVehicleBacksIntoItsBayWithoutCrabbingTest::RunTest(const FString& Parameters)
{
	using namespace ReverseFixture;

	// THE REQUIREMENT, STATED AS SOMETHING THE MACHINE CHECKS. A pre-computed manoeuvre is only
	// safe if the curve suits the vehicle playing it back - playback has no error term, so a
	// curve the body cannot hold is crabbed silently. Asked of EVERY vehicle in the fleet, at
	// the tightest arc each one is allowed, and at one just inside that limit which must be
	// refused rather than driven badly.
	const TArray<TPair<FString, FAirframe>> Fleet = GroundFleet();
	if (!TestTrue(TEXT("there is a ground fleet to check"), Fleet.Num() > 0))
	{
		return false;
	}

	for (const TPair<FString, FAirframe>& Vehicle : Fleet)
	{
		const FAirframe& Airframe = Vehicle.Value;
		const double Limit = Airframe.TightestReversibleRadius();

		AddInfo(FString::Printf(TEXT("%s: wheelbase %.0f, reverse limit %.0f uu"),
			*Vehicle.Key, Airframe.Wheelbase(), Limit));

		if (!TestTrue(*FString::Printf(TEXT("%s steers geometrically"), *Vehicle.Key), Limit > 0.0))
		{
			continue;
		}

		// AT ITS LIMIT, with a whisker of margin so floating point does not decide the result.
		FReverseRun Run;
		TestTrue(
			*FString::Printf(TEXT("%s arms on an arc at its own reverse limit (%.0f uu)"),
				*Vehicle.Key, Limit),
			Run.Start(ArcPlan(Limit * 1.01), Airframe, /*InReverseSpeed=*/100.0));

		// AND REFUSES ONE INSIDE IT. This is the half that matters: a manoeuvre that armed on
		// anything would guarantee nothing at all.
		FReverseRun TooTight;
		TestFalse(
			*FString::Printf(TEXT("%s refuses an arc inside its reverse limit"), *Vehicle.Key),
			TooTight.Start(ArcPlan(Limit * 0.8), Airframe, /*InReverseSpeed=*/100.0));

		// DRIVEN TO THE END, and the body faces AWAY from the way it is moving the whole time.
		FVector2D Position = FVector2D::ZeroVector;
		double Heading = 0.0;
		int32 Frames = 0;
		while (Run.Advance(1.0 / 30.0, /*StopWithin=*/1000.0, Position, Heading) && Frames < 10000)
		{
			++Frames;
		}

		TestTrue(*FString::Printf(TEXT("%s completes the manoeuvre"), *Vehicle.Key),
			Run.HasArrived());

		// The arc ends heading +Y; backing along it, the body faces -Y.
		TestTrue(
			*FString::Printf(TEXT("%s finishes facing away from its direction of travel (%.0f deg)"),
				*Vehicle.Key, FMath::RadiansToDegrees(Heading)),
			FMath::IsNearlyEqual(FMath::Abs(FMath::UnwindRadians(Heading)),
				UE_DOUBLE_HALF_PI, 0.05));
	}

	return true;
}

#endif
