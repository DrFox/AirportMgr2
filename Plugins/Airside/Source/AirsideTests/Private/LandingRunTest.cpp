#include "CoreMinimal.h"
#include "Entities/AircraftType.h"
#include "Misc/AutomationTest.h"
#include "Model/LandingRun.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// Prefixed against the UNITY build - these test files share one translation unit.

	/** Flies an arrival to completion, reporting what happened at each transition. */
	struct FLandingTrace
	{
		double TouchdownAt = 0.0;
		double TouchdownSpeed = 0.0;
		double TouchdownPitch = 0.0;
		double TouchdownSinkRate = 0.0;
		double ApproachSinkRate = 0.0;
		double VacatedAt = 0.0;
		double VacatedSpeed = 0.0;
		double Seconds = 0.0;
		double LowestAltitudeWhileFlying = 0.0;
		bool bReachedGround = false;
		bool bVacated = false;
	};

	FLandingTrace FlyLanding(FLandingRun& Run, double Step = 1.0 / 60.0, double Limit = 600.0)
	{
		FLandingTrace Trace;

		FVector2D At = FVector2D::ZeroVector;
		double Heading = 0.0;
		double Altitude = 0.0;
		double Pitch = 0.0;
		double PreviousAltitude = Run.Altitude;
		ELandingPhase Previous = Run.Phase;

		while (Run.Advance(Step, At, Heading, Altitude, Pitch) && Trace.Seconds < Limit)
		{
			Trace.Seconds += Step;

			// Sink rate is MEASURED from the altitude actually flown, not read off a field.
			// A model that reported a rate it did not fly would pass every assertion below
			// while the aircraft dropped like a brick.
			const double Sink = (PreviousAltitude - Altitude) / Step;

			if (Run.Phase == ELandingPhase::Approach)
			{
				Trace.ApproachSinkRate = Sink;
			}

			if (Previous == ELandingPhase::Flare && Run.Phase == ELandingPhase::Rollout)
			{
				Trace.bReachedGround = true;
				Trace.TouchdownAt = Run.Travelled;
				Trace.TouchdownSpeed = Run.Speed;
				Trace.TouchdownPitch = Pitch;
				Trace.TouchdownSinkRate = Sink;
			}

			if (Run.Phase == ELandingPhase::Flare)
			{
				Trace.LowestAltitudeWhileFlying = Altitude;
			}

			if (Run.Phase == ELandingPhase::Vacated)
			{
				Trace.bVacated = true;
				Trace.VacatedAt = Run.Travelled;
				Trace.VacatedSpeed = Run.Speed;
				break;
			}

			PreviousAltitude = Altitude;
			Previous = Run.Phase;
		}

		return Trace;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLandingRunTest,
	"Airside.Model.LandingRun",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FLandingRunTest::RunTest(const FString& Parameters)
{
	const FGroundPerformance Ground = UAircraftType::PiperMeridianGround();
	const FClimbPerformance Climb = UAircraftType::PiperMeridianClimb();
	const FApproachPerformance Approach = UAircraftType::PiperMeridianApproach();

	// A generous runway, so nothing below is measuring a refusal by accident.
	constexpr double LongRunway = 150000.0;

	// 1. IT ARMS, AND IT STARTS SHORT OF THE THRESHOLD IN THE AIR.
	//
	//    Travelled is signed for exactly this: an aircraft on final is BEFORE the threshold,
	//    and giving it a separate "distance to run" would be a second answer to where it is.
	{
		FLandingRun Run;
		if (!TestTrue(TEXT("a long runway accepts the arrival"),
			Run.Start(FVector2D::ZeroVector, FVector2D(1.0, 0.0), LongRunway,
				Ground, Climb, Approach)))
		{
			return false;
		}

		TestTrue(FString::Printf(TEXT("it joins short of the threshold (%.0f uu)"), Run.Travelled),
			Run.Travelled < 0.0);
		TestEqual(TEXT("at the approach altitude"), Run.Altitude, Approach.FinalAltitude);
		TestEqual(TEXT("at Vref"), Run.Speed, Ground.Landing.SpeedCap);

		// The nose sits where the SPEED puts it. On approach the wing needs its angle and
		// the flight path points down, so the attitude is the difference - a shallow nose-up.
		const double Expected =
			Climb.RequiredAngleAt(Ground.Landing.SpeedCap, Ground.Takeoff.SpeedCap)
			- Approach.GlideslopeDegrees;
		TestEqual(TEXT("with the attitude its speed and slope imply"), Run.Pitch, Expected, 1.0e-9);
	}

	// 2. THE MEASUREMENT. THE FLARE ARRESTS THE DESCENT.
	//
	//    This is the whole point of the phase and the one thing a scripted altitude ramp
	//    cannot do. On the glideslope the aircraft is sinking at Vref x sin(3 deg); it must
	//    reach the ground appreciably slower than that, because the nose came up and the
	//    wing caught the weight.
	//
	//    Asserted as a RATIO of the approach sink rate rather than as an absolute figure, so
	//    it keeps measuring the flare if Vref or the glideslope are ever retuned.
	{
		FLandingRun Run;
		Run.Start(FVector2D::ZeroVector, FVector2D(1.0, 0.0), LongRunway, Ground, Climb, Approach);
		const FLandingTrace Trace = FlyLanding(Run);

		if (!TestTrue(TEXT("the aircraft reaches the ground"), Trace.bReachedGround))
		{
			return false;
		}

		TestTrue(FString::Printf(
			TEXT("the approach sinks at the glideslope rate (%.0f uu/s)"), Trace.ApproachSinkRate),
			Trace.ApproachSinkRate > 150.0);

		TestTrue(FString::Printf(
			TEXT("the flare arrests it: touchdown at %.0f uu/s against %.0f on approach"),
			Trace.TouchdownSinkRate, Trace.ApproachSinkRate),
			Trace.TouchdownSinkRate < Trace.ApproachSinkRate * 0.5);

		// 3. AND IT TOUCHES DOWN NOSE-UP AND SLOWER THAN IT APPROACHED, which is what makes
		//    it a landing rather than an arrival on the nosewheel at Vref.
		TestTrue(FString::Printf(TEXT("nose-up at touchdown (%.1f deg)"), Trace.TouchdownPitch),
			Trace.TouchdownPitch > 5.0);

		TestTrue(FString::Printf(
			TEXT("speed bled in the flare: %.0f uu/s at touchdown against Vref %.0f"),
			Trace.TouchdownSpeed, Ground.Landing.SpeedCap),
			Trace.TouchdownSpeed < Ground.Landing.SpeedCap);

		// 4. TOUCHDOWN IS PAST THE THRESHOLD, not before it. An aircraft that reached the
		//    ground while Travelled was still negative has landed short, in the grass.
		TestTrue(FString::Printf(TEXT("it lands ON the runway (%.0f uu past the threshold)"),
			Trace.TouchdownAt),
			Trace.TouchdownAt > 0.0);

		// 5. IT VACATES, AT TAXI SPEED, WITHIN THE RUNWAY.
		//
		//    The last of those is the one the refusal in Start is promising, so it is worth
		//    measuring on a run that was accepted rather than trusting the arithmetic.
		if (TestTrue(TEXT("the arrival completes"), Trace.bVacated))
		{
			TestTrue(FString::Printf(TEXT("down to taxi speed (%.0f uu/s)"), Trace.VacatedSpeed),
				Trace.VacatedSpeed <= FMath::Max(Ground.Taxi.SpeedCap, Ground.MinTaxiSpeed) + 1.0);

			TestTrue(FString::Printf(
				TEXT("and stopped on the runway: vacated %.0f uu of %.0f available"),
				Trace.VacatedAt, LongRunway),
				Trace.VacatedAt < LongRunway);
		}

		// 6. THE GROUND ROLL, read back rather than asserted from the brochure.
		//
		//    Landing.Decel was DERIVED from the published 31,100 uu roll at Vref. This model
		//    touches down slower than Vref because the flare bleeds speed, so the roll must
		//    come out SHORTER - if it ever came out longer, the flare has stopped bleeding
		//    and the derivation behind that constant no longer describes this code.
		const double Roll = Trace.VacatedAt - Trace.TouchdownAt;
		TestTrue(FString::Printf(
			TEXT("ground roll %.0f uu, shorter than the 31100 published at full Vref"), Roll),
			Roll > 0.0 && Roll < 31100.0);
	}

	// 7. A RUNWAY IT CANNOT STOP ON IS REFUSED, not attempted.
	//
	//    The mirror of FTakeoffRun's refusal, and the user chose it over a go-around: a
	//    go-around is a second flight phase and doubles this.
	{
		const double Needed = FLandingRun::RequiredLandingDistance(Ground, Approach);
		TestTrue(FString::Printf(TEXT("stopping needs a real distance (%.0f uu)"), Needed),
			Needed > 0.0);

		FLandingRun Run;
		TestFalse(TEXT("a runway shorter than that is refused"),
			Run.Start(FVector2D::ZeroVector, FVector2D(1.0, 0.0), Needed * 0.5,
				Ground, Climb, Approach));

		TestTrue(TEXT("and one longer than it is not"),
			Run.Start(FVector2D::ZeroVector, FVector2D(1.0, 0.0), Needed * 1.1,
				Ground, Climb, Approach));
	}

	// 8. AN AIRFRAME WITH NO LANDING FIGURES DECLINES rather than flying a nonsense - and
	//    can still taxi, which is why FGroundPerformance::IsSet does not require Landing.
	{
		FGroundPerformance NoLanding = Ground;
		NoLanding.Landing = FGroundRegime();
		NoLanding.Landing.SpeedCap = 0.0;

		TestTrue(TEXT("it can still move about the airport"), NoLanding.IsSet());

		FLandingRun Run;
		TestFalse(TEXT("but it cannot land"),
			Run.Start(FVector2D::ZeroVector, FVector2D(1.0, 0.0), LongRunway,
				NoLanding, Climb, Approach));
	}

	// 9. ADVANCE IS THE ONLY THING THAT MOVES IT, and it declines once done - the same
	//    contract as the follower and the departure, for the same reason: a caller that
	//    ignores the return value must leave its aircraft where it was, not at the origin.
	{
		FLandingRun Run;
		Run.Start(FVector2D::ZeroVector, FVector2D(1.0, 0.0), LongRunway, Ground, Climb, Approach);
		FlyLanding(Run);

		FVector2D At(12345.0, 6789.0);
		double Heading = 4.0;
		double Altitude = 999.0;
		double Pitch = 42.0;

		TestFalse(TEXT("a finished arrival declines to advance"),
			Run.Advance(1.0 / 60.0, At, Heading, Altitude, Pitch));
		TestEqual(TEXT("and leaves the caller's position untouched"), At, FVector2D(12345.0, 6789.0));
		TestEqual(TEXT("and its altitude"), Altitude, 999.0);
	}

	return true;
}

#endif
