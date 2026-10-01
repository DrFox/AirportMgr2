#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "AirsideTestsLog.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadMeshBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"
#include "Testing/AirsideTestGraph.h"

// A TAXIWAY LEAVING A RUNWAY'S END, IN LINE WITH IT (samples/colours.png, 2026-10-01). Every
// runway junction test before this one met the runway at its SIDE, or at its end at 90 or 45
// degrees; the in-line case broke two rules built for the side:
//   - the holding position's slab floor, (runway half + taxiway half) / sin(angle), floored
//     the angle at 10 degrees and put the bar 81 m down a 30 m taxiway, out in the grass -
//     aircraft crabbed off the pavement to it and back;
//   - the flare, which follows an exit arc's turn through each runway/taxiway corner, read a
//     corner a hair under 180 degrees as a turn with a near-infinite radius and paved a wedge
//     down one side only.

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** A 20 m runway W..E ending at E, and the player's layout off its end: a 30 m taxiway in
	 *  line (bent by Skew radians), then a right-angle corner away from it. */
	struct FRunwayEndLayout
	{
		URoadNetwork* Net = nullptr;
		FRoadSegmentId Runway, InLine, Away;
		FVector2D E = FVector2D(0.0, 0.0);
		FVector2D C;
		double RunwayHalf = 1000.0;
		double TaxiwayHalf = 400.0;
	};

	FRunwayEndLayout LayRunwayEnd(double Skew, double InLineLength = 3000.0)
	{
		FRunwayEndLayout Out;
		Out.Net = NewObject<URoadNetwork>(GetTransientPackage());
		URoadProfile* Runway = URoadProfile::MakeTransient(2.0 * Out.RunwayHalf, 1500.0, 200.0);
		Runway->bContinuousThroughJunctions = true;
		Runway->AllowedPavements.Reset();
		Runway->ExitLength = 6000.0;
		URoadProfile* Taxiway = URoadProfile::MakeTransient(2.0 * Out.TaxiwayHalf, 1500.0, 80.0);

		Out.C = FVector2D(FMath::Cos(Skew), FMath::Sin(Skew)) * InLineLength;
		const FRoadNodeId W = Out.Net->AddNode(FVector2D(-40000.0, 0.0));
		const FRoadNodeId E = Out.Net->AddNode(Out.E);
		const FRoadNodeId C = Out.Net->AddNode(Out.C);
		const FRoadNodeId N = Out.Net->AddNode(Out.C + FVector2D(0.0, -20000.0));
		Out.Runway = Out.Net->AddStraightSegment(W, E, Runway);
		Out.InLine = Out.Net->AddStraightSegment(E, C, Taxiway);
		Out.Away = Out.Net->AddStraightSegment(C, N, Taxiway);
		return Out;
	}

	/** Whether any triangle of the built pavement covers P. */
	bool IsPaved(const FRoadMeshBuffers& Buffers, const FVector2D& P)
	{
		for (int32 I = 0; I + 2 < Buffers.Indices.Num(); I += 3)
		{
			const FVector2D A(Buffers.Positions[Buffers.Indices[I]]);
			const FVector2D B(Buffers.Positions[Buffers.Indices[I + 1]]);
			const FVector2D C(Buffers.Positions[Buffers.Indices[I + 2]]);
			const double D1 = FVector2D::CrossProduct(B - A, P - A);
			const double D2 = FVector2D::CrossProduct(C - B, P - B);
			const double D3 = FVector2D::CrossProduct(A - C, P - C);
			if ((D1 >= 0.0 && D2 >= 0.0 && D3 >= 0.0) || (D1 <= 0.0 && D2 <= 0.0 && D3 <= 0.0))
			{
				return true;
			}
		}
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRunwayEndInLineHoldTest,
	"Airside.Build.RunwayEnd.InLineHoldStaysOnItsTaxiway",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayEndInLineHoldTest::RunTest(const FString& Parameters)
{
	// 0 and 1.95 (the reported layout) and 8, inside the in-line band; 30, outside it, where the
	// taxiway veers off the end as an exit and keeps its flare.
	for (const double SkewDegrees : { 0.0, 1.95, 8.0, 30.0 })
	{
		const FRunwayEndLayout L = LayRunwayEnd(FMath::DegreesToRadians(SkewDegrees), SkewDegrees >= 30.0 ? 12000.0 : 3000.0);
		const FRoadSolveResult Solved = TestGraph::Derive(*L.Net);
		TestEqual(FString::Printf(TEXT("%.0f deg: every node solves"), SkewDegrees), Solved.FailedNodes, 0);

		const FGuidelineNodeId Hold = ExitArcNodeFor(*L.Net, L.InLine, /*bEndA=*/true);
		if (!TestTrue(TEXT("the in-line taxiway's runway end exists"), Hold.IsSet()))
		{
			continue;
		}
		const FGuidelineNode* Node = L.Net->GetGuidelineNode(Hold);
		const FVector2D Axis = (L.C - L.E).GetSafeNormal();
		const double Along = FVector2D::DotProduct(Node->Position - L.E, Axis);
		const double Length = FVector2D::Distance(L.C, L.E);
		TestTrue(FString::Printf(TEXT("%.0f deg: it is a runway-holding position"), SkewDegrees),
			Node->HoldingPosition == EHoldingPositionKind::Runway);
		// THE BUG: 8063 uu down a 3000 uu taxiway. A holding position belongs to the taxiway
		// it is painted on; past C it is in the grass, and the route walks there and back.
		TestTrue(FString::Printf(TEXT("%.0f deg: the hold sits on its own taxiway, %.0f uu along of %.0f"), SkewDegrees, Along, Length),
			Along > 0.0 && Along < Length);
	}

	return true;
}

/**
 * THE WEDGE OF samples/colours.png, and the node that paved nothing. A taxiway a hair off the
 * runway's line met its end as a corner between two nearly parallel edges 600 uu apart: 17 km
 * to their meeting point at 1.95 degrees, and the flare's radius larger still. Measured on the
 * built triangles, the pavement the player sees: the taxiway is paved where it leaves the end,
 * the runway is paved full width up to it, and beside the taxiway, past the end, is grass - no
 * one-sided wedge of runway-width paving.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRunwayEndInLinePavedTest,
	"Airside.Build.RunwayEnd.InLineEndIsPaved",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayEndInLinePavedTest::RunTest(const FString& Parameters)
{
	for (const double SkewDegrees : { 0.0, 1.95, 8.0 })
	{
		const FRunwayEndLayout L = LayRunwayEnd(FMath::DegreesToRadians(SkewDegrees));
		const FRoadSolveResult Solved = TestGraph::Derive(*L.Net);
		TestEqual(FString::Printf(TEXT("%.2f deg: every node solves"), SkewDegrees), Solved.FailedNodes, 0);

		FRoadMeshBuilder Builder(10.0);
		Builder.Build(*L.Net, Solved, 1);
		const FRoadMeshBuffers& Buffers = Builder.GetBuffers();

		const FVector2D Axis = (L.C - L.E).GetSafeNormal();
		const FVector2D Across(-Axis.Y, Axis.X);
		const FVector2D West(-1.0, 0.0);
		const FVector2D North(0.0, 1.0);

		TestTrue(FString::Printf(TEXT("%.2f deg: the taxiway is paved just past the runway's end"), SkewDegrees),
			IsPaved(Buffers, L.E + Axis * 150.0));
		TestTrue(FString::Printf(TEXT("%.2f deg: and halfway down"), SkewDegrees),
			IsPaved(Buffers, L.E + Axis * 1500.0));
		for (const double Side : { -1.0, 1.0 })
		{
			// 50 uu inside each edge, 5 m short of the end: the runway is full width to it.
			TestTrue(FString::Printf(TEXT("%.2f deg: the runway is paved to its edge on the %s side"), SkewDegrees, Side > 0.0 ? TEXT("left") : TEXT("right")),
				IsPaved(Buffers, L.E + West * 500.0 + North * Side * (L.RunwayHalf - 50.0)));
			// Between the taxiway's edge (400) and the runway's (1000), 15 m out: grass. The
			// wedge paved this on one side only.
			TestFalse(FString::Printf(TEXT("%.2f deg: no runway-width wedge beside the taxiway on the %s side"), SkewDegrees, Side > 0.0 ? TEXT("left") : TEXT("right")),
				IsPaved(Buffers, L.E + Axis * 1500.0 + Across * Side * 700.0));
		}
	}
	return true;
}

#endif
