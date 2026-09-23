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
	 * exists to catch. Both limits are measured on their own, in Airside.Model.TurnRate.
	 */
	FChassis HeadingTestUnlimitedChassis()
	{
		FChassis Chassis;
		Chassis.Ground.MaxTurnRateDegPerSec = 1.0e6;
		Chassis.Ground.Taxi.Accel = 1.0e9;
		Chassis.Ground.Taxi.Decel = 1.0e9;
		return Chassis;
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
	"Airside.Model.FollowerHeading",
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

		const FChassis Chassis = HeadingTestUnlimitedChassis();
		FRouteFollower Follower;
		Follower.Start(Plan, Chassis);

		// Sixty frames a second, which is the rate the jerk was actually seen at.
		constexpr double Frame = 1.0 / 60.0;

		double Worst = 0.0;
		double Previous = 0.0;
		bool bHavePrevious = false;

		while (!Follower.HasArrived())
		{
			FVector2D At;
			double Heading = 0.0;
			if (!Follower.Advance(Frame, Chassis, At, Heading))
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

/**
 * Pins GuidelineGeom::PointAtDistance's hinted overload (issue #190) against the plain one,
 * which stays the ORACLE - see the header - by construction, walking a LONG, mixed route
 * forward. FRouteFollower::Advance and FClaimPass::SampleBody both feed a checkpoint from one
 * call into the next; this proves that checkpoint never disagrees with asking fresh, which is
 * the one thing "results must be bitwise identical" needs of it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPointAtDistanceHintMatchesOracleTest,
	"Airside.Model.PointAtDistanceHintMatchesOracle",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPointAtDistanceHintMatchesOracleTest::RunTest(const FString& Parameters)
{
	// TWENTY ARCS WELDED END TO END - hundreds of vertices, mixing the curved interpolation
	// (VertexDirection's averaging) with the straight short-circuit (IsStraight), so the walk
	// crosses many span boundaries of both kinds, which is exactly where a checkpoint could
	// disagree with a fresh walk if the hint's carried-over Walked value were computed wrong.
	TArray<FVector2D> LongRoute;
	for (int32 Arc = 0; Arc < 20; ++Arc)
	{
		TArray<FVector2D> Piece = QuarterCircle(400.0 + Arc * 17.0, 16);
		const FVector2D Offset = LongRoute.Num() > 0 ? LongRoute.Last() : FVector2D::ZeroVector;
		for (FVector2D& Point : Piece)
		{
			Point += Offset;
		}
		if (LongRoute.Num() > 0)
		{
			// DROP THE SHARED VERTEX, the way a route welds consecutive edges into one
			// polyline (FRoutePlan::Polyline's own comment) - two coincident points would
			// hand PointAtDistance a zero-length span it has to skip, which this test does
			// not need to exercise on top of everything else it already covers.
			Piece.RemoveAt(0);
		}
		LongRoute.Append(Piece);
	}

	const double TotalLength = GuidelineGeom::PolylineLength(LongRoute);
	TestTrue(TEXT("the fixture route is actually long"), TotalLength > 10000.0);

	int32 HintVertex = 1;
	double HintWalked = 0.0;
	bool bAllMatch = true;
	FString Mismatch;

	// FAR FINER THAN THE VERTEX SPACING, so the sweep lands inside spans as often as it lands
	// on their boundaries - both are places a checkpoint could go wrong in a different way.
	//
	// AND THREE STEPS PAST THE END: an agent that arrives, or overshoots by a frame, keeps
	// asking after Distance > TotalLength, and the fallback that answers it checkpoints
	// differently from the main loop (Points.Num(), not the last span - see the header). A
	// sweep that stopped exactly at TotalLength would never re-enter PointAtDistance with the
	// hint already parked there, which is exactly where that checkpoint was once wrong.
	constexpr int32 Samples = 2000;
	constexpr int32 OvershootSteps = 3;
	for (int32 Step = 0; Step <= Samples + OvershootSteps && bAllMatch; ++Step)
	{
		const double Distance = Step <= Samples
			? TotalLength * (static_cast<double>(Step) / Samples)
			: TotalLength + (Step - Samples) * 137.0;

		FVector2D OraclePosition;
		double OracleHeading = 0.0;
		const bool bOracleOk = GuidelineGeom::PointAtDistance(LongRoute, Distance, OraclePosition, OracleHeading);

		FVector2D HintedPosition;
		double HintedHeading = 0.0;
		const bool bHintedOk = GuidelineGeom::PointAtDistance(
			LongRoute, Distance, HintedPosition, HintedHeading, HintVertex, HintWalked);

		if (bOracleOk != bHintedOk)
		{
			bAllMatch = false;
			Mismatch = FString::Printf(TEXT("at distance %.3f: oracle returned %d, hinted %d"),
				Distance, bOracleOk, bHintedOk);
			break;
		}

		// BITWISE, not nearly-equal (the two share one loop body - see the header - so a
		// real divergence is a checkpoint bug, not rounding that an epsilon would paper over).
		if (bOracleOk && (!OraclePosition.Equals(HintedPosition, 0.0) || OracleHeading != HintedHeading))
		{
			bAllMatch = false;
			Mismatch = FString::Printf(
				TEXT("at distance %.3f: oracle (%.9f,%.9f)/%.12f vs hinted (%.9f,%.9f)/%.12f"),
				Distance, OraclePosition.X, OraclePosition.Y, OracleHeading,
				HintedPosition.X, HintedPosition.Y, HintedHeading);
			break;
		}
	}

	TestTrue(FString::Printf(TEXT("the hint matches the oracle walking forward (%s)"), *Mismatch),
		bAllMatch);

	// THE CHECKPOINT ITSELF MUST NOT DRIFT PAST THE TRUE LENGTH. The "past the end" fallback
	// answers from Points.Last() regardless of Walked, so a Walked that crept upward on every
	// further overshoot call could hide behind a still-correct position for a while and only
	// misreport a nearer-the-end Distance much later - this catches the drift directly rather
	// than waiting for it to surface as a wrong point. See PointAtDistance's own comment on
	// why the fallback checkpoints at Points.Num(), not the last span.
	TestTrue(FString::Printf(TEXT("the hint never overshoots the true length (%.3f vs %.3f)"),
		HintWalked, TotalLength), HintWalked <= TotalLength + UE_DOUBLE_KINDA_SMALL_NUMBER);

	return true;
}

#endif
