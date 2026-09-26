#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Build/TurnShape.h"
#include "Content/AirsideSettings.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"
#include "Testing/BendProbe.h"

#if WITH_DEV_AUTOMATION_TESTS

// CLASSIFYTURN DECIDES THE SHAPE ONCE (issue #305): a bend's arc concentric with the pavement's
// inner edge, a width taper's S, or the plain chord - replacing the `BendArc.Num() > 0` /
// `bTaperS` flag pair FRoadGuidelineBuilder::Build used to re-derive independently at piece
// emission and again, 90 lines later, in the lock warning. One fixture per shape, each derived
// the production way (content tiers or a plain profile, the solver, then the builder), so
// ClassifyTurn is asked exactly what Build itself asks it - not a hand-built approximation that
// might pass for the wrong reason.

namespace ClassifyTurnFixture
{
	/** What ClassifyTurn needs for one side of a turn, read back from the node Build already
	 *  left it at (BendProbe::FTurnChain::From/To) via its Origin - the same (Segment, end,
	 *  guideline index) DeriveSegmentGuidelines stamped on it when it derived the edge. */
	struct FEnd
	{
		FRoadSegmentId Segment;
		const URoadProfile* Profile = nullptr;
		ETraversalClass Class = ETraversalClass::GroundVehicle;
	};

	FEnd EndOf(const URoadNetwork& Net, FGuidelineNodeId NodeId)
	{
		FEnd Out;
		const FGuidelineNode* Node = Net.GetGuidelineNode(NodeId);
		if (Node == nullptr || !Node->Origin.IsSet())
		{
			return Out;
		}
		Out.Segment = Node->Origin.Segment;
		const FRoadSegment* Segment = Net.GetSegment(Out.Segment);
		Out.Profile = Segment ? Net.ProfileFor(*Segment) : nullptr;
		if (Out.Profile != nullptr && Out.Profile->Guidelines.IsValidIndex(Node->Origin.GuidelineIndex))
		{
			Out.Class = Out.Profile->Guidelines[Node->Origin.GuidelineIndex].Class;
		}
		return Out;
	}

	/**
	 * Runs ClassifyTurn exactly as Build does, on the first turn chain BendProbe::TurnsAt finds
	 * at Node - its From/To are the same nodes Build's own Turn.A/Turn.B were, before Build asked
	 * the question this pins. Chord when nothing qualifies (no turn at all, or an end whose
	 * segment vanished), which is also ClassifyTurn's own default so a missing fixture never
	 * misreads as one of the other two shapes.
	 */
	ETurnShape ClassifyFirstTurnAt(const URoadNetwork& Net, const FRoadSolveResult& Solved, FRoadNodeId Node)
	{
		const TArray<BendProbe::FTurnChain> Turns = BendProbe::TurnsAt(Net, Node);
		if (Turns.Num() == 0)
		{
			return ETurnShape::Chord;
		}
		const FEnd From = EndOf(Net, Turns[0].From);
		const FEnd To = EndOf(Net, Turns[0].To);
		const FJunctionResult* Junction = Solved.NodeResults.Find(Node.Index);
		const TArray<FRoadSegmentId>* ArmSegments = Solved.NodeArmSegments.Find(Node.Index);
		if (Junction == nullptr || ArmSegments == nullptr || From.Profile == nullptr || To.Profile == nullptr)
		{
			return ETurnShape::Chord;
		}
		// Turn.Control is only ever WRITTEN by ClassifyTurn (the TaperS case); its value going in
		// is never read, so a zeroed scratch edge is exactly as good as the one Build builds.
		FGuidelineEdge Turn;
		Turn.A = Turns[0].From;
		Turn.B = Turns[0].To;
		const FTurnGeometry Geometry = ClassifyTurn(Net, Solved, *Junction, Node.Index, Node, *ArmSegments,
			From.Segment, To.Segment, From.Profile, To.Profile, From.Class, To.Class, Turn);
		return Geometry.Shape;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClassifyTurnBendTest, "Airside.Build.ClassifyTurn.Bend",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FClassifyTurnBendTest::RunTest(const FString& Parameters)
{
	const TArray<URoadProfile*> Tiers = TestProfiles::ServiceTiers();
	if (!TestTrue(TEXT("the content set has its service-road tiers, and they load"),
		Tiers.Num() == 3 && !Tiers.Contains(nullptr))) { return false; }

	// BendLaneTest's own right-angle fixture, default corner: a real two-arm bend, both lanes
	// GroundVehicle-classed, concentric with a rounded inner corner.
	const TestGraph::FCornerFixture Bend = TestGraph::Corner(Tiers[0]);
	const ETurnShape Shape = ClassifyTurnFixture::ClassifyFirstTurnAt(*Bend.Net, Bend.Solved, Bend.Corner);
	return TestTrue(TEXT("a two-arm road bend classifies as BendArc"), Shape == ETurnShape::BendArc);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClassifyTurnWidthTaperTest, "Airside.Build.ClassifyTurn.WidthTaper",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FClassifyTurnWidthTaperTest::RunTest(const FString& Parameters)
{
	const TArray<URoadProfile*> Tiers = TestProfiles::ServiceTiers();
	if (!TestTrue(TEXT("the content set has its service-road tiers, and they load"),
		Tiers.Num() == 3 && !Tiers.Contains(nullptr))) { return false; }

	// WidthTaperTest's own straight width-step fixture: Narrow meets Wide on a straight line, so
	// the lane ends are offset but the arms are not a corner.
	const TestGraph::FCornerFixture Step = TestGraph::Corner(
		Tiers[0], Tiers[2], FVector2D(6000.0, 0.0), FVector2D(12000.0, 0.0));
	const ETurnShape Shape = ClassifyTurnFixture::ClassifyFirstTurnAt(*Step.Net, Step.Solved, Step.Corner);
	return TestTrue(TEXT("a straight width step classifies as TaperS"), Shape == ETurnShape::TaperS);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClassifyTurnPlainTJunctionTest, "Airside.Build.ClassifyTurn.PlainTJunction",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FClassifyTurnPlainTJunctionTest::RunTest(const FString& Parameters)
{
	// A three-arm T-junction: BendArc and TaperS both require ArmSegments.Num() == 2, so this
	// falls to the plain chord whatever its geometry - the same fixture TwoWayLaneTest's
	// TJunctionTurns test uses, built inline here since that file discards its own FRoadSolveResult.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Profile = URoadProfile::MakeServiceRoadTransient();
	const FRoadNodeId Hub = Net->AddNode(FVector2D(0.0, 0.0));
	Net->AddStraightSegment(Hub, Net->AddNode(FVector2D(-20000.0, 0.0)), Profile);
	Net->AddStraightSegment(Hub, Net->AddNode(FVector2D(20000.0, 0.0)), Profile);
	Net->AddStraightSegment(Hub, Net->AddNode(FVector2D(0.0, 20000.0)), Profile);
	const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Net);
	FRoadGuidelineBuilder::Build(*Net, Solved, UAirsideSettings::ResolveRoadDesignVehicles());

	const ETurnShape Shape = ClassifyTurnFixture::ClassifyFirstTurnAt(*Net, Solved, Hub);
	return TestTrue(TEXT("a three-arm T-junction classifies as Chord"), Shape == ETurnShape::Chord);
}

#endif
