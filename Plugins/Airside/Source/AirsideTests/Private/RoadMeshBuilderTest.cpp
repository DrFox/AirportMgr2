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
	"Airside.Build.MeshBuilder",
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

	// Junctions BEFORE segments here, deliberately, and against the rule the production
	// rebuild paths follow. That rule exists so a segment owns UV1 at a shared cut
	// vertex; this file asserts nothing about UV1, and its vertex-count arithmetic below
	// measures welding by adding a segment to a builder that ALREADY holds its junction.
	// Reversing the order would not break welding, but it would destroy the property this
	// test is built to prove.
	FRoadMeshBuilder Builder(10.0);
	for (const TPair<int32, FJunctionResult>& Pair : Solved.NodeResults)
	{
		static const TArray<FRoadSegmentId> NoArms;
		const TArray<FRoadSegmentId>* Arms = Solved.NodeArmSegments.Find(Pair.Key);
		Builder.AddJunction(*Net, Pair.Key, Pair.Value, Arms ? *Arms : NoArms);
	}
	const int32 AfterJunctions = Builder.VertexCount();
	Builder.AddSegment(*Net, ToEast, 1);
	// East is a dead end (2-point rim, no triangles, AddJunction early-outs), so the A
	// end welds into Centre's junction (0 new) and the B end's ribbon cut line is new
	// (2). AddSegment also now builds the dead-end cap at East's own node position: two
	// more new vertices, offset from East by the profile's left/right half-widths (the
	// ribbon's own B-end vertices are cut back from East by the profile's half-width, so
	// the cap's vertices are necessarily distinct from them). Total: 2 + 2 = 4.
	// AddSegment now clamps to a minimum of three steps, so the ribbon has two INTERIOR
	// cross-sections as well as its two ends - that is where the junction blend holds 0 so
	// a centreline can survive in the middle of the segment. Two interior cross-sections
	// times two rails is 4 more vertices on top of the 4 above: 8.
	TestEqual(TEXT("segment welded its A end and built East's dead-end cap"),
		Builder.VertexCount() - AfterJunctions, 8);

	// The cap must actually reach the node, not just approach it.
	{
		const FRoadNode* EastNode = Net->GetNode(East);
		if (TestNotNull(TEXT("East node exists"), EastNode))
		{
			bool bReachesNode = false;
			for (const FVector3d& P : Builder.GetBuffers().Positions)
			{
				if (P.X == EastNode->Position.X)
				{
					bReachesNode = true;
					break;
				}
			}
			TestTrue(TEXT("mesh reaches the dead-end node's own X"), bReachesNode);
		}
	}

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
			// Unreal's front face is the OPPOSITE winding to the maths convention: it is
		// left-handed, so VectorUtil::Normal takes cross(C-A, B-A) and a triangle with
		// positive 2D signed area faces DOWN and is backface-culled. FRoadMeshBuilder
		// ::AddTriangle emits the swapped winding for that reason, so front-facing here
		// means NEGATIVE area. Asserting the maths convention is what let a whole slice
		// ship with every road facing the ground.
			TestTrue(TEXT("triangle faces up in Unreal's winding"), Area < 0.0);
		}
	};

	CheckMeshInvariants(Buffers);

	// Both blocks below key off the same segment; look it up once rather than repeating
	// the call in each scope, and fail loudly instead of crashing if it is ever missing.
	const FRoadSegment* ToEastSeg = Net->GetSegment(ToEast);
	if (!TestNotNull(TEXT("ToEast segment exists"), ToEastSeg))
	{
		return false;
	}

	// The weld can only fuse what the solver already shares. Assert that directly: the
	// CENTRE junction's own boundary polygon carries this segment's stored cut vertices
	// bitwise, not merely nearby. If this fails, the weld map is fusing nothing and every
	// "welded" assertion below is vacuously true.
	{
		const FJunctionResult* CentreResult = Solved.NodeResults.Find(Centre.Index);
		if (!TestNotNull(TEXT("centre node has a solved junction result"), CentreResult))
		{
			return false;
		}

		bool bBoundaryHasLeft  = false;
		bool bBoundaryHasRight = false;
		for (const FVector2D& Point : CentreResult->Boundary)
		{
			if (Point.X == ToEastSeg->LeftCutA.X  && Point.Y == ToEastSeg->LeftCutA.Y)  { bBoundaryHasLeft  = true; }
			if (Point.X == ToEastSeg->RightCutA.X && Point.Y == ToEastSeg->RightCutA.Y) { bBoundaryHasRight = true; }
		}
		TestTrue(TEXT("junction boundary carries the segment's left cut bitwise"),  bBoundaryHasLeft);
		TestTrue(TEXT("junction boundary carries the segment's right cut bitwise"), bBoundaryHasRight);
	}

	// THE POINT OF THE SLICE: the segment's end vertices and the junction's boundary
	// vertices are not merely coincident, they are the SAME vertex. Welding happens on
	// exact bits, so a crack cannot be represented in this buffer at all.
	{
		const FRoadSegment* Seg = ToEastSeg;

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
		// Normalise -0.0 to 0.0 exactly like WeldVertex does, so this check has the same
		// blind spot closed rather than reintroducing it: operator== on FVector2D treats
		// -0.0 and +0.0 as equal, but a raw TSet<FVector2D> key hashes them to different
		// buckets, which would otherwise let a real duplicate slip past "no duplicates".
		auto NormalizeSignedZero = [](double Value) { return Value == 0.0 ? 0.0 : Value; };

		TSet<FVector2D> Seen;
		int32 Duplicates = 0;
		for (const FVector3d& P : Buffers.Positions)
		{
			const FVector2D Key(NormalizeSignedZero(P.X), NormalizeSignedZero(P.Y));
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
			static const TArray<FRoadSegmentId> NoArms;
			const TArray<FRoadSegmentId>* Arms = Solved.NodeArmSegments.Find(Pair.Key);
			Baseline.AddJunction(*Net, Pair.Key, Pair.Value, Arms ? *Arms : NoArms);
		}
		Baseline.AddSegment(*Net, ToNorth, 1);

		FRoadMeshBuilder Subdivided(10.0);
		for (const TPair<int32, FJunctionResult>& Pair : Solved.NodeResults)
		{
			static const TArray<FRoadSegmentId> NoArms;
			const TArray<FRoadSegmentId>* Arms = Solved.NodeArmSegments.Find(Pair.Key);
			Subdivided.AddJunction(*Net, Pair.Key, Pair.Value, Arms ? *Arms : NoArms);
		}
		Subdivided.AddSegment(*Net, ToNorth, 8);

		CheckMeshInvariants(Subdivided.GetBuffers());
		TestTrue(TEXT("subdivided ribbon produced more vertices than the unsubdivided one"),
			Subdivided.VertexCount() > Baseline.VertexCount());
	}

	// The A-end cap branch in AddSegment never runs in any test above: every dead end
	// there sits at the segment's B end, so its correctness has rested on review
	// inspection alone. Author a segment the other way around - the dead end is the
	// segment's A end - and assert the same invariants plus that its cap vertices
	// appear. A separate network and builder keep the vertex-count assertions above
	// intact.
	{
		URoadNetwork* AEndNet = NewObject<URoadNetwork>(GetTransientPackage());
		URoadProfile* AEndProfile = URoadProfile::MakeTransient(W * 2.0, 1500.0);

		const FRoadNodeId AEndCentre = AEndNet->AddNode(FVector2D(100000.0, 0.0));
		const FRoadNodeId AEndEast   = AEndNet->AddNode(FVector2D(140000.0, 0.0));
		const FRoadNodeId AEndSouth  = AEndNet->AddNode(FVector2D(100000.0, -40000.0));

		// AEndCentre gets two arms, making it a real junction with no cap of its own.
		// SouthToCentre is authored South-to-Centre, so South - the dead end - is the
		// segment's A end: exactly the direction nothing else in this file exercises.
		AEndNet->AddStraightSegment(AEndCentre, AEndEast, AEndProfile);
		const FRoadSegmentId SouthToCentre =
			AEndNet->AddStraightSegment(AEndSouth, AEndCentre, AEndProfile);

		const FRoadSolveResult AEndSolved = FRoadNetworkSolver::SolveAll(*AEndNet);
		TestTrue(TEXT("A-end cap network solved"), AEndSolved.FailedNodes == 0);

		FRoadMeshBuilder AEndBuilder(10.0);
		for (const TPair<int32, FJunctionResult>& Pair : AEndSolved.NodeResults)
		{
			static const TArray<FRoadSegmentId> NoArms;
			const TArray<FRoadSegmentId>* Arms = AEndSolved.NodeArmSegments.Find(Pair.Key);
			AEndBuilder.AddJunction(*AEndNet, Pair.Key, Pair.Value, Arms ? *Arms : NoArms);
		}
		const int32 AEndAfterJunctions = AEndBuilder.VertexCount();
		AEndBuilder.AddSegment(*AEndNet, SouthToCentre, 1);

		// Mirrors the East-cap arithmetic above: the B end (AEndCentre) welds into the
		// junction fan already built (0 new), the A end's own ribbon cut line is new (2),
		// the A-end cap at AEndSouth's own node position adds 2 more, and the two interior
		// cross-sections forced by the three-step minimum add 4. Total: 8.
		TestEqual(TEXT("A-end cap adds the same 8 vertices as the B-end cap"),
			AEndBuilder.VertexCount() - AEndAfterJunctions, 8);

		CheckMeshInvariants(AEndBuilder.GetBuffers());

		// The arm runs north-south, so the cap's two new vertices are offset from the
		// node in X, not Y (unlike the earlier east-west case, which offset in Y): a
		// vertex sharing the node's own Y is the cap reaching it.
		const FRoadNode* AEndSouthNode = AEndNet->GetNode(AEndSouth);
		if (TestNotNull(TEXT("A-end south node exists"), AEndSouthNode))
		{
			bool bReachesNode = false;
			for (const FVector3d& P : AEndBuilder.GetBuffers().Positions)
			{
				if (P.Y == AEndSouthNode->Position.Y)
				{
					bReachesNode = true;
					break;
				}
			}
			TestTrue(TEXT("A-end cap reaches the dead-end node's own Y"), bReachesNode);
		}
	}

	// A node's solve failure only clears ITS OWN end's flag (bSolvedA or bSolvedB), so a
	// segment solved at only one end must still emit nothing - the other flag being
	// stranded true is exactly what would draw a triangle out to the world origin.
	// Stand in for that half-solved state directly, the way RoadNetworkTest.cpp already
	// stands in for the solver.
	{
		URoadNetwork* HalfNet = NewObject<URoadNetwork>(GetTransientPackage());
		URoadProfile* HalfProfile = URoadProfile::MakeTransient(W * 2.0, 1500.0);

		const FRoadNodeId P = HalfNet->AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId Q = HalfNet->AddNode(FVector2D(10000.0, 0.0));
		const FRoadSegmentId HalfSolved = HalfNet->AddStraightSegment(P, Q, HalfProfile);

		FRoadSegment* Mutable = HalfNet->GetSegmentMutable(HalfSolved);
		if (TestNotNull(TEXT("half-solved segment exists"), Mutable))
		{
			Mutable->LeftCutA = FVector2D(100.0, 100.0);
			Mutable->RightCutA = FVector2D(100.0, -100.0);
			Mutable->bSolvedA = true;
			// bSolvedB deliberately left false: only the A end solved.

			FRoadMeshBuilder HalfBuilder(10.0);
			HalfBuilder.AddSegment(*HalfNet, HalfSolved, 1);

			TestEqual(TEXT("half-solved segment adds no vertices"), HalfBuilder.VertexCount(), 0);
			TestEqual(TEXT("half-solved segment adds no indices"), HalfBuilder.GetBuffers().Indices.Num(), 0);
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
