#include "CoreMinimal.h"
#include "Build/RoadNetworkSolver.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Profiles/RoadProfile.h"
#include "Tool/RoadSnap.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE ROAD YOU CAN SEE IS THE ROAD YOU HIT.
 *
 * SegmentRadius is a band about the CENTRELINE, so on any road wider than twice that band
 * the outer pavement resolved Free - the plot tool refused to anchor while the cursor was
 * plainly on the tarmac (PIE, 2026-09-16). FRoadNodeSnapRule had already learned this for
 * junctions; this pins the same rule for the segments between them.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadSnapReachesAcrossThePavementTest,
	"Airside.Tool.SnapReachesAcrossThePavement",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadSnapReachesAcrossThePavementTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Network = NewObject<URoadNetwork>(GetTransientPackage());
	if (!TestNotNull(TEXT("network constructed"), Network)) { return false; }

	// 12 m wide, so 6 m of pavement either side of the centreline - four times the 1.5 m
	// SegmentRadius below, which is what makes the gap this test covers reachable at all.
	URoadProfile* Wide = URoadProfile::MakeTransient(1200.0, 100.0, 0.0);
	if (!TestNotNull(TEXT("a wide profile"), Wide)) { return false; }

	const FRoadNodeId West = Network->AddNode(FVector2D(-5000.0, 0.0));
	const FRoadNodeId East = Network->AddNode(FVector2D(5000.0, 0.0));
	Network->AddStraightSegment(West, East, Wide);

	FRoadSnapSettings Settings;
	Settings.NodeRadius = 150.0;
	Settings.SegmentRadius = 150.0;
	Settings.MinSplitFromEndpoint = 50.0;
	Settings.bSnapToSegments = true;

	const FRoadSnapChain Chain;

	// 4 m off the centreline: well outside SegmentRadius, and well INSIDE the road.
	const FRoadSnapResult OnPavement =
		Chain.Resolve(*Network, FVector2D(0.0, 400.0), Settings);
	TestTrue(TEXT("a cursor on the tarmac snaps to the road it is standing on"),
		OnPavement.Kind == ERoadSnapKind::Segment);

	// AND THE ROAD STILL ENDS. Reach is the road's own half width, not an excuse to snap
	// from anywhere - a cursor off the pavement by more than the near-miss margin is Free,
	// or the player could never place anything beside a road again.
	const FRoadSnapResult Beyond =
		Chain.Resolve(*Network, FVector2D(0.0, 900.0), Settings);
	TestTrue(TEXT("but a cursor clear of the pavement is still Free"),
		Beyond.Kind == ERoadSnapKind::Free);

	// A NARROW ROAD KEEPS THE AUTHORED MARGIN. The reach is a floor, not a replacement, so
	// hovering just off a thin road still finds it.
	URoadProfile* Thin = URoadProfile::MakeTransient(100.0, 100.0, 0.0);
	if (!TestNotNull(TEXT("a thin profile"), Thin)) { return false; }
	URoadNetwork* Second = NewObject<URoadNetwork>(GetTransientPackage());
	const FRoadNodeId A = Second->AddNode(FVector2D(-5000.0, 0.0));
	const FRoadNodeId B = Second->AddNode(FVector2D(5000.0, 0.0));
	Second->AddStraightSegment(A, B, Thin);

	const FRoadSnapResult NearMiss =
		Chain.Resolve(*Second, FVector2D(0.0, 120.0), Settings);
	TestTrue(TEXT("just off a 1 m road still finds it, on SegmentRadius alone"),
		NearMiss.Kind == ERoadSnapKind::Segment);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadSnapTest,
	"Airside.Tool.Snap",
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

		// And the junction reach off with it. A dead end on this profile paves ~200 uu, so
		// with the reach live the node rule claims this whole neighbourhood and the split
		// floor below is unreachable - which is the correct behaviour, asserted in
		// Airside.Tool.JunctionClearance. This block is about the floor itself, so it opts
		// out of the reach the way a caller would.
		NarrowNodes.JunctionSnapFactor = 0.0;

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

		// The same cursor with the reach LIVE resolves to the node instead, even though
		// NodeRadius is only 10 uu. This is the pair that shows the factor is what decides
		// it, rather than some other difference between the two calls.
		const FRoadSnapResult WithReach =
			Chain.Resolve(*Network, FVector2D(30.0, 20.0), Settings);
		TestTrue(TEXT("the junction reach claims what a 10 uu NodeRadius could not"),
			WithReach.Kind == ERoadSnapKind::Node);
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

/**
 * THE CHEAP REJECT, issue #167. Before this, every live node outside the fixed radius paid a
 * full FRoadNetworkSolver::NodeClaims (a junction solve, with the TArray allocations that go
 * with assembling arms and a boundary polygon) just to be told the cursor was nowhere near
 * it - on an airport of more than a handful of nodes, the dominant cost of every hover.
 * MaxPossibleNodeClaimReach's whole job is to answer "could this node possibly be it" from
 * data already on the node, with no solve, so a node the cursor cannot possibly be inside
 * never reaches NodeClaims at all - while one it genuinely could be inside still does, and
 * still snaps.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadNodeSnapRuleCheapRejectTest,
	"Airside.Tool.NodeSnapDoesNotSolveWhatCannotBeInReach",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadNodeSnapRuleCheapRejectTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Network = NewObject<URoadNetwork>(GetTransientPackage());
	if (!TestNotNull(TEXT("network constructed"), Network)) { return false; }

	// A DEAD END, deliberately: FRoadNetworkSolver::NodeClaims' own comment records that a
	// node with no polygon - a dead end among them - falls back to a plain circle at the
	// arm's half-width, which is what makes this fixture's answer predictable without having
	// to reason about a real corner's fitted boundary. Hub has exactly one incident segment;
	// Far the same. 10 m wide, so 500 uu of half-width either side of the centreline.
	URoadProfile* Profile = URoadProfile::MakeTransient(1000.0, 100.0, 0.0);
	if (!TestNotNull(TEXT("a profile"), Profile)) { return false; }

	const FRoadNodeId Hub = Network->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId Far = Network->AddNode(FVector2D(2000.0, 0.0));
	Network->AddStraightSegment(Hub, Far, Profile);

	FRoadSnapSettings Settings;
	Settings.NodeRadius = 150.0;
	Settings.SegmentRadius = 150.0;
	Settings.bSnapToSegments = false;   // isolates the node rule from the segment rule
	Settings.JunctionSnapFactor = 1.0;

	const FRoadNodeSnapRule Rule;
	FRoadSnapQuery Query;
	FRoadSnapResult Result;

	// FAR OUTSIDE ANYTHING this 2000 uu chord could pave. The cheap reject must decline both
	// nodes here before NodeClaims runs for either of them.
	FRoadNetworkSolver::ResetNodeClaimsCallCountForTest();
	Query.Cursor = FVector2D(50000.0, 0.0);
	const bool bFarClaimed = Rule.Resolve(*Network, Query, Settings, Result);
	TestFalse(TEXT("a cursor far beyond any possible reach is not claimed"), bFarClaimed);
	TestEqual(TEXT("and the solve never ran for it - this is the line that goes red if the "
					"cheap reject is removed"),
		FRoadNetworkSolver::NodeClaimsCallCountForTest, 0);

	// WITHIN HUB'S OWN DEAD-END REACH (its arm's 500 uu half-width), past the fixed 150 uu
	// radius - the same shape as Airside.Tool.Snap's "the junction reach claims what
	// NodeRadius could not", on the simplest topology that answer is unambiguous for.
	FRoadNetworkSolver::ResetNodeClaimsCallCountForTest();
	Query.Cursor = FVector2D(300.0, 0.0);
	const bool bNearClaimed = Rule.Resolve(*Network, Query, Settings, Result);
	TestTrue(TEXT("a cursor inside the dead end's own reach still snaps"), bNearClaimed);
	TestTrue(TEXT("to the hub"), Result.Kind == ERoadSnapKind::Node && Result.Node == Hub);
	TestTrue(TEXT("and the solve DID run to decide it"),
		FRoadNetworkSolver::NodeClaimsCallCountForTest > 0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
