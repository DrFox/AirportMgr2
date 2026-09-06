#include "CoreMinimal.h"
#include "Build/RoadMeshBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Profiles/RoadProfile.h"
#include "Tool/RoadPlacement.h"
#include "Tool/RoadSnap.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// Prefixed against the unity build - test files share one translation unit.

	/** Builds a V: centre node, one arm east, one arm at Degrees from it, both ArmLength long. */
	struct FCornerFixture
	{
		URoadNetwork* Net = nullptr;
		FRoadNodeId Centre, East, Other;
		FRoadSegmentId ToEast, ToOther;

		FCornerFixture(double Degrees, double ArmLength, URoadProfile* Profile)
		{
			Net = NewObject<URoadNetwork>();
			const double R = FMath::DegreesToRadians(Degrees);
			Centre = Net->AddNode(FVector2D(0.0, 0.0));
			East = Net->AddNode(FVector2D(ArmLength, 0.0));
			Other = Net->AddNode(FVector2D(ArmLength * FMath::Cos(R), ArmLength * FMath::Sin(R)));
			ToEast = Net->AddStraightSegment(Centre, East, Profile);
			ToOther = Net->AddStraightSegment(Centre, Other, Profile);
		}
	};

	/** Sign of every triangle's winding in the buffers; 0 if any is degenerate or they disagree. */
	int32 CornerWindingSign(const FRoadMeshBuffers& Buffers)
	{
		int32 Sign = 0;
		for (int32 I = 0; I + 2 < Buffers.Indices.Num(); I += 3)
		{
			const FVector3d& A = Buffers.Positions[Buffers.Indices[I]];
			const FVector3d& B = Buffers.Positions[Buffers.Indices[I + 1]];
			const FVector3d& C = Buffers.Positions[Buffers.Indices[I + 2]];
			const double Z = FVector2D::CrossProduct(FVector2D(B - A), FVector2D(C - A));
			if (FMath::Abs(Z) < 1e-3)
			{
				return 0;
			}
			const int32 This = Z > 0.0 ? 1 : -1;
			if (Sign == 0) { Sign = This; }
			else if (Sign != This) { return 0; }
		}
		return Sign;
	}

	void CornerBuild(const FCornerFixture& F, const FRoadSolveResult& Solved, FRoadMeshBuilder& Builder)
	{
		const TArray<FRoadSegmentId> NoArms;
		for (const TPair<int32, FJunctionResult>& Pair : Solved.NodeResults)
		{
			const TArray<FRoadSegmentId>* Arms = Solved.NodeArmSegments.Find(Pair.Key);
			Builder.AddJunction(*F.Net, Pair.Key, Pair.Value, Arms ? *Arms : NoArms);
		}
		Builder.AddSegment(*F.Net, F.ToEast, 1);
		Builder.AddSegment(*F.Net, F.ToOther, 1);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCornerFitTest,
	"Airside.Build.AcuteCornerNeverFolds",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FCornerFitTest::RunTest(const FString& Parameters)
{
	// THE BUG OF 2026-09-06: draw two short roads meeting at an acute angle and the road
	// vanishes. The fillet's cut distance grows as the corner closes; when the two cuts of a
	// segment pass each other the ribbon is built backwards, its triangles wind the other
	// way, and in Unreal that face points DOWN - "under the level surface". Measured here
	// on winding, against a reference the same builder produced for a corner that fits.
	URoadProfile* Profile = URoadProfile::MakeTransient(400.0, 1500.0);   // half-width 200, radius 1500

	int32 Reference = 0;
	{
		// A comfortable right angle with long arms: this is what "faces up" looks like.
		FCornerFixture Wide(90.0, 40000.0, Profile);
		const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Wide.Net);
		FRoadMeshBuilder Builder(10.0);
		CornerBuild(Wide, Solved, Builder);
		Reference = CornerWindingSign(Builder.GetBuffers());
		TestTrue(TEXT("the reference corner has one consistent winding"), Reference != 0);
	}

	{
		// 30 degrees, arms 3000 uu. At zero radius the inner corner sits 200/tan(15) = 746 uu
		// from the node; the dead end has no floor, so 2254 uu of slack remains and a REDUCED
		// radius fits.
		FCornerFixture Acute(30.0, 3000.0, Profile);
		const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Acute.Net);
		TestEqual(TEXT("a corner that can fit with a smaller radius is solved, not failed"), Solved.FailedNodes, 0);

		for (const FRoadSegmentId Id : { Acute.ToEast, Acute.ToOther })
		{
			const FRoadSegment* S = Acute.Net->GetSegment(Id);
			if (!TestNotNull(TEXT("segment"), S) || !TestTrue(TEXT("both ends solved"), S->bSolvedA && S->bSolvedB)) { continue; }
			const FVector2D CentreA = (S->LeftCutA + S->RightCutA) * 0.5;
			const FVector2D CentreB = (S->LeftCutB + S->RightCutB) * 0.5;
			const FVector2D Along = Acute.Net->GetNode(S->B)->Position - Acute.Net->GetNode(S->A)->Position;
			TestTrue(TEXT("the two cuts stay in order along the segment - the ribbon is not folded"),
				FVector2D::DotProduct(CentreB - CentreA, Along) > 0.0);
			TestTrue(TEXT("the two trims fit inside the segment - the invariant the ribbon needs"),
				S->TrimA + S->TrimB < 3000.0);
		}

		FRoadMeshBuilder Builder(10.0);
		CornerBuild(Acute, Solved, Builder);
		TestTrue(TEXT("the acute corner produced a mesh"), Builder.GetBuffers().Indices.Num() > 0);
		TestEqual(TEXT("every triangle winds the way the reference does - nothing faces down"),
			CornerWindingSign(Builder.GetBuffers()), Reference);
	}

	{
		// A ZIG-ZAG: 30-degree corners at BOTH ends of a 1200 uu middle segment. Each end's
		// zero-radius reach is 746, and 746 + 746 > 1200 is the fold. A dead end only trims
		// by its half-width, which is why a lone V does not fold. No radius fits: the honest
		// answer is FAILED nodes with nothing drawn from them.
		URoadNetwork* Net = NewObject<URoadNetwork>();
		// At N1 the arms run east (to N2) and at bearing -30 degrees (to N0): a 30-degree
		// corner. At N2 they run west (to N1) and at bearing 150 degrees (to N3): another.
		// N0 below the axis and N3 above it, so the two outer segments never cross.
		const double R30 = FMath::DegreesToRadians(30.0);
		const double R150 = FMath::DegreesToRadians(150.0);
		const FRoadNodeId N0 = Net->AddNode(FVector2D(1200.0 * FMath::Cos(-R30), 1200.0 * FMath::Sin(-R30)));
		const FRoadNodeId N1 = Net->AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId N2 = Net->AddNode(FVector2D(1200.0, 0.0));
		const FRoadNodeId N3 = Net->AddNode(FVector2D(1200.0, 0.0) + FVector2D(1200.0 * FMath::Cos(R150), 1200.0 * FMath::Sin(R150)));
		const FRoadSegmentId S01 = Net->AddStraightSegment(N0, N1, Profile);
		const FRoadSegmentId Middle = Net->AddStraightSegment(N1, N2, Profile);
		const FRoadSegmentId S23 = Net->AddStraightSegment(N2, N3, Profile);

		const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Net);
		TestTrue(TEXT("a corner no radius can fit fails its node"), Solved.FailedNodes >= 1);

		const FRoadSegment* S = Net->GetSegment(Middle);
		if (TestNotNull(TEXT("middle segment"), S))
		{
			TestFalse(TEXT("the middle segment is not solved at a failed end - it will not be drawn folded"),
				S->bSolvedA && S->bSolvedB);
		}

		FRoadMeshBuilder Builder(10.0);
		const TArray<FRoadSegmentId> NoArms;
		for (const TPair<int32, FJunctionResult>& Pair : Solved.NodeResults)
		{
			const TArray<FRoadSegmentId>* Arms = Solved.NodeArmSegments.Find(Pair.Key);
			Builder.AddJunction(*Net, Pair.Key, Pair.Value, Arms ? *Arms : NoArms);
		}
		for (const FRoadSegmentId Id : { S01, Middle, S23 })
		{
			Builder.AddSegment(*Net, Id, 1);
		}
		const int32 Sign = CornerWindingSign(Builder.GetBuffers());
		TestTrue(TEXT("whatever was drawn faces up or nothing was drawn"),
			Builder.GetBuffers().Indices.Num() == 0 || Sign == Reference);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCornerPlacementTest,
	"Airside.Tool.PlacementRefusesCornerThatCannotFit",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FCornerPlacementTest::RunTest(const FString& Parameters)
{
	// The same corner, judged BEFORE it exists: the ghost turns red and says why, instead of
	// the solver failing a node the player already drew.
	URoadProfile* Profile = URoadProfile::MakeTransient(400.0, 1500.0);
	FRoadPlacementLimits Limits;
	Limits.MinSegmentLength = 250.0;
	Limits.MinTurnDegrees = 25.0;
	Limits.NewRoadHalfWidth = 200.0;

	auto Judge = [&](double ArmLength)
	{
		URoadNetwork* Net = NewObject<URoadNetwork>();
		const FRoadNodeId Centre = Net->AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId East = Net->AddNode(FVector2D(ArmLength, 0.0));
		Net->AddStraightSegment(Centre, East, Profile);

		const double R = FMath::DegreesToRadians(30.0);
		FRoadSnapResult To;
		To.Kind = ERoadSnapKind::Free;
		To.Position = FVector2D(ArmLength * FMath::Cos(R), ArmLength * FMath::Sin(R));
		return RoadPlacement::Validate(*Net, Centre, To, Limits);
	};

	// The new road's corner needs 746 at the start; its far end is a dead end with no floor.
	TestEqual(TEXT("30 degrees with 3000 uu arms is a corner the solver can fit"), Judge(3000.0), ERoadPlacement::Valid);
	TestEqual(TEXT("30 degrees with 700 uu arms cannot hold its corner (746) and is refused"), Judge(700.0), ERoadPlacement::TooShortForCorner);

	Limits.NewRoadHalfWidth = 0.0;
	TestEqual(TEXT("a zero half-width disables the check, so callers without a profile are unchanged"),
		Judge(700.0), ERoadPlacement::Valid);

	TestTrue(TEXT("the refusal has its own words"),
		FString(RoadPlacement::Describe(ERoadPlacement::TooShortForCorner)) != FString(RoadPlacement::Describe(ERoadPlacement::TooShort)));

	// The drag path: the same corner judged at a PROPOSED node position. A right-angle V
	// with 3000 uu arms fits where it stands; dragged past its east end to (3400, -1000) the
	// east arm is ~1077 uu long and the angle between the arms closes to ~19 degrees, whose
	// floor of ~1213 no longer fits - said before the move happens.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>();
		const FRoadNodeId Centre = Net->AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId East = Net->AddNode(FVector2D(3000.0, 0.0));
		const FRoadNodeId North = Net->AddNode(FVector2D(0.0, 3000.0));
		Net->AddStraightSegment(Centre, East, Profile);
		Net->AddStraightSegment(Centre, North, Profile);
		TestTrue(TEXT("a right angle with 3000 uu arms fits where it stands"),
			RoadPlacement::NodeCornersFit(*Net, Centre, FVector2D(0.0, 0.0)));
		TestFalse(TEXT("dragged into an acute short corner it does not fit"),
			RoadPlacement::NodeCornersFit(*Net, Centre, FVector2D(3400.0, -1000.0)));
		TestTrue(TEXT("a dead end moved nearer keeps a corner that still fits"),
			RoadPlacement::NodeCornersFit(*Net, East, FVector2D(2000.0, 0.0)));
	}
	return true;
}

#endif
