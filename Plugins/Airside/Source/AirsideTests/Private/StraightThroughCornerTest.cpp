#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Profiles/RoadProfile.h"
#include "Solve/RoadGeom.h"
#include "Tool/RoadPlacement.h"
#include "Tool/RoadSnap.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * A ROAD MAY BE EXTENDED DEAD STRAIGHT. Reported from PIE, samples/issue1.png: a ghost
 * refused with "too short to hold the corner" while continuing a road along its own line -
 * where there is no corner at all.
 *
 * THE CAUSE IS THAT sin IS SYMMETRIC. RoadGeom::CornerReachAtZeroRadius guarded on
 * `sin(Theta) < 1e-9` to catch the HAIRPIN - two arms doubling back, where the corner's reach
 * genuinely diverges - and that guard also catches Theta = pi, which is the exact opposite: a
 * straight-through node, no corner, zero reach. One test per layer, because the arithmetic and
 * the refusal are in different modules and either could regress alone.
 *
 * THE GUIDES DID NOT CAUSE IT, THEY EXPOSED IT. At 179.9 degrees the reach computes to about
 * a thousandth of a half-width and passes; only exactly 180 refuses. Landing there took luck
 * until "along this road" started snapping the click onto the line every time.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCornerReachIsZeroStraightThroughTest,
	"Airside.Solve.CornerReachIsZeroStraightThrough",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FCornerReachIsZeroStraightThroughTest::RunTest(const FString& Parameters)
{
	const double Half = 200.0;

	// STRAIGHT THROUGH. Both tangents point AWAY from the node they meet at, so a road
	// continued along its own line puts them at pi to each other - see RoadPlacement's own
	// comment: "small means a hairpin, 180 means straight through".
	double AlongA = -1.0;
	double AlongB = -1.0;
	if (TestTrue(TEXT("a straight-through node has a corner reach at all"),
		RoadGeom::CornerReachAtZeroRadius(Half, Half, UE_DOUBLE_PI, AlongA, AlongB)))
	{
		TestEqual(TEXT("and it consumes nothing of the arm ahead"), AlongA, 0.0);
		TestEqual(TEXT("nor of the arm behind"), AlongB, 0.0);
	}

	// UNEQUAL WIDTHS, STILL STRAIGHT. The two inner edges are parallel lines a width-step
	// apart and never meet, so there is no intersection to solve - and still no corner to
	// hold. A wide road meeting a narrow one head-on just steps down at the node.
	double WideA = -1.0;
	double NarrowB = -1.0;
	if (TestTrue(TEXT("a width step straight through is not a corner either"),
		RoadGeom::CornerReachAtZeroRadius(400.0, 150.0, UE_DOUBLE_PI, WideA, NarrowB)))
	{
		TestEqual(TEXT("the wide arm gives up nothing"), WideA, 0.0);
		TestEqual(TEXT("and neither does the narrow one"), NarrowB, 0.0);
	}

	// THE HAIRPIN STILL REFUSES, which is the half of the old guard that was right. Without
	// this leg the fix could be "always return true" and the test would not notice.
	double HairpinA = 0.0;
	double HairpinB = 0.0;
	TestFalse(TEXT("but a hairpin doubling back has no finite reach"),
		RoadGeom::CornerReachAtZeroRadius(Half, Half, 0.0, HairpinA, HairpinB));

	// AND AN ORDINARY CORNER IS UNTOUCHED. A right angle between equal widths reaches exactly
	// one half-width along each arm - w / tan(45 degrees). The control that says this change
	// moved the two degenerate ends and nothing in between.
	double SquareA = 0.0;
	double SquareB = 0.0;
	if (TestTrue(TEXT("a right-angled corner still solves"),
		RoadGeom::CornerReachAtZeroRadius(Half, Half, UE_DOUBLE_PI / 2.0, SquareA, SquareB)))
	{
		TestTrue(TEXT("reaching one half-width along each arm"),
			FMath::IsNearlyEqual(SquareA, Half, 1.0e-6) && FMath::IsNearlyEqual(SquareB, Half, 1.0e-6));
	}

	return true;
}

/**
 * AND THE GHOST SAYS SO: the same case through RoadPlacement::Validate, which is what the
 * player actually met.
 *
 * A SEPARATE TEST FROM THE ARITHMETIC ABOVE because the refusal needs NewRoadHalfWidth set,
 * and that is the gate the whole corner-fit rule sits behind - every caller from before the
 * rule passes 0 and never reaches it. A test that left it 0 would pass on the broken build.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadExtendsStraightAheadTest,
	"Airside.Tool.RoadExtendsStraightAhead",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadExtendsStraightAheadTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Network = NewObject<URoadNetwork>(GetTransientPackage());
	if (!TestNotNull(TEXT("a network"), Network)) { return false; }

	URoadProfile* Profile = URoadProfile::MakeTransient(400.0, 100.0, 40.0);
	if (!TestNotNull(TEXT("a profile"), Profile)) { return false; }

	// A road running EAST from West to Hub. Continuing east from Hub is the reported gesture.
	const FRoadNodeId West = Network->AddNode(FVector2D(-4000.0, 0.0));
	const FRoadNodeId Hub = Network->AddNode(FVector2D(0.0, 0.0));
	if (!TestTrue(TEXT("the road behind exists"),
		Network->AddStraightSegment(West, Hub, Profile).IsSet()))
	{
		return false;
	}

	FRoadPlacementLimits Limits;
	Limits.MinSegmentLength = 250.0;
	Limits.MinTurnDegrees = 25.0;

	// THE GATE. Without a half-width the corner-fit rule is skipped entirely and this test
	// would pass against the bug - which is exactly why the bug survived to reach PIE.
	Limits.NewRoadHalfWidth = Profile->GetMaxHalfWidth();
	if (!TestTrue(TEXT("the new road has a half-width, so the corner rule runs"),
		Limits.NewRoadHalfWidth > 0.0))
	{
		return false;
	}

	FRoadSnapResult Ahead;
	Ahead.Kind = ERoadSnapKind::Free;
	Ahead.Position = FVector2D(4000.0, 0.0);

	TestEqual(TEXT("a road may be continued along its own line"),
		static_cast<int32>(RoadPlacement::Validate(*Network, Hub, Ahead, Limits)),
		static_cast<int32>(ERoadPlacement::Valid));

	// A HAIR OFF STRAIGHT WAS ALWAYS FINE, and saying so pins what the bug actually was: not
	// "straight roads are refused" but "EXACTLY straight is", which is why it took a guide
	// that lands the click precisely on the line to make it a common sight.
	FRoadSnapResult NearlyAhead;
	NearlyAhead.Kind = ERoadSnapKind::Free;
	NearlyAhead.Position = FVector2D(4000.0, 7.0);
	TestEqual(TEXT("as it always could a fraction off it"),
		static_cast<int32>(RoadPlacement::Validate(*Network, Hub, NearlyAhead, Limits)),
		static_cast<int32>(ERoadPlacement::Valid));

	// AND DOUBLING BACK IS STILL REFUSED. Drawing west from Hub, back along the road that is
	// already there, is the hairpin the guard was written for. TooSharp catches it before the
	// corner rule does - which is fine, and worth pinning: the point is that it is refused,
	// and that this fix did not open it up.
	FRoadSnapResult Backwards;
	Backwards.Kind = ERoadSnapKind::Free;
	Backwards.Position = FVector2D(-4000.0, 1.0);
	TestNotEqual(TEXT("but doubling back along it is still refused"),
		static_cast<int32>(RoadPlacement::Validate(*Network, Hub, Backwards, Limits)),
		static_cast<int32>(ERoadPlacement::Valid));

	return true;
}

#endif
