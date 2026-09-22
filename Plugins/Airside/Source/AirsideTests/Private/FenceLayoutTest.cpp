#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Solve/FenceLayout.h"
#include "Solve/PlotYard.h"
#include "Solve/RoadGeom.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** An axis-aligned rectangle, CCW, south edge (y = 0) first. */
	TArray<FVector2D> FenceRect(double Width, double Depth)
	{
		return { FVector2D(0.0, 0.0), FVector2D(Width, 0.0),
		         FVector2D(Width, Depth), FVector2D(0.0, Depth) };
	}

	/** The asset contract's spec with the depot gate. */
	FenceLayout::FSpec FenceGatedSpec()
	{
		FenceLayout::FSpec Spec;
		Spec.GateWidthUu = PlotYard::GateCorridorUu;
		return Spec;
	}

	/** Posts of one kind on the line y = 0 (the south edge), sorted by X. */
	TArray<double> FenceSouthXs(const FenceLayout::FLayout& Layout, FenceLayout::EPostKind Kind)
	{
		TArray<double> Xs;
		for (const FenceLayout::FPost& Post : Layout.Posts)
		{
			if (Post.Kind == Kind && FMath::IsNearlyZero(Post.Position.Y, 1e-9))
			{
				Xs.Add(Post.Position.X);
			}
		}
		Xs.Sort();
		return Xs;
	}
}

/**
 * A 10 m square: a heavy post on every corner, three line posts per edge, sixteen bays.
 *
 * 1000 / 250 = 4 bays exactly, so this is the case where nothing stretches - every count is
 * forced by the spacing alone, and a wrong one means the subdivision rule is wrong.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFenceLayoutSquareTest,
	"Airside.Solve.FenceLayout.Square",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFenceLayoutSquareTest::RunTest(const FString& Parameters)
{
	const TArray<FVector2D> Outline = FenceRect(1000.0, 1000.0);
	const FenceLayout::FLayout Layout =
		FenceLayout::Solve(Outline, FVector2D(500.0, 0.0), FenceLayout::FSpec());

	TestEqual(TEXT("a heavy post on each of the four corners"),
		Layout.CountOf(FenceLayout::EPostKind::Corner), 4);
	TestEqual(TEXT("three line posts on each of four 10 m edges"),
		Layout.CountOf(FenceLayout::EPostKind::Line), 12);
	TestEqual(TEXT("no gate asked for, none made"), Layout.CountOf(FenceLayout::EPostKind::Gate), 0);
	TestFalse(TEXT("and it says so"), Layout.bHasGate);
	TestEqual(TEXT("four bays per edge"), Layout.Spans.Num(), 16);

	// ON THE VERTICES EXACTLY, not near them: a corner post a hair off the corner is a post the
	// fabric of the next edge does not reach.
	for (const FVector2D& Vertex : Outline)
	{
		bool bFound = false;
		for (const FenceLayout::FPost& Post : Layout.Posts)
		{
			bFound |= Post.Kind == FenceLayout::EPostKind::Corner && Post.Position == Vertex;
		}
		TestTrue(*FString::Printf(TEXT("a corner post stands on (%.0f, %.0f)"), Vertex.X, Vertex.Y), bFound);
	}

	// THE OUTWARD BISECTOR. At (0,0) the edges run in from the north and out to the east, so
	// outward is south-west: -135 degrees.
	for (const FenceLayout::FPost& Post : Layout.Posts)
	{
		if (Post.Kind == FenceLayout::EPostKind::Corner && Post.Position == FVector2D(0.0, 0.0))
		{
			TestEqual(TEXT("the corner post at the origin faces out along the bisector"),
				FMath::RadiansToDegrees(Post.YawRad), -135.0, 1e-9);
		}
	}

	const TArray<double> Line = FenceSouthXs(Layout, FenceLayout::EPostKind::Line);
	if (TestEqual(TEXT("three line posts on the south edge"), Line.Num(), 3))
	{
		TestEqual(TEXT("at 2.5 m"), Line[0], 250.0, 1e-9);
		TestEqual(TEXT("at 5 m"), Line[1], 500.0, 1e-9);
		TestEqual(TEXT("at 7.5 m"), Line[2], 750.0, 1e-9);
	}
	return true;
}

/**
 * An 11 m edge takes four bays of 2.75 m, not four of 2.5 m and a 1 m remainder.
 *
 * THE GREY BOX DROPPED THE REMAINDER, which read as a gap at every corner of every plot the
 * player did not happen to drag to a multiple of 2.5 m. The posts are rigid; only the
 * fabric's U takes up the 10%.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFenceLayoutStretchesEvenlyTest,
	"Airside.Solve.FenceLayout.StretchesEvenly",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFenceLayoutStretchesEvenlyTest::RunTest(const FString& Parameters)
{
	const FenceLayout::FLayout Layout = FenceLayout::Solve(
		FenceRect(1100.0, 1000.0), FVector2D(550.0, 0.0), FenceLayout::FSpec());

	const TArray<double> Line = FenceSouthXs(Layout, FenceLayout::EPostKind::Line);
	if (!TestEqual(TEXT("round(11 / 2.5) = 4 bays, so three line posts"), Line.Num(), 3))
	{
		return false;
	}
	TestEqual(TEXT("every 2.75 m"), Line[0], 275.0, 1e-9);
	TestEqual(TEXT("every 2.75 m"), Line[1], 550.0, 1e-9);
	TestEqual(TEXT("every 2.75 m"), Line[2], 825.0, 1e-9);

	// U IS DISTANCE ALONG THE EDGE, so a stretched bay stretches the texture rather than
	// restarting it: the south edge's last bay ends at 1100 / 240.
	double MaxU = 0.0;
	for (const FenceLayout::FSpan& Span : Layout.Spans)
	{
		if (Span.A.Y < 0.0 && Span.B.Y < 0.0)
		{
			MaxU = FMath::Max(MaxU, Span.U1);
		}
	}
	TestEqual(TEXT("U runs to the edge length over the tile"), MaxU, 1100.0 / 240.0, 1e-9);
	return true;
}

/**
 * The gate is the truck corridor's width, centred on the pose, with a heavy post each side.
 *
 * THE GAP AND THE LANE ARE ONE NUMBER. PlotYard keeps GateCorridorUu clear from the gate
 * inward; an opening of any other width is a fence that disagrees with the yard about where
 * the truck drives.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFenceLayoutGateIsTheCorridorTest,
	"Airside.Solve.FenceLayout.GateIsTheCorridor",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFenceLayoutGateIsTheCorridorTest::RunTest(const FString& Parameters)
{
	const FenceLayout::FLayout Layout =
		FenceLayout::Solve(FenceRect(2000.0, 2400.0), FVector2D(1000.0, 0.0), FenceGatedSpec());

	if (!TestTrue(TEXT("a 20 m frontage holds a gate"), Layout.bHasGate)) { return false; }

	const TArray<double> Gate = FenceSouthXs(Layout, FenceLayout::EPostKind::Gate);
	if (!TestEqual(TEXT("a gate post each side of the opening"), Gate.Num(), 2)) { return false; }
	TestEqual(TEXT("the opening is exactly the truck corridor"),
		Gate[1] - Gate[0], PlotYard::GateCorridorUu, 1e-9);
	TestEqual(TEXT("centred on the gate"), (Gate[0] + Gate[1]) * 0.5, 1000.0, 1e-9);
	TestTrue(TEXT("and reported where it is"), Layout.GateCentre.Equals(FVector2D(1000.0, 0.0), 1e-9));

	// NO FABRIC ACROSS THE OPENING. The whole point of the gate; a span over it would be a
	// closed gate drawn as an open one.
	for (const FenceLayout::FSpan& Span : Layout.Spans)
	{
		const FVector2D Mid = (Span.A + Span.B) * 0.5;
		TestFalse(TEXT("no fabric hangs across the gate"),
			Mid.Y < 0.0 && Mid.X > Gate[0] && Mid.X < Gate[1]);
	}
	return true;
}

/**
 * A gate asked for near a corner slides along the edge until a full bay separates it from
 * the corner - it never swallows the corner post.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFenceLayoutGateSlidesClearOfCornerTest,
	"Airside.Solve.FenceLayout.GateSlidesClearOfCorner",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFenceLayoutGateSlidesClearOfCornerTest::RunTest(const FString& Parameters)
{
	const FenceLayout::FLayout Layout =
		FenceLayout::Solve(FenceRect(2000.0, 2400.0), FVector2D(100.0, 0.0), FenceGatedSpec());

	if (!TestTrue(TEXT("still gated"), Layout.bHasGate)) { return false; }
	const TArray<double> Gate = FenceSouthXs(Layout, FenceLayout::EPostKind::Gate);
	if (!TestEqual(TEXT("two gate posts"), Gate.Num(), 2)) { return false; }
	TestEqual(TEXT("slid to one spacing clear of the corner"), Gate[0], 250.0, 1e-9);
	TestEqual(TEXT("and still the corridor's width"), Gate[1] - Gate[0], PlotYard::GateCorridorUu, 1e-9);
	return true;
}

/**
 * An edge too short for the gate plus a bay either side gets no gate, and says so.
 *
 * 800 uu < 620 + 2 x 250. Squeezing the gate in anyway would stand a gate post on top of a
 * corner post; refusing is what lets the presenter warn instead.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFenceLayoutShortEdgeHasNoGateTest,
	"Airside.Solve.FenceLayout.ShortEdgeHasNoGate",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFenceLayoutShortEdgeHasNoGateTest::RunTest(const FString& Parameters)
{
	const FenceLayout::FLayout Layout =
		FenceLayout::Solve(FenceRect(800.0, 2400.0), FVector2D(400.0, 0.0), FenceGatedSpec());

	TestFalse(TEXT("no gate on an 8 m frontage"), Layout.bHasGate);
	TestEqual(TEXT("so no gate posts"), Layout.CountOf(FenceLayout::EPostKind::Gate), 0);
	TestTrue(TEXT("but the fence still stands"), Layout.Spans.Num() > 0);
	return true;
}

/**
 * Every bay of fabric hangs outside the posts, FaceOffsetUu off the line, and the fabric is
 * continuous round each corner.
 *
 * MEASURED, NOT NAMED: the offset is taken as the distance from each span's midpoint to the
 * outline, and continuity as bitwise equality of the two spans that meet at a corner - a
 * mitre computed twice with different arguments would leave a crack a hair wide.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFenceLayoutFabricHangsOutsideTest,
	"Airside.Solve.FenceLayout.FabricHangsOutside",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFenceLayoutFabricHangsOutsideTest::RunTest(const FString& Parameters)
{
	const TArray<FVector2D> Outline = FenceRect(2000.0, 2400.0);
	const FenceLayout::FLayout Layout =
		FenceLayout::Solve(Outline, FVector2D(1000.0, 0.0), FenceGatedSpec());

	for (const FenceLayout::FSpan& Span : Layout.Spans)
	{
		const FVector2D Mid = (Span.A + Span.B) * 0.5;
		TestFalse(TEXT("the fabric is outside the plot"), RoadGeom::PointInPolygon(Outline, Mid));

		// Distance to the rectangle's nearest side.
		const double ToSide = FMath::Min(
			FMath::Min(FMath::Abs(Mid.X), FMath::Abs(Mid.X - 2000.0)),
			FMath::Min(FMath::Abs(Mid.Y), FMath::Abs(Mid.Y - 2400.0)));
		TestEqual(TEXT("hung 3 uu off the post line"), ToSide, 3.0, 1e-9);
	}

	// CONTINUOUS ROUND THE WHOLE RING, corners included: every bay ends exactly - bitwise -
	// where another begins, except the one bay that ends at the gate. Compared with == on
	// purpose; a tolerance here would pass a crack.
	int32 Continued = 0;
	for (const FenceLayout::FSpan& Span : Layout.Spans)
	{
		for (const FenceLayout::FSpan& Next : Layout.Spans)
		{
			if (Next.A == Span.B)
			{
				++Continued;
				break;
			}
		}
	}
	TestEqual(TEXT("the fabric is one unbroken ring but for the gate"),
		Continued, Layout.Spans.Num() - 1);
	return true;
}

/**
 * A plot stored clockwise fences exactly like the same plot stored counter-clockwise.
 *
 * The facade winds pads counter-clockwise today, but a solver correct only for the caller that
 * happens to get the winding right is how the pad once faced DOWN (RoadEditFacadeSurfaces.cpp).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFenceLayoutWindingDoesNotMatterTest,
	"Airside.Solve.FenceLayout.WindingDoesNotMatter",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFenceLayoutWindingDoesNotMatterTest::RunTest(const FString& Parameters)
{
	const TArray<FVector2D> Ccw = FenceRect(2000.0, 2400.0);
	const TArray<FVector2D> Cw = { Ccw[0], Ccw[3], Ccw[2], Ccw[1] };

	const FenceLayout::FLayout A = FenceLayout::Solve(Ccw, FVector2D(1000.0, 0.0), FenceGatedSpec());
	const FenceLayout::FLayout B = FenceLayout::Solve(Cw, FVector2D(1000.0, 0.0), FenceGatedSpec());

	// NOT EMPTY FIRST: two empty layouts agree about everything, and this passed against a
	// stub Solve that returned nothing until this line was added.
	if (!TestTrue(TEXT("there is a fence to compare"), A.Posts.Num() > 0)) { return false; }
	if (!TestEqual(TEXT("the same number of posts"), A.Posts.Num(), B.Posts.Num())) { return false; }
	TestEqual(TEXT("the same number of bays"), A.Spans.Num(), B.Spans.Num());

	// AS A SET: the two start from different vertices, so the ORDER legitimately differs.
	for (const FenceLayout::FPost& Post : A.Posts)
	{
		bool bFound = false;
		for (const FenceLayout::FPost& Other : B.Posts)
		{
			bFound |= Other.Kind == Post.Kind && Other.Position.Equals(Post.Position, 1e-9);
		}
		TestTrue(TEXT("every post stands in the same place"), bFound);
	}
	for (const FenceLayout::FSpan& Span : B.Spans)
	{
		TestFalse(TEXT("and the clockwise plot's fabric is still outside"),
			RoadGeom::PointInPolygon(Ccw, (Span.A + Span.B) * 0.5));
	}
	return true;
}

#endif
