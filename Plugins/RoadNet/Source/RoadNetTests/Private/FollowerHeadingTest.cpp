#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RouteFollower.h"
#include "Model/RouteSearch.h"
#include "Solve/GuidelineGeom.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// Prefixed against the UNITY build - these test files share one translation unit.

	/** Signed shortest angle between two headings, in degrees. */
	double HeadingDeltaDegrees(double FromRadians, double ToRadians)
	{
		return FMath::Abs(FMath::RadiansToDegrees(
			FMath::UnwindRadians(ToRadians - FromRadians)));
	}

	/**
	 * Ground performance with every limit deliberately WIDE OPEN.
	 *
	 * This test measures the HEADING FUNCTION - that PointAtDistance interpolates across a
	 * span instead of holding the segment's own direction. A turn-rate limit would smooth
	 * that staircase back out, and an acceleration limit would creep the agent so slowly
	 * that no span produced a visible step: either way it would pass on the very bug it
	 * exists to catch. Both limits are measured on their own, in RoadNet.Model.TurnRate.
	 */
	FGroundPerformance HeadingTestUnlimitedGround()
	{
		FGroundPerformance Ground;
		Ground.MaxTurnRateDegPerSec = 1.0e6;
		Ground.Taxi.Accel = 1.0e9;
		Ground.Taxi.Decel = 1.0e9;
		return Ground;
	}

	/** A quarter circle of radius R, as the sampled polyline a swept lead-in produces. */
	TArray<FVector2D> QuarterCircle(double Radius, int32 Samples)
	{
		TArray<FVector2D> Points;
		for (int32 Step = 0; Step < Samples; ++Step)
		{
			const double T = static_cast<double>(Step) / (Samples - 1);
			const double Angle = T * UE_DOUBLE_PI * 0.5;
			Points.Emplace(Radius * FMath::Sin(Angle), Radius * (1.0 - FMath::Cos(Angle)));
		}
		return Points;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFollowerHeadingTest,
	"RoadNet.Model.FollowerHeading",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFollowerHeadingTest::RunTest(const FString& Parameters)
{
	// 1. THE MEASUREMENT. Walking a curve in small steps must turn the agent smoothly.
	//
	//    Heading used to be the direction of the whole polyline SEGMENT the agent stood in:
	//    constant across the segment, then a jump at the vertex. On a straight route every
	//    segment is parallel and nothing shows; on a 90 degree sweep sampled 16 times it is
	//    fifteen jumps of six degrees, which is what a Piper rounding a corner looked like.
	{
		const TArray<FVector2D> Curve = QuarterCircle(2500.0, 16);
		const double Length = GuidelineGeom::PolylineLength(Curve);

		// Far finer than the sampling, so the test is asking about the HEADING FUNCTION
		// rather than about the samples: a step per sample would step vertex to vertex and
		// see the same six degrees whether or not it interpolates.
		constexpr int32 Steps = 400;

		double Worst = 0.0;
		double Previous = 0.0;
		bool bHavePrevious = false;

		for (int32 Step = 0; Step <= Steps; ++Step)
		{
			const double Distance = Length * (static_cast<double>(Step) / Steps);

			FVector2D At;
			double Heading = 0.0;
			if (!GuidelineGeom::PointAtDistance(Curve, Distance, At, Heading))
			{
				continue;
			}

			if (bHavePrevious)
			{
				Worst = FMath::Max(Worst, HeadingDeltaDegrees(Previous, Heading));
			}
			Previous = Heading;
			bHavePrevious = true;
		}

		// A quarter turn over 400 steps is 0.225 degrees a step if it is spread evenly.
		// One degree leaves room for the ends without admitting a six degree jump.
		TestTrue(FString::Printf(
			TEXT("heading turns smoothly along a curve (worst step %.2f deg)"), Worst),
			Worst < 1.0);
	}

	// 2. A straight line still reports exactly its own direction. The interpolation must
	//    not introduce wobble where there is no curvature - most of an airport is straight.
	{
		const TArray<FVector2D> Line = { FVector2D(0.0, 0.0), FVector2D(1000.0, 0.0),
			FVector2D(2000.0, 0.0) };

		for (double Distance = 0.0; Distance <= 2000.0; Distance += 100.0)
		{
			FVector2D At;
			double Heading = 0.0;
			if (GuidelineGeom::PointAtDistance(Line, Distance, At, Heading))
			{
				TestTrue(TEXT("a straight polyline reports a constant heading"),
					HeadingDeltaDegrees(Heading, 0.0) < 1e-6);
			}
		}
	}

	// 3. Position is untouched. Only the heading was ever wrong, and a "fix" that moved the
	//    agent off the line would break the contract that it walks what the overlay drew.
	{
		const TArray<FVector2D> Line = { FVector2D(0.0, 0.0), FVector2D(1000.0, 0.0) };

		FVector2D At;
		double Heading = 0.0;
		if (TestTrue(TEXT("a point resolves mid-segment"),
			GuidelineGeom::PointAtDistance(Line, 250.0, At, Heading)))
		{
			TestEqual(TEXT("position is still the exact distance along"), At.X, 250.0);
			TestEqual(TEXT("and stays on the line"), At.Y, 0.0);
		}
	}

	// 3b. A REAL corner is NOT smoothed, and that is the distinction the whole thing rests
	//     on. Smoothing a sampled vertex recovers the tangent the samples approximate;
	//     smoothing a genuine corner would have the agent facing 22 degrees off its
	//     direction of travel through the turn - an aircraft crabbing down the taxiway,
	//     which is worse than the snap it replaced.
	//
	//     Told apart by how sharply the vertex bends: Sample() cannot produce more than
	//     about 12 degrees at one, so anything sharper was meant.
	{
		const TArray<FVector2D> Corner = { FVector2D(0.0, 0.0), FVector2D(1000.0, 0.0),
			FVector2D(1000.0, 1000.0) };

		FVector2D At;
		double Heading = 0.0;

		// Half way up the second leg: travelling due north, and facing due north.
		if (GuidelineGeom::PointAtDistance(Corner, 1500.0, At, Heading))
		{
			TestTrue(TEXT("through a real corner, facing is still the direction of travel"),
				HeadingDeltaDegrees(Heading, UE_DOUBLE_PI * 0.5) < 1e-6);
		}

		// And half way along the first: due east, not already turning.
		if (GuidelineGeom::PointAtDistance(Corner, 500.0, At, Heading))
		{
			TestTrue(TEXT("and it does not start turning before it gets there"),
				HeadingDeltaDegrees(Heading, 0.0) < 1e-6);
		}
	}

	// 4. The follower carries it through. The agent is what the player watches, so assert
	//    the property where it is actually consumed rather than only one level down.
	{
		FRoutePlan Plan;
		Plan.Result = ERouteResult::Found;
		Plan.Polyline = QuarterCircle(2500.0, 16);
		Plan.Length = GuidelineGeom::PolylineLength(Plan.Polyline);

		FRouteFollower Follower;
		Follower.Start(Plan, HeadingTestUnlimitedGround());

		// Sixty frames a second, which is the rate the jerk was actually seen at.
		constexpr double Frame = 1.0 / 60.0;

		double Worst = 0.0;
		double Previous = 0.0;
		bool bHavePrevious = false;

		while (!Follower.HasArrived())
		{
			FVector2D At;
			double Heading = 0.0;
			if (!Follower.Advance(Frame, At, Heading))
			{
				break;
			}

			if (bHavePrevious)
			{
				Worst = FMath::Max(Worst, HeadingDeltaDegrees(Previous, Heading));
			}
			Previous = Heading;
			bHavePrevious = true;
		}

		// At 10 m/s a 2500 uu quarter turn takes about four seconds, so a smooth turn is
		// well under a degree a frame. The old behaviour put six degrees into single frames.
		TestTrue(FString::Printf(
			TEXT("an agent turns smoothly frame to frame (worst %.2f deg)"), Worst),
			Worst < 2.0);
	}

	return true;
}

#endif
