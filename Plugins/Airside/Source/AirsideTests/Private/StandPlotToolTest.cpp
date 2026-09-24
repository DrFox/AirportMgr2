#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Content/AirsideSettings.h"
#include "Entities/AircraftType.h"
#include "Entities/EntityDefinition.h"
#include "InputCoreTypes.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadAgent.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoutePolicy.h"
#include "Model/RouteSearch.h"
#include "Present/AirsideTraffic.h"
#include "Present/RoadNetworkActor.h"
#include "Profiles/RoadProfile.h"
#include "Solve/IcaoCode.h"
#include "Solve/RoadGeom.h"
#include "Solve/StandBox.h"
#include "Tool/BuildSession.h"
#include "Tool/PlotGesture.h"
#include "Tool/RoadEditTarget.h"
#include "Tool/StandPlotTool.h"
#include "Tool/ToolReadout.h"

#if WITH_DEV_AUTOMATION_TESTS

// NAMED, NOT ANONYMOUS - the tests module is a UNITY build, and an anonymous-namespace helper
// of a common name (LayRoad, LiveEntities, ...) compiles alone and collides with another
// file's copy the moment both land in one translation unit. PlotPlaceToolTest.cpp has both.
namespace StandPlotToolFixture
{
	/** Free-snap, 150uu radius - see TestTool::ContextAt (#104). The stand tool reads no
	 *  snap: its first click SEARCHES for a taxiway (PlotGesture::NearestRoad). */
	FToolContext At(ARoadNetworkActor* Actor, const FVector2D& Where)
	{
		return TestTool::ContextAt(*Actor, Where);
	}

	/** A real road SEGMENT along X at Y, 200 m long - the anchor search reads the road graph,
	 *  not a guideline, so a test that laid only guidelines would anchor on nothing. */
	void LayRoad(ARoadNetworkActor* Actor, double Y, ERoadKind Kind)
	{
		IRoadEditTarget* Target = Actor;
		const int32 West = Target->PlaceNode(FVector2D(-10000.0, Y));
		const int32 East = Target->PlaceNode(FVector2D(10000.0, Y));
		Target->ConnectNodes(West, East, Kind, INDEX_NONE);
	}

	/** Alive AND a stand - never a bare count, which a depot would also satisfy. */
	int32 LiveStands(const ARoadNetworkActor* Actor)
	{
		int32 Count = 0;
		for (const FEntityInstance& Entity : Actor->Network->GetEntities())
		{
			if (Entity.bAlive && Entity.IsStand()) { ++Count; }
		}
		return Count;
	}

	int32 LiveEntities(const ARoadNetworkActor* Actor)
	{
		int32 Count = 0;
		for (const FEntityInstance& Entity : Actor->Network->GetEntities())
		{
			if (Entity.bAlive) { ++Count; }
		}
		return Count;
	}

	FToolReadout ReadoutOf(const FStandPlotTool& Tool, const FToolContext& Context)
	{
		FToolReadoutCollector Collector;
		Tool.BuildReadout(Context, Collector);
		return Collector.Readout;
	}

	/** The value of one readout fact, or "<absent>" - distinct from every real value. */
	FString FactOf(const FToolReadout& Readout, const TCHAR* Label)
	{
		for (const TPair<FString, FString>& Fact : Readout.Facts)
		{
			if (Fact.Key == Label) { return Fact.Value; }
		}
		return TEXT("<absent>");
	}

	/** What the readout's Stand fact should say for a W x D rectangle, from IcaoCode itself. */
	FString ExpectedStandFact(double Width, double Depth)
	{
		const FString Letter = IcaoCode::LetterForStandSize(Width, Depth);
		return Letter.IsEmpty() ? FString(TEXT("-")) : FString::Printf(TEXT("Code %s"), *Letter);
	}

	/** The narrowest entrance width the gesture can produce that is at least Floor - the
	 *  frontage moves in steps, so a letter's exact floor width is usually not reachable. */
	double ReachableWidthAtLeast(double Floor)
	{
		if (Floor <= PlotGesture::MinFrontageUu) { return PlotGesture::MinFrontageUu; }
		return PlotGesture::MinFrontageUu
			+ FMath::CeilToDouble((Floor - PlotGesture::MinFrontageUu) / PlotGesture::FrontageStepUu)
			* PlotGesture::FrontageStepUu;
	}

	/** Where the first click sits: 10 m north of a taxiway laid along Y = 0, inside reach. */
	const FVector2D AnchorCursor(0.0, 1000.0);

	/** The anchor the tool pinned - Rect's corner 0, read back rather than predicted. */
	FVector2D PinnedAnchor(const FStandPlotTool& Tool, ARoadNetworkActor* Actor)
	{
		TArray<FVector2D> Shown;
		Tool.Rect(At(Actor, AnchorCursor), Shown);
		return Shown.Num() > 0 ? Shown[0] : FVector2D::ZeroVector;
	}

	/**
	 * Anchor, entrance end Width east, depth Depth north: the three clicks. Width must be one
	 * the frontage steps can reach (ReachableWidthAtLeast) and Depth a whole metre, or the
	 * rectangle drawn is not the one asked for. Returns whether the gesture reached Confirm.
	 */
	bool DrawStand(FStandPlotTool& Tool, ARoadNetworkActor* Actor, double Width, double Depth)
	{
		Tool.OnClick(At(Actor, AnchorCursor));
		if (Tool.GetStage() != EStandStage::Entrance) { return false; }
		const FVector2D Anchor = PinnedAnchor(Tool, Actor);
		Tool.OnClick(At(Actor, Anchor + FVector2D(Width, 0.0)));
		Tool.OnClick(At(Actor, Anchor + FVector2D(Width, Depth)));
		return Tool.GetStage() == EStandStage::Confirm;
	}

	/** A fresh network with one taxiway at Y = 0 and a stand definition to build from. */
	void TaxiwayWorld(ARoadNetworkActor* Actor)
	{
		Actor->ClearNetwork();
		Actor->StandDefinition = UEntityDefinition::MakeStandTransient();
		LayRoad(Actor, 0.0, ERoadKind::Taxiway);
	}

	/** Records lines by style and every label, for the preview assertions. */
	struct FStandPlotSink : IToolPreviewSink
	{
		struct FLine { FVector2D From; FVector2D To; EPreviewStyle Style; };
		TArray<FLine> Lines;
		TArray<TPair<FString, EPreviewStyle>> Labels;

		virtual void Marker(const FVector2D&, EPreviewStyle) override {}
		virtual void Line(const FVector2D& From, const FVector2D& To, EPreviewStyle Style) override
		{
			Lines.Add({ From, To, Style });
		}
		virtual void CrossMark(const FVector2D&, const FVector2D&, EPreviewStyle) override {}
		virtual void Label(const FVector2D&, const FString& Text, EPreviewStyle Style) override
		{
			Labels.Emplace(Text, Style);
		}

		bool Says(const FString& Fragment) const
		{
			return Labels.ContainsByPredicate(
				[&](const TPair<FString, EPreviewStyle>& L) { return L.Key.Contains(Fragment); });
		}

		/** Whether a line in Style touches Point - how a transformed corner is found. */
		bool TouchesIn(const FVector2D& Point, EPreviewStyle Style) const
		{
			return Lines.ContainsByPredicate([&](const FLine& L)
			{
				return L.Style == Style && (L.From.Equals(Point, 1.0) || L.To.Equals(Point, 1.0));
			});
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandPlotAnchorsOnTaxiwayOnlyTest,
	"Airside.Tool.StandPlot.AnchorsOnTaxiwayOnly",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandPlotAnchorsOnTaxiwayOnlyTest::RunTest(const FString& Parameters)
{
	using namespace StandPlotToolFixture;

	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	// A SERVICE ROAD ONLY, at exactly the reach a taxiway is anchored from below. An aircraft
	// stand opening onto a road trucks use would have its arrivals taxi on one.
	Actor->ClearNetwork();
	LayRoad(Actor, 0.0, ERoadKind::ServiceRoad);
	{
		FStandPlotTool Tool;
		Tool.OnClick(At(Actor, AnchorCursor));
		TestEqual(TEXT("a click beside a service road anchors nothing - stands open off taxiways"),
			static_cast<int32>(Tool.GetStage()), static_cast<int32>(EStandStage::Idle));

		const FToolReadout Readout = ReadoutOf(Tool, At(Actor, AnchorCursor));
		TestTrue(TEXT("and the readout says what to move near, so the refusal is not silent"),
			Readout.Warnings.ContainsByPredicate([](const FString& W) { return W.Contains(TEXT("taxiway")); }));
	}

	// THE SAME CLICK, beside a taxiway, starts the entrance edge.
	TaxiwayWorld(Actor);
	{
		FStandPlotTool Tool;
		Tool.OnClick(At(Actor, AnchorCursor));
		TestEqual(TEXT("a click within reach of a taxiway pins the entrance's first end"),
			static_cast<int32>(Tool.GetStage()), static_cast<int32>(EStandStage::Entrance));

		// OFF THE CARRIAGEWAY, as the depot's anchor is: a stand whose entrance sat on the
		// centreline would be built over half the taxiway.
		const FVector2D Anchor = PinnedAnchor(Tool, Actor);
		TestTrue(TEXT("the anchor stands off the taxiway's centreline, on the cursor's side"),
			Anchor.Y > 0.0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandPlotWidthStepsTest,
	"Airside.Tool.StandPlot.WidthSteps",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandPlotWidthStepsTest::RunTest(const FString& Parameters)
{
	using namespace StandPlotToolFixture;

	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
	TaxiwayWorld(Actor);

	FStandPlotTool Tool;
	Tool.OnClick(At(Actor, AnchorCursor));
	if (!TestEqual(TEXT("anchored"), static_cast<int32>(Tool.GetStage()), static_cast<int32>(EStandStage::Entrance))) { return false; }
	const FVector2D Anchor = PinnedAnchor(Tool, Actor);

	// THE DEPOT'S STEPS, read from PlotGesture rather than retyped: a stand and a depot drawn
	// off one grid must be able to sit flush, which only a shared quantum allows.
	const double Min = PlotGesture::MinFrontageUu;
	const double Step = PlotGesture::FrontageStepUu;
	struct FCase { double Reach; double Expected; const TCHAR* Why; };
	const FCase Cases[] = {
		{ Min + 2.4 * Step, Min + 2.0 * Step, TEXT("under half a step past a mark rounds back to it") },
		{ Min + 2.6 * Step, Min + 3.0 * Step, TEXT("over half a step rounds on to the next mark") },
		{ Min * 0.3,        Min,              TEXT("a drag shorter than the minimum is held at the minimum") },
		{ -(Min + 4.1 * Step), Min + 4.0 * Step, TEXT("dragging the other way along the taxiway steps the same") },
	};
	for (const FCase& Case : Cases)
	{
		TArray<FVector2D> Shown;
		Tool.Rect(At(Actor, Anchor + FVector2D(Case.Reach, 300.0)), Shown);
		if (!TestEqual(TEXT("four corners while the entrance is dragged"), Shown.Num(), 4)) { continue; }
		TestEqual(Case.Why, StandBox::WidthOf(Shown), Case.Expected, 1e-6);
	}

	// AND THE CLICK PINS THE STEPPED WIDTH, not the raw reach.
	Tool.OnClick(At(Actor, Anchor + FVector2D(Min + 2.4 * Step, 300.0)));
	TArray<FVector2D> Pinned;
	Tool.Rect(At(Actor, Anchor + FVector2D(9999.0, 2000.0)), Pinned);
	if (TestEqual(TEXT("four corners once the entrance is pinned"), Pinned.Num(), 4))
	{
		TestEqual(TEXT("the pinned entrance keeps the stepped width whatever the cursor does next"),
			StandBox::WidthOf(Pinned), Min + 2.0 * Step, 1e-6);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandPlotLetterAtThresholdsTest,
	"Airside.Tool.StandPlot.LetterAtThresholds",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandPlotLetterAtThresholdsTest::RunTest(const FString& Parameters)
{
	using namespace StandPlotToolFixture;

	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	// EVERY LETTER, at its own depth floor and one depth step (1 m, the tool's rounding) short
	// of it. The letter must change EXACTLY there - the threshold is IcaoCode's, so a tool that
	// kept its own table, or measured a different rectangle than it drew, lands on the wrong
	// side of one of these twelve.
	for (EIcaoCode Letter : { EIcaoCode::A, EIcaoCode::B, EIcaoCode::C, EIcaoCode::D, EIcaoCode::E, EIcaoCode::F })
	{
		const double Width = ReachableWidthAtLeast(IcaoCode::StandWidthForLetter(Letter));
		const double Depth = IcaoCode::StandDepthForLetter(Letter);

		for (const double Short : { 0.0, 100.0 })
		{
			TaxiwayWorld(Actor);
			FStandPlotTool Tool;
			if (!TestTrue(FString::Printf(TEXT("Code %s: the gesture reaches Confirm"), IcaoCode::ToLetter(Letter)),
				DrawStand(Tool, Actor, Width, Depth - Short)))
			{
				continue;
			}

			const FToolReadout Readout = ReadoutOf(Tool, At(Actor, AnchorCursor));
			TestEqual(
				*FString::Printf(TEXT("Code %s floor %s: the Stand fact is IcaoCode's letter for the drawn size"),
					IcaoCode::ToLetter(Letter), Short == 0.0 ? TEXT("exactly") : TEXT("less 1 m of depth")),
				FactOf(Readout, TEXT("Stand")), ExpectedStandFact(Width, Depth - Short));

			// AT THE FLOOR ITSELF, THE LETTER BY NAME - not only "whatever IcaoCode says", which
			// would pass on a table and a tool that were both wrong the same way.
			if (Short == 0.0)
			{
				TestEqual(*FString::Printf(TEXT("Code %s drawn exactly at its floor reads as Code %s"),
						IcaoCode::ToLetter(Letter), IcaoCode::ToLetter(Letter)),
					FactOf(Readout, TEXT("Stand")), FString(TEXT("Code ")) + IcaoCode::ToLetter(Letter));
			}
		}
	}

	// AT THE C FLOOR, THE NEXT LETTER'S LEVER: how much wider and deeper D needs, from the
	// same table. The player is told what to drag, not left to guess the thresholds.
	{
		TaxiwayWorld(Actor);
		const double Width = ReachableWidthAtLeast(IcaoCode::StandWidthForLetter(EIcaoCode::C));
		const double Depth = IcaoCode::StandDepthForLetter(EIcaoCode::C);
		FStandPlotTool Tool;
		if (TestTrue(TEXT("a Code C floor stand reaches Confirm"), DrawStand(Tool, Actor, Width, Depth)))
		{
			const int32 Wider = FMath::CeilToInt((IcaoCode::StandWidthForLetter(EIcaoCode::D) - Width) / 100.0);
			const int32 Deeper = FMath::CeilToInt((IcaoCode::StandDepthForLetter(EIcaoCode::D) - Depth) / 100.0);
			const FToolReadout Readout = ReadoutOf(Tool, At(Actor, AnchorCursor));
			TestEqual(TEXT("the Next fact names D and both deficits in whole metres"),
				FactOf(Readout, TEXT("Next")),
				FString::Printf(TEXT("Code D: %d m wider, %d m deeper"), Wider, Deeper));
			TestEqual(TEXT("and three of three points are pinned"),
				FactOf(Readout, TEXT("Stand Points")), FString(TEXT("3/3")));
			TestTrue(TEXT("and a buildable letter lights Build"), Readout.bCommittable);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandPlotTooSmallNotCommittableTest,
	"Airside.Tool.StandPlot.TooSmallNotCommittable",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandPlotTooSmallNotCommittableTest::RunTest(const FString& Parameters)
{
	using namespace StandPlotToolFixture;

	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
	TaxiwayWorld(Actor);

	// 15 x 15 m: the narrowest entrance the steps allow, and far short of Code A's depth.
	FStandPlotTool Tool;
	if (!TestTrue(TEXT("a 15 x 15 m stand still locks - no letter is a refusal, not an error"),
		DrawStand(Tool, Actor, PlotGesture::MinFrontageUu, 1500.0)))
	{
		return false;
	}

	const FToolReadout Readout = ReadoutOf(Tool, At(Actor, AnchorCursor));
	TestFalse(TEXT("Build stays grey over a stand no aircraft fits"), Readout.bCommittable);
	TestEqual(TEXT("the Stand fact says there is no letter"), FactOf(Readout, TEXT("Stand")), FString(TEXT("-")));

	// THE FACADE'S OWN REASON, verbatim: the tool asks WhyStandRefused rather than wording its
	// own, so the bar and the refusal at commit can never disagree.
	TArray<FVector2D> Shown;
	Tool.Rect(At(Actor, AnchorCursor), Shown);
	const FString Why = static_cast<IRoadEditTarget*>(Actor)->WhyStandRefused(Shown);
	TestTrue(TEXT("the facade refuses it for size"), Why.Contains(TEXT("more")));
	TestTrue(TEXT("and the readout's warning is that same sentence"), Readout.Warnings.Contains(Why));

	// AND BUILD DOES NOTHING, so a grey button is not merely cosmetic.
	Tool.OnCommit(At(Actor, AnchorCursor));
	TestEqual(TEXT("committing a refused stand places nothing"), LiveStands(Actor), 0);
	TestEqual(TEXT("and leaves the gesture locked for the player to cancel back"), static_cast<int32>(Tool.GetStage()), static_cast<int32>(EStandStage::Confirm));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandPlotCommitPlacesTest,
	"Airside.Tool.StandPlot.CommitPlaces",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandPlotCommitPlacesTest::RunTest(const FString& Parameters)
{
	using namespace StandPlotToolFixture;

	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
	TaxiwayWorld(Actor);

	const double Width = ReachableWidthAtLeast(IcaoCode::StandWidthForLetter(EIcaoCode::C));
	const double Depth = IcaoCode::StandDepthForLetter(EIcaoCode::C);

	FStandPlotTool Tool;
	if (!TestTrue(TEXT("a Code C stand reaches Confirm"), DrawStand(Tool, Actor, Width, Depth))) { return false; }

	// NOTHING BEFORE BUILD: the third click locks, it does not build - the review beat.
	TestEqual(TEXT("locking places nothing"), LiveStands(Actor), 0);

	TArray<FVector2D> Drawn;
	Tool.Rect(At(Actor, AnchorCursor), Drawn);

	Tool.OnCommit(At(Actor, AnchorCursor));
	if (!TestEqual(TEXT("Build places exactly one stand"), LiveStands(Actor), 1)) { return false; }
	TestEqual(TEXT("and the tool is ready for the next"), static_cast<int32>(Tool.GetStage()), static_cast<int32>(EStandStage::Idle));

	for (const FEntityInstance& Entity : Actor->Network->GetEntities())
	{
		if (!Entity.bAlive || !Entity.IsStand()) { continue; }
		if (!TestEqual(TEXT("the stand carries the drawn rectangle as its outline"), Entity.Outline.Num(), 4)) { break; }

		// THE SAME RECTANGLE THE GHOST SHOWED - measured, not assumed. The facade may reverse
		// the winding, so size and letter are compared rather than corner order.
		TestEqual(TEXT("as wide as drawn"), StandBox::WidthOf(Entity.Outline), StandBox::WidthOf(Drawn), 1.0);
		TestEqual(TEXT("as deep as drawn"), StandBox::DepthOf(Entity.Outline), StandBox::DepthOf(Drawn), 1.0);

		const TOptional<EIcaoCode> Letter = StandBox::LetterOf(Entity.Outline);
		if (TestTrue(TEXT("the outline reads as a letter"), Letter.IsSet()))
		{
			TestEqual(TEXT("and it is Code C"), FString(IcaoCode::ToLetter(*Letter)), FString(TEXT("C")));
		}
	}
	return true;
}

/**
 * FINAL REVIEW I4: STANDS IN A ROW. The first thing a player does after drawing one stand is
 * draw the next beside it, anchored at the first one's far corner on the same taxiway grid -
 * so the two share an edge to within the ulps two independent sums of the same figures land
 * on. The overlap test used RoadGeom::PointInPolygon, whose on-edge answer is undefined, so
 * the neighbour could be refused as "overlaps stand 0" by a floating-point coin flip.
 *
 * TOUCHING IS NOT OVERLAPPING; a real metre of overlap still is - both halves, or a check
 * that simply stopped looking would pass the first.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandPlotFlushNeighboursBothPlaceTest,
	"Airside.Tool.StandPlot.FlushNeighboursBothPlace",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandPlotFlushNeighboursBothPlaceTest::RunTest(const FString& Parameters)
{
	using namespace StandPlotToolFixture;

	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
	TaxiwayWorld(Actor);

	const double Width = ReachableWidthAtLeast(IcaoCode::StandWidthForLetter(EIcaoCode::C));
	const double Depth = IcaoCode::StandDepthForLetter(EIcaoCode::C);

	TArray<FVector2D> First;
	{
		FStandPlotTool Tool;
		if (!TestTrue(TEXT("the first stand reaches Confirm"), DrawStand(Tool, Actor, Width, Depth))) { return false; }
		Tool.Rect(At(Actor, AnchorCursor), First);
		Tool.OnCommit(At(Actor, AnchorCursor));
		if (!TestEqual(TEXT("the first stand places"), LiveStands(Actor), 1)) { return false; }
	}

	// THE SECOND ANCHOR, clicked at the first one's far entrance corner - the corner the player
	// sees, not a figure this test computes and the tool might not reproduce.
	const FVector2D FarCorner = First[1];
	FStandPlotTool Tool;
	Tool.OnClick(At(Actor, FarCorner));
	if (!TestEqual(TEXT("a click on the first stand's far corner anchors on the taxiway"),
		static_cast<int32>(Tool.GetStage()), static_cast<int32>(EStandStage::Entrance))) { return false; }
	TArray<FVector2D> Shown;
	Tool.Rect(At(Actor, FarCorner), Shown);
	if (!TestTrue(TEXT("the grid puts the second anchor ON the first stand's corner, or this is not a flush row"),
		Shown.Num() > 0 && Shown[0].Equals(FarCorner, 1.0))) { return false; }
	const FVector2D Anchor = Shown[0];
	Tool.OnClick(At(Actor, Anchor + FVector2D(Width, 0.0)));
	Tool.OnClick(At(Actor, Anchor + FVector2D(Width, Depth)));
	if (!TestEqual(TEXT("the second stand reaches Confirm"),
		static_cast<int32>(Tool.GetStage()), static_cast<int32>(EStandStage::Confirm))) { return false; }

	const FToolReadout Readout = ReadoutOf(Tool, At(Actor, Anchor));
	TestTrue(FString::Printf(TEXT("Build is lit for a neighbour sharing only an edge (warnings: %s)"),
		*FString::Join(Readout.Warnings, TEXT("; "))), Readout.bCommittable);
	Tool.OnCommit(At(Actor, Anchor));
	TestEqual(TEXT("and both stands stand"), LiveStands(Actor), 2);

	// A REAL METRE OF OVERLAP is still refused: the first stand's own rectangle slid back
	// 100 uu short of flush.
	TArray<FVector2D> Overlapping = First;
	for (FVector2D& Corner : Overlapping) { Corner += FVector2D(Width - 100.0, 0.0); }
	IRoadEditTarget* Target = Actor;
	const FString Why = Target->WhyStandRefused(Overlapping);
	TestTrue(FString::Printf(TEXT("a stand overlapping its neighbour by 1 m is refused ('%s')"), *Why),
		Why.Contains(TEXT("overlaps")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandPlotCancelStepsBackTest,
	"Airside.Tool.StandPlot.CancelStepsBack",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandPlotCancelStepsBackTest::RunTest(const FString& Parameters)
{
	using namespace StandPlotToolFixture;

	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
	TaxiwayWorld(Actor);

	FStandPlotTool Tool;
	if (!TestTrue(TEXT("locked"), DrawStand(Tool, Actor, 6000.0, 5500.0))) { return false; }

	// ONE STAGE AT A TIME, the depot's answer to a misclick: binning the whole gesture is a
	// harsher response than the mistake deserves.
	const FToolContext Here = At(Actor, AnchorCursor);
	Tool.OnCancel(Here);
	TestEqual(TEXT("Confirm steps back to Depth"), static_cast<int32>(Tool.GetStage()), static_cast<int32>(EStandStage::Depth));
	Tool.OnCancel(Here);
	TestEqual(TEXT("Depth steps back to Entrance"), static_cast<int32>(Tool.GetStage()), static_cast<int32>(EStandStage::Entrance));
	Tool.OnCancel(Here);
	TestEqual(TEXT("Entrance steps back to Idle"), static_cast<int32>(Tool.GetStage()), static_cast<int32>(EStandStage::Idle));
	Tool.OnCancel(Here);
	TestEqual(TEXT("and Idle stays Idle"), static_cast<int32>(Tool.GetStage()), static_cast<int32>(EStandStage::Idle));
	TestEqual(TEXT("nothing was placed on the way"), LiveStands(Actor), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandPlotRemoveTakesStandsOnlyTest,
	"Airside.Tool.StandPlot.RemoveTakesStandsOnly",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandPlotRemoveTakesStandsOnlyTest::RunTest(const FString& Parameters)
{
	using namespace StandPlotToolFixture;

	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
	TaxiwayWorld(Actor);
	Actor->FuelDepotDefinition = UEntityDefinition::MakeFuelDepotTransient();

	FStandPlotTool Tool;
	if (!TestTrue(TEXT("a stand drawn"), DrawStand(Tool, Actor, 6000.0, 5500.0))) { return false; }
	TArray<FVector2D> Drawn;
	Tool.Rect(At(Actor, AnchorCursor), Drawn);
	Tool.OnCommit(At(Actor, AnchorCursor));
	if (!TestEqual(TEXT("and built"), LiveStands(Actor), 1)) { return false; }

	// A DEPOT, well clear of the stand, placed the pre-plot way so FindEntityAt picks it by pose.
	const FVector2D DepotAt(-6000.0, 8000.0);
	IRoadEditTarget* Target = Actor;
	if (!TestTrue(TEXT("a depot placed"), Target->PlaceEntity(DepotAt, 0.0, EPlaceableEntity::FuelDepot) != INDEX_NONE))
	{
		return false;
	}
	if (!TestTrue(TEXT("the pick really finds the depot there, so the refusal below is the tool's"),
		Target->FindEntityAt(DepotAt, 150.0) != INDEX_NONE))
	{
		return false;
	}

	FToolContext OverDepot = At(Actor, DepotAt);
	OverDepot.bRemoveModifier = true;
	FStandPlotSink DepotPreview;
	Tool.BuildPreview(OverDepot, DepotPreview);
	TestFalse(TEXT("Remove over a depot offers nothing - the depot tool removes depots"),
		DepotPreview.Says(TEXT("remove")));
	Tool.OnClick(OverDepot);
	TestEqual(TEXT("and clicking it deletes nothing"), LiveEntities(Actor), 2);

	// OVER THE STAND'S GROUND - its middle, well away from the stop mark, because every stand
	// now has an outline and is picked by it.
	const FVector2D StandMiddle = (Drawn[0] + Drawn[2]) * 0.5;
	FToolContext OverStand = At(Actor, StandMiddle);
	OverStand.bRemoveModifier = true;
	FStandPlotSink StandPreview;
	Tool.BuildPreview(OverStand, StandPreview);
	TestTrue(TEXT("Remove over a stand says so"), StandPreview.Says(TEXT("remove stand")));
	Tool.OnClick(OverStand);
	TestEqual(TEXT("and the click deletes the stand"), LiveStands(Actor), 0);
	TestEqual(TEXT("leaving the depot"), LiveEntities(Actor), 1);
	TestEqual(TEXT("no gesture was started by a Remove click"), static_cast<int32>(Tool.GetStage()), static_cast<int32>(EStandStage::Idle));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandPlotRegistryKeyThreeTest,
	"Airside.Tool.StandPlot.RegistryKeyThree",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandPlotRegistryKeyThreeTest::RunTest(const FString& Parameters)
{
	using namespace StandPlotToolFixture;

	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
	TaxiwayWorld(Actor);

	const FToolRegistration* Three = nullptr;
	for (const FToolRegistration& Entry : ToolRegistry())
	{
		if (Entry.Key == EKeys::Three) { Three = &Entry; }
	}
	if (!TestNotNull(TEXT("key 3 is registered"), Three)) { return false; }

	TUniquePtr<IBuildTool> Made = Three->Make();
	if (!TestTrue(TEXT("and makes a tool"), Made.IsValid())) { return false; }
	TestEqual(TEXT("named Stand"), Made->GetDisplayName().ToString(), FString(TEXT("Stand")));

	// THE NAME ALONE CANNOT TELL THE OLD TOOL FROM THE NEW - both said "Stand". Behaviour can:
	// the press-drag-release tool placed a stand on a click and stayed idle; the drawn-stand
	// tool anchors an entrance edge and places nothing until Build.
	Made->OnClick(At(Actor, AnchorCursor));
	TestFalse(TEXT("a click beside a taxiway starts a gesture rather than dropping a stand"), Made->IsIdle());
	TestEqual(TEXT("and places nothing yet"), LiveStands(Actor), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandPlotPreviewDrawsTheKeepOutTest,
	"Airside.Tool.StandPlot.PreviewDrawsTheKeepOut",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandPlotPreviewDrawsTheKeepOutTest::RunTest(const FString& Parameters)
{
	using namespace StandPlotToolFixture;

	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
	TaxiwayWorld(Actor);

	const double Width = ReachableWidthAtLeast(IcaoCode::StandWidthForLetter(EIcaoCode::C));
	const double Depth = IcaoCode::StandDepthForLetter(EIcaoCode::C);
	FStandPlotTool Tool;
	if (!TestTrue(TEXT("a Code C stand locked"), DrawStand(Tool, Actor, Width, Depth))) { return false; }

	TArray<FVector2D> Shown;
	Tool.Rect(At(Actor, AnchorCursor), Shown);
	const FVector2D Inward(0.0, 1.0);
	const StandBox::FStandPose Pose = StandBox::PoseFor(Shown[0], Shown[1], Inward, EIcaoCode::C);
	const FVector2D Left = RoadGeom::PerpCCW(Pose.Facing);
	const double HalfSpan = 0.5 * IcaoCode::MaxWingspanForLetter(EIcaoCode::C);

	FStandPlotSink Sink;
	Tool.BuildPreview(At(Actor, AnchorCursor), Sink);

	// THE KEEP-OUT, in the stand's own frame transformed by the pose the commit will store:
	// the box IcaoCode::WingKeepOutContains tests, not an impression of it. Each corner is
	// checked, so a box drawn about the wrong origin or with its axes swapped misses.
	const double Fwd = IcaoCode::WingFwdForLetter(EIcaoCode::C);
	const double Aft = IcaoCode::WingAftForLetter(EIcaoCode::C);
	for (const FVector2D& Local : { FVector2D(Fwd, HalfSpan), FVector2D(Fwd, -HalfSpan),
		FVector2D(Aft, HalfSpan), FVector2D(Aft, -HalfSpan) })
	{
		TestTrue(TEXT("the local box's own corner is the keep-out's edge"),
			IcaoCode::WingKeepOutContains(EIcaoCode::C, Local));
		const FVector2D World = Pose.Position + Pose.Facing * Local.X + Left * Local.Y;
		TestTrue(*FString::Printf(TEXT("a Pending edge reaches keep-out corner (%.0f, %.0f)"), Local.X, Local.Y),
			Sink.TouchesIn(World, EPreviewStyle::Pending));
	}

	// THE LEAD-IN: entrance midpoint to the stop mark, the line an arrival taxis in along.
	TestTrue(TEXT("a Pending lead-in reaches the stop mark"), Sink.TouchesIn(Pose.Position, EPreviewStyle::Pending));
	TestTrue(TEXT("from the entrance edge's midpoint"),
		Sink.TouchesIn((Shown[0] + Shown[1]) * 0.5, EPreviewStyle::Pending));

	TestTrue(TEXT("and the letter is labelled on the ground"), Sink.Says(TEXT("Code C")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandPlotZeroDepthClickStaysTest,
	"Airside.Tool.StandPlot.ZeroDepthClickStays",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandPlotZeroDepthClickStaysTest::RunTest(const FString& Parameters)
{
	using namespace StandPlotToolFixture;

	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
	TaxiwayWorld(Actor);

	FStandPlotTool Tool;
	Tool.OnClick(At(Actor, AnchorCursor));
	const FVector2D Anchor = PinnedAnchor(Tool, Actor);
	Tool.OnClick(At(Actor, Anchor + FVector2D(6000.0, 0.0)));
	if (!TestEqual(TEXT("dragging the depth"),
		static_cast<int32>(Tool.GetStage()), static_cast<int32>(EStandStage::Depth))) { return false; }

	// ON THE ENTRANCE LINE: zero depth folds the rectangle onto its own entrance edge, and a
	// click that locked it would leave the player holding a shape only Cancel escapes.
	Tool.OnClick(At(Actor, Anchor + FVector2D(3000.0, 0.0)));
	TestEqual(TEXT("a click on the entrance line does not lock a zero-depth stand"),
		static_cast<int32>(Tool.GetStage()), static_cast<int32>(EStandStage::Depth));

	// BEHIND IT, toward the taxiway: clamped to zero depth, so refused the same way.
	Tool.OnClick(At(Actor, Anchor + FVector2D(3000.0, -500.0)));
	TestEqual(TEXT("nor does a click behind it, on the taxiway side"),
		static_cast<int32>(Tool.GetStage()), static_cast<int32>(EStandStage::Depth));
	return true;
}

namespace StandPlotToolFixture
{
	/**
	 * Taxiways at about 30 degrees with non-integer node coordinates - the case where the
	 * tool's unit vectors are 1 give or take an ulp, so a rectangle drawn exactly at a letter's
	 * floor measures an ulp either side of it unless StandBox rounds its lengths.
	 *
	 * SEVERAL ANGLES, because whether a given angle lands an ulp low is luck: the first single
	 * 30-degree fixture written for this passed WITHOUT the rounding, measuring nothing.
	 */
	const double DiagonalDegrees[] = { 30.0, 28.7, 31.9, 33.3, 26.1, 37.7, 29.45 };
	const FVector2D DiagonalCentre(12.3456, -56.7891);
	const double DiagonalHalfLength = 10000.3137;

	void DiagonalWorld(ARoadNetworkActor* Actor, const FVector2D& A, const FVector2D& B)
	{
		Actor->ClearNetwork();
		Actor->StandDefinition = UEntityDefinition::MakeStandTransient();
		IRoadEditTarget* Target = Actor;
		const int32 NodeA = Target->PlaceNode(A);
		const int32 NodeB = Target->PlaceNode(B);
		Target->ConnectNodes(NodeA, NodeB, ERoadKind::Taxiway, INDEX_NONE);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandPlotDiagonalTaxiwayReadsItsLetterTest,
	"Airside.Tool.StandPlot.DiagonalTaxiwayReadsItsLetter",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandPlotDiagonalTaxiwayReadsItsLetterTest::RunTest(const FString& Parameters)
{
	using namespace StandPlotToolFixture;

	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	for (const double Degrees : DiagonalDegrees)
	{
		const FVector2D Dir(FMath::Cos(FMath::DegreesToRadians(Degrees)), FMath::Sin(FMath::DegreesToRadians(Degrees)));
		const FVector2D DiagonalA = DiagonalCentre - Dir * DiagonalHalfLength;
		const FVector2D DiagonalB = DiagonalCentre + Dir * DiagonalHalfLength;
		const FVector2D Unit = (DiagonalB - DiagonalA).GetSafeNormal();
		const FVector2D Left = RoadGeom::PerpCCW(Unit);
		const FVector2D Mid = (DiagonalA + DiagonalB) * 0.5;

		// EVERY LETTER AT ITS EXACT FLOOR, from BOTH SIDES of the taxiway - the two sides wind the
		// rectangle opposite ways, and the facade reverses one of them, measuring the OPPOSITE
		// edge. A and B cannot be built yet, so for them only the readout's letter is asserted.
		for (EIcaoCode Letter : { EIcaoCode::A, EIcaoCode::B, EIcaoCode::C, EIcaoCode::D, EIcaoCode::E, EIcaoCode::F })
		{
			const bool bBuildable = Letter >= EIcaoCode::C;
			const double Width = ReachableWidthAtLeast(IcaoCode::StandWidthForLetter(Letter));
			const double Depth = IcaoCode::StandDepthForLetter(Letter);
			const FString Expected = FString(TEXT("Code ")) + IcaoCode::ToLetter(Letter);

			for (const double Side : { 1.0, -1.0 })
			{
				const TCHAR* SideName = Side > 0.0 ? TEXT("left") : TEXT("right");
				const TCHAR* Name = IcaoCode::ToLetter(Letter);
				DiagonalWorld(Actor, DiagonalA, DiagonalB);

				// Start 50 m back along the taxiway so even Code F's 110 m entrance stays beside it.
				const FVector2D Start = Mid - Unit * 5000.0 + Left * (Side * 1000.0);
				FStandPlotTool Tool;
				Tool.OnClick(At(Actor, Start));
				if (!TestEqual(*FString::Printf(TEXT("%.2f deg, Code %s %s: anchored"), Degrees, Name, SideName),
					static_cast<int32>(Tool.GetStage()), static_cast<int32>(EStandStage::Entrance))) { continue; }

				TArray<FVector2D> Anchored;
				Tool.Rect(At(Actor, Start), Anchored);
				const FVector2D Anchor = Anchored[0];
				const FVector2D Inward = Left * Side;
				Tool.OnClick(At(Actor, Anchor + Unit * Width));
				Tool.OnClick(At(Actor, Anchor + Unit * Width + Inward * Depth));
				if (!TestEqual(*FString::Printf(TEXT("%.2f deg, Code %s %s: locked"), Degrees, Name, SideName),
					static_cast<int32>(Tool.GetStage()), static_cast<int32>(EStandStage::Confirm))) { continue; }

				const FToolReadout Readout = ReadoutOf(Tool, At(Actor, Start));
				TestEqual(*FString::Printf(TEXT("%.2f deg, Code %s %s: a stand drawn exactly at the floor reads as its letter"),
					Degrees, Name, SideName), FactOf(Readout, TEXT("Stand")), Expected);
				if (!bBuildable) { continue; }

				TestTrue(*FString::Printf(TEXT("%.2f deg, Code %s %s: and Build is lit"), Degrees, Name, SideName),
					Readout.bCommittable);

				// THE COMMIT MEASURES THE REVERSED OUTLINE on one of the two sides - the edge the
				// readout did not measure. A lit Build and a refused commit is the bug this pins.
				Tool.OnCommit(At(Actor, Start));
				const FEntityInstance* Placed = nullptr;
				for (const FEntityInstance& Entity : Actor->Network->GetEntities())
				{
					if (Entity.bAlive && Entity.IsStand()) { Placed = &Entity; }
				}
				if (!TestNotNull(*FString::Printf(TEXT("%.2f deg, Code %s %s: the commit places it"), Degrees, Name, SideName),
					Placed)) { continue; }
				const TOptional<EIcaoCode> Read = StandBox::LetterOf(Placed->Outline);
				TestTrue(*FString::Printf(TEXT("%.2f deg, Code %s %s: and the stored outline reads as the same letter"),
					Degrees, Name, SideName), Read.IsSet() && *Read == Letter);
			}
		}
	}
	return true;
}

// --- Carried over from StandPlaceToolTest.cpp (Airside.Tool.StandPlace) ---------------------
//
// That file tested FStandPlaceTool's press-drag-release, deleted with the tool. What it said
// about the STAND - its definition, removal, undo, anchor survival and in-use warning - is
// still true of a drawn stand and is asserted here through the tool that now places one.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandPlotDesignAircraftIsNotTheStandTest,
	"Airside.Tool.StandPlot.DesignAircraftIsNotTheStand",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandPlotDesignAircraftIsNotTheStandTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	// THROUGH THE RESOLVER, not the raw property. StandDefinition stays null unless somebody
	// authored one: the actor used to have the content default written INTO it, which is what
	// silently gave every level a material set and a stand it never asked for. Asking the
	// resolver is now the only way to know what the actor will actually place.
	UEntityDefinition* Stand = Actor->ResolveStandDefinition();
	if (!TestNotNull(TEXT("the actor resolves a stand definition"), Stand)) { return false; }
	TestTrue(TEXT("and its anchors are all named and distinct"),
		UEntityDefinition::HasUsableAnchorIds(Stand));

	// A Code C stand takes an A320 AND a 737-800, and their doors are metres apart. Where a
	// service is REQUIRED therefore belongs to the aircraft; what the ground can PROVIDE
	// belongs to the stand. Baking one type's geometry into the stand is the bug this split
	// exists to prevent, and this is the assertion that would catch it coming back.
	const UAircraftType* Design = Stand->DesignAircraft.Get();
	if (!TestNotNull(TEXT("the stand names a design aircraft"), Design)) { return false; }

	TestTrue(TEXT("the aircraft carries the plan footprint"), Design->Footprint.IsSet());
	TestTrue(TEXT("and its service points are all named and distinct"),
		UAircraftType::HasUsableServiceIds(Design));
	TestTrue(TEXT("the stand provides fuel"), Stand->Provides(EServiceRole::Fuel));

	// The stand's own anchors are GROUND fixtures. None of them may be a place on an
	// airframe, so none should sit where a door does.
	TestTrue(TEXT("the stand declares ground fixtures"), Stand->Anchors.Num() > 0);

	// THE PROPERTY THE SPLIT BUYS. Two Code C types, same stand, and their hold doors
	// must NOT land in the same place - if they did, the stand could have carried the
	// geometry after all and this whole change bought nothing.
	UAircraftType* Boeing = NewObject<UAircraftType>(GetTransientPackage());
	UAircraftType::Build737(Boeing);

	const FEntityAnchor* AirbusHold = Design->ServicePoints.FindByPredicate(
		[](const FEntityAnchor& Each) { return Each.Id == FName(TEXT("HoldFwd")); });
	const FEntityAnchor* BoeingHold = Boeing->ServicePoints.FindByPredicate(
		[](const FEntityAnchor& Each) { return Each.Id == FName(TEXT("HoldFwd")); });

	if (TestNotNull(TEXT("the A320 has a forward hold"), AirbusHold)
		&& TestNotNull(TEXT("and so does the 737"), BoeingHold))
	{
		TestTrue(TEXT("their forward holds are at DIFFERENT stations"),
			FMath::Abs(AirbusHold->LocalPosition.X - BoeingHold->LocalPosition.X) > 100.0);
	}

	TestTrue(TEXT("and the two types are not the same length"),
		FMath::Abs(Design->Footprint.TailX - Boeing->Footprint.TailX) > 100.0);

	// Four sides of the envelope plus fuselage, wing and tailplane: seven segments.
	TArray<FVector2D> Outline;
	UAircraftType::BuildFootprintLines(Design->Footprint, Outline);
	TestEqual(TEXT("the outline is seven segments"), Outline.Num(), 14);

	// Every service point must fall within the aircraft it belongs to, or the outline
	// is decoration rather than orientation.
	const double HalfSpan = Design->Footprint.Wingspan * 0.5;
	for (const FEntityAnchor& Point : Design->ServicePoints)
	{
		TestTrue(FString::Printf(TEXT("service point %s lies within the wingspan"), *Point.Id.ToString()),
			FMath::Abs(Point.LocalPosition.Y) <= HalfSpan);
		TestTrue(FString::Printf(TEXT("service point %s lies along the fuselage"), *Point.Id.ToString()),
			Point.LocalPosition.X <= Design->Footprint.NoseX
			&& Point.LocalPosition.X >= Design->Footprint.TailX);
	}

	// An unauthored footprint draws nothing rather than a degenerate dot at the origin.
	FEntityFootprint Empty;
	TestFalse(TEXT("an unauthored footprint reports itself unset"), Empty.IsSet());
	UAircraftType::BuildFootprintLines(Empty, Outline);
	TestEqual(TEXT("and produces no segments at all"), Outline.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandPlotRemoveTakesItsAnchorNodesTest,
	"Airside.Tool.StandPlot.RemoveTakesItsAnchorNodes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandPlotRemoveTakesItsAnchorNodesTest::RunTest(const FString& Parameters)
{
	using namespace StandPlotToolFixture;

	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
	TaxiwayWorld(Actor);

	FStandPlotTool Tool;
	if (!TestTrue(TEXT("a stand drawn"), DrawStand(Tool, Actor, 6000.0, 5500.0))) { return false; }
	TArray<FVector2D> Drawn;
	Tool.Rect(At(Actor, AnchorCursor), Drawn);
	Tool.OnCommit(At(Actor, AnchorCursor));
	if (!TestEqual(TEXT("a stand to remove"), LiveStands(Actor), 1)) { return false; }

	TArray<FGuidelineNodeId> Owned;
	for (const FEntityInstance& Entity : Actor->Network->GetEntities())
	{
		if (!Entity.bAlive || !Entity.IsStand()) { continue; }
		for (const FResolvedAnchor& Anchor : Entity.ResolvedAnchors) { Owned.Add(Anchor.Node); }
	}
	TestTrue(TEXT("the stand resolved anchors to take with it"), Owned.Num() > 0);

	FToolContext Remove = At(Actor, (Drawn[0] + Drawn[2]) * 0.5);
	Remove.bRemoveModifier = true;
	Tool.OnClick(Remove);

	TestEqual(TEXT("ctrl-click removes the stand"), LiveStands(Actor), 0);
	for (const FGuidelineNodeId& Node : Owned)
	{
		TestNull(TEXT("and its anchor nodes went with it"), Actor->Network->GetGuidelineNode(Node));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandPlotUndoIsOneEditTest,
	"Airside.Tool.StandPlot.UndoIsOneEdit",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandPlotUndoIsOneEditTest::RunTest(const FString& Parameters)
{
	using namespace StandPlotToolFixture;

	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
	TaxiwayWorld(Actor);

	FStandPlotTool Tool;
	if (!TestTrue(TEXT("a stand drawn"), DrawStand(Tool, Actor, 6000.0, 5500.0))) { return false; }
	Tool.OnCommit(At(Actor, AnchorCursor));
	if (!TestEqual(TEXT("placed"), LiveStands(Actor), 1)) { return false; }

	int32 Anchors = 0;
	for (const FEntityInstance& Entity : Actor->Network->GetEntities())
	{
		if (Entity.bAlive && Entity.IsStand()) { Anchors = Entity.ResolvedAnchors.Num(); }
	}

	// Placing is one edit, not nine: the stand and every anchor come and go together.
	TestEqual(TEXT("as a single named edit"), Actor->PeekUndoLabel(), FString(TEXT("place stand")));
	TestTrue(TEXT("undo takes it back"), Actor->Undo());
	TestEqual(TEXT("all of it"), LiveStands(Actor), 0);

	TestTrue(TEXT("and redo returns it"), Actor->Redo());
	int32 Restored = -1;
	for (const FEntityInstance& Entity : Actor->Network->GetEntities())
	{
		if (Entity.bAlive && Entity.IsStand()) { Restored = Entity.ResolvedAnchors.Num(); }
	}
	TestEqual(TEXT("with its anchors"), Restored, Anchors);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandPlotAnchorsSurviveRebuildsTest,
	"Airside.Tool.StandPlot.AnchorsSurviveRebuilds",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandPlotAnchorsSurviveRebuildsTest::RunTest(const FString& Parameters)
{
	using namespace StandPlotToolFixture;

	// --- THE PROPERTY THE WHOLE DESIGN RESTS ON ---------------------------------------
	//
	// FRoadGuidelineBuilder::Build destroys and re-adds every DERIVED guideline node on
	// each run, advancing its generation - so anything holding a derived node's handle
	// across a rebuild finds it dangling. A stand's anchors are exactly that: handles,
	// stored, and expected to outlive every edit made elsewhere on the airport.
	//
	// They survive because they are created NON-DERIVED and the orphan sweep requires
	// bDerived. Asserted here through the TOOL, because that is the path a player takes.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
	TaxiwayWorld(Actor);

	FStandPlotTool Tool;
	if (!TestTrue(TEXT("a stand drawn"), DrawStand(Tool, Actor, 6000.0, 5500.0))) { return false; }
	Tool.OnCommit(At(Actor, AnchorCursor));
	if (!TestEqual(TEXT("and built"), LiveStands(Actor), 1)) { return false; }

	TArray<FGuidelineNodeId> Anchors;
	TArray<FVector2D> Positions;
	for (const FEntityInstance& Entity : Actor->Network->GetEntities())
	{
		if (!Entity.bAlive || !Entity.IsStand()) { continue; }
		for (const FResolvedAnchor& Anchor : Entity.ResolvedAnchors)
		{
			Anchors.Add(Anchor.Node);
			const FGuidelineNode* Node = Actor->Network->GetGuidelineNode(Anchor.Node);
			Positions.Add(Node != nullptr ? Node->Position : FVector2D::ZeroVector);
		}
	}
	TestTrue(TEXT("anchors to follow"), Anchors.Num() > 0);

	// More taxiway for the builder to churn, drawn after the stand exists.
	IRoadEditTarget* Target = Actor;
	const int32 Hub = Target->PlaceNode(FVector2D(-30000.0, -20000.0));
	const int32 East = Target->PlaceNode(FVector2D(-18000.0, -20000.0));
	Target->ConnectNodes(Hub, East, ERoadKind::Taxiway, INDEX_NONE);

	// Twice, because the sweep only bites on a rebuild that finds nodes from a previous
	// one - a single pass would leave the interesting case untested.
	const FRoadSolveResult First = FRoadNetworkSolver::SolveAll(*Actor->Network);
	FRoadGuidelineBuilder::Build(*Actor->Network, First, UAirsideSettings::ResolveLargestServiceVehicle());
	const FRoadSolveResult Second = FRoadNetworkSolver::SolveAll(*Actor->Network);
	FRoadGuidelineBuilder::Build(*Actor->Network, Second, UAirsideSettings::ResolveLargestServiceVehicle());

	for (int32 Index = 0; Index < Anchors.Num(); ++Index)
	{
		const FGuidelineNode* Node = Actor->Network->GetGuidelineNode(Anchors[Index]);
		if (!TestNotNull(TEXT("an anchor handle still resolves after two rebuilds"), Node)) { continue; }

		// And to the SAME node, not merely to something. A rebuild that re-pointed an
		// anchor would satisfy a null check and still send the fuel truck elsewhere.
		TestTrue(TEXT("at the position it was placed at"), Node->Position.Equals(Positions[Index], 0.01));
		TestFalse(TEXT("and it is still not owned by the derivation"), Node->bDerived);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandPlotRemoveNamesTheAircraftInUseTest,
	"Airside.Tool.StandPlot.RemoveNamesTheAircraftInUse",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandPlotRemoveNamesTheAircraftInUseTest::RunTest(const FString& Parameters)
{
	using namespace StandPlotToolFixture;

	// A stand IN USE is labelled before it is deleted (spec 2026-09-07-stand-occupancy §6).
	// The click still removes it; the label is the warning.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
	TaxiwayWorld(Actor);

	FStandPlotTool Tool;
	if (!TestTrue(TEXT("a stand drawn"), DrawStand(Tool, Actor, 6000.0, 5500.0))) { return false; }
	TArray<FVector2D> Drawn;
	Tool.Rect(At(Actor, AnchorCursor), Drawn);
	Tool.OnCommit(At(Actor, AnchorCursor));
	if (!TestEqual(TEXT("placed a stand to occupy"), LiveStands(Actor), 1)) { return false; }

	FGuidelineNodeId Pose;
	for (const FEntityInstance& E : Actor->Network->GetEntities()) { if (E.bAlive && E.IsStand()) { Pose = E.PoseNode; } }
	if (!TestTrue(TEXT("the stand has a pose node"), Pose.IsSet())) { return false; }

	// An authored way in, so an aircraft can be sent to it without routing the taxiway.
	const FGuidelineNode* PoseNode = Actor->Network->GetGuidelineNode(Pose);
	if (!TestNotNull(TEXT("the pose node resolves"), PoseNode)) { return false; }
	const FVector2D PoseAt = PoseNode->Position;
	const FGuidelineNodeId From = Actor->Network->AddGuidelineNode(PoseAt + FVector2D(0.0, 20000.0), false);
	{
		FGuidelineEdge Edge;
		Edge.A = From; Edge.B = Pose;
		Edge.Control = PoseAt + FVector2D(0.0, 10000.0);
		Edge.AllowedTraffic = FTrafficMask::All();
		Edge.Direction = EGuidelineDir::Bidirectional;
		Edge.bDerived = false;
		Actor->Network->AddGuidelineEdge(MoveTemp(Edge));
	}
	FRouteQuery Q; Q.Errand = ERouteErrand::GraphProbe; Q.Policy = FRoutePolicy::For(Q.Errand); Q.Start = From; Q.Goal = Pose; Q.Class = ETraversalClass::Aircraft;
	if (!TestTrue(TEXT("an aircraft is sent to the stand"),
		Actor->DispatchAgent(RouteSearch::Find(*Actor->Network, Q), UAirsideSettings::ResolveDefaultAirframe()))) { return false; }
	const int32 Id = Actor->GetTraffic()->GetNewestAgentId();

	FToolContext Remove = At(Actor, (Drawn[0] + Drawn[2]) * 0.5);
	Remove.bRemoveModifier = true;
	FStandPlotSink InUse;
	Tool.BuildPreview(Remove, InUse);
	TestTrue(TEXT("the remove preview names the aircraft using the stand"),
		InUse.Says(FString::Printf(TEXT("in use by aircraft %d"), Id)));

	Actor->GetTraffic()->RetireAgent(Id);
	FStandPlotSink Free;
	Tool.BuildPreview(Remove, Free);
	TestTrue(TEXT("still says remove"), Free.Says(TEXT("remove stand")));
	TestFalse(TEXT("a free stand carries no in-use label"), Free.Says(TEXT("in use")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandPlotDrawnStandTakesAnArrivalTest,
	"Airside.Tool.StandPlot.DrawnStandTakesAnArrival",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandPlotDrawnStandTakesAnArrivalTest::RunTest(const FString& Parameters)
{
	using namespace StandPlotToolFixture;

	// TASK 9: THE SEAM FROM TOOL TO ADMISSION. Every earlier task proved its own layer in
	// isolation - the gesture measures a rectangle, the facade turns it into a stand with a
	// captured letter, ArrivalPlanner reads DesignWingspan back through IcaoCode::StandAdmits
	// - but nothing before this test drew a stand with the same clicks a player makes and
	// then landed an aircraft on the airport that now has one. ArrivalDispatchTest is the
	// smallest existing end-to-end fixture (grep LandAircraft/DispatchArrival) and this
	// follows its shape - a runway sized to the airframe, an exit, a taxiway - but places the
	// stand through FStandPlotTool rather than Actor->PlaceStand, which is the one thing that
	// fixture does not exercise.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	// A 737-800: Build737 gives it Code C (Wingspan 3580 uu, under Code C's 3600 uu ceiling -
	// IcaoCode::LetterForWingspan), and it is one of the two fixtures named for this test.
	// Ground is authored; Climb and Approach are left at their struct defaults, which are all
	// positive and so IsSet() - enough to fly this fixture's landing.
	UAircraftType* Type = NewObject<UAircraftType>(GetTransientPackage());
	UAircraftType::Build737(Type);
	FAirframe Airframe = Type->Airframe();

	// STEER LAW, CORRECTED FOR A FIXTURE THAT NOW ACTUALLY MOVES. Build737's own comment calls
	// this "the PAPER 737... nothing ever flies it", and its Ground carries no SteerAxleX/
	// FixedAxleX for exactly that reason - the flying 737 is DA_Aircraft_Plane4's measured
	// rig, not this one. UAircraftType::SteerLaw defaults RollingSteer, so taken unchanged this
	// airframe would fail FAirframe::NeedsSteerLawWarning and WarnIfSteerLawUnsupported would
	// log a LogAirside Error the first time a phase binds it - which AutomationTestFramework
	// treats as a test failure regardless of what RunTest returns. Pivot is what
	// EffectiveSteerLaw falls back to anyway for an unmeasured wheelbase; setting it here
	// merely skips the one-time warning for a fixture that is deliberately using the paper
	// type to fly.
	Airframe.Chassis.SteerLaw = ESteerLaw::Pivot;

	// FORCES Network INTO EXISTENCE ON THE ACTOR ITSELF, exactly as ArrivalDispatchTest does,
	// so the FTestAirport built below lands on Actor->Network rather than an orphan object the
	// tool's clicks (which read Context.Network() off the actor) would never see.
	Actor->PlaceNode(FVector2D(-100000.0, -100000.0));
	if (!TestNotNull(TEXT("the actor has a network"), Actor->Network.Get())) { return false; }

	// A CODE C DEFINITION WITH NO CONTENT ASSET: ResolveStandDefinitionFor(C) forwards to
	// ResolveStandDefinition() unchanged (ARoadNetworkActor's own comment on why), which reads
	// this property - the same fixture every other test in this file uses, so drawing a Code C
	// stand needs no DA_Stand_CodeC to be loaded in a bare automation run.
	Actor->StandDefinition = UEntityDefinition::MakeStandTransient();

	// THE AIRPORT: a runway sized to the 737, one exit, a taxiway south of it - NOT derived yet
	// (bDerived=false, matching ArrivalDispatchTest's own "world variant") and NO STAND
	// (StandCount=0): the stand under test is drawn through the tool below, not FTestAirport's
	// own placement, which is exactly the seam this test exists to cross.
	const FTestAirport Fixture =
		FTestAirport::Build(Airframe, { .StandCount = 0, .bDerived = false }, Actor->Network.Get());
	const FVector2D ThresholdAt = Fixture.Threshold;
	const FVector2D ExitAt = Fixture.ExitAt;

	// THE TAXIWAY RUNS SOUTH FROM THE EXIT - FTestAirport::Build's own single-exit shape lays
	// it ExitNode -> ExitNode + (0, -20000) - so the click sits 10 m east of its centreline
	// (within PlotGesture::AnchorReachUu, 20 m) and 10000 uu down it, leaving room for a
	// Code C rectangle (59 x 55 m) on both ends before the exit and the taxiway's dead end.
	// NOT NAMED AnchorCursor - StandPlotToolFixture already declares one at file scope, and a
	// local of the same name would shadow it (C4459) rather than reuse it: this test anchors
	// on FTestAirport's own taxiway, not TaxiwayWorld's.
	const FVector2D ClickAt = ExitAt + FVector2D(1000.0, -10000.0);

	FStandPlotTool Tool;
	Tool.OnClick(At(Actor, ClickAt));
	if (!TestEqual(TEXT("the click anchors on the taxiway FTestAirport laid"),
		static_cast<int32>(Tool.GetStage()), static_cast<int32>(EStandStage::Entrance)))
	{
		return false;
	}
	const FVector2D Anchor = PinnedAnchor(Tool, Actor);

	// ALONG AND INWARD, NOT ASSUMED: PlotGesture::AnchorAt takes Along from the segment's own
	// A -> B order, and FTestAirport lays the taxiway ExitNode -> TaxiEnd, i.e. south; Inward
	// is whichever side of the centreline the cursor fell on (RoadGeom::PerpCCW((0,-1)) is
	// (1,0), and the anchor cursor sits east of it), i.e. east. Both read off PlotGesture.cpp
	// directly rather than re-derived here, so a change to either rule is caught by the four
	// corners not landing where this test expects rather than by this test silently agreeing
	// with whatever the tool did.
	const FVector2D Along(0.0, -1.0);
	const FVector2D Inward(1.0, 0.0);
	const double Width = ReachableWidthAtLeast(IcaoCode::StandWidthForLetter(EIcaoCode::C));
	const double Depth = IcaoCode::StandDepthForLetter(EIcaoCode::C);

	Tool.OnClick(At(Actor, Anchor + Along * Width));
	if (!TestEqual(TEXT("the entrance edge pins and the depth drag begins"),
		static_cast<int32>(Tool.GetStage()), static_cast<int32>(EStandStage::Depth)))
	{
		return false;
	}
	Tool.OnClick(At(Actor, Anchor + Along * Width + Inward * Depth));
	if (!TestEqual(TEXT("a Code C floor rectangle locks"),
		static_cast<int32>(Tool.GetStage()), static_cast<int32>(EStandStage::Confirm)))
	{
		return false;
	}

	const FToolReadout Readout = ReadoutOf(Tool, At(Actor, ClickAt));
	if (!TestTrue(TEXT("Build is lit over a Code C rectangle beside a taxiway"), Readout.bCommittable))
	{
		return false;
	}

	// BUILD, THROUGH THE TOOL - PlaceStandInPlot, WhyStandRefused, CommitPurchase and the
	// facade's own NotifyChanged(Topology), which is what re-derives the guideline graph and
	// links the stand's anchors - none of it called by hand, which is the whole point of
	// drawing the stand this way rather than through Actor->PlaceStand.
	Tool.OnCommit(At(Actor, ClickAt));
	if (!TestEqual(TEXT("Build places exactly one stand"), LiveStands(Actor), 1)) { return false; }

	FGuidelineNodeId StandPose;
	for (const FEntityInstance& Entity : Actor->Network->GetEntities())
	{
		if (Entity.bAlive && Entity.IsStand()) { StandPose = Entity.PoseNode; }
	}
	if (!TestTrue(TEXT("the drawn stand has a pose node"), StandPose.IsSet())) { return false; }

	// THE MEASUREMENT: ORDERING AN ARRIVAL ON AN AIRPORT WHOSE ONLY STAND WAS DRAWN THROUGH
	// THE TOOL PRODUCES AN AIRCRAFT THAT PARKS ON IT. Everything above can be perfect and this
	// can still be false - ArrivalDispatchTest's own reason for existing, applied to the new
	// path.
	const int32 Before = Actor->GetAgentCount();
	const bool bDispatched = Actor->DispatchArrival(ThresholdAt, Airframe);
	if (!TestTrue(TEXT("an arrival is accepted on a runway with a drawn stand beside its exit"),
		bDispatched))
	{
		return false;
	}
	TestEqual(TEXT("and an aircraft exists as a result"), Actor->GetAgentCount(), Before + 1);

	// RUN TO COMPLETION, bounded as ArrivalDispatchTest's own loop is: 6000 * 0.1 s = 600
	// simulated seconds, comfortably past the landing roll plus the taxi to the drawn stand.
	int32 Ticks = 0;
	while (Actor->LastAgentPhaseForTest() != EAgentPhase::Parked && Ticks < 6000)
	{
		Actor->Tick(0.1f);
		++Ticks;
	}
	if (!TestEqual(FString::Printf(TEXT("the arrival parks within %d ticks (phase %d)"),
		Ticks, static_cast<int32>(Actor->LastAgentPhaseForTest())),
		Actor->LastAgentPhaseForTest(), EAgentPhase::Parked))
	{
		return false;
	}

	// THE ASSERTION THE BRIEF NAMES: the agent's own stand node is the drawn stand's PoseNode
	// - not merely "some stand parked somewhere", which a phase check alone cannot tell apart
	// from an agent that stalled over the wrong node. GoalNode is set from the route
	// ArrivalPlanner::ChooseStand returned at dispatch and nothing past that point repoints it
	// (see FRoadAgent::Advance's Parked branches), so reading it back here after parking is
	// reading the same node dispatch chose.
	const int32 AgentId = Actor->GetTraffic()->GetNewestAgentId();
	const FRoadAgent* Agent = Actor->GetTraffic()->GetModel()->FindAgent(AgentId);
	if (!TestNotNull(TEXT("the parked agent resolves"), Agent)) { return false; }
	TestTrue(TEXT("the agent's stand node is the drawn stand's own PoseNode - the seam from ")
		TEXT("tool to admission is wired: a stand the tool builds is one the planner chooses"),
		Agent->GoalNode == StandPose);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
