#include "CoreMinimal.h"
#include "Build/RoadMeshBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Present/RoadNetworkActor.h"
#include "Profiles/RoadProfile.h"
#include "Tool/BuildSession.h"
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
	FBendJunctionPavedTest,
	"Airside.Build.BendJunctionIsPaved",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBendJunctionPavedTest::RunTest(const FString& Parameters)
{
	// THE SECOND WAY A ROAD VANISHED ON 2026-09-06. Rebuilt from the user's own log: a bend of
	// ~93 degrees between two segments of 2127 and 1857 uu on the 400-wide default profile.
	// The fitted fillet's outer arc bulges past the node, so the node is outside its own
	// rim, the rim centroid is too, and the solver's fan refused - leaving the corner
	// pavement missing while both ribbons drew. The builder must pave it anyway.
	URoadProfile* Profile = URoadProfile::MakeTransient(400.0, 1500.0);
	URoadNetwork* Net = NewObject<URoadNetwork>();
	const FRoadNodeId N5 = Net->AddNode(FVector2D(1528.0, -2036.0));
	const FRoadNodeId N4 = Net->AddNode(FVector2D(679.0, -86.0));
	const FRoadNodeId N2 = Net->AddNode(FVector2D(2344.0, 737.0));
	Net->AddStraightSegment(N5, N4, Profile);
	Net->AddStraightSegment(N4, N2, Profile);

	const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Net);
	TestEqual(TEXT("every node solves"), Solved.FailedNodes, 0);

	const FJunctionResult* Bend = Solved.NodeResults.Find(N4.Index);
	if (!TestNotNull(TEXT("the bend has a junction result"), Bend)) { return false; }
	TestTrue(TEXT("the fixture reproduces the refused fan - the rim is present but the solver emitted no triangles"),
		Bend->Triangles.Num() == 0 && Bend->Boundary.Num() >= 4);

	FRoadMeshBuilder Builder(10.0);
	const TArray<FRoadSegmentId>* Arms = Solved.NodeArmSegments.Find(N4.Index);
	const TArray<FRoadSegmentId> NoArms;
	Builder.AddJunction(*Net, N4.Index, *Bend, Arms ? *Arms : NoArms);
	const FRoadMeshBuffers& B = Builder.GetBuffers();
	TestTrue(TEXT("the corner is paved"), B.Indices.Num() > 0);

	// Measured, not narrated: every triangle faces up (negative signed area is the front
	// face here - see ShortSegmentTest), and together they cover the rim's own area.
	double Covered = 0.0;
	int32 Down = 0;
	for (int32 I = 0; I + 2 < B.Indices.Num(); I += 3)
	{
		const FVector3d& P0 = B.Positions[B.Indices[I]];
		const FVector3d& P1 = B.Positions[B.Indices[I + 1]];
		const FVector3d& P2 = B.Positions[B.Indices[I + 2]];
		const double Area2 = (P1.X - P0.X) * (P2.Y - P0.Y) - (P1.Y - P0.Y) * (P2.X - P0.X);
		if (Area2 > 0.0) { ++Down; }
		Covered += FMath::Abs(Area2) * 0.5;
	}
	TestEqual(TEXT("no triangle faces down"), Down, 0);

	double RimArea2 = 0.0;
	const int32 RimCount = Bend->Boundary.Num() - 1;
	for (int32 I = 0; I < RimCount; ++I)
	{
		const FVector2D& P = Bend->Boundary[I];
		const FVector2D& Q = Bend->Boundary[(I + 1) % RimCount];
		RimArea2 += P.X * Q.Y - Q.X * P.Y;
	}
	TestEqual(TEXT("the triangles cover the rim's area"), Covered, FMath::Abs(RimArea2) * 0.5, FMath::Abs(RimArea2) * 0.005);
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

	auto JudgeWith = [&](double ArmLength, const FRoadPlacementLimits& WithLimits)
	{
		URoadNetwork* Net = NewObject<URoadNetwork>();
		const FRoadNodeId Centre = Net->AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId East = Net->AddNode(FVector2D(ArmLength, 0.0));
		Net->AddStraightSegment(Centre, East, Profile);

		const double R = FMath::DegreesToRadians(30.0);
		FRoadSnapResult To;
		To.Kind = ERoadSnapKind::Free;
		To.Position = FVector2D(ArmLength * FMath::Cos(R), ArmLength * FMath::Sin(R));
		return RoadPlacement::Validate(*Net, Centre, To, WithLimits);
	};
	auto Judge = [&](double ArmLength) { return JudgeWith(ArmLength, Limits); };

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

	// THROUGH MakeTunables, not a hand-built FRoadPlacementLimits - issue #93. Before that
	// issue, only the runtime driver ever resolved NewRoadHalfWidth from a live profile; the
	// editor tool's own Limits never got one at all, so a corner it drew here would have
	// judged Valid where PIE judged TooShortForCorner. Both drivers now go through this one
	// function, so this is the seam that makes such a divergence impossible again.
	{
		ARoadNetworkActor* Actor = NewObject<ARoadNetworkActor>(GetTransientPackage());
		if (!TestNotNull(TEXT("actor constructed"), Actor))
		{
			return false;
		}
		Actor->Profile = Profile;
		Actor->PlacementLimits.MinSegmentLength = 250.0;
		Actor->PlacementLimits.MinTurnDegrees = 25.0;
		Actor->Snap.NodeRadius = 321.0;

		const FBuildSessionTunables Tunables = Actor->MakeTunables(0.0);
		TestEqual(TEXT("MakeTunables resolves NewRoadHalfWidth from the actor's own profile"),
			Tunables.Limits.NewRoadHalfWidth, 200.0);
		TestEqual(TEXT("MakeTunables carries the actor's own Snap through unchanged"),
			Tunables.Snap.NodeRadius, 321.0);
		TestEqual(TEXT("ViewWorldWidth 0 leaves ToolPickRadius at the class default, for the "
			"caller (the runtime driver) that overwrites it with its own view fact"),
			Tunables.ToolPickRadius, FBuildSessionTunables().ToolPickRadius);

		const FBuildSessionTunables ViewScaled = Actor->MakeTunables(20000.0);
		TestEqual(TEXT("a positive ViewWorldWidth sizes ToolPickRadius off it instead"),
			ViewScaled.ToolPickRadius, 20000.0 * 0.02);

		TestEqual(TEXT("MakeTunables' Limits refuse the same 700 uu corner Judge() does"),
			JudgeWith(700.0, Tunables.Limits), ERoadPlacement::TooShortForCorner);
		TestEqual(TEXT("and accept the same 3000 uu one"),
			JudgeWith(3000.0, Tunables.Limits), ERoadPlacement::Valid);
	}
	return true;
}

#endif
