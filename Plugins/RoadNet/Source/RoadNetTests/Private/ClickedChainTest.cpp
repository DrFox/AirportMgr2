#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Build/RoadMeshBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadClickedChainTest,
	"RoadNet.Build.ClickedChain",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadClickedChainTest::RunTest(const FString& Parameters)
{
	// What the runtime tool actually builds: a chain of clicks at assorted angles, at
	// the scale and profile the top-down view produces. Every earlier test used either
	// gallery-scale arms or a single tidy segment; none walked a chain the way a player
	// draws one, which is the case that renders with patchy shading on screen.
	constexpr double TotalWidth = 200.0;
	constexpr double FilletRadius = 100.0;

	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Profile = URoadProfile::MakeTransient(TotalWidth, FilletRadius);

	const TArray<FVector2D> Clicked = {
		FVector2D(0.0, 0.0),
		FVector2D(1500.0, 200.0),
		FVector2D(2600.0, 1400.0),
		FVector2D(1900.0, 2900.0),
		FVector2D(300.0, 2500.0)
	};

	TArray<FRoadNodeId> Nodes;
	for (const FVector2D& Point : Clicked)
	{
		Nodes.Add(Net->AddNode(Point));
	}

	TArray<FRoadSegmentId> Segments;
	for (int32 Index = 0; Index + 1 < Nodes.Num(); ++Index)
	{
		Segments.Add(Net->AddStraightSegment(Nodes[Index], Nodes[Index + 1], Profile));
	}

	const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Net);
	TestEqual(TEXT("every node in the chain solves"), Solved.FailedNodes, 0);
	TestEqual(TEXT("every node produces a result"), Solved.SolvedNodes, Clicked.Num());

	constexpr double ZHeight = 200.0;
	FRoadMeshBuilder Builder(ZHeight);
	for (const FRoadSegmentId SegmentId : Segments)
	{
		Builder.AddSegment(*Net, SegmentId, 1);
	}
	for (const TPair<int32, FJunctionResult>& Pair : Solved.NodeResults)
	{
		static const TArray<FRoadSegmentId> NoArms;
		const TArray<FRoadSegmentId>* Arms = Solved.NodeArmSegments.Find(Pair.Key);
		Builder.AddJunction(*Net, Pair.Key, Pair.Value, Arms ? *Arms : NoArms);
	}

	const FRoadMeshBuffers& Buffers = Builder.GetBuffers();
	TestTrue(TEXT("the chain produced geometry"), Buffers.Indices.Num() > 0);

	// Every vertex on one plane. A stray Z is what "the roads look like they are at
	// different heights" would actually be.
	for (const FVector3d& P : Buffers.Positions)
	{
		TestEqual(TEXT("vertex sits exactly on the road plane"), P.Z, ZHeight);
	}

	// Every triangle facing up. A backwards one is culled from above, and its face
	// normal drags the averaged vertex normals of everything it touches away from +Z,
	// which is what shades neighbouring triangles differently on a flat surface.
	int32 Inverted = 0;
	double WorstArea = 0.0;
	for (int32 Slot = 0; Slot + 2 < Buffers.Indices.Num(); Slot += 3)
	{
		const FVector3d& A = Buffers.Positions[Buffers.Indices[Slot]];
		const FVector3d& B = Buffers.Positions[Buffers.Indices[Slot + 1]];
		const FVector3d& C = Buffers.Positions[Buffers.Indices[Slot + 2]];

		const double Area = 0.5 * ((B.X - A.X) * (C.Y - A.Y) - (B.Y - A.Y) * (C.X - A.X));
		// Unreal's front face is the OPPOSITE winding to the maths convention: it is
		// left-handed, so VectorUtil::Normal takes cross(C-A, B-A) and a triangle with
		// positive 2D signed area faces DOWN and is backface-culled. FRoadMeshBuilder
		// ::AddTriangle emits the swapped winding for that reason, so front-facing here
		// means NEGATIVE area. Asserting the maths convention is what let a whole slice
		// ship with every road facing the ground.
		if (Area >= 0.0)
		{
			++Inverted;
			WorstArea = FMath::Max(WorstArea, Area);
		}
	}

	TestEqual(
		FString::Printf(TEXT("no inside-out triangles in a clicked chain (worst area %.1f)"), WorstArea),
		Inverted, 0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
