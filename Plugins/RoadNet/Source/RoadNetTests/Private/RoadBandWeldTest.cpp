#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Build/RoadMeshBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Build/RoadProfileBands.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadBandWeldTest,
	"RoadNet.Build.BandWeld",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadBandWeldTest::RunTest(const FString& Parameters)
{
	constexpr double Width = 800.0;
	constexpr double Shoulder = 120.0;

	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Profile = URoadProfile::MakeTransient(Width, 200.0, Shoulder);

	const FRoadNodeId Centre = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId East   = Net->AddNode(FVector2D(12000.0, 0.0));
	const FRoadNodeId North  = Net->AddNode(FVector2D(0.0, 12000.0));
	const FRoadSegmentId ToEast  = Net->AddStraightSegment(Centre, East,  Profile);
	Net->AddStraightSegment(Centre, North, Profile);

	const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Net);
	TestEqual(TEXT("every node solved"), Solved.FailedNodes, 0);

	FRoadMeshBuilder Builder(10.0);
	Builder.Build(*Net, Solved, 3);
	const FRoadMeshBuffers& Buffers = Builder.GetBuffers();

	// THE CONTRACT, extended to bands. A band vertex is not stored anywhere: the ribbon
	// and the junction rim each derive it from the same two cut vertices through
	// CutLinePoint. If they ever stop agreeing bitwise, the shoulder tears open along
	// every cut line - the same seam this project exists to make unrepresentable, one
	// step inboard of where slice 2a proved it closed.
	{
		const FRoadSegment* Seg = Net->GetSegment(ToEast);
		if (!TestNotNull(TEXT("east segment resolves"), Seg))
		{
			return false;
		}

		const FRoadProfileBands Bands = FRoadProfileBands::FromProfile(Profile);
		TestEqual(TEXT("shouldered profile gives four boundaries"), Bands.Alphas.Num(), 4);

		for (int32 Boundary = 0; Boundary < Bands.Alphas.Num(); ++Boundary)
		{
			const FVector2D Expected = FRoadMeshBuilder::CutLinePoint(
				Seg->RightCutA, Seg->LeftCutA, Bands.Alphas[Boundary]);

			int32 Matches = 0;
			for (const FVector3d& P : Buffers.Positions)
			{
				if (P.X == Expected.X && P.Y == Expected.Y)
				{
					++Matches;
				}
			}

			// Exactly one: the ribbon and the rim both produced it and it welded.
			TestEqual(
				FString::Printf(TEXT("band boundary %d is present exactly once"), Boundary),
				Matches, 1);
		}
	}

	// The ground blend reaches 0 somewhere - the shoulder's outer edge - and 1 elsewhere.
	// Without both, the fade either does not exist or swallows the whole road.
	{
		bool bFoundFaded = false;
		bool bFoundSolid = false;
		for (const FVector2f& Masks : Buffers.UV2)
		{
			if (Masks.Y <= 0.0f) { bFoundFaded = true; }
			if (Masks.Y >= 1.0f) { bFoundSolid = true; }
		}
		TestTrue(TEXT("some vertex fades to nothing"), bFoundFaded);
		TestTrue(TEXT("some vertex stays solid"), bFoundSolid);
	}

	// Facing is unchanged by subdivision. Unreal's front face is the opposite winding to
	// the maths convention, so front-facing means NEGATIVE 2D signed area.
	{
		int32 Backfacing = 0;
		for (int32 Slot = 0; Slot + 2 < Buffers.Indices.Num(); Slot += 3)
		{
			const FVector3d& A = Buffers.Positions[Buffers.Indices[Slot]];
			const FVector3d& B = Buffers.Positions[Buffers.Indices[Slot + 1]];
			const FVector3d& C = Buffers.Positions[Buffers.Indices[Slot + 2]];
			if (0.5 * ((B.X - A.X) * (C.Y - A.Y) - (B.Y - A.Y) * (C.X - A.X)) >= 0.0)
			{
				++Backfacing;
			}
		}
		TestEqual(TEXT("no backfacing triangle after subdivision"), Backfacing, 0);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
