#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Tool/BuildSession.h"
#include "Tool/PlotPlaceTool.h"
#include "Tool/RoadBuildTool.h"
#include "Tool/RoadEditTarget.h"
#include "Tool/SnapGuideChain.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/**
	 * Records the guide's own geometry, which is what this file is about.
	 *
	 * The plot's existing ghost sink (PlotPlaceToolTest) counts styles and measures depth;
	 * here the ENDPOINTS matter, because "a dashed line to the thing" is a claim about where
	 * the line goes, and a style count cannot tell a line to the frontage from one to the
	 * world origin.
	 */
	struct FGuideSink : IToolPreviewSink
	{
		struct FRecordedLine
		{
			FVector2D From = FVector2D::ZeroVector;
			FVector2D To = FVector2D::ZeroVector;
			EPreviewStyle Style = EPreviewStyle::Pending;
		};

		TArray<FRecordedLine> Lines;
		TArray<FString> GuideLabels;

		virtual void Marker(const FVector2D& At, EPreviewStyle Style) override {}
		virtual void Line(const FVector2D& From, const FVector2D& To, EPreviewStyle Style) override
		{
			Lines.Add({ From, To, Style });
		}
		virtual void CrossMark(const FVector2D& At, const FVector2D& Along, EPreviewStyle Style) override {}
		virtual void Label(const FVector2D& At, const FString& Text, EPreviewStyle Style) override
		{
			if (Style == EPreviewStyle::Guide) { GuideLabels.Add(Text); }
		}

		TArray<FRecordedLine> Of(EPreviewStyle Style) const
		{
			return Lines.FilterByPredicate(
				[Style](const FRecordedLine& L) { return L.Style == Style; });
		}
	};

	/** An east-west service road through the origin, long enough to anchor anywhere on. */
	void LayServiceRoad(ARoadNetworkActor* Actor)
	{
		IRoadEditTarget* Target = Actor;
		const int32 West = Target->PlaceNode(FVector2D(-20000.0, 0.0));
		const int32 East = Target->PlaceNode(FVector2D(20000.0, 0.0));
		Target->ConnectNodes(West, East, ERoadKind::ServiceRoad, INDEX_NONE);
	}

	/**
	 * The registry index of the fuel-depot tool, BY ID - never a literal. The registry is the
	 * one list, and a test that hard-coded 8 would break on the next tool added to it.
	 */
	int32 DepotToolIndex()
	{
		const TConstArrayView<FToolRegistration> Registry = ToolRegistry();
		for (int32 Index = 0; Index < Registry.Num(); ++Index)
		{
			if (Registry[Index].Id == FName(TEXT("FuelDepot"))) { return Index; }
		}
		return INDEX_NONE;
	}

	/**
	 * A depot gesture driven through a real FBuildSession, with the frontage already pinned.
	 *
	 * THE CONTEXTS COME FROM MakeContext, the seam both drivers go through, rather than from
	 * TestTool::ContextAt - which is the whole point of this file. Every Solve/ and chain test
	 * in this stage passes on a tool that ignores FToolContext::Guide entirely; only a context
	 * the SESSION built carries a guide at all.
	 */
	struct FDepotGesture
	{
		FAirsideTestWorld TestWorld;
		FBuildSession Session;
		FBuildSessionTunables Tunables;
		IBuildTool* Tool = nullptr;

		/** Two corners pinned: the anchor and the frontage's far end. */
		TArray<FVector2D> Frontage;

		FToolContext At(const FVector2D& Where) const
		{
			return Session.MakeContext(TestWorld.Actor, Where, Tunables, false, false);
		}

		FPlotPlaceTool* Plot() const { return static_cast<FPlotPlaceTool*>(Tool); }
	};

	/** Builds the world, lays the road and pins the frontage. Null Tool on any failure. */
	bool StartGesture(FDepotGesture& Out)
	{
		if (Out.TestWorld.World == nullptr || Out.TestWorld.Actor == nullptr) { return false; }
		LayServiceRoad(Out.TestWorld.Actor);

		const int32 Depot = DepotToolIndex();
		if (Depot == INDEX_NONE) { return false; }

		Out.Tunables = Out.TestWorld.Actor->MakeTunables(10000.0);
		Out.Session.SelectTool(Depot);
		Out.Tool = Out.Session.GetActiveTool();
		if (Out.Tool == nullptr) { return false; }

		// Anchor from OFF the road, on its north side, then run the frontage east.
		Out.Tool->OnClick(Out.At(FVector2D(0.0, 1000.0)));
		Out.Tool->OnClick(Out.At(FVector2D(6000.0, 1000.0)));

		Out.Plot()->Quad(Out.At(FVector2D(6000.0, 3000.0)), Out.Frontage);
		return Out.Frontage.Num() >= 2;
	}
}

/**
 * THE COMPOSITION TEST, and the one that fails if the tool never calls the chain.
 *
 * What is being pinned is the whole path: the tool declares an anchor, the session resolves a
 * guide against it, and the tool reads the answer back into the corner it draws and pins.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotCornerFollowsTheGuideTest,
	"Airside.Tool.PlotCornerFollowsTheGuide",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotCornerFollowsTheGuideTest::RunTest(const FString& Parameters)
{
	FDepotGesture Gesture;
	if (!TestTrue(TEXT("a depot gesture with its frontage pinned"), StartGesture(Gesture)))
	{
		return false;
	}

	// A CORNER DRAGGED ALMOST SQUARE: out along +Y from the frontage's far end and 60 uu east
	// of it - about 1.7 degrees off the perpendicular, inside the 7-degree tolerance.
	const FVector2D FarEnd = Gesture.Frontage[1];
	const FToolContext Guided = Gesture.At(FarEnd + FVector2D(60.0, 2000.0));

	// A PLOT DRAGS A BOUNDARY, not a centreline, and its half-widths are zero because there is
	// no pavement either side of a corner. Asserted here rather than left implied: the
	// displacement rule in FApronLineGuideSource turns on exactly this, and a tool that answered
	// Centreline by omission would push every apron guide out by a half-width it does not have.
	FGuideAnchor Shape;
	if (TestTrue(TEXT("the plot tool describes an anchor"),
		Gesture.Tool->DescribeGuideAnchor(Gesture.TestWorld.Actor->Network,
			Gesture.TestWorld.Actor, Shape)))
	{
		TestEqual(TEXT("and says it is dragging a boundary"),
			static_cast<int32>(Shape.Point), static_cast<int32>(EDragPoint::Boundary));
		TestEqual(TEXT("with no width to either side of it"), Shape.HalfWidthLeft, 0.0);
		TestEqual(TEXT("on either side"), Shape.HalfWidthRight, 0.0);
	}

	if (!TestTrue(TEXT("the driver resolved a guide for a corner dragged near square"),
		Guided.Guide.bActive))
	{
		return false;
	}

	TArray<FVector2D> Shown;
	Gesture.Plot()->Quad(Guided, Shown);
	if (!TestEqual(TEXT("the quad shows four corners"), Shown.Num(), 4)) { return false; }

	// EXACTLY SQUARE, not merely nearer. The corner the player gets IS the guide's point, so
	// an assertion with a loose tolerance would pass on a tool that ignored the guide and
	// simply took a cursor that was already close.
	TestTrue(TEXT("the guided corner lands exactly square to the frontage"),
		FMath::IsNearlyEqual(Shown[2].X, FarEnd.X, 1.0e-6));
	TestFalse(TEXT("and therefore not where the raw cursor was"),
		FMath::IsNearlyEqual(Guided.Cursor.X, FarEnd.X, 1.0e-6));

	// THE DASHED LINE, AND WHERE IT GOES: to the frontage's other end - the edge being squared
	// to - and from the corner the quad actually shows.
	FGuideSink Sink;
	Gesture.Tool->BuildPreview(Guided, Sink);

	const TArray<FGuideSink::FRecordedLine> GuideLines = Sink.Of(EPreviewStyle::Guide);
	if (!TestEqual(TEXT("exactly one guide line is drawn"), GuideLines.Num(), 1))
	{
		return false;
	}
	TestTrue(TEXT("it starts at the corner the quad shows"),
		GuideLines[0].From.Equals(Shown[2], 1.0e-6));
	TestTrue(TEXT("and ends at the far end of the edge it is squared to"),
		GuideLines[0].To.Equals(Shown[0], 1.0e-6));

	if (!TestEqual(TEXT("and one label says which"), Sink.GuideLabels.Num(), 1))
	{
		return false;
	}
	TestEqual(TEXT("naming the reference the tool supplied"),
		Sink.GuideLabels[0], FString(TEXT("square to the frontage")));

	// THE PLOT EDGES ARE STILL DRAWN, in their own styles. A guide that had replaced the
	// boundary rather than joined it would pass every assertion above.
	TestTrue(TEXT("the boundary is still drawn beside the guide"),
		Sink.Of(EPreviewStyle::Pinned).Num() + Sink.Of(EPreviewStyle::Provisional).Num() >= 4);

	return true;
}

/**
 * TWO ALIGNMENTS ON THE LAST CORNER, which is the 2026-09-17 request in its own geometry:
 * square to the frontage from the anchor, AND level with the back corner already placed. The
 * two are perpendicular, so they genuinely cross - and the corner lands where they do, which
 * is the corner that makes the plot an exact rectangle.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotLastCornerTakesTwoGuidesTest,
	"Airside.Tool.PlotLastCornerTakesTwoGuides",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotLastCornerTakesTwoGuidesTest::RunTest(const FString& Parameters)
{
	FDepotGesture Gesture;
	if (!TestTrue(TEXT("a depot gesture with its frontage pinned"), StartGesture(Gesture)))
	{
		return false;
	}

	// Pin the first back corner exactly square, 2000 uu out from the frontage's far end.
	const FVector2D FarEnd = Gesture.Frontage[1];
	Gesture.Tool->OnClick(Gesture.At(FarEnd + FVector2D(0.0, 2000.0)));

	TArray<FVector2D> ThreePinned;
	Gesture.Plot()->Quad(Gesture.At(FarEnd + FVector2D(0.0, 2000.0)), ThreePinned);
	if (!TestEqual(TEXT("three corners are pinned"), ThreePinned.Num(), 4)) { return false; }

	const FVector2D Anchor = ThreePinned[0];
	const FVector2D BackFar = ThreePinned[2];

	// THE LAST CORNER, dragged near the rectangle's corner but not on it: 70 uu off square
	// from the anchor, and 80 uu off level with the back corner. Both inside tolerance, and
	// neither exact - so landing exactly on both can only come from the intersection.
	const FVector2D Target(Anchor.X + 70.0, BackFar.Y - 80.0);
	const FToolContext Guided = Gesture.At(Target);

	if (!TestTrue(TEXT("the driver resolved a guide"), Guided.Guide.bActive)) { return false; }
	if (!TestEqual(TEXT("and two alignments hold at once"), Guided.Guide.Winners.Num(), 2))
	{
		return false;
	}

	TArray<FVector2D> Shown;
	Gesture.Plot()->Quad(Guided, Shown);
	if (!TestEqual(TEXT("the quad shows four corners"), Shown.Num(), 4)) { return false; }

	// EXACT ON BOTH COUNTS, which is what makes both labels honest.
	TestTrue(TEXT("the last corner is exactly square to the frontage from the anchor"),
		FMath::IsNearlyEqual(Shown[3].X, Anchor.X, 1.0e-6));
	TestTrue(TEXT("and exactly level with the back corner already placed"),
		FMath::IsNearlyEqual(Shown[3].Y, BackFar.Y, 1.0e-6));

	// TWO LINES AND TWO LABELS, one per guide.
	FGuideSink Sink;
	Gesture.Tool->BuildPreview(Guided, Sink);
	TestEqual(TEXT("a dashed line is drawn for each guide"),
		Sink.Of(EPreviewStyle::Guide).Num(), 2);
	if (!TestEqual(TEXT("and a label for each"), Sink.GuideLabels.Num(), 2)) { return false; }

	// NAMING THE CORNER AS THE BAR COUNTS IT. The readout says "Plot Points: 3/4", so the
	// corner pinned third is "corner 3" on screen and must be "corner 3" in the label.
	TestTrue(TEXT("one label names the frontage it is square to"),
		Sink.GuideLabels.ContainsByPredicate([](const FString& L)
			{ return L.Contains(TEXT("the frontage")); }));
	TestTrue(TEXT("and the other names the corner it is level with"),
		Sink.GuideLabels.ContainsByPredicate([](const FString& L)
			{ return L.Contains(TEXT("corner 3")); }));

	return true;
}

/**
 * THE CONTROL: the guide must be OFF most of the time. A corner dragged nowhere near an
 * alignment keeps the cursor it was given and nothing is drawn - otherwise the feature is a
 * constraint the player never asked for rather than an aid.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotCornerIgnoresADistantGuideTest,
	"Airside.Tool.PlotCornerIgnoresADistantGuide",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotCornerIgnoresADistantGuideTest::RunTest(const FString& Parameters)
{
	FDepotGesture Gesture;
	if (!TestTrue(TEXT("a depot gesture with its frontage pinned"), StartGesture(Gesture)))
	{
		return false;
	}

	// 30 degrees off the perpendicular, and 15 off the nearest world axis: nothing is within
	// the 7-degree tolerance.
	const FVector2D FarEnd = Gesture.Frontage[1];
	const FToolContext Free = Gesture.At(FarEnd + FVector2D(1150.0, 2000.0));

	TestFalse(TEXT("a corner dragged between alignments is offered no guide"),
		Free.Guide.bActive);

	TArray<FVector2D> Shown;
	Gesture.Plot()->Quad(Free, Shown);
	if (!TestEqual(TEXT("the quad still shows four corners"), Shown.Num(), 4)) { return false; }
	TestTrue(TEXT("and the corner is exactly where the cursor is"),
		Shown[2].Equals(Free.Cursor, 1.0e-6));

	FGuideSink Sink;
	Gesture.Tool->BuildPreview(Free, Sink);
	TestEqual(TEXT("with no guide line drawn"), Sink.Of(EPreviewStyle::Guide).Num(), 0);
	TestEqual(TEXT("and no guide label"), Sink.GuideLabels.Num(), 0);

	return true;
}

/**
 * THE WINNER DIES WITH THE GESTURE. The session holds it so hysteresis can work; a winner that
 * outlived the gesture would be held into the NEXT one by that same rule, and the player would
 * get a guide off an edge that no longer exists.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotGuideEndsWithTheGestureTest,
	"Airside.Tool.PlotGuideEndsWithTheGesture",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotGuideEndsWithTheGestureTest::RunTest(const FString& Parameters)
{
	FDepotGesture Gesture;
	if (!TestTrue(TEXT("a depot gesture with its frontage pinned"), StartGesture(Gesture)))
	{
		return false;
	}

	const FVector2D Square = Gesture.Frontage[1] + FVector2D(60.0, 2000.0);
	if (!TestTrue(TEXT("a guide is live mid-gesture"), Gesture.At(Square).Guide.bActive))
	{
		return false;
	}

	// PUT THE TOOL DOWN. SelectTool deactivates the depot tool, which returns it to Idle, so it
	// offers no anchor and the session must forget the winner it was holding.
	Gesture.Session.SelectTool(0);
	TestFalse(TEXT("with the tool put down, the guide is gone"),
		Gesture.At(Square).Guide.bActive);

	// AND IT DOES NOT COME BACK on re-selecting the tool at the same cursor: the gesture starts
	// from Idle, which has no anchor at all.
	const int32 Depot = DepotToolIndex();
	Gesture.Session.SelectTool(Depot);
	TestFalse(TEXT("and a fresh gesture does not inherit it"),
		Gesture.At(Square).Guide.bActive);

	return true;
}

#endif
