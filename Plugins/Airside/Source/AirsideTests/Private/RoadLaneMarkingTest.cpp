#include "CoreMinimal.h"
#include "Build/RoadLaneMarkingBuilder.h"
#include "Build/RoadMeshSink.h"
#include "Build/RoadNetworkSolver.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoadLaneMarkingTest, "Airside.Build.LaneMarking",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadLaneMarkingTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FRoadNodeId A = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId B = Net->AddNode(FVector2D(20000.0, 0.0));
	const FRoadSegmentId Seg = Net->AddStraightSegment(A, B, URoadProfile::MakeServiceRoadTransient());
	// A taxiway beside it, which must paint nothing.
	Net->AddStraightSegment(Net->AddNode(FVector2D(0.0, 50000.0)), Net->AddNode(FVector2D(20000.0, 50000.0)),
		URoadProfile::MakeTransient(2300.0, 1500.0));
	FRoadNetworkSolver::SolveAll(*Net);

	const FRoadSegment* Road = Net->GetSegment(Seg);
	if (!TestNotNull(TEXT("the road"), Road)) { return false; }
	const double First = Road->TrimA;
	const double Last = 20000.0 - Road->TrimB;

	auto Paint = [&](int32& OutSegments, TArray<FVector2D>& OutCentroids) -> int32
	{
		FRoadMeshBuffers Buffers;
		const int32 Dashes = FRoadLaneMarkingBuilder::Build(*Net, 1.0, Buffers, &OutSegments);
		TestEqual(TEXT("four vertices per dash"), Buffers.Positions.Num(), Dashes * 4);
		for (int32 Q = 0; Q + 3 < Buffers.Positions.Num(); Q += 4)
		{
			FVector2D C = FVector2D::ZeroVector;
			for (int32 K = 0; K < 4; ++K)
			{
				const FVector3d& P = Buffers.Positions[Q + K];
				C += FVector2D(P.X, P.Y) * 0.25;
				TestTrue(TEXT("paint stays between the road's own cut lines, out of any junction"),
					P.X >= First - 1e-6 && P.X <= Last + 1e-6);
			}
			OutCentroids.Add(C);
		}
		return Dashes;
	};

	int32 Segments = 0;
	TArray<FVector2D> Centroids;
	const int32 Dashes = Paint(Segments, Centroids);
	TestEqual(TEXT("only the two-lane road is painted"), Segments, 1);

	const double Usable = Last - First;
	const double Pitch = FRoadLaneMarkingBuilder::DashLength + FRoadLaneMarkingBuilder::DashGap;
	const int32 Expected = FMath::FloorToInt32(Usable / Pitch)
		+ (FMath::Fmod(Usable, Pitch) > 0.0 ? 1 : 0);
	TestEqual(TEXT("one dash per pitch along the usable length"), Dashes, Expected);
	for (const FVector2D& C : Centroids)
	{
		TestTrue(TEXT("on the centreline, which the drive side does not move"), FMath::Abs(C.Y) < 1e-6);
	}

	Net->SetDriveSide(EDriveSide::Left);
	FRoadNetworkSolver::SolveAll(*Net);
	int32 SegmentsLeft = 0;
	TArray<FVector2D> CentroidsLeft;
	TestEqual(TEXT("flipping the drive side repaints nothing"), Paint(SegmentsLeft, CentroidsLeft), Dashes);
	TestTrue(TEXT("in the same places"), CentroidsLeft == Centroids);
	return true;
}

#endif
