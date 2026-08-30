#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Profiles/RoadProfile.h"
#include "Tool/RoadSnap.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadSnapTest,
	"RoadNet.Tool.Snap",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadSnapTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Network = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Profile = URoadProfile::MakeTransient(200.0, 100.0, 20.0);
	if (!TestNotNull(TEXT("network constructed"), Network))
	{
		return false;
	}

	FRoadSnapSettings Settings;
	Settings.NodeRadius = 150.0;
	Settings.SegmentRadius = 150.0;
	Settings.MinSplitFromEndpoint = 50.0;
	Settings.bSnapToSegments = true;

	const FRoadSnapChain Chain;
	TestEqual(TEXT("the default chain installs node then segment"), Chain.NumRules(), 2);

	// Nothing to snap to at all. Free must still carry a usable position, because every
	// caller reads Position without branching on Kind.
	{
		const FRoadSnapResult Nothing = Chain.Resolve(*Network, FVector2D(10.0, 20.0), Settings);
		TestTrue(TEXT("an empty network resolves Free"), Nothing.Kind == ERoadSnapKind::Free);
		TestTrue(TEXT("Free hands back the cursor untouched"), Nothing.Position == FVector2D(10.0, 20.0));
	}

	// The fixture: one long road on the X axis, and a separate pair of bare nodes up at
	// y = 1000 where no segment can interfere with a node-versus-node comparison.
	const FRoadNodeId NodeA = Network->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId NodeB = Network->AddNode(FVector2D(2000.0, 0.0));
	const FRoadSegmentId Chord = Network->AddStraightSegment(NodeA, NodeB, Profile);
	const FRoadNodeId NodeC = Network->AddNode(FVector2D(0.0, 1000.0));
	const FRoadNodeId NodeD = Network->AddNode(FVector2D(200.0, 1000.0));
	TestTrue(TEXT("fixture segment created"), Chord.IsSet());

	// --- Rule 1: an existing node ----------------------------------------------------
	{
		const FRoadSnapResult OnNode = Chain.Resolve(*Network, FVector2D(100.0, 0.0), Settings);
		TestTrue(TEXT("a cursor inside NodeRadius resolves Node"), OnNode.Kind == ERoadSnapKind::Node);
		TestTrue(TEXT("it names the node it found"), OnNode.Node == NodeA);

		// Exact, not near. Reusing a node has to land on the coordinates the graph
		// already holds - a snap that returns the cursor instead would place the click
		// up to NodeRadius away from the node it claims to have snapped to.
		TestTrue(TEXT("the position moves onto the node itself"),
			OnNode.Position == FVector2D(0.0, 0.0));
	}

	// Two candidates in range: the nearer wins, not the first found.
	{
		const FRoadSnapResult Nearer = Chain.Resolve(*Network, FVector2D(110.0, 1000.0), Settings);
		TestTrue(TEXT("two nodes in range still resolves Node"), Nearer.Kind == ERoadSnapKind::Node);
		TestTrue(TEXT("the nearer of two candidates wins"), Nearer.Node == NodeD);
	}

	// --- Rule 2: an existing segment -------------------------------------------------
	{
		const FRoadSnapResult OnChord = Chain.Resolve(*Network, FVector2D(1000.0, 100.0), Settings);
		TestTrue(TEXT("a cursor beside a segment resolves Segment"), OnChord.Kind == ERoadSnapKind::Segment);
		TestTrue(TEXT("it names the segment it would split"), OnChord.Segment == Chord);
		TestTrue(TEXT("halfway along"), FMath::IsNearlyEqual(OnChord.SegmentT, 0.5, 1e-9));
		TestTrue(TEXT("the position is projected onto the segment"),
			OnChord.Position.Equals(FVector2D(1000.0, 0.0), 1e-9));
	}

	// --- Priority: node outranks segment ---------------------------------------------
	//
	// The load-bearing ordering assertion. This cursor is in range of BOTH rules and
	// each would happily claim it: 85 uu from node A, 60 uu off the chord, and 60 uu
	// along it - clear of MinSplitFromEndpoint, so the segment rule has no reason of its
	// own to decline. Only the chain's order decides. Swap the two rules and this fails.
	{
		const FRoadSnapResult Contested = Chain.Resolve(*Network, FVector2D(60.0, 60.0), Settings);
		TestTrue(TEXT("a node beats a segment when both are in range"),
			Contested.Kind == ERoadSnapKind::Node);
		TestTrue(TEXT("and it is the node, not a split"), Contested.Node == NodeA);
	}

	// Out of range of everything falls all the way through to Free. Taken beside the
	// middle of the chord so it is the RADIUS being tested and not the endpoint rule.
	{
		const FRoadSnapResult TooFar = Chain.Resolve(*Network, FVector2D(1000.0, -160.0), Settings);
		TestTrue(TEXT("past both radii resolves Free"), TooFar.Kind == ERoadSnapKind::Free);
		TestTrue(TEXT("and keeps the cursor"), TooFar.Position == FVector2D(1000.0, -160.0));
	}

	// --- The segment rule's own floors -----------------------------------------------
	//
	// These need the node rule out of the way to be observable at all, so they run with a
	// deliberately tiny NodeRadius. With the default radius the node rule claims this
	// whole neighbourhood first and the floors below can never be reached.
	{
		FRoadSnapSettings NarrowNodes = Settings;
		NarrowNodes.NodeRadius = 10.0;

		const FRoadSnapResult TooCloseToEnd =
			Chain.Resolve(*Network, FVector2D(30.0, 20.0), NarrowNodes);
		TestTrue(TEXT("a split within MinSplitFromEndpoint is refused"),
			TooCloseToEnd.Kind == ERoadSnapKind::Free);

		// The contrast, and the reason the case above is about the floor rather than
		// about being near a node: same segment, same offset, just past the floor.
		const FRoadSnapResult ClearOfEnd =
			Chain.Resolve(*Network, FVector2D(60.0, 20.0), NarrowNodes);
		TestTrue(TEXT("just past MinSplitFromEndpoint is allowed"),
			ClearOfEnd.Kind == ERoadSnapKind::Segment);

		// Beyond the A end entirely: the closest point on the segment IS the endpoint,
		// so the rule stands down rather than proposing a split on top of a node.
		const FRoadSnapResult BeyondEnd =
			Chain.Resolve(*Network, FVector2D(-30.0, 20.0), NarrowNodes);
		TestTrue(TEXT("a cursor beyond the end does not split at the endpoint"),
			BeyondEnd.Kind == ERoadSnapKind::Free);
	}

	// Turning segment snapping off leaves rule 1 and the Free fallback intact.
	{
		FRoadSnapSettings NoSegments = Settings;
		NoSegments.bSnapToSegments = false;

		const FRoadSnapResult Disabled = Chain.Resolve(*Network, FVector2D(1000.0, 100.0), NoSegments);
		TestTrue(TEXT("bSnapToSegments off resolves Free beside a segment"),
			Disabled.Kind == ERoadSnapKind::Free);

		const FRoadSnapResult StillNodes = Chain.Resolve(*Network, FVector2D(100.0, 0.0), NoSegments);
		TestTrue(TEXT("bSnapToSegments off leaves node snapping alone"),
			StillNodes.Kind == ERoadSnapKind::Node);
	}

	// --- Dead slots ------------------------------------------------------------------
	//
	// Removal leaves the slot in place with bAlive false, so a rule that iterates the
	// arrays without checking would keep snapping to things that are gone. Run last:
	// these mutate the fixture.
	{
		TestTrue(TEXT("removing node D"), Network->RemoveNode(NodeD));

		const FRoadSnapResult AfterRemoval = Chain.Resolve(*Network, FVector2D(110.0, 1000.0), Settings);

		// Compared by SLOT, deliberately ignoring the generation. RemoveNode bumps the
		// counter, so `!= NodeD` is true the moment the node dies whether or not the rule
		// actually skipped it - it would pass on a rule that snaps happily to dead slots.
		TestTrue(TEXT("the dead node's slot is not snapped to"),
			AfterRemoval.Node.Index != NodeD.Index);
		TestTrue(TEXT("the surviving node is found instead"), AfterRemoval.Node == NodeC);

		TestTrue(TEXT("removing the chord"), Network->RemoveSegment(Chord));

		const FRoadSnapResult NoChord = Chain.Resolve(*Network, FVector2D(1000.0, 100.0), Settings);
		TestTrue(TEXT("a removed segment is not split"), NoChord.Kind == ERoadSnapKind::Free);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
