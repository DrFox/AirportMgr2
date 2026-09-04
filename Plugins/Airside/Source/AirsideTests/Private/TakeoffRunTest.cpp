#include "CoreMinimal.h"
#include "Entities/AircraftType.h"
#include "Misc/AutomationTest.h"
#include "Model/TakeoffRun.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// Prefixed against the UNITY build - these test files share one translation unit.

	constexpr double TakeoffFrame = 1.0 / 60.0;

	/** A runway pointing due east, long enough for a Meridian with room to spare. */
	constexpr double TakeoffRunwayLength = 100000.0;   // 1 km
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTakeoffRunTest,
	"Airside.Model.TakeoffRun",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTakeoffRunTest::RunTest(const FString& Parameters)
{
	const FGroundPerformance Piper = UAircraftType::PiperMeridianGround();
	const FClimbPerformance Climb = UAircraftType::PiperMeridianClimb();

	if (!TestTrue(TEXT("the Meridian has take-off and climb performance"),
		Piper.Takeoff.IsSet() && Climb.IsSet()))
	{
		return false;
	}

	// 1. THE GROUND ROLL IS THE PUBLISHED ONE.
	//
	//    This is the number that says the aircraft is behaving like a Meridian rather than
	//    like a fast taxi. Rotation is 85 KIAS - 4400 uu/s - and the sea-level roll is about
	//    1000 ft, 30500 uu. Those two published figures give the acceleration, so measuring
	//    the roll back out of the simulation checks the arithmetic in both directions.
	{
		FTakeoffRun Run;
		const bool bArmed = Run.Start(FVector2D::ZeroVector, FVector2D(1.0, 0.0),
			TakeoffRunwayLength, Piper, Climb, /*InHeading=*/0.0);

		if (!TestTrue(TEXT("a 1 km runway takes a Meridian"), bArmed))
		{
			return false;
		}

		double RollDistance = -1.0;
		double WorstAccel = 0.0;

		// Seeded from the ARMED speed, not from zero. A departure starts already creeping at
		// MinTaxiSpeed - it taxied to the threshold - so measuring against zero reports the
		// arming itself as an 80 uu/s step and calls it a 4800 uu/s2 acceleration.
		double Previous = Run.Speed;

		for (int32 Frame = 0; Frame < 6000; ++Frame)
		{
			FVector2D At;
			double Heading = 0.0, Altitude = 0.0, Pitch = 0.0;
			if (!Run.Advance(TakeoffFrame, At, Heading, Altitude, Pitch))
			{
				break;
			}

			if (Run.Phase == ETakeoffPhase::Roll)
			{
				WorstAccel = FMath::Max(WorstAccel, (Run.Speed - Previous) / TakeoffFrame);
			}
			Previous = Run.Speed;

			if (RollDistance < 0.0 && Run.Phase == ETakeoffPhase::Rotate)
			{
				RollDistance = Run.Travelled;
			}
		}

		AddInfo(FString::Printf(TEXT("ground roll %.0f uu, published about 30500"), RollDistance));

		TestTrue(FString::Printf(
			TEXT("it rotates after about the published roll (%.0f uu)"), RollDistance),
			RollDistance > 27000.0 && RollDistance < 34000.0);

		TestTrue(FString::Printf(
			TEXT("and never accelerates harder than %.0f uu/s2 (worst %.0f)"),
			Piper.Takeoff.Accel, WorstAccel),
			WorstAccel <= Piper.Takeoff.Accel + 0.01);
	}

	// 2. IT ROTATES AT Vr, NOT BEFORE. The nose staying down past rotation speed, or coming
	//    up before it, are the two ways this looks wrong to anyone who has flown.
	{
		FTakeoffRun Run;
		Run.Start(FVector2D::ZeroVector, FVector2D(1.0, 0.0), TakeoffRunwayLength,
			Piper, Climb, 0.0);

		double SpeedAtRotation = -1.0;
		double PitchWhileRolling = 0.0;

		for (int32 Frame = 0; Frame < 6000; ++Frame)
		{
			FVector2D At;
			double Heading = 0.0, Altitude = 0.0, Pitch = 0.0;
			if (!Run.Advance(TakeoffFrame, At, Heading, Altitude, Pitch))
			{
				break;
			}

			if (Run.Phase == ETakeoffPhase::Roll)
			{
				PitchWhileRolling = FMath::Max(PitchWhileRolling, Pitch);
			}
			if (SpeedAtRotation < 0.0 && Run.Phase == ETakeoffPhase::Rotate)
			{
				SpeedAtRotation = Run.Speed;
			}
		}

		TestEqual(TEXT("the nose stays down for the whole roll"), PitchWhileRolling, 0.0);
		TestTrue(FString::Printf(
			TEXT("and comes up at Vr, %.0f uu/s (was %.0f)"),
			Piper.Takeoff.SpeedCap, SpeedAtRotation),
			SpeedAtRotation >= Piper.Takeoff.SpeedCap - 1.0);
	}

	// 3. IT LEAVES THE GROUND, CLIMBS AND GOES. The whole departure has to terminate: an
	//    agent that never reports done is an aircraft that never despawns.
	{
		FTakeoffRun Run;
		Run.Start(FVector2D::ZeroVector, FVector2D(1.0, 0.0), TakeoffRunwayLength,
			Piper, Climb, 0.0);

		double Elapsed = 0.0;
		double TopPitch = 0.0;
		double WorstClimb = 0.0;
		double PreviousAltitude = 0.0;
		FVector2D Last = FVector2D::ZeroVector;

		while (Elapsed < 300.0)
		{
			FVector2D At;
			double Heading = 0.0, Altitude = 0.0, Pitch = 0.0;
			if (!Run.Advance(TakeoffFrame, At, Heading, Altitude, Pitch))
			{
				break;
			}
			Elapsed += TakeoffFrame;
			Last = At;
			TopPitch = FMath::Max(TopPitch, Pitch);
			WorstClimb = FMath::Max(WorstClimb, (Altitude - PreviousAltitude) / TakeoffFrame);
			PreviousAltitude = Altitude;
		}

		TestTrue(FString::Printf(TEXT("the departure finishes (%.1f s)"), Elapsed),
			Run.HasCleared());

		TestTrue(FString::Printf(TEXT("holding the climb attitude (%.1f deg)"), TopPitch),
			FMath::IsNearlyEqual(TopPitch, Climb.ClimbPitchDegrees, 0.5));

		TestTrue(FString::Printf(
			TEXT("and never climbing faster than %.0f uu/s (worst %.0f)"),
			Climb.ClimbRate, WorstClimb),
			WorstClimb <= Climb.ClimbRate + 1.0);

		// STRAIGHT AHEAD. There are no turns in this departure, so anything off the runway
		// centreline is the direction being applied wrongly.
		TestTrue(FString::Printf(TEXT("it departs along the runway (off by %.1f uu)"),
			FMath::Abs(Last.Y)), FMath::Abs(Last.Y) < 1e-6);
		TestTrue(TEXT("and keeps going past the far end, being airborne"),
			Last.X > TakeoffRunwayLength);
	}

	// 4. IT LINES UP FIRST, at the airframe's turn rate. An aircraft that snapped onto the
	//    runway heading would throw away the whole of the turn-rate model one frame before
	//    the most-watched moment in the game.
	{
		FTakeoffRun Run;

		// Arriving at the threshold pointing back down the runway - a backtrack, which is
		// exactly how a light aircraft reaches the threshold of a runway it will depart from.
		Run.Start(FVector2D::ZeroVector, FVector2D(1.0, 0.0), TakeoffRunwayLength,
			Piper, Climb, /*InHeading=*/UE_DOUBLE_PI);

		double WorstYawRate = 0.0;
		double Previous = Run.Heading;
		double LineUpSeconds = 0.0;

		for (int32 Frame = 0; Frame < 6000 && Run.Phase == ETakeoffPhase::LineUp; ++Frame)
		{
			FVector2D At;
			double Heading = 0.0, Altitude = 0.0, Pitch = 0.0;
			Run.Advance(TakeoffFrame, At, Heading, Altitude, Pitch);
			LineUpSeconds += TakeoffFrame;

			WorstYawRate = FMath::Max(WorstYawRate, FMath::Abs(FMath::RadiansToDegrees(
				FMath::UnwindRadians(Heading - Previous))) / TakeoffFrame);
			Previous = Heading;
		}

		AddInfo(FString::Printf(TEXT("line-up took %.1f s for 180 degrees"), LineUpSeconds));

		TestTrue(FString::Printf(
			TEXT("it turns onto the runway at the airframe's %.0f deg/s (worst %.1f)"),
			Piper.MaxTurnRateDegPerSec, WorstYawRate),
			WorstYawRate <= Piper.MaxTurnRateDegPerSec + 0.01);

		TestEqual(TEXT("and only then rolls"), Run.Phase, ETakeoffPhase::Roll);
		TestEqual(TEXT("having not moved down the runway while turning"), Run.Travelled, 0.0);
	}

	// 5. A RUNWAY TOO SHORT IS REFUSED. v2/2a is a published figure, and a strip under it is
	//    one this aircraft cannot leave - rolling anyway would simulate an overrun.
	{
		const double Needed = FTakeoffRun::RequiredRoll(Piper);
		AddInfo(FString::Printf(TEXT("roll needed %.0f uu"), Needed));

		TestTrue(FString::Printf(TEXT("the needed roll matches the published 30500 (%.0f)"), Needed),
			Needed > 27000.0 && Needed < 34000.0);

		FTakeoffRun Short;
		TestFalse(TEXT("a runway shorter than the roll is refused"),
			Short.Start(FVector2D::ZeroVector, FVector2D(1.0, 0.0), Needed * 0.5,
				Piper, Climb, 0.0));
		TestTrue(TEXT("and the refused run reports nothing to fly"), Short.HasCleared());

		FTakeoffRun Long;
		TestTrue(TEXT("one just over it is taken"),
			Long.Start(FVector2D::ZeroVector, FVector2D(1.0, 0.0), Needed * 1.05,
				Piper, Climb, 0.0));
	}

	return true;
}

#endif
