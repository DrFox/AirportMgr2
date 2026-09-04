#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Profiles/RoadProfile.h"
#include "Tool/RoadPlacement.h"
#include "Tool/RoadSnap.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** A snap result naming an existing node, as the node rule would produce. */
	FRoadSnapResult NodeSnap(const URoadNetwork& Network, FRoadNodeId Node)
	{
		FRoadSnapResult Result;
		Result.Kind = ERoadSnapKind::Node;
		Result.Node = Node;
		Result.Position = Network.GetNode(Node)->Position;
		return Result;
	}

	/** A snap result at open ground, as the Free fallback would produce. */
	FRoadSnapResult FreeSnap(const FVector2D& Where)
	{
		FRoadSnapResult Result;
		Result.Kind = ERoadSnapKind::Free;
		Result.Position = Where;
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadPlacementTest,
	"Airside.Tool.Placement",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadPlacementTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Network = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Profile = URoadProfile::MakeTransient(200.0, 100.0, 20.0);
	if (!TestNotNull(TEXT("network constructed"), Network))
	{
		return false;
	}

	FRoadPlacementLimits Limits;
	Limits.MinSegmentLength = 250.0;
	Limits.MinTurnDegrees = 25.0;

	// A road running east from the origin. Everything below is judged at Hub, which has
	// exactly one arm - pointing east - so the corner maths has something to bite on.
	const FRoadNodeId Hub = Network->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId East = Network->AddNode(FVector2D(2000.0, 0.0));
	const FRoadNodeId Loose = Network->AddNode(FVector2D(0.0, 2000.0));
	TestTrue(TEXT("fixture segment created"),
		Network->AddStraightSegment(Hub, East, Profile).IsSet());

	// Open ground, well clear and square to the existing arm.
	TestTrue(TEXT("a long segment square to the existing arm is valid"),
		RoadPlacement::Validate(*Network, Hub, FreeSnap(FVector2D(0.0, 1500.0)), Limits)
			== ERoadPlacement::Valid);

	// An unconnected node is a perfectly good target.
	TestTrue(TEXT("connecting to an unjoined node is valid"),
		RoadPlacement::Validate(*Network, Hub, NodeSnap(*Network, Loose), Limits)
			== ERoadPlacement::Valid);

	TestTrue(TEXT("a node cannot run a segment to itself"),
		RoadPlacement::Validate(*Network, Hub, NodeSnap(*Network, Hub), Limits)
			== ERoadPlacement::SameNode);

	// The pair is already joined by the fixture segment. Caught by walking Hub's own
	// incidence, so it holds whichever end the second segment would be drawn from.
	TestTrue(TEXT("a second segment between the same pair is refused"),
		RoadPlacement::Validate(*Network, Hub, NodeSnap(*Network, East), Limits)
			== ERoadPlacement::AlreadyJoined);
	TestTrue(TEXT("and refused from the other end too"),
		RoadPlacement::Validate(*Network, East, NodeSnap(*Network, Hub), Limits)
			== ERoadPlacement::AlreadyJoined);

	// Length. 249 against a limit of 250 - one unit inside, so the comparison itself is
	// under test rather than the order of magnitude.
	TestTrue(TEXT("one unit under the minimum length is refused"),
		RoadPlacement::Validate(*Network, Hub, FreeSnap(FVector2D(0.0, 249.0)), Limits)
			== ERoadPlacement::TooShort);
	TestTrue(TEXT("exactly the minimum length is allowed"),
		RoadPlacement::Validate(*Network, Hub, FreeSnap(FVector2D(0.0, 250.0)), Limits)
			== ERoadPlacement::Valid);

	// Turn angle, against the existing arm that leaves Hub heading east. A target east
	// and slightly north makes a narrow corner with it.
	{
		const double Narrow = FMath::DegreesToRadians(20.0);
		const FVector2D TwentyDegrees(FMath::Cos(Narrow) * 1500.0, FMath::Sin(Narrow) * 1500.0);
		TestTrue(TEXT("a 20 degree corner is too sharp"),
			RoadPlacement::Validate(*Network, Hub, FreeSnap(TwentyDegrees), Limits)
				== ERoadPlacement::TooSharp);

		const double Wide = FMath::DegreesToRadians(30.0);
		const FVector2D ThirtyDegrees(FMath::Cos(Wide) * 1500.0, FMath::Sin(Wide) * 1500.0);
		TestTrue(TEXT("a 30 degree corner is allowed"),
			RoadPlacement::Validate(*Network, Hub, FreeSnap(ThirtyDegrees), Limits)
				== ERoadPlacement::Valid);
	}

	// Drawing straight along the existing arm is the degenerate case of a sharp turn:
	// zero degrees against it. Distinct from SameNode - the far end is open ground.
	TestTrue(TEXT("doubling straight back along an arm is too sharp"),
		RoadPlacement::Validate(*Network, Hub, FreeSnap(FVector2D(1500.0, 0.0)), Limits)
			== ERoadPlacement::TooSharp);

	// Straight through is 180 degrees against the arm, the widest corner there is, and
	// must never be mistaken for a tight one.
	TestTrue(TEXT("continuing straight through is valid"),
		RoadPlacement::Validate(*Network, Hub, FreeSnap(FVector2D(-1500.0, 0.0)), Limits)
			== ERoadPlacement::Valid);

	// A node with no arms has no corner to measure, so the turn rule must stand aside
	// rather than reject for want of anything to compare against.
	TestTrue(TEXT("a node with no arms accepts any direction"),
		RoadPlacement::Validate(*Network, Loose, FreeSnap(FVector2D(0.0, 4000.0)), Limits)
			== ERoadPlacement::Valid);

	// A dead handle is reported as such, not silently treated as valid.
	{
		FRoadNodeId Ghost;
		Ghost.Index = 999;
		Ghost.Generation = 0;
		TestTrue(TEXT("a start node that is not live is refused"),
			RoadPlacement::Validate(*Network, Ghost, FreeSnap(FVector2D(0.0, 1500.0)), Limits)
				== ERoadPlacement::NoStart);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
