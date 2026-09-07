#include "CoreMinimal.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteSearch.h"
#include "Profiles/RoadProfile.h"
#include "Solve/GuidelineGeom.h"

#if WITH_DEV_AUTOMATION_TESTS

DEFINE_LOG_CATEGORY_STATIC(LogExitArcTest, Log, All);

namespace
{
	/** The guideline node a segment's derived guideline ENDS on, found by identity. */
	FGuidelineNodeId ExitArcNodeFor(const URoadNetwork& Net, FRoadSegmentId Segment, bool bEndA)
	{
		const TArray<FGuidelineNode>& Nodes = Net.GetGuidelineNodes();
		for (int32 Index = 0; Index < Nodes.Num(); ++Index)
		{
			const FGuidelineNode& Node = Nodes[Index];
			if (Node.bAlive && Node.Origin.Segment == Segment && Node.Origin.bEndA == bEndA
				&& Node.Origin.GuidelineIndex == 0)
			{
				return Net.GuidelineNodeIdAt(Index);
			}
		}
		return FGuidelineNodeId();
	}

	/** The alive guideline node nearest a position, and how far off it is. */
	FGuidelineNodeId ExitArcNodeNear(const URoadNetwork& Net, const FVector2D& At, double& OutMiss)
	{
		const TArray<FGuidelineNode>& Nodes = Net.GetGuidelineNodes();
		FGuidelineNodeId Best;
		OutMiss = TNumericLimits<double>::Max();
		for (int32 Index = 0; Index < Nodes.Num(); ++Index)
		{
			if (!Nodes[Index].bAlive) { continue; }
			const double Miss = FVector2D::Distance(Nodes[Index].Position, At);
			if (Miss < OutMiss)
			{
				OutMiss = Miss;
				Best = Net.GuidelineNodeIdAt(Index);
			}
		}
		return Best;
	}

	/** An alive derived TURN PATH (no DerivedFrom) joining two nodes, either way round. */
	const FGuidelineEdge* ExitArcTurnBetween(const URoadNetwork& Net, FGuidelineNodeId P, FGuidelineNodeId Q)
	{
		for (const FGuidelineEdge& Edge : Net.GetGuidelineEdges())
		{
			if (!Edge.bAlive || !Edge.bDerived || Edge.DerivedFrom.IsSet()) { continue; }
			if ((Edge.A == P && Edge.B == Q) || (Edge.A == Q && Edge.B == P))
			{
				return &Edge;
			}
		}
		return nullptr;
	}

	double ExitArcDegreesBetween(const FVector2D& U, const FVector2D& V)
	{
		const FVector2D A = U.GetSafeNormal();
		const FVector2D B = V.GetSafeNormal();
		return FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector2D::DotProduct(A, B), -1.0, 1.0)));
	}

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
	void ExitArcMeasure(const FGuidelineEdge& Turn, FGuidelineNodeId From, const URoadNetwork& Net,
		const FVector2D& FromDir, const FVector2D& ToDir,
		double& OutStartError, double& OutEndError, double& OutWorstVertex)
	{
		const FVector2D PA = Net.GetGuidelineNode(Turn.A)->Position;
		const FVector2D PB = Net.GetGuidelineNode(Turn.B)->Position;
		const FVector2D Start = (Turn.A == From) ? PA : PB;
		const FVector2D End = (Turn.A == From) ? PB : PA;

		TArray<FVector2D> Fine;
		GuidelineGeom::Sample(Start, Turn.Control, End, Fine, 64);
		OutStartError = ExitArcDegreesBetween(Fine[1] - Fine[0], FromDir);
		OutEndError = ExitArcDegreesBetween(Fine.Last() - Fine[Fine.Num() - 2], ToDir);

		TArray<FVector2D> Points;
		GuidelineGeom::Sample(Start, Turn.Control, End, Points, 16);
		OutWorstVertex = 0.0;
		for (int32 Vertex = 1; Vertex + 1 < Points.Num(); ++Vertex)
		{
			OutWorstVertex = FMath::Max(OutWorstVertex,
				ExitArcDegreesBetween(Points[Vertex] - Points[Vertex - 1], Points[Vertex + 1] - Points[Vertex]));
		}
	}

	/** The solver's cut distance for Segment's arm at road node NodeIndex, or 0. */
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

	// 2. THE TAXIWAY ENDS ExitLength BACK FROM X - or at its pavement cut if that is further,
	//    which at an acute 45 degree corner it is (edge intersection plus the fillet's
	//    tangent, about 6044 here) - and keeps its identity (a holding-position mark is keyed by it).
	const FGuidelineNodeId TEnd = ExitArcNodeFor(*Net, XT, /*bEndA=*/true);
	if (!TestTrue(TEXT("the taxiway's runway end exists by identity"), TEnd.IsSet())) { return false; }
	{
		const FVector2D At = Net->GetGuidelineNode(TEnd)->Position;
		const double Along = FVector2D::DotProduct(At, TowardT);
		const double Off = FMath::Abs(FVector2D::CrossProduct(At, TowardT));
		const double Cut = ExitArcCutDistance(Solved, X.Index, XT);
		const double Expected = FMath::Max(ExitLength, Cut);
		TestTrue(FString::Printf(TEXT("taxiway end is max(ExitLength, cut %.0f) = %.0f down its own axis (%.1f) and on it (%.1f off)"), Cut, Expected, Along, Off),
			FMath::Abs(Along - Expected) < 1.0 && Off < 1.0);
	}

	// 3. THE EXIT ARC: from the upstream strip node to the taxiway end, tangent to the runway
	//    leaving it and tangent to the taxiway arriving, with no corner in between.
	if (MissUp < 1.0)
	{
		const FGuidelineEdge* Exit = ExitArcTurnBetween(*Net, SUp, TEnd);
		if (TestNotNull(TEXT("an exit turn joins the upstream strip node to the taxiway end"), Exit))
		{
			double StartError = 0.0, EndError = 0.0, Worst = 0.0;
			ExitArcMeasure(*Exit, SUp, *Net, East, TowardT, StartError, EndError, Worst);
			UE_LOG(LogExitArcTest, Log, TEXT("45 deg exit: leaves the runway %.2f deg off, meets the taxiway %.2f deg off, worst vertex %.2f deg"),
				StartError, EndError, Worst);
			TestTrue(FString::Printf(TEXT("exit leaves the centreline tangent (%.2f deg off)"), StartError), StartError < TangentTolerance);
			TestTrue(FString::Printf(TEXT("exit meets the taxiway tangent (%.2f deg off)"), EndError), EndError < TangentTolerance);
			TestTrue(FString::Printf(TEXT("no corner on the exit (worst vertex %.2f deg)"), Worst), Worst < VertexTolerance);
		}
	}

	// 4. THE ENTRY ARC on the downstream side, for a departure lining up in the roll
	//    direction - and a backtrack entry is the exit arc walked the other way.
	if (MissDown < 1.0)
	{
		const FGuidelineEdge* Entry = ExitArcTurnBetween(*Net, SDown, TEnd);
		if (TestNotNull(TEXT("an entry turn joins the taxiway end to the downstream strip node"), Entry))
		{
			double StartError = 0.0, EndError = 0.0, Worst = 0.0;
			ExitArcMeasure(*Entry, TEnd, *Net, -TowardT, East, StartError, EndError, Worst);
			TestTrue(FString::Printf(TEXT("entry leaves the taxiway tangent (%.2f deg off)"), StartError), StartError < TangentTolerance);
			TestTrue(FString::Printf(TEXT("entry joins the centreline tangent (%.2f deg off)"), EndError), EndError < TangentTolerance);
			// A 135 DEGREE HAIRPIN: this taxiway is angled for LEAVING an eastbound runway, so
			// joining it eastbound means turning back on oneself. Tangent at both ends all the
			// same, and spread over the arc rather than taken at one vertex - a stub would show
			// the whole 135 at one chord. The speed profile will crawl it, as it should.
			TestTrue(FString::Printf(TEXT("no single corner on the hairpin entry (worst vertex %.2f deg of 135)"), Worst), Worst < 25.0);
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
			double StartError = 0.0, EndError = 0.0, Worst = 0.0;
			ExitArcMeasure(*Turn, SW, *Net, -East, FVector2D(0.0, -1.0), StartError, EndError, Worst);
			UE_LOG(LogExitArcTest, Log, TEXT("90 deg exit: %.2f / %.2f deg off, worst vertex %.2f deg"), StartError, EndError, Worst);
			TestTrue(FString::Printf(TEXT("90 degree exit tangent both ends (%.2f, %.2f deg off)"), StartError, EndError),
				StartError < TangentTolerance && EndError < TangentTolerance);
			TestTrue(FString::Printf(TEXT("and cornerless (worst vertex %.2f deg)"), Worst), Worst < 2.0 * VertexTolerance);
		}
	}

	// 7. A TAXIWAY TOO SHORT FOR THE FULL SET-BACK clamps to 45 percent of itself and stays
	//    tangent - the arc shortens, it does not disappear or overrun the taxiway's far end.
	{
		const FGuidelineNodeId ZEnd = ExitArcNodeFor(*Net, EZ, true);
		const FGuidelineNodeId ZFar = ExitArcNodeFor(*Net, EZ, false);
		double Miss = 0.0;
		const FGuidelineNodeId SE = ExitArcNodeNear(*Net, FVector2D(40000.0 - ExitLength, 0.0), Miss);
		if (TestTrue(TEXT("the short taxiway's ends exist"), ZEnd.IsSet() && ZFar.IsSet()) && Miss < 1.0)
		{
			const FVector2D EAt(40000.0, 0.0);
			const double Back = FVector2D::Distance(Net->GetGuidelineNode(ZEnd)->Position, EAt);
			const double Far = FVector2D::Distance(Net->GetGuidelineNode(ZFar)->Position, EAt);
			TestTrue(FString::Printf(TEXT("short taxiway's end is set back less than ExitLength (%.0f) and short of its far end (%.0f)"), Back, Far),
				Back < ExitLength - 1.0 && Back < Far * 0.5);
			const FGuidelineEdge* Turn = ExitArcTurnBetween(*Net, SE, ZEnd);
			if (TestNotNull(TEXT("the short taxiway still gets its arc"), Turn))
			{
				double StartError = 0.0, EndError = 0.0, Worst = 0.0;
				ExitArcMeasure(*Turn, SE, *Net, East, FVector2D(1.0, -1.0).GetSafeNormal(), StartError, EndError, Worst);
				// Twice the tolerance on the short side: the tangent lengths are uneven (6000
				// on the runway, ~2200 on the stub) so the curvature bunches at the short end
				// and even a 64-sample chord sits further off the tangent there.
				TestTrue(FString::Printf(TEXT("clamped arc still tangent both ends (%.2f, %.2f deg off)"), StartError, EndError),
					StartError < TangentTolerance && EndError < 2.0 * TangentTolerance);
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

#endif

#if WITH_DEV_AUTOMATION_TESTS

#include "Build/AnchorLink.h"
#include "Content/AirsideSettings.h"
#include "Entities/EntityDefinition.h"
#include "Model/ArrivalPlanner.h"

namespace
{
	/**
	 * A runway long enough for the Piper to stop before the exit, one 45 degree taxiway, and
	 * a stand beside it. Shared by the holding-position, planner and handover tests so all three
	 * argue about the same junction. X sits 60000 uu from the W threshold because the exit
	 * arc begins ExitLength before it, and that start must be past the landing distance
	 * (about 37000) or the planner would rightly skip it for a later node.
	 */
	struct FExitArcAirport
	{
		URoadNetwork* Net = nullptr;
		FRoadSegmentId RW1, RW2, XT;
		FVector2D XAt = FVector2D(20000.0, 0.0);
		double ExitLength = 6000.0;
		FVector2D Threshold = FVector2D(-40000.0, 0.0);
	};

	FExitArcAirport ExitArcBuildAirport(UObject* Outer, bool bWithStand, double XDistance = 60000.0)
	{
		FExitArcAirport Out;
		Out.XAt = FVector2D(-40000.0 + XDistance, 0.0);
		Out.Net = NewObject<URoadNetwork>(Outer);
		URoadProfile* Runway = URoadProfile::MakeTransient(1800.0, 1500.0, 180.0);
		Runway->bContinuousThroughJunctions = true;
		Runway->ExitLength = Out.ExitLength;
		URoadProfile* Taxiway = URoadProfile::MakeTransient(2300.0, 1500.0, 230.0);
		const FRoadNodeId W = Out.Net->AddNode(Out.Threshold);
		const FRoadNodeId X = Out.Net->AddNode(Out.XAt);
		const FRoadNodeId E = Out.Net->AddNode(FVector2D(FMath::Max(60000.0, Out.XAt.X + 40000.0), 0.0));
		const FRoadNodeId T = Out.Net->AddNode(Out.XAt + FVector2D(20000.0, -20000.0));
		Out.RW1 = Out.Net->AddStraightSegment(W, X, Runway);
		Out.RW2 = Out.Net->AddStraightSegment(X, E, Runway);
		Out.XT = Out.Net->AddStraightSegment(X, T, Taxiway);
		const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Out.Net);
		FRoadGuidelineBuilder::Build(*Out.Net, Solved);
		if (bWithStand)
		{
			// Faces east (heading 0), so its lead-in casts WEST and meets the 45 degree
			// taxiway at (34000, -14000), 11000 uu away - inside FAnchorLink's reach.
			UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
			Out.Net->PlaceEntity(Stand, Stand->Anchors, Out.XAt + FVector2D(25000.0, -14000.0), 0.0);
			FAnchorLink::Build(*Out.Net);
		}
		return Out;
	}
}

/**
 * The holding-position bar is keyed by the taxiway's END (FGuidelineEndRef), and the end has
 * moved to the arc's start. The mark must follow it through a rebuild, or a bar the player
 * placed at a runway would come back on the wrong node - or nowhere.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRunwayExitArcHoldingPositionTest,
	"Airside.Build.RunwayExitArcHoldingPosition",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayExitArcHoldingPositionTest::RunTest(const FString& Parameters)
{
	FExitArcAirport A = ExitArcBuildAirport(GetTransientPackage(), /*bWithStand=*/false);
	const FGuidelineNodeId TEnd = ExitArcNodeFor(*A.Net, A.XT, /*bEndA=*/true);
	if (!TestTrue(TEXT("the taxiway's runway end exists"), TEnd.IsSet())) { return false; }
	const FVector2D Before = A.Net->GetGuidelineNode(TEnd)->Position;
	// At least ExitLength: the cut distance wins at this acute corner (see RunwayExitArc).
	TestTrue(FString::Printf(TEXT("that end is the arc start, at least ExitLength from X (%.0f)"), FVector2D::Distance(Before, A.XAt)),
		FVector2D::Distance(Before, A.XAt) >= A.ExitLength - 1.0);
	TestTrue(TEXT("a bar is set on it"), A.Net->SetIntermediateHoldingPosition(TEnd, A.RW1));

	// Rebuild from scratch, as a save/load or any edit does.
	const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*A.Net);
	FRoadGuidelineBuilder::Build(*A.Net, Solved);

	const FGuidelineNodeId After = ExitArcNodeFor(*A.Net, A.XT, /*bEndA=*/true);
	if (TestTrue(TEXT("the end still exists by identity after the rebuild"), After.IsSet()))
	{
		const FGuidelineNode* Node = A.Net->GetGuidelineNode(After);
		TestTrue(TEXT("the bar came back on it"), Node->HoldingPositionFor == A.RW1);
		TestTrue(FString::Printf(TEXT("at the same place (moved %.1f uu)"), FVector2D::Distance(Node->Position, Before)),
			FVector2D::Distance(Node->Position, Before) < 1.0);
	}
	return true;
}

/**
 * What falls out of the geometry without a planner change, measured rather than assumed:
 * the first strip node past the landing distance that reaches a stand is now the arc's
 * start, so the rollout ends where the taxi begins.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FArrivalExitAtArcStartTest,
	"Airside.Model.ArrivalExitAtArcStart",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FArrivalExitAtArcStartTest::RunTest(const FString& Parameters)
{
	FExitArcAirport A = ExitArcBuildAirport(GetTransientPackage(), /*bWithStand=*/true);
	const FAirframe Airframe = UAirsideSettings::ResolveDefaultAirframe();
	const FArrivalPlan Plan = ArrivalPlanner::Plan(*A.Net, A.Threshold - FVector2D(1000.0, 0.0), Airframe);
	if (!TestTrue(FString::Printf(TEXT("the arrival is planned: %s"), *ArrivalPlanner::DescribeRefusal(Plan)), Plan.IsValid()))
	{
		return false;
	}
	double Miss = 0.0;
	const FGuidelineNodeId SUp = ExitArcNodeNear(*A.Net, A.XAt - FVector2D(A.ExitLength, 0.0), Miss);
	TestTrue(TEXT("the upstream arc start exists"), Miss < 1.0);
	TestTrue(TEXT("the planner's exit IS the arc start, not the junction node"), Plan.Exit == SUp);
	const double Expected = FVector2D::DotProduct(A.XAt - FVector2D(A.ExitLength, 0.0) - A.Threshold, FVector2D(1.0, 0.0));
	TestTrue(FString::Printf(TEXT("so the rollout vacates at the arc start (%.0f of %.0f)"), Plan.VacateAt, Expected),
		FMath::Abs(Plan.VacateAt - Expected) < 1.0);
	TestTrue(TEXT("and the taxi-in begins on the centreline there"),
		Plan.TaxiIn.Polyline.Num() > 1
		&& FVector2D::Distance(Plan.TaxiIn.Polyline[0], A.Net->GetGuidelineNode(SUp)->Position) < 1.0);
	return true;
}

#endif

#if WITH_DEV_AUTOMATION_TESTS

#include "Model/GroundTraffic.h"
#include "Model/RoadAgent.h"

/**
 * THE REPORT OF 2026-09-06: "rolls out to the exit, stops (immediately to 0 m/s), then
 * seems to respawn facing the exit route and accelerates along the taxiway". Two motion
 * models met at a point and neither carried anything across. This measures the whole
 * ground run - touchdown to parked, the handover frame included - and asserts no speed or
 * heading step larger than the airframe could make in one tick.
 *
 * Red on the old handover twice over: speed 800 -> 0 (Start reset it) and heading by the
 * taxiway's 45 degrees (seeded from a straight stub).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficVacatedHandoverIsContinuousTest,
	"Airside.Model.Traffic.VacatedHandoverIsContinuous",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficVacatedHandoverIsContinuousTest::RunTest(const FString& Parameters)
{
	FExitArcAirport A = ExitArcBuildAirport(GetTransientPackage(), /*bWithStand=*/true);
	const FAirframe Airframe = UAirsideSettings::ResolveDefaultAirframe();
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Id = Traffic->DispatchArrival(*A.Net, A.Threshold - FVector2D(1000.0, 0.0), Airframe, 10.0);
	if (!TestTrue(TEXT("the arrival is dispatched"), Id > 0)) { return false; }

	constexpr double Dt = 1.0 / 60.0;
	// What one tick may change, with half again for the frame the handover spends twice.
	const double SpeedStepAllowed = FMath::Max3(Airframe.Ground.Landing.Decel, Airframe.Ground.Taxi.Accel,
		Airframe.Ground.Taxi.Decel) * Dt * 1.5 + 1.0;
	const double HeadingStepAllowed = FMath::DegreesToRadians(Airframe.Ground.MaxTurnRateDegPerSec) * Dt * 1.5 + 1.0e-4;

	bool bHadGround = false;
	FVector2D PrevAt = FVector2D::ZeroVector;
	double PrevHeading = 0.0;
	double PrevSpeed = -1.0;
	double WorstSpeedStep = 0.0, WorstHeadingStep = 0.0;
	double WorstSpeedAt = 0.0, WorstHeadingAt = 0.0;
	bool bSawTaxi = false, bParked = false;
	int32 Ticks = 0;
	for (; Ticks < 600 * 60; ++Ticks)
	{
		Traffic->Advance(Dt, A.Net);
		const FRoadAgent* Agent = Traffic->FindAgent(Id);
		if (Agent == nullptr) { break; }
		if (Agent->Phase == EAgentPhase::Parked) { bParked = true; break; }
		bSawTaxi = bSawTaxi || Agent->Phase == EAgentPhase::Taxiing;

		const FAgentMotion& M = Agent->LastMotion;
		if (M.Altitude > 0.0)
		{
			bHadGround = false;   // airborne: nothing to compare across yet
			continue;
		}
		const double Speed = bHadGround ? FVector2D::Distance(M.Position, PrevAt) / Dt : -1.0;
		if (bHadGround && PrevSpeed >= 0.0)
		{
			const double SpeedStep = FMath::Abs(Speed - PrevSpeed);
			const double HeadingStep = FMath::Abs(FMath::UnwindRadians(M.Heading - PrevHeading));
			if (SpeedStep > WorstSpeedStep) { WorstSpeedStep = SpeedStep; WorstSpeedAt = Ticks * Dt; }
			if (HeadingStep > WorstHeadingStep) { WorstHeadingStep = HeadingStep; WorstHeadingAt = Ticks * Dt; }
		}
		// THE HANDOVER, tick by tick, so a failure is read off the numbers: the frame the
		// phase flips and the two either side of it.
		if (Agent->Phase == EAgentPhase::Taxiing && Agent->Follower.Travelled < 60.0)
		{
			UE_LOG(LogExitArcTest, Log,
				TEXT("t=%.3f phase %d at (%.1f, %.1f) hdg %.2f deg measured %.1f uu/s; follower travelled %.1f speed %.1f; rollout travelled %.1f speed %.1f"),
				Ticks * Dt, static_cast<int32>(Agent->Phase), M.Position.X, M.Position.Y,
				FMath::RadiansToDegrees(M.Heading), Speed,
				Agent->Follower.Travelled, Agent->Follower.Speed, Agent->Arrival.Travelled, Agent->Arrival.Speed);
		}
		else if (Agent->Phase == EAgentPhase::Arriving && Agent->Arrival.Travelled > Agent->Arrival.VacateAt - 40.0)
		{
			UE_LOG(LogExitArcTest, Log,
				TEXT("t=%.3f phase %d at (%.1f, %.1f) hdg %.2f deg measured %.1f uu/s; rollout travelled %.1f of %.1f speed %.1f"),
				Ticks * Dt, static_cast<int32>(Agent->Phase), M.Position.X, M.Position.Y,
				FMath::RadiansToDegrees(M.Heading), Speed,
				Agent->Arrival.Travelled, Agent->Arrival.VacateAt, Agent->Arrival.Speed);
		}
		PrevAt = M.Position;
		PrevHeading = M.Heading;
		PrevSpeed = Speed;
		bHadGround = true;
	}

	UE_LOG(LogExitArcTest, Log,
		TEXT("Handover measured over %d ticks: worst speed step %.1f uu/s per tick at %.2f s (allowed %.1f), worst heading step %.3f deg at %.2f s (allowed %.3f), parked %d"),
		Ticks, WorstSpeedStep, WorstSpeedAt, SpeedStepAllowed,
		FMath::RadiansToDegrees(WorstHeadingStep), WorstHeadingAt, FMath::RadiansToDegrees(HeadingStepAllowed), bParked);

	TestTrue(TEXT("the aircraft taxied after landing"), bSawTaxi);
	TestTrue(TEXT("and parked within ten minutes"), bParked);
	TestTrue(FString::Printf(TEXT("no speed step beyond one tick's braking or acceleration (worst %.1f uu/s at %.2f s, allowed %.1f)"),
			WorstSpeedStep, WorstSpeedAt, SpeedStepAllowed),
		WorstSpeedStep <= SpeedStepAllowed);
	TestTrue(FString::Printf(TEXT("no heading step beyond one tick's turn (worst %.3f deg at %.2f s, allowed %.3f)"),
			FMath::RadiansToDegrees(WorstHeadingStep), WorstHeadingAt, FMath::RadiansToDegrees(HeadingStepAllowed)),
		WorstHeadingStep <= HeadingStepAllowed);
	return true;
}

#include "Model/LandingRun.h"

/**
 * THE AIRCRAFT THAT ROLLED STRAIGHT PAST ITS EXIT (PIE, 2026-09-06, the first build with
 * arcs). The exit arc began between the distance the aircraft is actually slowed by and the
 * margined "needed" figure, so it was ruled unusable; the junction's own node-end, 6000 uu
 * further on, was not - and from there the only way off is along the runway to the next
 * split and back through the hairpin. Two rules, both measured here: usability is judged at
 * the raw slowed-by distance, and a node whose route begins along the strip is no exit.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FArrivalTakesTheArcNotTheJunctionTest,
	"Airside.Model.ArrivalTakesTheArcNotTheJunction",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FArrivalTakesTheArcNotTheJunctionTest::RunTest(const FString& Parameters)
{
	const FAirframe Airframe = UAirsideSettings::ResolveDefaultAirframe();
	const double Raw = FLandingRun::RequiredLandingDistance(Airframe.Ground, Airframe.Climb, Airframe.Approach);
	const double Needed = Raw * FLandingRun::LandingMargin;
	// The junction 3000 past the margined figure: its arc starts 3000 SHORT of it, and well
	// past the raw one.
	FExitArcAirport A = ExitArcBuildAirport(GetTransientPackage(), /*bWithStand=*/true, Needed + 3000.0);
	const double ArcStart = Needed + 3000.0 - A.ExitLength;
	TestTrue(FString::Printf(TEXT("fixture: arc start %.0f lies between slowed-by %.0f and needed %.0f"), ArcStart, Raw, Needed),
		ArcStart > Raw && ArcStart < Needed);

	const FArrivalPlan Plan = ArrivalPlanner::Plan(*A.Net, A.Threshold - FVector2D(1000.0, 0.0), Airframe);
	if (!TestTrue(FString::Printf(TEXT("the arrival is planned: %s"), *ArrivalPlanner::DescribeRefusal(Plan)), Plan.IsValid()))
	{
		return false;
	}
	double Miss = 0.0;
	const FGuidelineNodeId SUp = ExitArcNodeNear(*A.Net, A.XAt - FVector2D(A.ExitLength, 0.0), Miss);
	TestTrue(TEXT("the upstream arc start exists"), Miss < 1.0);
	TestTrue(FString::Printf(TEXT("the exit is the arc start (vacating at %.0f, arc start %.0f, junction %.0f)"),
			Plan.VacateAt, ArcStart, Needed + 3000.0),
		Plan.Exit == SUp && FMath::Abs(Plan.VacateAt - ArcStart) < 1.0);

	bool bAlongRunway = false;
	for (const FRouteStep& Step : Plan.TaxiIn.Steps)
	{
		const FGuidelineEdge* Edge = A.Net->GetGuidelineEdge(Step.Edge);
		bAlongRunway = bAlongRunway || (Edge && Edge->DerivedFrom.IsSet() && A.Net->IsRunwaySegment(Edge->DerivedFrom));
	}
	TestFalse(TEXT("the taxi-in never runs along the runway - it turns off, forward, at the arc"), bAlongRunway);
	TestTrue(TEXT("and its first span heads down the runway, not back"),
		Plan.TaxiIn.Polyline.Num() > 1
		&& FVector2D::DotProduct(Plan.TaxiIn.Polyline[1] - Plan.TaxiIn.Polyline[0], FVector2D(1.0, 0.0)) > 0.0);
	return true;
}

#endif
