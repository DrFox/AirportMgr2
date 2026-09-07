#include "CoreMinimal.h"
#include "Build/HoldingPositionMarkingBuilder.h"
#include "Build/RoadMeshBuilder.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** The guideline node derived for one end of Segment, or unset. M2FullWidth prefix: unity build. */
	FGuidelineNodeId M2FullWidthNodeFor(const URoadNetwork& Net, FRoadSegmentId Segment, bool bEndA)
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
 * THE HOLDING POSITION SITS WHERE THE TAXIWAY IS ITS OWN WIDTH. The flare fillet (PR #58)
 * widens the pavement between the runway and the taxiway's cut line, and the bar is
 * painted the taxiway's width - so a holding position inside the flare reads as a bar
 * that stops short of the pavement edge (the user's report, 2026-09-07). The cut line is
 * where the ribbon begins at full width, so the taxiway's end - and the bar on it - must
 * sit at or beyond its own trim, on every exit angle the flare takes: shallow (30, the
 * widest flare), 45 and square.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHoldingPositionFullWidthTest,
	"Airside.Build.HoldingPositionAtFullWidth",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FHoldingPositionFullWidthTest::RunTest(const FString& Parameters)
{
	constexpr double TaxiwayWidth = 2300.0;
	// Long exits at three angles, where the flare's own fillet lands its cut at the arc
	// start; a short square stub; and the case from the player's level (probe, 2026-09-07:
	// ends 1452-2498 uu from the node, cuts 1552-3381): a SHALLOW exit on a SHORT arm. The
	// arc clamps to 45 percent of the arm, the slab-clearance floor is 2050 / sin(40) =
	// 3189, and the ACUTE corner's kerb fillet meets the taxiway edge about 4000 down it -
	// its corner vertex alone sits a half width over tan(20) along - so the cut is past
	// both, and the bar sat 800 uu inside the corner's scallop.
	struct FCase { double Angle; double Length; };
	const FCase Cases[] = { { PI / 6.0, 60000.0 }, { PI / 4.0, 60000.0 }, { PI / 2.0, 60000.0 }, { PI / 2.0, 3200.0 }, { FMath::DegreesToRadians(40.0), 5500.0 } };
	for (const FCase& Case : Cases)
	{
		const double Angle = Case.Angle;
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		URoadProfile* Runway = URoadProfile::MakeTransient(1800.0, 1500.0, 180.0);
		Runway->bContinuousThroughJunctions = true;
		Runway->ExitLength = 6000.0;
		URoadProfile* Taxiway = URoadProfile::MakeTransient(TaxiwayWidth, 1500.0, 230.0);

		// Runway W -> E, a long taxiway leaving X at Angle below east.
		const FRoadNodeId W = Net->AddNode(FVector2D(-80000.0, 0.0));
		const FRoadNodeId X = Net->AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId E = Net->AddNode(FVector2D(80000.0, 0.0));
		const FVector2D Exit(FMath::Cos(-Angle), FMath::Sin(-Angle));
		const FRoadNodeId T = Net->AddNode(Exit * Case.Length);
		Net->AddStraightSegment(W, X, Runway);
		Net->AddStraightSegment(X, E, Runway);
		const FRoadSegmentId XT = Net->AddStraightSegment(X, T, Taxiway);

		const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Net);
		if (!TestEqual(FString::Printf(TEXT("%.0f degrees: every node solves"), FMath::RadiansToDegrees(Angle)), Solved.FailedNodes, 0)) { continue; }
		FRoadGuidelineBuilder::Build(*Net, Solved);

		const FRoadSegment* Segment = Net->GetSegment(XT);
		const FGuidelineNodeId End = M2FullWidthNodeFor(*Net, XT, /*bEndA=*/true);
		if (!TestTrue(TEXT("the taxiway's runway end exists"), Segment != nullptr && End.IsSet())) { continue; }
		const FGuidelineNode* Node = Net->GetGuidelineNode(End);
		TestEqual(TEXT("and is the derived runway-holding position"), Node->HoldingPosition, EHoldingPositionKind::Runway);

		// WHERE THE PAVEMENT IS THE TAXIWAY'S WIDTH, measured on the mesh the player sees: the
		// half-extent of pavement across the taxiway at each station along it, from the node
		// outward. The first station at which it is the profile's half width is where the
		// flare ends, whatever the solver's cut says.
		FRoadMeshBuilder Builder(0.0, 512.0);
		Builder.Build(*Net, Solved, 1);
		const FRoadMeshBuffers& Mesh = Builder.GetBuffers();
		auto ExtentAt = [&](double Station)
		{
			double Extent = 0.0;
			for (int32 Tri = 0; Tri * 3 + 2 < Mesh.Indices.Num(); ++Tri)
			{
				for (int32 Corner = 0; Corner < 3; ++Corner)
				{
					const FVector3d& P3 = Mesh.Positions[Mesh.Indices[Tri * 3 + Corner]];
					const FVector3d& Q3 = Mesh.Positions[Mesh.Indices[Tri * 3 + (Corner + 1) % 3]];
					const FVector2D P(P3.X, P3.Y), Q(Q3.X, Q3.Y);
					const double AP = FVector2D::DotProduct(P, Exit) - Station;
					const double AQ = FVector2D::DotProduct(Q, Exit) - Station;
					if ((AP < 0.0) == (AQ < 0.0) || FMath::IsNearlyEqual(AP, AQ)) { continue; }
					const FVector2D Hit = P + (Q - P) * (AP / (AP - AQ));
					// Off the runway slab only: at a shallow exit the cross-line through a
					// station near the node crosses the strip itself, and the strip is not
					// the pavement whose width the bar is meant to span.
					if (FMath::Abs(Hit.Y) <= 900.0 + 1.0) { continue; }
					Extent = FMath::Max(Extent, FMath::Abs(FVector2D::CrossProduct(Exit, Hit)));
				}
			}
			return Extent;
		};
		double FullWidthAt = -1.0;
		for (double Station = 0.0; Station < Case.Length; Station += 50.0)
		{
			if (ExtentAt(Station) <= TaxiwayWidth * 0.5 + 1.0) { FullWidthAt = Station; break; }
		}
		const double Cut = Segment->TrimA;
		const double Along = FVector2D::DotProduct(Node->Position, Exit);
		const double Off = FMath::Abs(FVector2D::CrossProduct(Node->Position, Exit));
		const double ExtentAtHold = ExtentAt(Along);
		AddInfo(FString::Printf(TEXT("%.0f degrees: cut %.0f, holding position %.0f down the taxiway, pavement half-extent there %.0f (profile %.0f), full width from %.0f"),
			FMath::RadiansToDegrees(Angle), Cut, Along, ExtentAtHold, TaxiwayWidth * 0.5, FullWidthAt));
		TestTrue(FString::Printf(TEXT("%.0f degrees, %.0f long: the pavement is the taxiway's width at the holding position (%.0f of %.0f)"), FMath::RadiansToDegrees(Angle), Case.Length, ExtentAtHold, TaxiwayWidth * 0.5),
			ExtentAtHold <= TaxiwayWidth * 0.5 + 1.0);
		TestTrue(FString::Printf(TEXT("%.0f degrees, %.0f long: the holding position (%.0f) is at or beyond where full width begins (%.0f)"), FMath::RadiansToDegrees(Angle), Case.Length, Along, FullWidthAt),
			FullWidthAt >= 0.0 && Along >= FullWidthAt - 50.0);
		TestTrue(FString::Printf(TEXT("and at or beyond the solver's cut (%.0f)"), Cut), Along >= Cut - 1.0);
		TestTrue(FString::Printf(TEXT("on the centreline (%.1f off)"), Off), Off < 1.0);
		TestTrue(TEXT("and still on the taxiway, short of its far end"), Along < Case.Length - Segment->TrimB);

		// The bar itself reaches exactly the taxiway's edges - measured on the buffers.
		FRoadMeshBuffers Buffers;
		TestEqual(TEXT("one position painted"), FHoldingPositionMarkingBuilder::Build(*Net, 0.0, Buffers), 1);
		double MaxAcross = 0.0;
		for (const FVector3d& P : Buffers.Positions)
		{
			const FVector2D Offset(P.X - Node->Position.X, P.Y - Node->Position.Y);
			MaxAcross = FMath::Max(MaxAcross, FMath::Abs(FVector2D::CrossProduct(Exit, Offset)));
		}
		TestTrue(FString::Printf(TEXT("the bar spans the taxiway's half width (%.1f of %.1f)"), MaxAcross, TaxiwayWidth * 0.5),
			FMath::Abs(MaxAcross - TaxiwayWidth * 0.5) < 1.0e-6);
	}
	return true;
}

#endif
