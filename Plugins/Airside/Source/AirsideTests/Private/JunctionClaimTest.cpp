#include "CoreMinimal.h"
#include "Build/RoadNetworkSolver.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Profiles/RoadProfile.h"
#include "Tool/RoadSnap.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FJunctionClaimTest,
	"Airside.Tool.JunctionClaimsPavementOnly",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FJunctionClaimTest::RunTest(const FString& Parameters)
{
	// 2026-09-06: with the road tool chaining from a tight junction, the cursor a hand's
	// width off the pavement still read "same node" and no road could start there. The
	// junction claimed the cursor within a CIRCLE of its deepest cut, and a tight corner's
	// fitted cut runs deep along its arms - so the circle covered open ground beside it.
	//
	// A 3-way junction on the 400-wide default profile: east, west, and a 30-degree arm.
	// The 30-degree corner pushes the cut along the east arm far out, so a point 800 uu
	// east sits on the junction pavement while a point 800 uu SOUTH is open ground.
	URoadProfile* Profile = URoadProfile::MakeTransient(400.0, 1500.0);
	URoadNetwork* Net = NewObject<URoadNetwork>();
	const FRoadNodeId Centre = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId East = Net->AddNode(FVector2D(3000.0, 0.0));
	const FRoadNodeId West = Net->AddNode(FVector2D(-3000.0, 0.0));
	const double R30 = FMath::DegreesToRadians(30.0);
	const FRoadNodeId Spur = Net->AddNode(FVector2D(3000.0 * FMath::Cos(R30), 3000.0 * FMath::Sin(R30)));
	Net->AddStraightSegment(Centre, East, Profile);
	Net->AddStraightSegment(Centre, West, Profile);
	Net->AddStraightSegment(Centre, Spur, Profile);

	const FVector2D OnPavement(800.0, 0.0);      // along the east arm, inside the cut
	const FVector2D OpenGround(0.0, -800.0);     // straight south: no arm, no pavement

	// The old rule's premise, kept as evidence: the reach circle DID cover the open ground.
	TestTrue(TEXT("the junction's reach exceeds 800 uu - the circle rule would have claimed the open ground"),
		FRoadNetworkSolver::NodeReach(*Net, Centre) > 800.0);

	TestTrue(TEXT("the junction claims a point on its own pavement"),
		FRoadNetworkSolver::NodeClaims(*Net, Centre, OnPavement));
	TestFalse(TEXT("and does not claim open ground beside it"),
		FRoadNetworkSolver::NodeClaims(*Net, Centre, OpenGround));

	FRoadSnapSettings Settings;
	Settings.NodeRadius = 150.0;
	Settings.SegmentRadius = 150.0;
	Settings.MinSplitFromEndpoint = 50.0;
	Settings.bSnapToSegments = false;
	Settings.JunctionSnapFactor = 1.0;
	FRoadSnapChain Chain;

	const FRoadSnapResult Paved = Chain.Resolve(*Net, OnPavement, Settings);
	TestEqual(TEXT("the snap chain resolves the paved point to the node"), Paved.Kind, ERoadSnapKind::Node);
	const FRoadSnapResult Open = Chain.Resolve(*Net, OpenGround, Settings);
	TestEqual(TEXT("and leaves the open ground free - a road can start there"), Open.Kind, ERoadSnapKind::Free);

	// A dead end has no polygon: it still claims within its cap and no further.
	TestTrue(TEXT("a dead end claims within its cap"), FRoadNetworkSolver::NodeClaims(*Net, East, FVector2D(3000.0, 150.0)));
	TestFalse(TEXT("and not beyond it"), FRoadNetworkSolver::NodeClaims(*Net, East, FVector2D(3000.0, 600.0)));
	return true;
}

#endif
