#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Build/RoadMeshBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMesh/MeshNormals.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadMeshAttributeTest,
	"RoadNet.Build.MeshAttributes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadMeshAttributeTest::RunTest(const FString& Parameters)
{
	constexpr double TotalWidth = 800.0;
	constexpr double FilletRadius = 200.0;
	constexpr double TexelsPerUnit = 512.0;
	constexpr double ZHeight = 10.0;

	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Profile = URoadProfile::MakeTransient(TotalWidth, FilletRadius);

	// A bend, so there is a real junction with a fan as well as two dead ends.
	const FRoadNodeId Centre = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId East   = Net->AddNode(FVector2D(12000.0, 0.0));
	const FRoadNodeId North  = Net->AddNode(FVector2D(0.0, 12000.0));
	const FRoadSegmentId ToEast  = Net->AddStraightSegment(Centre, East,  Profile);
	const FRoadSegmentId ToNorth = Net->AddStraightSegment(Centre, North, Profile);

	const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Net);
	TestEqual(TEXT("every node solved"), Solved.FailedNodes, 0);

	// Segments FIRST, then junctions. The order is the contract: it decides who owns
	// UV1 at a shared cut vertex.
	FRoadMeshBuilder Builder(ZHeight, TexelsPerUnit);
	Builder.AddSegment(*Net, ToEast, 1);
	Builder.AddSegment(*Net, ToNorth, 1);
	for (const TPair<int32, FJunctionResult>& Pair : Solved.NodeResults)
	{
		Builder.AddJunction(Pair.Value);
	}

	const FRoadMeshBuffers& Buffers = Builder.GetBuffers();

	// Every channel stays parallel to Positions, or the sink cannot index them.
	TestEqual(TEXT("UV0 is parallel to positions"), Buffers.UV0.Num(), Buffers.Positions.Num());
	TestEqual(TEXT("UV1 is parallel to positions"), Buffers.UV1.Num(), Buffers.Positions.Num());
	TestEqual(TEXT("UV2 masks are parallel to positions"), Buffers.UV2.Num(), Buffers.Positions.Num());

	// UV0 is a pure function of world position. This is what makes the asphalt continuous
	// across a junction boundary for free (design spec 6.3) - it cannot disagree between a
	// junction and a segment, because it never depends on which one wrote it.
	for (int32 Index = 0; Index < Buffers.Positions.Num(); ++Index)
	{
		const FVector3d& P = Buffers.Positions[Index];
		TestEqual(FString::Printf(TEXT("UV0.X derives from X at vertex %d"), Index),
			Buffers.UV0[Index].X, static_cast<float>(P.X / TexelsPerUnit));
		TestEqual(FString::Printf(TEXT("UV0.Y derives from Y at vertex %d"), Index),
			Buffers.UV0[Index].Y, static_cast<float>(P.Y / TexelsPerUnit));
	}

	// `along` at the B end is the ribbon's length. Note this does NOT test the ordering
	// rule, whatever it may look like: East is a dead end, so its rim has two points,
	// Triangles comes back empty and AddJunction early-returns without ever touching
	// these vertices. It would pass in either order. The ordering rule is tested
	// explicitly further down.
	{
		const FRoadSegment* Seg = Net->GetSegment(ToEast);
		if (!TestNotNull(TEXT("east segment resolves"), Seg))
		{
			return false;
		}

		const double Length = FVector2D::Distance(
			Net->GetNodes()[Centre.Index].Position, Net->GetNodes()[East.Index].Position);
		const double ExpectedAlong = Length - Seg->TrimB - Seg->TrimA;

		int32 Found = INDEX_NONE;
		for (int32 Index = 0; Index < Buffers.Positions.Num(); ++Index)
		{
			if (Buffers.Positions[Index].X == Seg->LeftCutB.X &&
				Buffers.Positions[Index].Y == Seg->LeftCutB.Y)
			{
				Found = Index;
				break;
			}
		}

		if (TestTrue(TEXT("the B-end cut vertex is in the buffer"), Found != INDEX_NONE))
		{
			TestTrue(
				FString::Printf(TEXT("along at the B end is the ribbon length (got %f, want %f)"),
					Buffers.UV1[Found].Y, ExpectedAlong),
				FMath::IsNearlyEqual(static_cast<double>(Buffers.UV1[Found].Y), ExpectedAlong, 1.0));
		}
	}

	// Lateral is signed across the profile: left positive, right negative, and its
	// magnitude is the half-width. Checked on the A-end cut vertices, which are shared
	// with the Centre junction - so these are exactly the vertices the ordering rule is
	// about, and they must carry the SEGMENT's lateral rather than the junction's 0.
	//
	// Guarded on having found the vertex at all: an unguarded search that matches nothing
	// asserts nothing and reports green.
	{
		const FRoadSegment* Seg = Net->GetSegment(ToEast);
		const float HalfLeft  = static_cast<float>(Profile->GetHalfWidthLeft());
		const float HalfRight = static_cast<float>(Profile->GetHalfWidthRight());

		int32 LeftIndex = INDEX_NONE;
		int32 RightIndex = INDEX_NONE;
		for (int32 Index = 0; Index < Buffers.Positions.Num(); ++Index)
		{
			if (Buffers.Positions[Index].X == Seg->LeftCutA.X &&
				Buffers.Positions[Index].Y == Seg->LeftCutA.Y)
			{
				LeftIndex = Index;
			}
			if (Buffers.Positions[Index].X == Seg->RightCutA.X &&
				Buffers.Positions[Index].Y == Seg->RightCutA.Y)
			{
				RightIndex = Index;
			}
		}

		if (TestTrue(TEXT("the A-end left cut is in the buffer"), LeftIndex != INDEX_NONE))
		{
			TestEqual(TEXT("left rail lateral is +HalfWidthLeft"), Buffers.UV1[LeftIndex].X, HalfLeft);
		}
		if (TestTrue(TEXT("the A-end right cut is in the buffer"), RightIndex != INDEX_NONE))
		{
			TestEqual(TEXT("right rail lateral is -HalfWidthRight"), Buffers.UV1[RightIndex].X, -HalfRight);
		}
	}

	// Junction blend reaches full on the vertices a junction owns.
	{
		bool bFoundJunctionOwned = false;
		for (const FVector2f& Masks : Buffers.UV2)
		{
			if (Masks.X >= 1.0f)
			{
				bFoundJunctionOwned = true;
				break;
			}
		}
		TestTrue(TEXT("junction-owned vertices carry full junction blend"), bFoundJunctionOwned);
	}

	// Nothing may sit on the centreline unfaded except where a segment put it there.
	//
	// This is the assertion that catches painting a junction yellow: an arc sample is
	// welded by nobody, so if the junction gave it lateral 0 AND a blend of 0, the
	// marking mask reads it as centreline with nothing to fade it, and the fillet renders
	// as a solid fan of paint. Ribbon vertices never sit at lateral 0 - they are the two
	// rails - so any vertex that does is junction-owned and must be fully blended.
	{
		int32 UnfadedOnCentreline = 0;
		for (int32 Index = 0; Index < Buffers.Positions.Num(); ++Index)
		{
			if (FMath::Abs(Buffers.UV1[Index].X) < 1.0f && Buffers.UV2[Index].X < 1.0f)
			{
				++UnfadedOnCentreline;
			}
		}
		TestEqual(
			FString::Printf(TEXT("no unfaded vertex sits on the centreline (%d found)"),
				UnfadedOnCentreline),
			UnfadedOnCentreline, 0);
	}

	// Build() must produce exactly what the correct hand-ordering produces. It exists so
	// no caller has to remember the order, which only helps if it actually gets it right -
	// and a Build() that silently ordered them the other way would look identical here
	// were it not compared against a hand-built reference.
	{
		FRoadMeshBuilder Built(ZHeight, TexelsPerUnit);
		Built.Build(*Net, Solved, 1);

		const FRoadMeshBuffers& FromBuild = Built.GetBuffers();
		TestEqual(TEXT("Build produces the same vertex count"),
			FromBuild.Positions.Num(), Buffers.Positions.Num());
		TestEqual(TEXT("Build produces the same triangle count"),
			FromBuild.Indices.Num(), Buffers.Indices.Num());

		// Bitwise, because these are the same welded positions reached by the same route.
		int32 Mismatches = 0;
		const int32 Count = FMath::Min(FromBuild.Positions.Num(), Buffers.Positions.Num());
		for (int32 Index = 0; Index < Count; ++Index)
		{
			if (FromBuild.Positions[Index] != Buffers.Positions[Index] ||
				FromBuild.UV1[Index] != Buffers.UV1[Index] ||
				FromBuild.UV2[Index] != Buffers.UV2[Index])
			{
				++Mismatches;
			}
		}
		TestEqual(TEXT("Build matches the hand-ordered build vertex for vertex"), Mismatches, 0);
	}

	// THE ORDERING RULE, made executable. Build the same network junctions-first and the
	// shared cut vertices come out carrying the junction's lateral of 0 instead of the
	// segment's half-width. Without this, nothing in the suite fails when the order is
	// reversed - and the failure it causes is markings jumping at one end of every
	// segment, which no test asserts on and no exception reports.
	{
		FRoadMeshBuilder Wrong(ZHeight, TexelsPerUnit);
		for (const TPair<int32, FJunctionResult>& Pair : Solved.NodeResults)
		{
			Wrong.AddJunction(Pair.Value);
		}
		Wrong.AddSegment(*Net, ToEast, 1);
		Wrong.AddSegment(*Net, ToNorth, 1);

		const FRoadSegment* Seg = Net->GetSegment(ToEast);
		const FRoadMeshBuffers& Bad = Wrong.GetBuffers();

		int32 BadIndex = INDEX_NONE;
		for (int32 Index = 0; Index < Bad.Positions.Num(); ++Index)
		{
			if (Bad.Positions[Index].X == Seg->LeftCutA.X &&
				Bad.Positions[Index].Y == Seg->LeftCutA.Y)
			{
				BadIndex = Index;
				break;
			}
		}

		if (TestTrue(TEXT("the shared cut vertex exists in the wrong-order build"),
				BadIndex != INDEX_NONE))
		{
			TestNotEqual(
				TEXT("junctions-first loses the segment's lateral at a shared cut vertex"),
				Bad.UV1[BadIndex].X, static_cast<float>(Profile->GetHalfWidthLeft()));
		}
	}

	// The overlays the material actually samples. The buffers being right proves nothing
	// about what the component receives - that gap is exactly where slice 2a's invisible
	// surface hid, so it gets a test of its own.
	{
		UE::Geometry::FDynamicMesh3 Mesh;
		for (const FVector3d& Position : Buffers.Positions)
		{
			Mesh.AppendVertex(Position);
		}
		for (int32 Slot = 0; Slot + 2 < Buffers.Indices.Num(); Slot += 3)
		{
			Mesh.AppendTriangle(
				Buffers.Indices[Slot], Buffers.Indices[Slot + 1], Buffers.Indices[Slot + 2]);
		}

		// THE SURFACE MUST FACE UP, measured the way Unreal measures it.
		//
		// Every winding check in this suite computed a 2D signed area and demanded it be
		// positive - the maths convention. Unreal is left-handed and VectorUtil::Normal
		// takes cross(C-A, B-A), so those triangles faced DOWN and were backface-culled by
		// every single-sided material. It stayed invisible for a whole slice because the
		// placeholder colour override substitutes Unreal's two-sided vertex-colour debug
		// material, so the first real material was the first thing to look.
		//
		// Asserting on the normal the engine actually computes cannot make that mistake:
		// it measures the property that matters rather than a convention chosen by hand.
		UE::Geometry::FMeshNormals::QuickComputeVertexNormals(Mesh);
		int32 DownwardNormals = 0;
		for (const int32 VertexId : Mesh.VertexIndicesItr())
		{
			if (Mesh.GetVertexNormal(VertexId).Z <= 0.0f)
			{
				++DownwardNormals;
			}
		}
		TestEqual(
			FString::Printf(TEXT("every vertex normal points up (%d of %d point down)"),
				DownwardNormals, Mesh.VertexCount()),
			DownwardNormals, 0);

		FDynamicMeshSink::PopulateAttributes(Mesh, Buffers);

		if (!TestTrue(TEXT("attributes are enabled"), Mesh.HasAttributes()))
		{
			return false;
		}
		TestEqual(TEXT("three UV layers exist"), Mesh.Attributes()->NumUVLayers(), 3);

		// No colour overlay, deliberately. A UDynamicMeshComponent reads its colour overlay
		// the moment ColorOverrideMode leaves Constant - which assigning any material does -
		// and reading this one stopped the surface rendering at all. The masks live in UV2
		// so that path is never entered.
		TestFalse(TEXT("no colour overlay is created"), Mesh.Attributes()->HasPrimaryColors());

		const UE::Geometry::FDynamicMeshUVOverlay* UV0Layer = Mesh.Attributes()->GetUVLayer(0);
		const UE::Geometry::FDynamicMeshUVOverlay* UV1Layer = Mesh.Attributes()->GetUVLayer(1);

		TestEqual(TEXT("UV0 has one element per vertex"), UV0Layer->ElementCount(), Buffers.Positions.Num());
		TestEqual(TEXT("UV1 has one element per vertex"), UV1Layer->ElementCount(), Buffers.Positions.Num());

		// Every triangle must be set in both layers, or that triangle samples nothing.
		int32 UnsetUV0 = 0;
		int32 UnsetUV1 = 0;
		for (const int32 TriangleId : Mesh.TriangleIndicesItr())
		{
			if (!UV0Layer->IsSetTriangle(TriangleId)) { ++UnsetUV0; }
			if (!UV1Layer->IsSetTriangle(TriangleId)) { ++UnsetUV1; }
		}
		TestEqual(TEXT("every triangle has UV0"), UnsetUV0, 0);
		TestEqual(TEXT("every triangle has UV1"), UnsetUV1, 0);

		// UV2 carries the junction fade, so it needs the same coverage: a triangle corner
		// without it samples nothing and the fade stops working there.
		const UE::Geometry::FDynamicMeshUVOverlay* UV2Layer = Mesh.Attributes()->GetUVLayer(2);
		TestEqual(TEXT("UV2 has one element per vertex"),
			UV2Layer->ElementCount(), Buffers.Positions.Num());

		int32 UnsetUV2 = 0;
		for (const int32 TriangleId : Mesh.TriangleIndicesItr())
		{
			if (!UV2Layer->IsSetTriangle(TriangleId)) { ++UnsetUV2; }
		}
		TestEqual(TEXT("every triangle has UV2"), UnsetUV2, 0);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
