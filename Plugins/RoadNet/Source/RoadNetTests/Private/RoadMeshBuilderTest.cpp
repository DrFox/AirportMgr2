#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Build/RoadMeshBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** Signed area of a triangle projected on XY. Positive means CCW seen from +Z. */
	double TriangleArea2D(const FVector3d& A, const FVector3d& B, const FVector3d& C)
	{
		return 0.5 * ((B.X - A.X) * (C.Y - A.Y) - (B.Y - A.Y) * (C.X - A.X));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadMeshBuilderTest,
	"RoadNet.Build.MeshBuilder",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadMeshBuilderTest::RunTest(const FString& Parameters)
{
	constexpr double W = 1150.0;

	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Profile = URoadProfile::MakeTransient(W * 2.0, 1500.0);

	// A 90-degree bend, the case that failed in the original Blueprint system.
	const FRoadNodeId Centre = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId East   = Net->AddNode(FVector2D(40000.0, 0.0));
	const FRoadNodeId North  = Net->AddNode(FVector2D(0.0, 40000.0));
	const FRoadSegmentId ToEast  = Net->AddStraightSegment(Centre, East,  Profile);
	const FRoadSegmentId ToNorth = Net->AddStraightSegment(Centre, North, Profile);

	const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Net);
	TestTrue(TEXT("network solved"), Solved.FailedNodes == 0);

	FRoadMeshBuilder Builder(10.0);
	for (const TPair<int32, FJunctionResult>& Pair : Solved.NodeResults)
	{
		Builder.AddJunction(Pair.Value);
	}
	const int32 AfterJunctions = Builder.VertexCount();
	Builder.AddSegment(*Net, ToEast, 1);
	// East is a dead end (2-point rim, no triangles, AddJunction early-outs), so only
	// the B end is new: exactly 2 vertices. 4 would mean the A end failed to weld.
	TestEqual(TEXT("segment welded its A end into the junction"),
		Builder.VertexCount() - AfterJunctions, 2);
	Builder.AddSegment(*Net, ToNorth, 1);

	const FRoadMeshBuffers& Buffers = Builder.GetBuffers();

	// Shared invariant checks, reused below for the subdivided-ribbon buffer too.
	auto CheckMeshInvariants = [this](const FRoadMeshBuffers& CheckBuffers)
	{
		TestTrue(TEXT("mesh has vertices"), CheckBuffers.Positions.Num() > 0);
		TestTrue(TEXT("mesh has triangles"), CheckBuffers.Indices.Num() > 0);
		TestEqual(TEXT("indices come in threes"), CheckBuffers.Indices.Num() % 3, 0);

		for (const int32 Index : CheckBuffers.Indices)
		{
			TestTrue(TEXT("index in range"), Index >= 0 && Index < CheckBuffers.Positions.Num());
		}

		// Flat world: every vertex sits on the same plane.
		for (const FVector3d& P : CheckBuffers.Positions)
		{
			TestTrue(TEXT("vertex is finite"),
				FMath::IsFinite(P.X) && FMath::IsFinite(P.Y) && FMath::IsFinite(P.Z));
			TestTrue(TEXT("vertex is on the road plane"), FMath::IsNearlyEqual(P.Z, 10.0, 1e-9));
		}

		// Every triangle faces up. A wound-backwards triangle renders black or invisible.
		for (int32 Slot = 0; Slot + 2 < CheckBuffers.Indices.Num(); Slot += 3)
		{
			const double Area = TriangleArea2D(
				CheckBuffers.Positions[CheckBuffers.Indices[Slot]],
				CheckBuffers.Positions[CheckBuffers.Indices[Slot + 1]],
				CheckBuffers.Positions[CheckBuffers.Indices[Slot + 2]]);
			TestTrue(TEXT("triangle winds counter-clockwise"), Area > 0.0);
		}
	};

	CheckMeshInvariants(Buffers);

	// The weld can only fuse what the solver already shares. Assert that directly: the
	// CENTRE junction's own boundary polygon carries this segment's stored cut vertices
	// bitwise, not merely nearby. If this fails, the weld map is fusing nothing and every
	// "welded" assertion below is vacuously true.
	{
		const FRoadSegment* Seg = Net->GetSegment(ToEast);
		const FJunctionResult& CentreResult = Solved.NodeResults[Centre.Index];

		bool bBoundaryHasLeft  = false;
		bool bBoundaryHasRight = false;
		for (const FVector2D& Point : CentreResult.Boundary)
		{
			if (Point.X == Seg->LeftCutA.X  && Point.Y == Seg->LeftCutA.Y)  { bBoundaryHasLeft  = true; }
			if (Point.X == Seg->RightCutA.X && Point.Y == Seg->RightCutA.Y) { bBoundaryHasRight = true; }
		}
		TestTrue(TEXT("junction boundary carries the segment's left cut bitwise"),  bBoundaryHasLeft);
		TestTrue(TEXT("junction boundary carries the segment's right cut bitwise"), bBoundaryHasRight);
	}

	// THE POINT OF THE SLICE: the segment's end vertices and the junction's boundary
	// vertices are not merely coincident, they are the SAME vertex. Welding happens on
	// exact bits, so a crack cannot be represented in this buffer at all.
	{
		const FRoadSegment* Seg = Net->GetSegment(ToEast);

		int32 LeftMatches = 0;
		int32 RightMatches = 0;
		for (const FVector3d& P : Buffers.Positions)
		{
			if (P.X == Seg->LeftCutA.X && P.Y == Seg->LeftCutA.Y)  { ++LeftMatches; }
			if (P.X == Seg->RightCutA.X && P.Y == Seg->RightCutA.Y) { ++RightMatches; }
		}
		TestEqual(TEXT("left cut appears exactly once - welded, not duplicated"), LeftMatches, 1);
		TestEqual(TEXT("right cut appears exactly once - welded, not duplicated"), RightMatches, 1);
	}

	// No duplicate positions anywhere: welding is global, not per-primitive.
	{
		TSet<FVector2D> Seen;
		int32 Duplicates = 0;
		for (const FVector3d& P : Buffers.Positions)
		{
			const FVector2D Key(P.X, P.Y);
			if (Seen.Contains(Key)) { ++Duplicates; }
			Seen.Add(Key);
		}
		TestEqual(TEXT("no duplicated vertex positions"), Duplicates, 0);
	}

	// A sink receives what the builder holds.
	{
		struct FCountingSink : public IRoadMeshSink
		{
			int32 Vertices = 0;
			int32 Tris = 0;
			virtual void Accept(const FRoadMeshBuffers& In) override
			{
				Vertices = In.Positions.Num();
				Tris = In.Indices.Num() / 3;
			}
		};
		FCountingSink Sink;
		Builder.Emit(Sink);
		TestEqual(TEXT("sink got every vertex"), Sink.Vertices, Buffers.Positions.Num());
		TestEqual(TEXT("sink got every triangle"), Sink.Tris, Buffers.Indices.Num() / 3);
	}

	// The default RibbonSegments (8) never runs above: both AddSegment calls pass 1,
	// so Steps == 1 and the interior-lerp branch of AddSegment is never exercised.
	// Build a fresh pair of buffers - one unsubdivided, one subdivided - to cover it.
	{
		FRoadMeshBuilder Baseline(10.0);
		for (const TPair<int32, FJunctionResult>& Pair : Solved.NodeResults)
		{
			Baseline.AddJunction(Pair.Value);
		}
		Baseline.AddSegment(*Net, ToNorth, 1);

		FRoadMeshBuilder Subdivided(10.0);
		for (const TPair<int32, FJunctionResult>& Pair : Solved.NodeResults)
		{
			Subdivided.AddJunction(Pair.Value);
		}
		Subdivided.AddSegment(*Net, ToNorth, 8);

		CheckMeshInvariants(Subdivided.GetBuffers());
		TestTrue(TEXT("subdivided ribbon produced more vertices than the unsubdivided one"),
			Subdivided.VertexCount() > Baseline.VertexCount());
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
