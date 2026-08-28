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

	// The junction's shoulder fade. A junction's rim IS its outer edge and the only vertex
	// inboard of it is the fan apex, so fading rim-to-apex would fade the whole junction; a
	// ring of rim vertices pushed toward the apex is what gives the shoulder somewhere to
	// end.
	//
	// Both assertions below are written to be FALSE before the ring exists. The obvious
	// phrasing - "count solid vertices near the apex" - is satisfied by the segments' own
	// band vertices, which carry blend 1 and sit well inside this radius, so it passes
	// whether or not a ring was ever built and proves nothing.
	{
		const FJunctionResult* CentreResult = Solved.NodeResults.Find(Centre.Index);
		if (!TestNotNull(TEXT("centre node solved"), CentreResult))
		{
			return false;
		}

		const int32 ApexSlot = CentreResult->Boundary.Num() - 1;
		const FVector2D Apex = CentreResult->Boundary[ApexSlot];

		// An arc sample is a rim vertex no segment owns - it matches no arm's cut vertex
		// bitwise - so its ground blend is the junction's alone. Before the fade it was
		// solid; a faded one can only have come from the rim fade.
		{
			int32 ArcSamples = 0;
			int32 FadedArcSamples = 0;

			for (int32 Slot = 0; Slot < ApexSlot; ++Slot)
			{
				const FVector2D& Point = CentreResult->Boundary[Slot];

				bool bIsCutVertex = false;
				for (const FJunctionArmResult& Arm : CentreResult->Arms)
				{
					if ((Point.X == Arm.RightCut.X && Point.Y == Arm.RightCut.Y) ||
						(Point.X == Arm.LeftCut.X  && Point.Y == Arm.LeftCut.Y))
					{
						bIsCutVertex = true;
						break;
					}
				}
				if (bIsCutVertex)
				{
					continue;
				}

				++ArcSamples;
				for (int32 Index = 0; Index < Buffers.Positions.Num(); ++Index)
				{
					if (Buffers.Positions[Index].X == Point.X &&
						Buffers.Positions[Index].Y == Point.Y)
					{
						if (Buffers.UV2[Index].Y <= 0.0f)
						{
							++FadedArcSamples;
						}
						break;
					}
				}
			}

			TestTrue(TEXT("the bend's rim has arc samples to fade"), ArcSamples > 0);
			TestEqual(TEXT("every arc sample on the rim fades to nothing"),
				FadedArcSamples, ArcSamples);
		}

		// The ring itself. Before it exists the apex is the ONLY mesh vertex inboard of the
		// rim, so a vertex that is nearer the apex than every rim vertex and is not the apex
		// cannot exist. A fold - the ring overshooting the apex - would show up as a
		// backfacing triangle, which the check above already forbids across the whole buffer.
		{
			double NearestRim = TNumericLimits<double>::Max();
			for (int32 Slot = 0; Slot < ApexSlot; ++Slot)
			{
				NearestRim = FMath::Min(
					NearestRim, FVector2D::Distance(CentreResult->Boundary[Slot], Apex));
			}

			int32 Inboard = 0;
			for (int32 Index = 0; Index < Buffers.Positions.Num(); ++Index)
			{
				const FVector2D Flat(Buffers.Positions[Index].X, Buffers.Positions[Index].Y);
				const double ToApex = FVector2D::Distance(Flat, Apex);

				// Strictly between the apex and the innermost rim vertex.
				if (ToApex > 0.0 && ToApex < NearestRim)
				{
					++Inboard;
					TestTrue(TEXT("an inboard ring vertex is solid"),
						Buffers.UV2[Index].Y >= 1.0f);
				}
			}

			TestTrue(TEXT("the junction has a ring of vertices inboard of its whole rim"),
				Inboard > 0);
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
