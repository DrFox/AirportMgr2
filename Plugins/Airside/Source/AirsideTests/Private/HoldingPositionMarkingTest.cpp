#include "CoreMinimal.h"
#include "Build/HoldingPositionMarkingBuilder.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/MeshNormals.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FGuidelineNodeId M2MarkingNodeFor(const URoadNetwork& Net, FRoadSegmentId Segment, bool bEndA)
	{
		const TArray<FGuidelineNode>& Nodes = Net.GetGuidelineNodes();
		for (int32 Index = 0; Index < Nodes.Num(); ++Index)
		{
			if (Nodes[Index].bAlive && Nodes[Index].Origin.Segment == Segment && Nodes[Index].Origin.bEndA == bEndA)
			{
				return Net.GuidelineNodeIdAt(Index);
			}
		}
		return FGuidelineNodeId();
	}
}

/**
 * THE PAINT IS WHERE THE STOP IS. Measured on the buffers the component receives: the
 * runway pattern lies across the taxiway on the junction side of the node, within the
 * taxiway's width, in the road plane at the Z it was asked for, with UV1 zero so the road
 * material paints it as marking - and it faces UP the way Unreal measures it, which is the
 * check every winding test in this project once got wrong by hand.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHoldingPositionMarkingTest,
	"Airside.Build.HoldingPositionMarking",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FHoldingPositionMarkingTest::RunTest(const FString& Parameters)
{
	//   T ======= E ======= F        runway (east-west)
	//             |
	//             X                  taxiway E-X due south; its E end is the runway position
	constexpr double TaxiwayWidth = 2300.0;
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Runway = URoadProfile::MakeTransient(4500.0, 1500.0, 450.0);
	Runway->bContinuousThroughJunctions = true;
	URoadProfile* Taxiway = URoadProfile::MakeTransient(TaxiwayWidth, 1500.0, 230.0);
	const FRoadNodeId T = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId E = Net->AddNode(FVector2D(60000.0, 0.0));
	const FRoadNodeId F = Net->AddNode(FVector2D(100000.0, 0.0));
	Net->AddStraightSegment(T, E, Runway);
	Net->AddStraightSegment(E, F, Runway);
	const FRoadNodeId X = Net->AddNode(FVector2D(60000.0, -20000.0));
	const FRoadSegmentId Tx = Net->AddStraightSegment(E, X, Taxiway);
	const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Net);
	FRoadGuidelineBuilder::Build(*Net, Solved);

	const FGuidelineNodeId RunwayEnd = M2MarkingNodeFor(*Net, Tx, true);
	const FGuidelineNodeId FarEnd = M2MarkingNodeFor(*Net, Tx, false);
	if (!TestTrue(TEXT("both taxiway ends exist"), RunwayEnd.IsSet() && FarEnd.IsSet())) { return false; }
	TestTrue(TEXT("the runway end is a derived runway-holding position"),
		Net->GetGuidelineNode(RunwayEnd)->HoldingPosition == EHoldingPositionKind::Runway);

	// 1. THE RUNWAY PATTERN ALONE.
	constexpr double Z = 10.5;
	FRoadMeshBuffers Buffers;
	const int32 Painted = FHoldingPositionMarkingBuilder::Build(*Net, Z, Buffers);
	TestEqual(TEXT("one position painted"), Painted, 1);
	const int32 Triangles = Buffers.Indices.Num() / 3;
	// Two solid bars (a quad each) and two dashed bars of ceil(2300 / 180) = 13 dashes each.
	TestEqual(FString::Printf(TEXT("two solid and two dashed bars: (2 + 2 x 13) quads x 2 triangles (%d)"), Triangles),
		Triangles, (2 + 2 * 13) * 2);
	TestEqual(TEXT("one material id per triangle"), Buffers.MaterialIDs.Num(), Triangles);
	TestEqual(TEXT("one UV1 per vertex"), Buffers.UV1.Num(), Buffers.Positions.Num());

	const FVector2D NodeAt = Net->GetGuidelineNode(RunwayEnd)->Position;
	const FVector2D Toward(0.0, 1.0);   // from the node INTO the junction: the taxiway runs south, the runway is north
	double MaxAlong = -1.0e9, MinAlong = 1.0e9, MaxAcross = 0.0;
	bool bAllAtZ = true, bAllUV1Zero = true;
	for (int32 Index = 0; Index < Buffers.Positions.Num(); ++Index)
	{
		const FVector3d& P = Buffers.Positions[Index];
		bAllAtZ = bAllAtZ && FMath::Abs(P.Z - Z) < 1.0e-6;
		bAllUV1Zero = bAllUV1Zero && Buffers.UV1[Index].IsNearlyZero();
		const FVector2D Offset(P.X - NodeAt.X, P.Y - NodeAt.Y);
		const double Along = FVector2D::DotProduct(Offset, Toward);
		MaxAlong = FMath::Max(MaxAlong, Along);
		MinAlong = FMath::Min(MinAlong, Along);
		MaxAcross = FMath::Max(MaxAcross, FMath::Abs(FVector2D::CrossProduct(Toward, Offset)));
	}
	TestTrue(TEXT("every vertex is in the road plane at the Z asked for"), bAllAtZ);
	TestTrue(TEXT("every vertex carries UV1 = 0, so the road material paints it as marking"), bAllUV1Zero);
	TestTrue(FString::Printf(TEXT("the pattern starts AT the node (nearest vertex %.1f uu along) - the stopped nose sits on the first line"), MinAlong),
		FMath::Abs(MinAlong) < 1.0e-6);
	const double PatternDepth = 4.0 * FHoldingPositionMarkingBuilder::LineWidth + 3.0 * FHoldingPositionMarkingBuilder::LineGap;
	TestTrue(FString::Printf(TEXT("and lies on the junction side of the node, %.0f uu deep (%.1f)"), PatternDepth, MaxAlong),
		FMath::Abs(MaxAlong - PatternDepth) < 1.0e-6);
	TestTrue(FString::Printf(TEXT("across the taxiway's full width and no further (%.1f of %.1f)"), MaxAcross, TaxiwayWidth * 0.5),
		FMath::Abs(MaxAcross - TaxiwayWidth * 0.5) < 1.0e-6);

	// FACES UP, measured the way Unreal measures it.
	{
		UE::Geometry::FDynamicMesh3 Mesh;
		for (const FVector3d& Position : Buffers.Positions) { Mesh.AppendVertex(Position); }
		for (int32 Slot = 0; Slot + 2 < Buffers.Indices.Num(); Slot += 3)
		{
			Mesh.AppendTriangle(Buffers.Indices[Slot], Buffers.Indices[Slot + 1], Buffers.Indices[Slot + 2]);
		}
		UE::Geometry::FMeshNormals::QuickComputeVertexNormals(Mesh);
		int32 Downward = 0;
		for (const int32 VertexId : Mesh.VertexIndicesItr())
		{
			if (Mesh.GetVertexNormal(VertexId).Z <= 0.0f) { ++Downward; }
		}
		TestEqual(TEXT("no vertex normal points down - the paint is visible from above"), Downward, 0);
		TestEqual(TEXT("and FDynamicMesh3 refused none of the triangles"), Mesh.TriangleCount(), Triangles);
	}

	// 2. AN INTERMEDIATE POSITION ADDS ONE DASHED BAR.
	TestTrue(TEXT("an intermediate position at the taxiway's far end"), Net->SetIntermediateHoldingPosition(FarEnd, true));
	FRoadMeshBuffers Both;
	TestEqual(TEXT("two positions painted"), FHoldingPositionMarkingBuilder::Build(*Net, Z, Both), 2);
	TestEqual(TEXT("one more dashed bar: 13 quads more"), Both.Indices.Num() / 3, Triangles + 13 * 2);

	return true;
}

#endif
