#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Build/RoadMeshBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadShortSegmentTest,
	"RoadNet.Build.ShortSegments",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadShortSegmentTest::RunTest(const FString& Parameters)
{
	// Roads at the scale a player actually draws them, rather than the gallery's, whose
	// 20,000 uu arms were picked precisely to clear the fillet reach and so hid this.
	//
	// A corner's cut distance is reach + |R / tan(Theta/2)|, where Theta is the angular
	// gap between adjacent arms. At a right angle with R = 1500 that is roughly
	// 1150 + 1500 = 2650 uu - longer than either of these 2500 uu segments. Design spec
	// section 5 step 4 states the solver deliberately does not clamp this, because
	// clamping needs a segment length the solver does not know, and that "a caller that
	// must fit a finite segment clamps the radius before calling". FRoadNetworkSolver is
	// that caller.
	constexpr double TotalWidth = 2300.0;
	constexpr double FilletRadius = 1500.0;
	constexpr double ArmLength = 2500.0;

	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Profile = URoadProfile::MakeTransient(TotalWidth, FilletRadius);

	// An L bend, the shape two clicks and a turn produce.
	const FRoadNodeId Start  = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId Corner = Net->AddNode(FVector2D(ArmLength, 0.0));
	const FRoadNodeId End    = Net->AddNode(FVector2D(ArmLength, ArmLength));

	const FRoadSegmentId First  = Net->AddStraightSegment(Start, Corner, Profile);
	const FRoadSegmentId Second = Net->AddStraightSegment(Corner, End, Profile);

	const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Net);
	TestTrue(TEXT("every node solved"), Solved.FailedNodes == 0);

	// The invariant a caller has to uphold: a segment cannot be trimmed past its own
	// length. Exceed it and the two cut lines cross, so the ribbon renders inside-out -
	// which on screen is a black, back-facing surface.
	const FRoadSegment* Seg = Net->GetSegment(First);
	TestTrue(TEXT("first segment solved"), Seg->bSolvedA && Seg->bSolvedB);
	TestTrue(
		FString::Printf(TEXT("trims %.1f + %.1f fit inside the %.1f uu segment"),
			Seg->TrimA, Seg->TrimB, ArmLength),
		Seg->TrimA + Seg->TrimB < ArmLength);

	// And the consequence, measured on the mesh rather than inferred from the trims.
	FRoadMeshBuilder Builder(10.0);
	Builder.AddSegment(*Net, First, 1);
	Builder.AddSegment(*Net, Second, 1);
	for (const TPair<int32, FJunctionResult>& Pair : Solved.NodeResults)
	{
		static const TArray<FRoadSegmentId> NoArms;
		const TArray<FRoadSegmentId>* Arms = Solved.NodeArmSegments.Find(Pair.Key);
		Builder.AddJunction(*Net, Pair.Key, Pair.Value, Arms ? *Arms : NoArms);
	}

	const FRoadMeshBuffers& Buffers = Builder.GetBuffers();
	int32 Inverted = 0;
	for (int32 Slot = 0; Slot + 2 < Buffers.Indices.Num(); Slot += 3)
	{
		const FVector3d& A = Buffers.Positions[Buffers.Indices[Slot]];
		const FVector3d& B = Buffers.Positions[Buffers.Indices[Slot + 1]];
		const FVector3d& C = Buffers.Positions[Buffers.Indices[Slot + 2]];
		// Unreal's front face is the OPPOSITE winding to the maths convention: it is
		// left-handed, so VectorUtil::Normal takes cross(C-A, B-A) and a triangle with
		// positive 2D signed area faces DOWN and is backface-culled. FRoadMeshBuilder
		// ::AddTriangle emits the swapped winding for that reason, so front-facing here
		// means NEGATIVE area. Asserting the maths convention is what let a whole slice
		// ship with every road facing the ground.
		if (0.5 * ((B.X - A.X) * (C.Y - A.Y) - (B.Y - A.Y) * (C.X - A.X)) >= 0.0)
		{
			++Inverted;
		}
	}
	TestEqual(TEXT("no inside-out triangles at player scale"), Inverted, 0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
