#include "CoreMinimal.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadMeshBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProfileFallbackTest,
	"Airside.Build.ProfileFallback",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FProfileFallbackTest::RunTest(const FString& Parameters)
{
	// A SEGMENT WITH NO PROFILE OF ITS OWN IS WHAT A SAVED LEVEL RELOADS AS.
	//
	// ARoadNetworkActor made its fallback profile with NewObject in the TRANSIENT package,
	// so it was never saved. The pointer each segment held serialised as null, and a level
	// holding four roads came back as four segments and zero triangles - the roads still in
	// the model, invisible, with nothing in the log to say why.
	//
	// Null was read TWICE and handled two ways: the solver took zero half-widths, so the
	// junctions collapsed; the builder dropped the segment, so there was no ribbon. Both
	// happened to mean "nothing", which is exactly how two readers of one fact hide from
	// each other. URoadNetwork::ProfileFor is now the only reader.
	constexpr double Width = 2300.0;

	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Fallback = URoadProfile::MakeTransient(Width, 1500.0, Width * 0.1);

	const FRoadNodeId West   = Net->AddNode(FVector2D(-20000.0, 0.0));
	const FRoadNodeId Centre = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId North  = Net->AddNode(FVector2D(0.0, 20000.0));

	// nullptr deliberately: this is the state a reloaded level is in, not a state a tool
	// can produce. Passing a profile here would test the case that already worked.
	Net->AddStraightSegment(West, Centre, nullptr);
	Net->AddStraightSegment(Centre, North, nullptr);

	// 1. Without a default there is still nothing, and that must stay true. A network with
	//    no profile anywhere has no width to draw and inventing one would put a road on
	//    screen that nothing in the model describes.
	{
		FRoadSolveResult Bare = FRoadNetworkSolver::SolveAll(*Net);
		FRoadMeshBuilder Builder(10.0);
		Builder.Build(*Net, Bare);

		TestEqual(TEXT("with no profile anywhere, nothing is drawn"),
			Builder.GetBuffers().Indices.Num() / 3, 0);
	}

	// 2. THE MEASUREMENT. With a default, the same segments build a real surface.
	{
		Net->DefaultProfile = Fallback;

		const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Net);
		TestEqual(TEXT("every node solves"), Solved.FailedNodes, 0);

		FRoadMeshBuilder Builder(10.0);
		Builder.Build(*Net, Solved);

		const int32 Triangles = Builder.GetBuffers().Indices.Num() / 3;
		TestTrue(FString::Printf(
			TEXT("segments with no profile of their own still build (%d triangles)"), Triangles),
			Triangles > 0);
	}

	// 3. AND THE SOLVER AGREES WITH THE BUILDER, which is the whole reason the fallback
	//    lives on the network rather than in each of them.
	//
	//    Measured on the junction boundary rather than on the triangle count: a builder
	//    given the fallback while the solver was not would still emit a ribbon, and it
	//    would meet a junction cut back to zero width. That reads as a gap in the road,
	//    which is a seam bug - the class of defect this project has paid most for.
	{
		Net->DefaultProfile = Fallback;
		const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Net);

		// Measured on the segment's own cut line, which is where the solver writes the
		// width it used. Left cut to right cut IS the road's width there; at zero
		// half-widths the two collapse onto each other, which is the "13 vertices, 0
		// triangles" the log reported.
		double WidestCut = 0.0;
		for (const FRoadSegment& Segment : Net->GetSegments())
		{
			if (Segment.bAlive)
			{
				WidestCut = FMath::Max(WidestCut,
					FVector2D::Distance(Segment.LeftCutA, Segment.RightCutA));
			}
		}

		TestTrue(FString::Printf(
			TEXT("the solver cut to a real width, not collapsed (%.0f uu across)"), WidestCut),
			WidestCut > Width * 0.5);
	}

	// 4. AND SO DOES THE MARKING GEOMETRY, which is the half of "agrees" that triangle
	//    counts cannot see.
	//
	//    UV1.X is the lateral offset in uu, and M_RoadSurface reads it to place the
	//    centreline: mask = 1 - saturate((|lateral| - CentrelineWidth) * MarkingSharpness).
	//    Where the builder took a null profile it got no bands, so every lateral came out
	//    zero - and a mask that is 1 everywhere paints the ENTIRE road MarkingColor. The
	//    road was the right shape, in the right material slot, and solid yellow.
	//
	//    That is why this is asserted here and not left to the triangle count: the geometry
	//    was correct throughout, because the SOLVER read ProfileFor while the BUILDER read
	//    Segment->Profile. Two readers of one fact again, exactly as the header of this
	//    test describes - the fix landed on the solver and the builder kept its own copy.
	{
		Net->DefaultProfile = Fallback;
		const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Net);

		FRoadMeshBuilder Builder(10.0);
		Builder.Build(*Net, Solved);

		float WidestLateral = 0.0f;
		for (const FVector2f& UV : Builder.GetBuffers().UV1)
		{
			WidestLateral = FMath::Max(WidestLateral, FMath::Abs(UV.X));
		}

		// Most of a half-width: the outermost band boundary sits at exactly half the road,
		// so anything near it proves the bands were read. Zero is the failure that shipped.
		TestTrue(FString::Printf(
			TEXT("a fallback profile still carries lateral UVs, so the centreline marking ")
			TEXT("is a line and not the whole road (widest |UV1.X| = %.0f uu of %.0f)"),
			WidestLateral, Width * 0.5),
			WidestLateral > Width * 0.4);
	}

	// 5. A segment that HAS a profile keeps it. The fallback is for the ones that lost
	//    theirs, and must never override an authored one.
	{
		URoadProfile* Narrow = URoadProfile::MakeTransient(Width * 0.5, 1500.0);
		const FRoadNodeId East = Net->AddNode(FVector2D(20000.0, 0.0));
		const FRoadSegmentId Own = Net->AddStraightSegment(Centre, East, Narrow);

		const FRoadSegment* Segment = Net->GetSegment(Own);
		if (TestNotNull(TEXT("the segment is there"), Segment))
		{
			TestEqual(TEXT("its own profile wins over the network's default"),
				Net->ProfileFor(*Segment), static_cast<const URoadProfile*>(Narrow));
		}
	}

	return true;
}

/**
 * THE THIRD READER. ProfileFallback above pinned the solver and the mesh builder to
 * URoadNetwork::ProfileFor; the guideline builder was written afterwards and read
 * Segment.Profile itself, so a level the player built in the editor, saved and reloaded
 * came back with every taxiway PAVED and none of them ROUTABLE: 16 segments, 2 derived
 * centrelines (the runways, whose profile is an asset), 42 stand lead-ins joining nothing,
 * and every arrival refused for "no route to a stand" (M_Starter, 2026-09-06). The mesh
 * had hidden the loss - the road looked right, so nobody asked whether the line under it
 * was there.
 *
 * Named without a dot under ProfileFallback: the automation tree drops a bare-named test
 * the moment a dotted child of it exists.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGuidelineProfileFallbackTest,
	"Airside.Build.GuidelineProfileFallback",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGuidelineProfileFallbackTest::RunTest(const FString& Parameters)
{
	constexpr double Width = 2300.0;

	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Fallback = URoadProfile::MakeTransient(Width, 1500.0, Width * 0.1);

	const FRoadNodeId West   = Net->AddNode(FVector2D(-20000.0, 0.0));
	const FRoadNodeId Centre = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId North  = Net->AddNode(FVector2D(0.0, 20000.0));

	// nullptr deliberately, as in ProfileFallback: the reloaded state, not a tool's.
	const FRoadSegmentId Arm1 = Net->AddStraightSegment(West, Centre, nullptr);
	const FRoadSegmentId Arm2 = Net->AddStraightSegment(Centre, North, nullptr);
	Net->DefaultProfile = Fallback;

	const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Net);
	TestEqual(TEXT("every node solves"), Solved.FailedNodes, 0);
	FRoadGuidelineBuilder::Build(*Net, Solved);

	// 1. THE MEASUREMENT. Each profile-less segment gets the centreline the fallback
	//    declares, exactly as it gets the fallback's ribbon.
	int32 FromArm1 = 0, FromArm2 = 0, TurnPaths = 0;
	for (const FGuidelineEdge& Edge : Net->GetGuidelineEdges())
	{
		if (!Edge.bAlive || !Edge.bDerived) { continue; }
		FromArm1 += Edge.DerivedFrom == Arm1 ? 1 : 0;
		FromArm2 += Edge.DerivedFrom == Arm2 ? 1 : 0;
		TurnPaths += Edge.DerivedFrom.IsSet() ? 0 : 1;
	}
	TestEqual(TEXT("a segment with no profile of its own derives the fallback's centreline (arm 1)"), FromArm1, 1);
	TestEqual(TEXT("a segment with no profile of its own derives the fallback's centreline (arm 2)"), FromArm2, 1);

	// 2. And the junction between two such segments gets its turn paths, which read BOTH
	//    ends' profiles and skipped the junction when either was null. One per direction
	//    of the bidirectional centreline.
	TestTrue(FString::Printf(TEXT("the junction between two fallback segments is turnable (%d turn path(s))"), TurnPaths),
		TurnPaths >= 1);

	// 3. A segment that HAS a profile still derives from its own, never the fallback -
	//    measured by width, which is where an authored narrow profile differs.
	{
		URoadProfile* Narrow = URoadProfile::MakeTransient(Width * 0.5, 1500.0, Width * 0.05);
		// MakeTransient leaves the centreline width at 0, the same as the fallback's; give
		// the authored one a width so the assertion below can tell the two apart.
		Narrow->Guidelines[0].Width = 500.0;
		const FRoadNodeId East = Net->AddNode(FVector2D(20000.0, 0.0));
		const FRoadSegmentId Own = Net->AddStraightSegment(Centre, East, Narrow);
		const FRoadSolveResult Again = FRoadNetworkSolver::SolveAll(*Net);
		FRoadGuidelineBuilder::Build(*Net, Again);

		const FGuidelineEdge* OwnLine = nullptr;
		for (const FGuidelineEdge& Edge : Net->GetGuidelineEdges())
		{
			if (Edge.bAlive && Edge.bDerived && Edge.DerivedFrom == Own) { OwnLine = &Edge; }
		}
		if (TestNotNull(TEXT("the authored segment derives a centreline too"), OwnLine))
		{
			TestEqual(TEXT("its centreline carries its OWN profile's width, not the fallback's"),
				OwnLine->Width, Narrow->Guidelines[0].Width);
			TestNotEqual(TEXT("and the two profiles do differ, so the assertion above discriminates"),
				Narrow->Guidelines[0].Width, Fallback->Guidelines[0].Width);
		}
	}

	return true;
}

#endif
