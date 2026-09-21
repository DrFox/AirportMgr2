#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "AirsideTestsLog.h"
#include "Build/ExitGeometry.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadMeshBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteSearch.h"
#include "Profiles/RoadProfile.h"
#include "Solve/GuidelineGeom.h"

// BUILD-LAYER EXIT ARC TESTS. Issue #105 item 13 split what was one 800-line file with three
// #if WITH_DEV_AUTOMATION_TESTS blocks and eight mid-file includes into this (the Build
// tests: geometry, holding positions, pavement) and ArrivalExitArcTest.cpp (the Model tests:
// planner and traffic behaviour against the same fixture). FExitArcAirport and the node/turn
// finders both files need moved to AirsideTestFixtures.h; what stays here is used by exactly
// one test each.

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	double ExitArcDegreesBetween(const FVector2D& U, const FVector2D& V)
	{
		const FVector2D A = U.GetSafeNormal();
		const FVector2D B = V.GetSafeNormal();
		return FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector2D::DotProduct(A, B), -1.0, 1.0)));
	}

	/** StartError/EndError/Worst from ExitArcMeasure - see that function's own comment. */
	struct FArcFit
	{
		double StartError = 0.0;
		double EndError = 0.0;
		double Worst = 0.0;
	};

	/**
	 * Samples a turn walking From -> To and reports: the first chord's error against
	 * FromDir, the last chord's error against ToDir, and the largest turn between two
	 * consecutive chords. Measured on GuidelineGeom::Sample - THE evaluator - so what is
	 * asserted is what the follower will actually walk.
	 *
	 * The end chords are taken at 64 samples, because a chord of a curve sits half a
	 * sample's turn off the tangent by construction - at 16 samples a 45 degree arc reads
	 * 1.4 degrees "off" while being exactly tangent. The corner check is at the default 16,
	 * which is the polyline the aircraft drives: a straight stub into the node shows there
	 * as one chord turning by the whole exit angle.
	 */
	FArcFit ExitArcMeasure(const FGuidelineEdge& Turn, FGuidelineNodeId From, const URoadNetwork& Net,
		const FVector2D& FromDir, const FVector2D& ToDir)
	{
		const FVector2D PA = Net.GetGuidelineNode(Turn.A)->Position;
		const FVector2D PB = Net.GetGuidelineNode(Turn.B)->Position;
		const FVector2D Start = (Turn.A == From) ? PA : PB;
		const FVector2D End = (Turn.A == From) ? PB : PA;

		FArcFit Fit;
		TArray<FVector2D> Fine;
		GuidelineGeom::Sample(Start, Turn.Control, End, Fine, 64);
		Fit.StartError = ExitArcDegreesBetween(Fine[1] - Fine[0], FromDir);
		Fit.EndError = ExitArcDegreesBetween(Fine.Last() - Fine[Fine.Num() - 2], ToDir);

		TArray<FVector2D> Points;
		GuidelineGeom::Sample(Start, Turn.Control, End, Points, 16);
		for (int32 Vertex = 1; Vertex + 1 < Points.Num(); ++Vertex)
		{
			Fit.Worst = FMath::Max(Fit.Worst,
				ExitArcDegreesBetween(Points[Vertex] - Points[Vertex - 1], Points[Vertex + 1] - Points[Vertex]));
		}
		return Fit;
	}

	/** The solver's cut distance for Segment's arm at road node NodeIndex, or 0. Unused
	 *  today - kept from before the split rather than deleted as a change this issue did not
	 *  ask for. */
	double ExitArcCutDistance(const FRoadSolveResult& Solved, int32 NodeIndex, FRoadSegmentId Segment)
	{
		const FJunctionResult* Result = Solved.NodeResults.Find(NodeIndex);
		const TArray<FRoadSegmentId>* Arms = Solved.NodeArmSegments.Find(NodeIndex);
		if (Result == nullptr || Arms == nullptr) { return 0.0; }
		for (int32 Index = 0; Index < Arms->Num() && Index < Result->Arms.Num(); ++Index)
		{
			if ((*Arms)[Index] == Segment) { return Result->Arms[Index].CutDistance; }
		}
		return 0.0;
	}
}

/**
 * THE STRAIGHT STUB OF samples/runwayexits.png. A taxiway meeting a runway got a turn path
 * from its cut point straight to the road node, because a continuous arm's guideline ends
 * ON the node and a quadratic whose control point is an endpoint is a line. The taxiway-to-
 * taxiway turn at the same node was a proper arc, which is the construction this test now
 * expects on the runway side too: the runway half split ExitLength before the node, the
 * taxiway ending ExitLength back, and the arc between them tangent at both ends.
 *
 * Every assertion is a measurement on the sampled polyline, never on the edge's control
 * point: a control point can be right while the samples the follower walks are wrong.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRunwayExitArcTest,
	"Airside.Build.RunwayExitArc",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayExitArcTest::RunTest(const FString& Parameters)
{
	constexpr double ExitLength = 6000.0;
	constexpr double TangentTolerance = 1.0;   // degrees
	constexpr double VertexTolerance = 12.0;   // degrees; a 45 degree stub fails by a mile

	//   W ======= X ======= E      runway, two halves; W and E are its ends
	//    \         \         \     WQ at 90 deg off the end, XT at 45 deg, EZ short at 45 deg
	//     Q         T         Z
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Runway = URoadProfile::MakeTransient(1800.0, 1500.0, 180.0);
	Runway->bContinuousThroughJunctions = true;
	Runway->ExitLength = ExitLength;
	URoadProfile* Taxiway = URoadProfile::MakeTransient(2300.0, 1500.0, 230.0);

	const FRoadNodeId W = Net->AddNode(FVector2D(-40000.0, 0.0));
	const FRoadNodeId X = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId E = Net->AddNode(FVector2D(40000.0, 0.0));
	const FRoadNodeId T = Net->AddNode(FVector2D(20000.0, -20000.0));
	const FRoadNodeId Q = Net->AddNode(FVector2D(-40000.0, -20000.0));
	const FRoadNodeId Z = Net->AddNode(FVector2D(44000.0, -4000.0));
	const FRoadSegmentId RW1 = Net->AddStraightSegment(W, X, Runway);
	const FRoadSegmentId RW2 = Net->AddStraightSegment(X, E, Runway);
	const FRoadSegmentId XT = Net->AddStraightSegment(X, T, Taxiway);
	const FRoadSegmentId WQ = Net->AddStraightSegment(W, Q, Taxiway);
	const FRoadSegmentId EZ = Net->AddStraightSegment(E, Z, Taxiway);

	const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Net);
	TestEqual(TEXT("every node solves"), Solved.FailedNodes, 0);
	FRoadGuidelineBuilder::Build(*Net, Solved);

	const FVector2D East(1.0, 0.0);
	const FVector2D TowardT = FVector2D(1.0, -1.0).GetSafeNormal();

	// 1. THE RUNWAY IS SPLIT ExitLength EITHER SIDE OF X, on the centreline.
	double MissUp = 0.0, MissDown = 0.0;
	const FGuidelineNodeId SUp = ExitArcNodeNear(*Net, FVector2D(-ExitLength, 0.0), MissUp);
	const FGuidelineNodeId SDown = ExitArcNodeNear(*Net, FVector2D(ExitLength, 0.0), MissDown);
	TestTrue(FString::Printf(TEXT("a strip node sits ExitLength upstream of X (off by %.1f uu)"), MissUp), MissUp < 1.0);
	TestTrue(FString::Printf(TEXT("a strip node sits ExitLength downstream of X (off by %.1f uu)"), MissDown), MissDown < 1.0);

	// 2. THE TAXIWAY ENDS ExitLength BACK FROM X - or where it clears the runway slab, or
	//    at its pavement cut, whichever is furthest (neither is, here: about 2900 and 5650)
	//    - and keeps its identity (a holding-position mark is keyed by it). The cut is a
	//    FLOOR since 2026-09-07, never the clamp it once was: the holding position must sit
	//    where the taxiway is its own width - see Airside.Build.HoldingPositionAtFullWidth.
	const FGuidelineNodeId TEnd = ExitArcNodeFor(*Net, XT, /*bEndA=*/true);
	if (!TestTrue(TEXT("the taxiway's runway end exists by identity"), TEnd.IsSet())) { return false; }
	{
		const FVector2D At = Net->GetGuidelineNode(TEnd)->Position;
		const double Along = FVector2D::DotProduct(At, TowardT);
		const double Off = FMath::Abs(FVector2D::CrossProduct(At, TowardT));
		const double Floor = ExitGeometry::TaxiwayEndFloor(900.0, 1150.0, PI / 4.0);
		// And never inside the corner's fillet: the pavement cut is a floor too (the
		// holding position must sit where the taxiway is its own width). Here the flare's
		// cut lands just short of the arc start, so ExitLength still wins.
		const double Cut = Net->GetSegment(XT)->TrimA;
		const double Expected = FMath::Max(FMath::Max(ExitLength, Floor), Cut);
		TestTrue(FString::Printf(TEXT("taxiway end is max(ExitLength, slab clearance %.0f, cut %.0f) = %.0f down its own axis (%.1f) and on it (%.1f off)"), Floor, Cut, Expected, Along, Off),
			FMath::Abs(Along - Expected) < 1.0 && Off < 1.0);
	}

	// 3. THE EXIT ARC: from the upstream strip node to the taxiway end, tangent to the runway
	//    leaving it and tangent to the taxiway arriving, with no corner in between.
	if (MissUp < 1.0)
	{
		const FGuidelineEdge* Exit = ExitArcTurnBetween(*Net, SUp, TEnd);
		if (TestNotNull(TEXT("an exit turn joins the upstream strip node to the taxiway end"), Exit))
		{
			const FArcFit Fit = ExitArcMeasure(*Exit, SUp, *Net, East, TowardT);
			UE_LOG(LogAirsideTests, Log, TEXT("45 deg exit: leaves the runway %.2f deg off, meets the taxiway %.2f deg off, worst vertex %.2f deg"),
				Fit.StartError, Fit.EndError, Fit.Worst);
			TestTrue(FString::Printf(TEXT("exit leaves the centreline tangent (%.2f deg off)"), Fit.StartError), Fit.StartError < TangentTolerance);
			TestTrue(FString::Printf(TEXT("exit meets the taxiway tangent (%.2f deg off)"), Fit.EndError), Fit.EndError < TangentTolerance);
			TestTrue(FString::Printf(TEXT("no corner on the exit (worst vertex %.2f deg)"), Fit.Worst), Fit.Worst < VertexTolerance);
		}
	}

	// 4. THE ENTRY ARC on the downstream side, for a departure lining up in the roll
	//    direction - and a backtrack entry is the exit arc walked the other way.
	if (MissDown < 1.0)
	{
		const FGuidelineEdge* Entry = ExitArcTurnBetween(*Net, SDown, TEnd);
		if (TestNotNull(TEXT("an entry turn joins the taxiway end to the downstream strip node"), Entry))
		{
			const FArcFit Fit = ExitArcMeasure(*Entry, TEnd, *Net, -TowardT, East);
			TestTrue(FString::Printf(TEXT("entry leaves the taxiway tangent (%.2f deg off)"), Fit.StartError), Fit.StartError < TangentTolerance);
			TestTrue(FString::Printf(TEXT("entry joins the centreline tangent (%.2f deg off)"), Fit.EndError), Fit.EndError < TangentTolerance);
			// A 135 DEGREE HAIRPIN: this taxiway is angled for LEAVING an eastbound runway, so
			// joining it eastbound means turning back on oneself. Tangent at both ends all the
			// same, and spread over the arc rather than taken at one vertex - a stub would show
			// the whole 135 at one chord. The speed profile will crawl it, as it should.
			TestTrue(FString::Printf(TEXT("no single corner on the hairpin entry (worst vertex %.2f deg of 135)"), Fit.Worst), Fit.Worst < 25.0);
		}
	}

	// 5. THE RUNWAY IS STILL ONE LINE: threshold to threshold over the split pieces and the
	//    through-turn at X, never touching a taxiway.
	{
		const FGuidelineNodeId WEnd = ExitArcNodeFor(*Net, RW1, true);
		const FGuidelineNodeId EEnd = ExitArcNodeFor(*Net, RW2, false);
		FRouteQuery Query;
		Query.Start = WEnd;
		Query.Goal = EEnd;
		Query.Class = ETraversalClass::Aircraft;
		const FRoutePlan Through = RouteSearch::Find(*Net, Query);
		if (TestTrue(TEXT("the runway routes end to end"), Through.IsValid()))
		{
			bool bOnRunway = true;
			for (const FRouteStep& Step : Through.Steps)
			{
				const FGuidelineEdge* Edge = Net->GetGuidelineEdge(Step.Edge);
				const bool bStrip = Edge && Edge->DerivedFrom.IsSet()
					&& (Edge->DerivedFrom == RW1 || Edge->DerivedFrom == RW2);
				const bool bThroughTurn = Edge && !Edge->DerivedFrom.IsSet()
					&& FVector2D::Distance(Net->GetGuidelineNode(Edge->A)->Position,
						Net->GetGuidelineNode(Edge->B)->Position) < 1.0;
				bOnRunway = bOnRunway && (bStrip || bThroughTurn);
			}
			TestTrue(TEXT("and every step of it is the strip or the zero-length through-turn at X"), bOnRunway);
			TestTrue(FString::Printf(TEXT("its length is the runway's (%.0f of 80000)"), Through.Length),
				FMath::Abs(Through.Length - 80000.0) < 10.0);
		}
	}

	// 6. A 90 DEGREE EXIT AT THE RUNWAY'S END is an arc too, just a tighter one.
	{
		const FGuidelineNodeId QEnd = ExitArcNodeFor(*Net, WQ, true);
		double Miss = 0.0;
		const FGuidelineNodeId SW = ExitArcNodeNear(*Net, FVector2D(-40000.0 + ExitLength, 0.0), Miss);
		TestTrue(FString::Printf(TEXT("a strip node sits ExitLength inside the W end (off by %.1f uu)"), Miss), Miss < 1.0);
		const FGuidelineEdge* Turn = (QEnd.IsSet() && Miss < 1.0) ? ExitArcTurnBetween(*Net, SW, QEnd) : nullptr;
		if (TestNotNull(TEXT("a turn joins the W-end strip node to the 90 degree taxiway"), Turn))
		{
			const FArcFit Fit = ExitArcMeasure(*Turn, SW, *Net, -East, FVector2D(0.0, -1.0));
			UE_LOG(LogAirsideTests, Log, TEXT("90 deg exit: %.2f / %.2f deg off, worst vertex %.2f deg"), Fit.StartError, Fit.EndError, Fit.Worst);
			TestTrue(FString::Printf(TEXT("90 degree exit tangent both ends (%.2f, %.2f deg off)"), Fit.StartError, Fit.EndError),
				Fit.StartError < TangentTolerance && Fit.EndError < TangentTolerance);
			TestTrue(FString::Printf(TEXT("and cornerless (worst vertex %.2f deg)"), Fit.Worst), Fit.Worst < 2.0 * VertexTolerance);
		}
	}

	// 7. A TAXIWAY TOO SHORT FOR THE FULL SET-BACK clamps to 45 percent of itself and stays
	//    tangent - the arc shortens, it does not disappear or overrun the taxiway's far end.
	{
		const FGuidelineNodeId ZEnd = ExitArcNodeFor(*Net, EZ, true);
		const FGuidelineNodeId ZFar = ExitArcNodeFor(*Net, EZ, false);
		const FVector2D EAt(40000.0, 0.0);
		const double SegmentLength = FVector2D::Distance(EAt, FVector2D(44000.0, -4000.0));
		// ONE LENGTH PER NODE, symmetric: the tightest arm decides - here the 5657 uu stub -
		// so the runway side is set back the same 45% of it, not ExitLength. An uneven arc
		// (60 m along the runway, 25 m down the stub) swung wide of the fillet.
		const double NodeLength = FMath::Min(ExitLength, FMath::Min(0.45 * SegmentLength, 0.45 * 40000.0));
		double Miss = 0.0;
		const FGuidelineNodeId SE = ExitArcNodeNear(*Net, EAt - FVector2D(NodeLength, 0.0), Miss);
		TestTrue(FString::Printf(TEXT("the runway side is set back the same %.0f as the stub (off by %.1f)"), NodeLength, Miss), Miss < 1.0);
		if (TestTrue(TEXT("the short taxiway's ends exist"), ZEnd.IsSet() && ZFar.IsSet()) && Miss < 1.0)
		{
			const double Back = FVector2D::Distance(Net->GetGuidelineNode(ZEnd)->Position, EAt);
			// max(slab clearance, node length): the end is never on the runway strip however
			// the clamp comes out - the first clamp put a 55 m stub's end (and its holding
			// position) inside the slab.
			const double Floor = ExitGeometry::TaxiwayEndFloor(900.0, 1150.0, PI / 4.0);
			const double Cut = Net->GetSegment(EZ)->TrimA;
			const double Expected = FMath::Max(FMath::Max(Floor, NodeLength), Cut);
			const FVector2D EndAt = Net->GetGuidelineNode(ZEnd)->Position;
			TestTrue(FString::Printf(TEXT("short taxiway's end sits at max(slab clearance %.0f, %.0f, cut %.0f) = %.0f (%.0f), off the strip (%.0f from the centreline)"), Floor, NodeLength, Cut, Expected, Back, FMath::Abs(EndAt.Y)),
				FMath::Abs(Back - Expected) < 1.0 && FMath::Abs(EndAt.Y) > 900.0);
			const FGuidelineEdge* Turn = ExitArcTurnBetween(*Net, SE, ZEnd);
			if (TestNotNull(TEXT("the short taxiway still gets its arc"), Turn))
			{
				const FArcFit Fit = ExitArcMeasure(*Turn, SE, *Net, East, FVector2D(1.0, -1.0).GetSafeNormal());
				// Twice the tolerance on the short side: the tangent lengths are uneven (6000
				// on the runway, ~2200 on the stub) so the curvature bunches at the short end
				// and even a 64-sample chord sits further off the tangent there.
				TestTrue(FString::Printf(TEXT("clamped arc still tangent both ends (%.2f, %.2f deg off)"), Fit.StartError, Fit.EndError),
					Fit.StartError < TangentTolerance && Fit.EndError < 2.0 * TangentTolerance);
			}
		}
	}

	// 8. A RUNWAY-ONLY JUNCTION IS UNTOUCHED: two halves, four end nodes, nothing split.
	{
		URoadNetwork* Bare = NewObject<URoadNetwork>(GetTransientPackage());
		const FRoadNodeId BW = Bare->AddNode(FVector2D(-40000.0, 0.0));
		const FRoadNodeId BX = Bare->AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId BE = Bare->AddNode(FVector2D(40000.0, 0.0));
		Bare->AddStraightSegment(BW, BX, Runway);
		Bare->AddStraightSegment(BX, BE, Runway);
		const FRoadSolveResult BareSolved = FRoadNetworkSolver::SolveAll(*Bare);
		FRoadGuidelineBuilder::Build(*Bare, BareSolved);
		int32 Alive = 0;
		for (const FGuidelineNode& Node : Bare->GetGuidelineNodes()) { Alive += Node.bAlive ? 1 : 0; }
		TestEqual(TEXT("a runway meeting only itself has its four end nodes and no set-back nodes"), Alive, 4);
	}

	return true;
}

/**
 * RUNWAY-HOLDING POSITIONS ARE DERIVED (spec 2026-09-07). Every taxiway end at a runway is
 * one, protecting the strip it meets; the runway's own nodes and the split nodes are not;
 * a rebuild reproduces them; and with the arcs off (ExitLength 0) the end sits at its cut
 * line and is one all the same. The player cannot clear one - it is the junction's.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRunwayHoldingPositionsAreDerivedTest,
	"Airside.Build.RunwayHoldingPositionsAreDerived",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayHoldingPositionsAreDerivedTest::RunTest(const FString& Parameters)
{
	FExitArcAirport A = ExitArcBuildAirport(GetTransientPackage(), /*bWithStand=*/false);
	const FGuidelineNodeId TEnd = ExitArcNodeFor(*A.Net, A.XT, /*bEndA=*/true);
	const FGuidelineNodeId TFar = ExitArcNodeFor(*A.Net, A.XT, /*bEndA=*/false);
	if (!TestTrue(TEXT("the taxiway's ends exist"), TEnd.IsSet() && TFar.IsSet())) { return false; }

	const FGuidelineNode* End = A.Net->GetGuidelineNode(TEnd);
	TestTrue(TEXT("the taxiway's runway end is a runway-holding position"),
		End->HoldingPosition == EHoldingPositionKind::Runway);
	TestTrue(TEXT("protecting the strip it meets"),
		End->HoldingPositionFor.IsSet() && A.Net->RunwayChain(A.RW1).Contains(End->HoldingPositionFor));
	TestTrue(TEXT("its far end is not"),
		A.Net->GetGuidelineNode(TFar)->HoldingPosition == EHoldingPositionKind::None);

	int32 RunwayPositions = 0, OnTheStrip = 0;
	for (const FGuidelineNode& Node : A.Net->GetGuidelineNodes())
	{
		if (!Node.bAlive) { continue; }
		RunwayPositions += Node.HoldingPosition == EHoldingPositionKind::Runway ? 1 : 0;
		if (Node.HoldingPosition != EHoldingPositionKind::None && FMath::Abs(Node.Position.Y) < 1.0)
		{
			++OnTheStrip;
		}
	}
	TestEqual(TEXT("exactly one: the one taxiway end at the runway"), RunwayPositions, 1);
	TestEqual(TEXT("and none on the centreline - the runway's own nodes and the split nodes are not positions"), OnTheStrip, 0);

	TestFalse(TEXT("the player cannot clear a derived position"), A.Net->SetIntermediateHoldingPosition(TEnd, false));
	TestFalse(TEXT("nor place one over it"), A.Net->SetIntermediateHoldingPosition(TEnd, true));
	TestEqual(TEXT("and no mark was recorded for it"), A.Net->GetHoldingPositionMarks().Num(), 0);

	// Rebuild from scratch, as a save/load or any edit does.
	const FVector2D Before = End->Position;
	const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*A.Net);
	FRoadGuidelineBuilder::Build(*A.Net, Solved);
	const FGuidelineNodeId After = ExitArcNodeFor(*A.Net, A.XT, /*bEndA=*/true);
	if (TestTrue(TEXT("the end still exists by identity after the rebuild"), After.IsSet()))
	{
		const FGuidelineNode* Node = A.Net->GetGuidelineNode(After);
		TestTrue(TEXT("and is a runway-holding position again"), Node->HoldingPosition == EHoldingPositionKind::Runway);
		TestTrue(FString::Printf(TEXT("at the same place (moved %.1f uu)"), FVector2D::Distance(Node->Position, Before)),
			FVector2D::Distance(Node->Position, Before) < 1.0);
	}

	// ARCS OFF: the position is the junction's, not the arc's.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		URoadProfile* Runway = URoadProfile::MakeTransient(1800.0, 1500.0, 180.0);
		Runway->bContinuousThroughJunctions = true;
		Runway->ExitLength = 0.0;
		URoadProfile* Taxiway = URoadProfile::MakeTransient(2300.0, 1500.0, 230.0);
		const FRoadNodeId W = Net->AddNode(FVector2D(-40000.0, 0.0));
		const FRoadNodeId X = Net->AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId E = Net->AddNode(FVector2D(40000.0, 0.0));
		const FRoadNodeId Q = Net->AddNode(FVector2D(0.0, -20000.0));
		const FRoadSegmentId R1 = Net->AddStraightSegment(W, X, Runway);
		Net->AddStraightSegment(X, E, Runway);
		const FRoadSegmentId XQ = Net->AddStraightSegment(X, Q, Taxiway);
		const FRoadSolveResult S2 = FRoadNetworkSolver::SolveAll(*Net);
		FRoadGuidelineBuilder::Build(*Net, S2);
		const FGuidelineNodeId QEnd = ExitArcNodeFor(*Net, XQ, true);
		if (TestTrue(TEXT("arcs off: the taxiway end exists"), QEnd.IsSet()))
		{
			const FGuidelineNode* Node = Net->GetGuidelineNode(QEnd);
			TestTrue(TEXT("arcs off: still a runway-holding position, at the cut line"),
				Node->HoldingPosition == EHoldingPositionKind::Runway && Net->RunwayChain(R1).Contains(Node->HoldingPositionFor));
			TestTrue(FString::Printf(TEXT("arcs off: within the pavement cut of the junction (%.0f uu from X)"), Node->Position.Size()),
				Node->Position.Size() < 6000.0);
		}
	}
	return true;
}

namespace
{
	/** True when P lies in the triangle ABC, edges included, in the road plane. */
	bool ExitArcInTriangle(const FVector2D& P, const FVector2D& A, const FVector2D& B, const FVector2D& C)
	{
		const double D1 = (P.X - B.X) * (A.Y - B.Y) - (A.X - B.X) * (P.Y - B.Y);
		const double D2 = (P.X - C.X) * (B.Y - C.Y) - (B.X - C.X) * (P.Y - C.Y);
		const double D3 = (P.X - A.X) * (C.Y - A.Y) - (C.X - A.X) * (P.Y - A.Y);
		const bool bNeg = (D1 < -1.0e-6) || (D2 < -1.0e-6) || (D3 < -1.0e-6);
		const bool bPos = (D1 > 1.0e-6) || (D2 > 1.0e-6) || (D3 > 1.0e-6);
		return !(bNeg && bPos);
	}

	bool ExitArcOnPavement(const FVector2D& P, const FRoadMeshBuffers& Pavement)
	{
		for (int32 Slot = 0; Slot + 2 < Pavement.Indices.Num(); Slot += 3)
		{
			const FVector3d& A = Pavement.Positions[Pavement.Indices[Slot]];
			const FVector3d& B = Pavement.Positions[Pavement.Indices[Slot + 1]];
			const FVector3d& C = Pavement.Positions[Pavement.Indices[Slot + 2]];
			if (ExitArcInTriangle(P, FVector2D(A.X, A.Y), FVector2D(B.X, B.Y), FVector2D(C.X, C.Y)))
			{
				return true;
			}
		}
		return false;
	}
}

/**
 * THE ARC STAYS ON THE PAVEMENT. samples/runway1.png (2026-09-07): an exit arc with 60 m of
 * tangent on the runway side and 30 m on a stub taxiway swung wide of the fillet. Measured
 * here the way the player saw it - every sampled point of every turn path at a runway
 * junction, tested against the triangles the surface builder actually paves - on short
 * stubs at 45, 60 and 90 degrees, which are the corners a real airport's exits are cut at.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRunwayExitArcOnPavementTest,
	"Airside.Build.RunwayExitArcOnPavement",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayExitArcOnPavementTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Runway = URoadProfile::MakeTransient(1800.0, 1500.0, 180.0);
	Runway->bContinuousThroughJunctions = true;
	URoadProfile* Taxiway = URoadProfile::MakeTransient(2300.0, 1500.0, 230.0);

	//   W ===== X1 ===== X2 ===== E     three junctions, a 5500 uu stub at each
	const FRoadNodeId W = Net->AddNode(FVector2D(-60000.0, 0.0));
	const FRoadNodeId X1 = Net->AddNode(FVector2D(-20000.0, 0.0));
	const FRoadNodeId X2 = Net->AddNode(FVector2D(20000.0, 0.0));
	const FRoadNodeId E = Net->AddNode(FVector2D(60000.0, 0.0));
	Net->AddStraightSegment(W, X1, Runway);
	Net->AddStraightSegment(X1, X2, Runway);
	Net->AddStraightSegment(X2, E, Runway);
	constexpr double Stub = 5500.0;
	const FRoadNodeId S45 = Net->AddNode(FVector2D(-20000.0, 0.0) + FVector2D(FMath::Cos(-PI / 4.0), FMath::Sin(-PI / 4.0)) * Stub);
	const FRoadNodeId S60 = Net->AddNode(FVector2D(20000.0, 0.0) + FVector2D(FMath::Cos(-PI / 3.0), FMath::Sin(-PI / 3.0)) * Stub);
	const FRoadNodeId S90 = Net->AddNode(FVector2D(60000.0, -Stub));
	Net->AddStraightSegment(X1, S45, Taxiway);
	Net->AddStraightSegment(X2, S60, Taxiway);
	Net->AddStraightSegment(E, S90, Taxiway);

	const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Net);
	TestEqual(TEXT("every node solves"), Solved.FailedNodes, 0);
	FRoadGuidelineBuilder::Build(*Net, Solved);

	// The pavement the player sees, from the same solve.
	FRoadMeshBuilder Pavement(10.0);
	Pavement.Build(*Net, Solved, 8);
	const FRoadMeshBuffers& Paved = Pavement.GetBuffers();
	TestTrue(TEXT("there is pavement to test against"), Paved.Indices.Num() > 0);

	// THE SWEPT BAND, not only the centreline: an aircraft's wheels and wing ride either
	// side of the line, so each sample is tested at the centre and 8 m to each side - the
	// runway's own half width less a margin, so the band never asks more of the strip than
	// the strip has. This is the property the flare fillet exists for, and it failed on
	// every stub before the flare (samples/runway2.png, 2026-09-07).
	constexpr double Band = 800.0;
	int32 Arcs = 0, Points = 0, Off = 0;
	FString Worst;
	for (const FGuidelineEdge& Edge : Net->GetGuidelineEdges())
	{
		if (!Edge.bAlive || !Edge.bDerived || Edge.DerivedFrom.IsSet()) { continue; }
		const FVector2D A = Net->GetGuidelineNode(Edge.A)->Position;
		const FVector2D B = Net->GetGuidelineNode(Edge.B)->Position;
		if (FVector2D::Distance(A, B) < 1.0) { continue; }   // the runway's through-turn
		++Arcs;
		TArray<FVector2D> Samples;
		GuidelineGeom::Sample(A, Edge.Control, B, Samples, 16);
		for (int32 Index = 0; Index < Samples.Num(); ++Index)
		{
			const FVector2D& P = Samples[Index];
			const FVector2D Ahead = Samples[FMath::Min(Index + 1, Samples.Num() - 1)] - Samples[FMath::Max(Index - 1, 0)];
			const FVector2D Side = FVector2D(-Ahead.Y, Ahead.X).GetSafeNormal();
			const FVector2D Probe[3] = { P, P + Side * Band, P - Side * Band };
			for (const FVector2D& Q : Probe)
			{
				++Points;
				if (!ExitArcOnPavement(Q, Paved))
				{
					++Off;
					if (Worst.IsEmpty())
					{
						Worst = FString::Printf(TEXT("first off-pavement point (%.0f, %.0f) on the turn %s -> %s"),
							Q.X, Q.Y, *A.ToString(), *B.ToString());
					}
				}
			}
		}
	}
	UE_LOG(LogAirsideTests, Log, TEXT("On-pavement: %d arcs, %d sampled points, %d off the pavement. %s"), Arcs, Points, Off, *Worst);
	TestTrue(TEXT("there are arcs to measure"), Arcs >= 6);
	TestEqual(FString::Printf(TEXT("every sampled point of every arc lies on the pavement (%d of %d off). %s"), Off, Points, *Worst), Off, 0);
	return true;
}

#endif
