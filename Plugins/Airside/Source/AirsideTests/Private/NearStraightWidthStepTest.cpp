#include "CoreMinimal.h"
#include "Build/RoadNetworkSolver.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Profiles/RoadProfile.h"
#include "Solve/JunctionSolver.h"
#include "Solve/RoadGeom.h"
#include "Tool/RoadPlacement.h"
#include "Tool/RoadSnap.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * A WIDTH STEP A HAIR OFF STRAIGHT IS NOT A CORNER. Reported from the editor mode on
 * M_ScaleGatwick, 2026-10-02: a 12 m taxiway drawn on from the end of a wider one, by hand and
 * so a fraction of a degree off its line, refused "too short to hold the corner" - where there
 * is no corner. The log said what the corner "needed":
 *
 *     LogRoadSolve: Warning: Node 0: segment 18 is 7433 uu long but its two corners need
 *     115107 + 1308 even with no fillet.
 *
 * 1.15 km of cut for a road that turns by a third of a degree. The two arms' inner edges are
 * a width step apart and nearly parallel, so the LINES through them meet dHalf / delta away -
 * and that meeting is on one arm's backward extension, behind the node, where nothing is paved.
 * It is no corner of the pavement, so it is not solved as one: both arms are cut as a bend at the
 * wider width (RoadGeom::StepReach, a few uu) and joined straight. #172 and its 2026-09-24
 * sequel fixed this at EXACTLY
 * straight (StraightThroughCornerTest.cpp) - a guide lands a click there, a hand does not.
 *
 * One test per layer, as there: the arithmetic, the junction that draws it, the validator the
 * player met, and the whole network solve that logged the warning.
 */

namespace NearStraightWidthStep
{
	// A Code-C-ish wide taxiway against the 12 m one the tool's first width lays.
	constexpr double WideHalf = 1250.0;
	constexpr double NarrowHalf = 600.0;

	/** Off straight by these, degrees: the reported third of a degree, then two and five. */
	const double OffDegrees[] = { 0.3, 2.0, 5.0 };

	FJunctionArm Arm(const FVector2D& Tangent, double Half)
	{
		FJunctionArm Out;
		Out.Tangent = Tangent;
		Out.HalfWidthLeft = Half;
		Out.HalfWidthRight = Half;
		Out.FilletRadius = URoadProfile::StandardTaxiwayFilletRadius;
		Out.MaxCutDistance = TNumericLimits<double>::Max();
		return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWidthStepNearStraightReachTest,
	"Airside.Solve.WidthStepNearStraight.ReachIsBounded",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FWidthStepNearStraightReachTest::RunTest(const FString& Parameters)
{
	using namespace NearStraightWidthStep;

	for (const double Degrees : OffDegrees)
	{
		const double Delta = FMath::DegreesToRadians(Degrees);
		const double Theta = UE_DOUBLE_PI - Delta;

		double AlongWide = -1.0;
		double AlongNarrow = -1.0;
		if (!TestTrue(FString::Printf(TEXT("%.1f deg off: the corner solves"), Degrees),
			RoadGeom::CornerReachAtZeroRadius(WideHalf, NarrowHalf, Theta, AlongWide, AlongNarrow)))
		{
			continue;
		}
		// A BEND AT THE WIDER WIDTH: WideHalf * tan(delta / 2) on both arms - 3.3 uu at a third of a
		// degree, where the intersection formula said ~124000 (measured on the unfixed build).
		const double Step = WideHalf * FMath::Tan(Delta * 0.5);
		TestTrue(FString::Printf(TEXT("%.1f deg off: both arms are cut as a bend at the wider width (%.3f / %.3f, expected %.3f)"),
			Degrees, AlongWide, AlongNarrow, Step),
			FMath::IsNearlyEqual(AlongWide, Step, 1.0e-6) && FMath::IsNearlyEqual(AlongNarrow, Step, 1.0e-6));

		// SYMMETRIC IN ITS ARGUMENTS: the same corner named the other way round.
		double SwappedNarrow = -1.0;
		double SwappedWide = -1.0;
		RoadGeom::CornerReachAtZeroRadius(NarrowHalf, WideHalf, Theta, SwappedNarrow, SwappedWide);
		TestTrue(FString::Printf(TEXT("%.1f deg off: and named narrow-first it says the same"), Degrees),
			SwappedWide == AlongWide && SwappedNarrow == AlongNarrow);
	}

	// A REAL BEND IS UNTOUCHED. Thirty degrees off straight is outside the step window, and
	// still the closed-form intersection: the control that this moved only the near-straight end.
	{
		const double Theta = UE_DOUBLE_PI - FMath::DegreesToRadians(30.0);
		double AlongWide = -1.0;
		double AlongNarrow = -1.0;
		RoadGeom::CornerReachAtZeroRadius(WideHalf, NarrowHalf, Theta, AlongWide, AlongNarrow);
		const double ExpectedNarrow = (WideHalf + NarrowHalf * FMath::Cos(Theta)) / FMath::Sin(Theta);
		TestTrue(FString::Printf(TEXT("30 deg off is still a corner: narrow reach %.1f, the intersection's %.1f"), AlongNarrow, ExpectedNarrow),
			FMath::IsNearlyEqual(AlongNarrow, ExpectedNarrow, 1.0e-6));
	}

	// AND EQUAL WIDTHS A HAIR OFF STRAIGHT KEEP THEIR HAIR OF A CORNER (w * tan(delta / 2)): every
	// chained road makes these, and they were never the problem.
	{
		const double Delta = FMath::DegreesToRadians(2.0);
		double AlongA = -1.0;
		double AlongB = -1.0;
		RoadGeom::CornerReachAtZeroRadius(NarrowHalf, NarrowHalf, UE_DOUBLE_PI - Delta, AlongA, AlongB);
		const double Expected = NarrowHalf * FMath::Tan(Delta * 0.5);
		TestTrue(FString::Printf(TEXT("equal widths 2 deg off: %.4f / %.4f, w tan(d/2) = %.4f"), AlongA, AlongB, Expected),
			FMath::IsNearlyEqual(AlongA, Expected, 1.0e-6) && FMath::IsNearlyEqual(AlongB, Expected, 1.0e-6));
	}

	return true;
}

/**
 * THE JUNCTION DRAWS THE STEP. Through FJunctionSolver, which is what both the solver's
 * zero-radius floor (the warning) and the mesh read: bounded cuts, a rim that fans, every cut
 * vertex on the rim bitwise (the weld contract), and the narrow arm's pavement clear of the
 * wide arm's - measured, not named.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWidthStepNearStraightJunctionTest,
	"Airside.Solve.WidthStepNearStraight.JunctionIsAStep",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FWidthStepNearStraightJunctionTest::RunTest(const FString& Parameters)
{
	using namespace NearStraightWidthStep;

	for (const double Degrees : OffDegrees)
	{
		// Both ways round: the bend's inside on the left, then on the right.
		for (const double Sign : { 1.0, -1.0 })
		{
			const double Delta = FMath::DegreesToRadians(Degrees) * Sign;
			const FVector2D WideDir(1.0, 0.0);
			const FVector2D NarrowDir = RoadGeom::Rotate(FVector2D(-1.0, 0.0), -Delta);

			FJunctionInput Input;
			Input.Position = FVector2D(1234.5, -678.9);
			Input.ArcSegments = 8;
			// CCW bearing order, as the solver is given them.
			const bool bWideFirst = RoadGeom::Bearing(WideDir) < RoadGeom::Bearing(NarrowDir);
			Input.Arms.Add(Arm(bWideFirst ? WideDir : NarrowDir, bWideFirst ? WideHalf : NarrowHalf));
			Input.Arms.Add(Arm(bWideFirst ? NarrowDir : WideDir, bWideFirst ? NarrowHalf : WideHalf));
			const int32 Wide = bWideFirst ? 0 : 1;
			const int32 Narrow = 1 - Wide;

			FJunctionResult Result = FJunctionSolver::SolveCuts(Input);
			FJunctionSolver::SolveBoundary(Input, Result);
			const FString Case = FString::Printf(TEXT("%+.1f deg off"), Degrees * Sign);
			if (!TestTrue(FString::Printf(TEXT("%s: the node solves"), *Case), Result.bValid))
			{
				continue;
			}

			// BOUNDED: under a narrow half-width on either arm, where the intersection was kilometres.
			TestTrue(FString::Printf(TEXT("%s: cuts %.2f (wide) and %.2f (narrow) are under a half-width, not dHalf / delta"),
				*Case, Result.Arms[Wide].CutDistance, Result.Arms[Narrow].CutDistance),
				Result.Arms[Wide].CutDistance < NarrowHalf && Result.Arms[Narrow].CutDistance < NarrowHalf);

			// NO OVERLAP: each arm's cut vertices are on or behind the OTHER arm's cut line, so neither
			// ribbon paves over the other's. Measured both ways round.
			for (const int32 Self : { Wide, Narrow })
			{
				const int32 Other = 1 - Self;
				const FVector2D OtherDir = Input.Arms[Other].Tangent;
				for (const FVector2D& Cut : { Result.Arms[Self].LeftCut, Result.Arms[Self].RightCut })
				{
					const double Into = FVector2D::DotProduct(Cut - Input.Position, OtherDir) - Result.Arms[Other].CutDistance;
					TestTrue(FString::Printf(TEXT("%s: a %s cut vertex sits %.6f uu past the %s arm's cut line (<= 0)"),
						*Case, Self == Wide ? TEXT("wide") : TEXT("narrow"), Into, Self == Wide ? TEXT("narrow") : TEXT("wide")),
						Into <= 1.0e-6);
				}
			}

			// THE WELD: every cut vertex is a rim vertex, bitwise.
			const TArray<FVector2D> Rim(Result.Boundary.GetData(), FMath::Max(0, Result.Boundary.Num() - 1));
			for (const FJunctionArmResult& ArmResult : Result.Arms)
			{
				TestTrue(FString::Printf(TEXT("%s: the left cut vertex is on the rim exactly"), *Case), Rim.Contains(ArmResult.LeftCut));
				TestTrue(FString::Printf(TEXT("%s: the right cut vertex is on the rim exactly"), *Case), Rim.Contains(ArmResult.RightCut));
			}

			// IT DRAWS: a fan, every triangle counter-clockwise, and no arc swinging out to the far
			// intersection - the rim stays within the two arms' cut lines.
			TestTrue(FString::Printf(TEXT("%s: the step is paved (triangles emitted)"), *Case), Result.Triangles.Num() > 0);
			int32 Clockwise = 0;
			for (int32 Slot = 0; Slot + 2 < Result.Triangles.Num(); Slot += 3)
			{
				const FVector2D& P0 = Result.Boundary[Result.Triangles[Slot]];
				const FVector2D& P1 = Result.Boundary[Result.Triangles[Slot + 1]];
				const FVector2D& P2 = Result.Boundary[Result.Triangles[Slot + 2]];
				Clockwise += FVector2D::CrossProduct(P1 - P0, P2 - P0) <= 0.0 ? 1 : 0;
			}
			TestEqual(FString::Printf(TEXT("%s: every triangle winds counter-clockwise"), *Case), Clockwise, 0);
			double Farthest = 0.0;
			for (const FVector2D& Point : Rim)
			{
				Farthest = FMath::Max(Farthest, FVector2D::Distance(Point, Input.Position));
			}
			TestTrue(FString::Printf(TEXT("%s: the rim reaches %.1f uu from the node, within the wide half-width's diagonal"), *Case, Farthest),
				Farthest <= FMath::Sqrt(2.0) * WideHalf);
		}
	}
	return true;
}

/** AND THE GHOST SAYS SO: the case the player met, through RoadPlacement::Validate. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWidthStepNearStraightValidatesTest,
	"Airside.Tool.WidthStepNearStraight.Validates",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FWidthStepNearStraightValidatesTest::RunTest(const FString& Parameters)
{
	using namespace NearStraightWidthStep;

	for (const double Degrees : OffDegrees)
	{
		URoadNetwork* Network = NewObject<URoadNetwork>(GetTransientPackage());
		URoadProfile* WideProfile = URoadProfile::MakeTransient(WideHalf * 2.0, URoadProfile::StandardTaxiwayFilletRadius);
		const FRoadNodeId West = Network->AddNode(FVector2D(-7433.0, 0.0));
		const FRoadNodeId Hub = Network->AddNode(FVector2D(0.0, 0.0));
		if (!TestTrue(TEXT("the wide taxiway behind exists"), Network->AddStraightSegment(West, Hub, WideProfile).IsSet()))
		{
			return false;
		}

		FRoadPlacementLimits Limits;
		Limits.MinSegmentLength = 250.0;
		Limits.MinTurnDegrees = 25.0;
		// THE GATE (see Airside.Tool.RoadExtendsStraightAhead): without it the corner rule never runs.
		Limits.NewRoadHalfWidth = NarrowHalf;

		FRoadSnapResult Ahead;
		Ahead.Kind = ERoadSnapKind::Free;
		Ahead.Position = RoadGeom::Rotate(FVector2D(7000.0, 0.0), FMath::DegreesToRadians(Degrees));
		const ERoadPlacement Verdict = RoadPlacement::Validate(*Network, Hub, Ahead, Limits);
		TestEqual(FString::Printf(TEXT("a 12 m taxiway drawn on %.1f deg off a wide one's line is valid (got %s)"),
			Degrees, Verdict == ERoadPlacement::Valid ? TEXT("valid") : RoadPlacement::Describe(Verdict)),
			static_cast<int32>(Verdict), static_cast<int32>(ERoadPlacement::Valid));
	}

	// GENUINELY TOO SHORT STILL REFUSES at a real bend: 60 degrees off, the narrow arm must reach
	// past its own length to clear the wide one. Without this "always valid" passes the above.
	{
		URoadNetwork* Network = NewObject<URoadNetwork>(GetTransientPackage());
		URoadProfile* WideProfile = URoadProfile::MakeTransient(WideHalf * 2.0, URoadProfile::StandardTaxiwayFilletRadius);
		const FRoadNodeId West = Network->AddNode(FVector2D(-7433.0, 0.0));
		const FRoadNodeId Hub = Network->AddNode(FVector2D(0.0, 0.0));
		Network->AddStraightSegment(West, Hub, WideProfile);
		FRoadPlacementLimits Limits;
		Limits.MinSegmentLength = 250.0;
		Limits.MinTurnDegrees = 25.0;
		Limits.NewRoadHalfWidth = NarrowHalf;
		FRoadSnapResult Short;
		Short.Kind = ERoadSnapKind::Free;
		Short.Position = RoadGeom::Rotate(FVector2D(800.0, 0.0), FMath::DegreesToRadians(60.0));
		TestEqual(TEXT("a short road at a real 60 degree bend is still too short to hold its corner"),
			static_cast<int32>(RoadPlacement::Validate(*Network, Hub, Short, Limits)),
			static_cast<int32>(ERoadPlacement::TooShortForCorner));
	}
	return true;
}

/** AND THE SOLVER THAT LOGGED IT: the whole network, both segments laid, the node paved. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWidthStepNearStraightSolvesTest,
	"Airside.Build.WidthStepNearStraight.Solves",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FWidthStepNearStraightSolvesTest::RunTest(const FString& Parameters)
{
	using namespace NearStraightWidthStep;

	for (const double Degrees : OffDegrees)
	{
		URoadNetwork* Network = NewObject<URoadNetwork>(GetTransientPackage());
		URoadProfile* WideProfile = URoadProfile::MakeTransient(WideHalf * 2.0, URoadProfile::StandardTaxiwayFilletRadius);
		URoadProfile* NarrowProfile = URoadProfile::MakeTransient(NarrowHalf * 2.0, URoadProfile::StandardTaxiwayFilletRadius);
		const FRoadNodeId West = Network->AddNode(FVector2D(-7433.0, 0.0));
		const FRoadNodeId Hub = Network->AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId East = Network->AddNode(RoadGeom::Rotate(FVector2D(7000.0, 0.0), FMath::DegreesToRadians(Degrees)));
		const FRoadSegmentId WideSeg = Network->AddStraightSegment(West, Hub, WideProfile);
		const FRoadSegmentId NarrowSeg = Network->AddStraightSegment(Hub, East, NarrowProfile);
		if (!TestTrue(TEXT("both taxiways exist"), WideSeg.IsSet() && NarrowSeg.IsSet()))
		{
			return false;
		}

		const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Network, 12, nullptr, EWideningTrace::ReadCached);
		const FJunctionResult* Junction = Solved.NodeResults.Find(Hub.Index);
		const FString Case = FString::Printf(TEXT("%.1f deg off"), Degrees);
		if (!TestNotNull(FString::Printf(TEXT("%s: the hub was solved"), *Case), Junction))
		{
			continue;
		}
		TestTrue(FString::Printf(TEXT("%s: the hub is valid - it was refused as needing ~1 km of cut"), *Case), Junction->bValid);
		TestTrue(FString::Printf(TEXT("%s: and paved"), *Case), Junction->Triangles.Num() > 0);

		const FRoadSegment* Wide = Network->GetSegment(WideSeg);
		const FRoadSegment* Narrow = Network->GetSegment(NarrowSeg);
		if (Wide != nullptr && Narrow != nullptr)
		{
			TestTrue(FString::Printf(TEXT("%s: the wide taxiway's hub end is cut %.1f uu, under a half-width"), *Case, Wide->TrimB),
				Wide->bSolvedB && Wide->TrimB < NarrowHalf);
			TestTrue(FString::Printf(TEXT("%s: the narrow taxiway's hub end is cut %.1f uu, under a half-width"), *Case, Narrow->TrimA),
				Narrow->bSolvedA && Narrow->TrimA < NarrowHalf);
		}
	}
	return true;
}

#endif
