#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Profiles/RoadProfile.h"
#include "Solve/GuideArbiter.h"
#include "Tool/ApronDrawTool.h"
#include "Tool/RoadEditTarget.h"
#include "Tool/RunwayTool.h"
#include "Tool/SnapGuideChain.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * A RUNWAY'S FIRST THRESHOLD IS AN ANCHOR, AND IT CARRIES NO REFERENCE.
 *
 * Guides exist only where a tool implements DescribeGuideAnchor, or opts into the base's free
 * start. Three registrations answered before 2026-09-20 and the runway tool was not one of
 * them, so drawing a runway was unguided whatever the toggles said - which is the inverse of
 * the report that started this work.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRunwayAnchorIsItsFirstThresholdTest,
	"Airside.Tool.RunwayAnchorIsItsFirstThreshold",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayAnchorIsItsFirstThresholdTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a network actor"), Actor)) { return false; }

	IRoadEditTarget* Target = Actor;
	FRunwayTool Tool;

	// NOTHING PENDING: before a threshold is down there is no point OF THE GESTURE'S OWN for a
	// line to swing around, so since 2026-09-20 the tool answers a FREE START instead of
	// declining - the driver puts the cursor in Origin and every angular candidate then sits
	// out inside the arbiter. What it must NOT do is anchor on (0, 0), which is what an
	// unflagged anchor here would mean.
	FGuideAnchor Idle;
	if (!TestTrue(TEXT("an idle runway tool offers a free start"),
		Tool.DescribeGuideAnchor(Actor->Network, Target, Idle)))
	{
		return false;
	}
	TestTrue(TEXT("flagged as one, so the driver knows to fill Origin with the cursor"),
		Idle.bFreeStart);
	TestTrue(TEXT("and carrying no origin of its own, because it has none to give"),
		Idle.Origin.IsNearlyZero());

	Tool.OnClick(TestTool::ContextAt(*Target, FVector2D(4000.0, 1000.0)));

	FGuideAnchor Anchor;
	if (!TestTrue(TEXT("with a threshold down, the tool offers an anchor"),
		Tool.DescribeGuideAnchor(Actor->Network, Target, Anchor)))
	{
		return false;
	}

	TestTrue(TEXT("the anchor swings around the threshold just clicked"),
		Anchor.Origin.Equals(FVector2D(4000.0, 1000.0), 1.0));

	// NO REFERENCE. A runway is two clicks and no chaining, so there is no incoming edge to
	// extend - and a zero here is what makes FExtendingGuideSource and FPointAlignGuideSource
	// correctly propose nothing rather than square the strip to something arbitrary.
	TestTrue(TEXT("and carries no reference, because a runway extends nothing"),
		Anchor.Reference.IsNearlyZero());

	// A CENTRELINE, like a road's: the strip is laid either side of the line between thresholds.
	TestEqual(TEXT("a runway drags a centreline"),
		static_cast<int32>(Anchor.Point), static_cast<int32>(EDragPoint::Centreline));

	return true;
}

/**
 * AN APRON ANCHORS ON ITS LAST CORNER, EXTENDING THE EDGE BEFORE IT.
 *
 * THE ANCHOR IS ON FOutlineDrawTool, not on FApronDrawTool: what makes it is the outline
 * gesture, which is the base's whole job.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApronAnchorExtendsItsLastEdgeTest,
	"Airside.Tool.ApronAnchorExtendsItsLastEdge",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FApronAnchorExtendsItsLastEdgeTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a network actor"), Actor)) { return false; }

	IRoadEditTarget* Target = Actor;
	FApronDrawTool Tool;

	// NOTHING DRAWN: a FREE START since 2026-09-20 - an outline may begin flush with a road
	// edge or in line with an apron already down. A BOUNDARY drag even there, because the very
	// first corner is as much a point on the shape's own limit as the fifth, and
	// FApronLineGuideSource reads exactly that to decide whether to displace by a half-width.
	FGuideAnchor Idle;
	if (!TestTrue(TEXT("an idle apron tool offers a free start"),
		Tool.DescribeGuideAnchor(Actor->Network, Target, Idle)))
	{
		return false;
	}
	TestTrue(TEXT("flagged as one, so the driver fills Origin with the cursor"), Idle.bFreeStart);
	TestEqual(TEXT("and the first corner is already a boundary, not a centreline"),
		static_cast<int32>(Idle.Point), static_cast<int32>(EDragPoint::Boundary));

	// ONE CORNER IS NEITHER. Not an edge - a point has no direction to extend - and not a free
	// start either, because the gesture HAS begun: IsIdle() is false, so the base declines.
	// This is the case that falls between the two, and it is the one an over-eager free start
	// would swallow, guiding the second corner as though the first had not been placed.
	Tool.OnClick(TestTool::ContextAt(*Target, FVector2D(0.0, 0.0)));
	FGuideAnchor OneCorner;
	TestFalse(TEXT("one corner is a point, with no edge to extend and no free start left"),
		Tool.DescribeGuideAnchor(Actor->Network, Target, OneCorner));

	// Two more, east then north: the last edge runs along +Y.
	Tool.OnClick(TestTool::ContextAt(*Target, FVector2D(6000.0, 0.0)));
	Tool.OnClick(TestTool::ContextAt(*Target, FVector2D(6000.0, 5000.0)));

	FGuideAnchor Anchor;
	if (!TestTrue(TEXT("with three corners down, the tool offers an anchor"),
		Tool.DescribeGuideAnchor(Actor->Network, Target, Anchor)))
	{
		return false;
	}

	TestTrue(TEXT("it swings around the last corner placed"),
		Anchor.Origin.Equals(FVector2D(6000.0, 5000.0), 1.0));
	TestTrue(TEXT("extending the edge before it, which runs north"),
		FMath::IsNearlyEqual(FMath::Abs(Anchor.Reference.Y), 1.0, 1.0e-6));
	TestTrue(TEXT("with the dashed line pointing back down that edge"),
		Anchor.ReferenceAt.Equals(FVector2D(6000.0, 0.0), 1.0));

	// A CORNER OF THE SHAPE ITSELF, so there is no pavement either side of it.
	TestEqual(TEXT("an apron drags a boundary"),
		static_cast<int32>(Anchor.Point), static_cast<int32>(EDragPoint::Boundary));
	TestEqual(TEXT("with no width to its left"), Anchor.HalfWidthLeft, 0.0);
	TestEqual(TEXT("nor its right"), Anchor.HalfWidthRight, 0.0);

	// EVERY CORNER BUT THE LAST, tagged as the gesture's own so the map's columns do not govern
	// them. The dragged corner is excluded: a point cannot line up with itself.
	if (!TestEqual(TEXT("the two corners behind it are offered to line up with"),
		Anchor.AlignTo.Num(), 2))
	{
		return false;
	}
	for (const FGuidePoint& Point : Anchor.AlignTo)
	{
		TestEqual(*FString::Printf(TEXT("'%s' belongs to this drawing"), *Point.Name),
			static_cast<int32>(Point.Reference),
			static_cast<int32>(SnapGuide::EReference::ThisGesture));
		TestFalse(TEXT("and none of them is the corner being dragged from"),
			Point.At.Equals(Anchor.Origin, 1.0));
	}
	TestEqual(TEXT("numbered as the player counts them, from one"),
		Anchor.AlignTo[0].Name, FString(TEXT("corner 1")));

	return true;
}

#endif
