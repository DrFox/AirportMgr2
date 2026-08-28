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

	// Edges of a node at the origin with an east arm and a north arm.
	// East heads +X, so its left edge is y = +W and its right edge is y = -W.
	// North heads +Y, so its left edge is x = -W and its right edge is x = +W.
	FRay2D EastLeft;   EastLeft.Origin   = FVector2D(0.0,  W);   EastLeft.Dir   = FVector2D( 1.0, 0.0);
	FRay2D EastRight;  EastRight.Origin  = FVector2D(0.0, -W);   EastRight.Dir  = FVector2D( 1.0, 0.0);
	FRay2D NorthLeft;  NorthLeft.Origin  = FVector2D(-W,  0.0);  NorthLeft.Dir  = FVector2D( 0.0, 1.0);
	FRay2D NorthRight; NorthRight.Origin = FVector2D( W,  0.0);  NorthRight.Dir = FVector2D( 0.0, 1.0);

	// --- Convex corner: the inside of the bend, Theta = 90 degrees ---
	{
		const RoadGeom::FFillet Inner = RoadGeom::SolveFillet(EastLeft, NorthRight, 500.0);

		TestTrue(TEXT("inner valid"), Inner.bValid);
		TestFalse(TEXT("inner not straight"), Inner.bStraightThrough);
		TestTrue(TEXT("inner theta 90deg"), FMath::IsNearlyEqual(Inner.Theta, UE_DOUBLE_PI * 0.5, 1e-9));
		TestTrue(TEXT("inner corner at (W,W)"), Inner.Corner.Equals(FVector2D(W, W), 1e-6));
		TestTrue(TEXT("inner magnitude equals R at 90deg"), FMath::IsNearlyEqual(Inner.Distance, 500.0, 1e-6));

		// Tangent points sit OUTWARD from the corner, never toward the node.
		TestTrue(TEXT("inner tangent A"), Inner.TangentA.Equals(FVector2D(W + 500.0, W), 1e-6));
		TestTrue(TEXT("inner tangent B"), Inner.TangentB.Equals(FVector2D(W, W + 500.0), 1e-6));
		TestTrue(TEXT("inner centre"), Inner.Centre.Equals(FVector2D(W + 500.0, W + 500.0), 1e-6));

		TestTrue(TEXT("inner radius to A"),
			FMath::IsNearlyEqual((Inner.TangentA - Inner.Centre).Length(), 500.0, 1e-6));
		TestTrue(TEXT("inner radius to B"),
			FMath::IsNearlyEqual((Inner.TangentB - Inner.Centre).Length(), 500.0, 1e-6));

		// The cut this corner demands: past the corner point by the fillet's reach.
		TestTrue(TEXT("inner paramA"), FMath::IsNearlyEqual(Inner.ParamA, W + 500.0, 1e-6));
		TestTrue(TEXT("inner paramB"), FMath::IsNearlyEqual(Inner.ParamB, W + 500.0, 1e-6));
	}

	// A bigger radius pushes the cut FURTHER back. It is never clamped down: a fillet
	// cannot be carved out of the corner without making the two arms overlap.
	{
		const RoadGeom::FFillet Small = RoadGeom::SolveFillet(EastLeft, NorthRight, 500.0);
		const RoadGeom::FFillet Large = RoadGeom::SolveFillet(EastLeft, NorthRight, 5000.0);

		TestTrue(TEXT("large radius preserved"), FMath::IsNearlyEqual(Large.Radius, 5000.0, 1e-6));
		TestTrue(TEXT("larger radius means a later cut"), Large.ParamA > Small.ParamA);
		TestTrue(TEXT("large paramA"), FMath::IsNearlyEqual(Large.ParamA, W + 5000.0, 1e-6));
		TestTrue(TEXT("large radius to A"),
			FMath::IsNearlyEqual((Large.TangentA - Large.Centre).Length(), 5000.0, 1e-6));
	}

	// --- Reflex corner: the outside of the same bend, Theta = 270 degrees ---
	{
		const RoadGeom::FFillet Outer = RoadGeom::SolveFillet(NorthLeft, EastRight, 3000.0);

		TestTrue(TEXT("outer valid"), Outer.bValid);
		TestFalse(TEXT("outer not straight"), Outer.bStraightThrough);
		TestTrue(TEXT("outer theta 270deg"), FMath::IsNearlyEqual(Outer.Theta, UE_DOUBLE_PI * 1.5, 1e-9));
		TestTrue(TEXT("outer corner at (-W,-W)"), Outer.Corner.Equals(FVector2D(-W, -W), 1e-6));

		// |R/tan| is taken, so the reach is positive on both sides of the bend.
		TestTrue(TEXT("outer magnitude positive"), Outer.Distance > 0.0);
		TestTrue(TEXT("outer magnitude equals R at 270deg"),
			FMath::IsNearlyEqual(Outer.Distance, 3000.0, 1e-6));

		TestTrue(TEXT("outer tangent A"), Outer.TangentA.Equals(FVector2D(-W, -W + 3000.0), 1e-6));
		TestTrue(TEXT("outer tangent B"), Outer.TangentB.Equals(FVector2D(-W + 3000.0, -W), 1e-6));

		// The centre flips to the OTHER side of edge A. This single sign is the whole
		// difference between rounding the inside and the outside of a bend.
		TestTrue(TEXT("outer centre"),
			Outer.Centre.Equals(FVector2D(-W + 3000.0, -W + 3000.0), 1e-6));

		TestTrue(TEXT("outer radius to A"),
			FMath::IsNearlyEqual((Outer.TangentA - Outer.Centre).Length(), 3000.0, 1e-6));
		TestTrue(TEXT("outer radius to B"),
			FMath::IsNearlyEqual((Outer.TangentB - Outer.Centre).Length(), 3000.0, 1e-6));

		// The reflex corner point sits BEHIND the node along both edges, so its
		// contribution to the cut is reach + magnitude and may be small or negative.
		TestTrue(TEXT("outer paramA"), FMath::IsNearlyEqual(Outer.ParamA, -W + 3000.0, 1e-6));
	}

	// A small radius on a reflex corner yields a negative parameter; the cut clamps it.
	{
		const RoadGeom::FFillet Tight = RoadGeom::SolveFillet(NorthLeft, EastRight, 100.0);
		TestTrue(TEXT("tight reflex valid"), Tight.bValid);
		TestTrue(TEXT("tight reflex paramA negative"), Tight.ParamA < 0.0);
		TestTrue(TEXT("tight reflex radius honoured"),
			FMath::IsNearlyEqual((Tight.TangentA - Tight.Centre).Length(), 100.0, 1e-6));
	}

	// --- Collinear: the COMMON case once long drags auto-subdivide (R9) ---
	{
		FRay2D WestLeft; WestLeft.Origin = FVector2D(0.0, -W); WestLeft.Dir = FVector2D(-1.0, 0.0);
		const RoadGeom::FFillet Straight = RoadGeom::SolveFillet(EastLeft, WestLeft, 1500.0);

		TestTrue(TEXT("collinear valid"), Straight.bValid);
		TestTrue(TEXT("collinear is straight-through"), Straight.bStraightThrough);
		TestTrue(TEXT("collinear contributes no cut"), FMath::IsNearlyEqual(Straight.ParamA, 0.0, 1e-9));
	}

	// --- Invariants across a full sweep of corner angles ---
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

			TestTrue(TEXT("sweep magnitude non-negative"), Swept.Distance >= 0.0);
			TestTrue(TEXT("sweep magnitude finite"), FMath::IsFinite(Swept.Distance));
			TestTrue(TEXT("sweep centre finite"),
				FMath::IsFinite(Swept.Centre.X) && FMath::IsFinite(Swept.Centre.Y));

			// The defining property of a fillet: the centre is exactly R from both
			// tangent points. If this ever fails the arc is not tangent to the edges,
			// and the junction boundary will kink where the arc meets the cut.
			TestTrue(TEXT("sweep tangent to edge A"),
				FMath::IsNearlyEqual((Swept.TangentA - Swept.Centre).Length(), 1500.0, 1e-5));
			TestTrue(TEXT("sweep tangent to edge B"),
				FMath::IsNearlyEqual((Swept.TangentB - Swept.Centre).Length(), 1500.0, 1e-5));

			// Tangent points always lie outward along their own edge from the corner.
			TestTrue(TEXT("sweep tangent A is outward"),
				FVector2D::DotProduct(Swept.TangentA - Swept.Corner, EdgeA.Dir) >= -1e-6);
			TestTrue(TEXT("sweep tangent B is outward"),
				FVector2D::DotProduct(Swept.TangentB - Swept.Corner, EdgeB.Dir) >= -1e-6);
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
				FMath::IsNearlyEqual((Point - Inner.Centre).Length(), 500.0, 1e-6));
		}

		// The arc must bulge toward the corner it is rounding, not away from it.
		const FVector2D Mid = Arc[Arc.Num() / 2];
		TestTrue(TEXT("arc bulges toward the corner"),
			(Mid - Inner.Corner).Length() < (Inner.Centre - Inner.Corner).Length());
	}

	// A reflex arc must sample the SHORT way round, not the long way.
	{
		const RoadGeom::FFillet Outer = RoadGeom::SolveFillet(NorthLeft, EastRight, 3000.0);
		TArray<FVector2D> Arc;
		RoadGeom::SampleArc(Outer, 8, Arc);

		TestEqual(TEXT("reflex arc point count"), Arc.Num(), 9);
		TestTrue(TEXT("reflex arc starts at tangent A"), Arc[0].Equals(Outer.TangentA, 1e-6));
		TestTrue(TEXT("reflex arc ends at tangent B"), Arc.Last().Equals(Outer.TangentB, 1e-6));

		double Traced = 0.0;
		for (int32 Index = 1; Index < Arc.Num(); ++Index)
		{
			Traced += (Arc[Index] - Arc[Index - 1]).Length();
		}
		const double QuarterArc = 0.5 * UE_DOUBLE_PI * 3000.0;
		TestTrue(TEXT("reflex arc takes the short way"), Traced < QuarterArc * 1.05);

		TestTrue(TEXT("reflex arc bulges toward the corner"),
			(Arc[Arc.Num() / 2] - Outer.Corner).Length() < (Outer.Centre - Outer.Corner).Length());
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
