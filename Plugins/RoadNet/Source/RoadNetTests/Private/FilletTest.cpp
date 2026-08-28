#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Solve/RoadGeom.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadFilletTest,
	"RoadNet.Solve.Fillet",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadFilletTest::RunTest(const FString& Parameters)
{
	constexpr double W = 1150.0;  // taxiway half-width

	// --- Convex corner: east segment's left edge vs north segment's right edge ---
	// East heads +X, so its left edge is the line y = +W.
	// North heads +Y, so its right edge is the line x = +W.
	FRay2D EastLeft;   EastLeft.Origin   = FVector2D(0.0, W); EastLeft.Dir   = FVector2D(1.0, 0.0);
	FRay2D NorthRight; NorthRight.Origin = FVector2D(W, 0.0); NorthRight.Dir = FVector2D(0.0, 1.0);

	{
		const RoadGeom::FFillet Inner = RoadGeom::SolveFillet(EastLeft, NorthRight, 500.0);
		TestTrue(TEXT("inner valid"), Inner.bValid);
		TestFalse(TEXT("inner not straight"), Inner.bStraightThrough);
		TestTrue(TEXT("inner theta 90deg"), FMath::IsNearlyEqual(Inner.Theta, UE_DOUBLE_PI * 0.5, 1e-9));
		TestTrue(TEXT("inner corner at (W,W)"), Inner.Corner.Equals(FVector2D(W, W), 1e-6));
		TestTrue(TEXT("inner d equals R"), FMath::IsNearlyEqual(Inner.Distance, 500.0, 1e-6));
		TestTrue(TEXT("inner tangent A"), Inner.TangentA.Equals(FVector2D(W - 500.0, W), 1e-6));
		TestTrue(TEXT("inner tangent B"), Inner.TangentB.Equals(FVector2D(W, W - 500.0), 1e-6));
		TestTrue(TEXT("inner centre"), Inner.Centre.Equals(FVector2D(W - 500.0, W - 500.0), 1e-6));
		TestTrue(TEXT("inner paramA non-negative"), Inner.ParamA >= -1e-9);
		TestTrue(TEXT("inner paramB non-negative"), Inner.ParamB >= -1e-9);

		// The centre must be exactly R from both tangent points, or it is not an arc.
		TestTrue(TEXT("inner radius to A"),
			FMath::IsNearlyEqual((Inner.TangentA - Inner.Centre).Length(), Inner.Radius, 1e-6));
		TestTrue(TEXT("inner radius to B"),
			FMath::IsNearlyEqual((Inner.TangentB - Inner.Centre).Length(), Inner.Radius, 1e-6));
	}

	// Convex clamping: asking for more than the half-width must reduce the radius to W.
	{
		const RoadGeom::FFillet Clamped = RoadGeom::SolveFillet(EastLeft, NorthRight, 5000.0);
		TestTrue(TEXT("convex radius clamped down"), FMath::IsNearlyEqual(Clamped.Radius, W, 1e-6));
		TestTrue(TEXT("clamped paramA is zero"), FMath::IsNearlyEqual(Clamped.ParamA, 0.0, 1e-6));
		TestTrue(TEXT("clamped paramB is zero"), FMath::IsNearlyEqual(Clamped.ParamB, 0.0, 1e-6));
	}

	// --- Reflex corner: north segment's left edge vs east segment's right edge ---
	// North heads +Y, so its left edge is the line x = -W.
	// East heads +X, so its right edge is the line y = -W.
	FRay2D NorthLeft; NorthLeft.Origin = FVector2D(-W, 0.0); NorthLeft.Dir = FVector2D(0.0, 1.0);
	FRay2D EastRight; EastRight.Origin = FVector2D(0.0, -W); EastRight.Dir = FVector2D(1.0, 0.0);

	{
		const RoadGeom::FFillet Outer = RoadGeom::SolveFillet(NorthLeft, EastRight, 3000.0);
		TestTrue(TEXT("outer valid"), Outer.bValid);
		TestFalse(TEXT("outer not straight"), Outer.bStraightThrough);
		TestTrue(TEXT("outer theta 270deg"), FMath::IsNearlyEqual(Outer.Theta, UE_DOUBLE_PI * 1.5, 1e-9));
		TestTrue(TEXT("outer corner at (-W,-W)"), Outer.Corner.Equals(FVector2D(-W, -W), 1e-6));

		// The sign flip is the whole point: d is negative past Theta = PI.
		TestTrue(TEXT("outer d is negative"), Outer.Distance < 0.0);
		TestTrue(TEXT("outer radius still positive"), Outer.Radius > 0.0);

		TestTrue(TEXT("outer tangent A"), Outer.TangentA.Equals(FVector2D(-W, -W + 3000.0), 1e-6));
		TestTrue(TEXT("outer tangent B"), Outer.TangentB.Equals(FVector2D(-W + 3000.0, -W), 1e-6));
		TestTrue(TEXT("outer centre"), Outer.Centre.Equals(FVector2D(-W + 3000.0, -W + 3000.0), 1e-6));
		TestTrue(TEXT("outer paramA non-negative"), Outer.ParamA >= -1e-9);
		TestTrue(TEXT("outer paramB non-negative"), Outer.ParamB >= -1e-9);
		TestTrue(TEXT("outer radius to A"),
			FMath::IsNearlyEqual((Outer.TangentA - Outer.Centre).Length(), Outer.Radius, 1e-6));
	}

	// Reflex clamping runs the OTHER way: asking for less than the half-width raises it.
	{
		const RoadGeom::FFillet Raised = RoadGeom::SolveFillet(NorthLeft, EastRight, 100.0);
		TestTrue(TEXT("reflex radius raised up"), FMath::IsNearlyEqual(Raised.Radius, W, 1e-6));
		TestTrue(TEXT("raised paramA is zero"), FMath::IsNearlyEqual(Raised.ParamA, 0.0, 1e-6));
		TestTrue(TEXT("raised paramB is zero"), FMath::IsNearlyEqual(Raised.ParamB, 0.0, 1e-6));
	}

	// --- Collinear: the COMMON case once long drags auto-subdivide (R9) ---
	{
		FRay2D WestLeft; WestLeft.Origin = FVector2D(0.0, -W); WestLeft.Dir = FVector2D(-1.0, 0.0);
		const RoadGeom::FFillet Straight = RoadGeom::SolveFillet(EastLeft, WestLeft, 1500.0);
		TestTrue(TEXT("collinear valid"), Straight.bValid);
		TestTrue(TEXT("collinear is straight-through"), Straight.bStraightThrough);
		TestTrue(TEXT("collinear radius is zero"), FMath::IsNearlyEqual(Straight.Radius, 0.0, 1e-9));
	}

	// --- Invariants across a full sweep of corner angles ---
	// A radius must never come out negative, and a tangent point must never land
	// behind its edge's origin, at ANY angle. These two properties are what the
	// junction boundary walk depends on.
	{
		for (int32 Step = 1; Step < 360; ++Step)
		{
			const double Angle = 2.0 * UE_DOUBLE_PI * static_cast<double>(Step) / 360.0;

			FRay2D EdgeA; EdgeA.Origin = FVector2D(0.0, W); EdgeA.Dir = FVector2D(1.0, 0.0);
			FRay2D EdgeB;
			EdgeB.Dir = FVector2D(FMath::Cos(Angle), FMath::Sin(Angle));
			EdgeB.Origin = -RoadGeom::PerpCCW(EdgeB.Dir) * W;

			const RoadGeom::FFillet Swept = RoadGeom::SolveFillet(EdgeA, EdgeB, 1500.0);

			TestTrue(TEXT("sweep always resolves"), Swept.bValid);
			if (Swept.bStraightThrough)
			{
				continue;
			}

			TestTrue(TEXT("sweep radius never negative"), Swept.Radius >= -1e-9);
			TestTrue(TEXT("sweep paramA never negative"), Swept.ParamA >= -1e-6);
			TestTrue(TEXT("sweep paramB never negative"), Swept.ParamB >= -1e-6);
			TestTrue(TEXT("sweep radius finite"), FMath::IsFinite(Swept.Radius));
			TestTrue(TEXT("sweep centre finite"),
				FMath::IsFinite(Swept.Centre.X) && FMath::IsFinite(Swept.Centre.Y));

			// The centre is equidistant from both tangent points, by definition.
			TestTrue(TEXT("sweep centre equidistant"),
				FMath::IsNearlyEqual((Swept.TangentA - Swept.Centre).Length(),
				                     (Swept.TangentB - Swept.Centre).Length(), 1e-5));
		}
	}

	// --- Arc sampling lands exactly on the tangent points ---
	{
		const RoadGeom::FFillet Inner = RoadGeom::SolveFillet(EastLeft, NorthRight, 500.0);
		TArray<FVector2D> Arc;
		RoadGeom::SampleArc(Inner, 8, Arc);

		TestEqual(TEXT("arc point count"), Arc.Num(), 9);
		TestTrue(TEXT("arc starts at tangent A"), Arc[0].Equals(Inner.TangentA, 1e-6));
		TestTrue(TEXT("arc ends at tangent B"), Arc.Last().Equals(Inner.TangentB, 1e-6));
		for (const FVector2D& Point : Arc)
		{
			TestTrue(TEXT("arc point lies on the circle"),
				FMath::IsNearlyEqual((Point - Inner.Centre).Length(), Inner.Radius, 1e-6));
		}
	}

	// A reflex arc must sample the SHORT way round, not the long way.
	{
		const RoadGeom::FFillet Outer = RoadGeom::SolveFillet(NorthLeft, EastRight, 3000.0);
		TArray<FVector2D> Arc;
		RoadGeom::SampleArc(Outer, 8, Arc);

		TestEqual(TEXT("reflex arc point count"), Arc.Num(), 9);
		TestTrue(TEXT("reflex arc starts at tangent A"), Arc[0].Equals(Outer.TangentA, 1e-6));
		TestTrue(TEXT("reflex arc ends at tangent B"), Arc.Last().Equals(Outer.TangentB, 1e-6));

		// A quarter-circle of radius 3000 has chord-to-chord length well under the
		// three-quarter arc it would trace if the sweep wrapped the wrong way.
		double Traced = 0.0;
		for (int32 Index = 1; Index < Arc.Num(); ++Index)
		{
			Traced += (Arc[Index] - Arc[Index - 1]).Length();
		}
		const double QuarterArc = 0.5 * UE_DOUBLE_PI * Outer.Radius;
		TestTrue(TEXT("reflex arc takes the short way"), Traced < QuarterArc * 1.05);
	}

	// Straight-through fillets must produce no arc points at all.
	{
		FRay2D WestLeft; WestLeft.Origin = FVector2D(0.0, -W); WestLeft.Dir = FVector2D(-1.0, 0.0);
		const RoadGeom::FFillet Straight = RoadGeom::SolveFillet(EastLeft, WestLeft, 1500.0);
		TArray<FVector2D> Arc;
		RoadGeom::SampleArc(Straight, 8, Arc);
		TestEqual(TEXT("straight-through yields no arc"), Arc.Num(), 0);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
