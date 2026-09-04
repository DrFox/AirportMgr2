#include "CoreMinimal.h"
#include "Build/RoadMeshBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Misc/AutomationTest.h"
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

	// 4. A segment that HAS a profile keeps it. The fallback is for the ones that lost
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

#endif
