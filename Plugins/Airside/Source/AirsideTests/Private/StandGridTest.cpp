#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/TaxiwayStrip.h"
#include "Profiles/RoadProfile.h"
#include "Entities/EntityDefinition.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Solve/IcaoCode.h"
#include "Tool/BuildSession.h"
#include "Tool/PlotGesture.h"
#include "Tool/RoadEditTarget.h"
#include "Tool/SnapGuideSettings.h"
#include "Tool/StandPlotTool.h"
#include "Solve/GuideArbiter.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * STANDS ON THE WORLD GRID, AND STANDS THAT LINE UP WITH EACH OTHER.
 *
 * THE 2026-09-27 REPORT: a stand drawn to the same depth as its neighbour did not line up with
 * it, because depth is measured from each stand's own entrance line and the anchor sits on each
 * segment's own bay grid. Every test drives the tool through FBuildSession::MakeContext, the
 * only thing that resolves a guide, so a stand tool that described a guide anchor and ignored
 * the answer would fail here rather than pass on the anchor alone.
 */
namespace StandGridFixture
{
	struct FStandSession
	{
		FAirsideTestWorld TestWorld;
		FBuildSession Session;
		FBuildSessionTunables Tunables;

		FToolContext At(const FVector2D& Where) const
		{
			return Session.MakeContext(TestWorld.Actor, Where, Tunables, false, false);
		}

		FStandPlotTool* Tool() const
		{
			// The registry's "Stand" entry IS an FStandPlotTool - see BuildSession.cpp's table.
			return static_cast<FStandPlotTool*>(Session.GetActiveTool());
		}

		void Click(const FVector2D& Where)
		{
			Tool()->OnClick(At(Where));
		}

		/** Corner N of what the tool shows for a cursor at Where. */
		FVector2D Corner(int32 N, const FVector2D& Where) const
		{
			TArray<FVector2D> Shown;
			Tool()->Rect(At(Where), Shown);
			return Shown.IsValidIndex(N) ? Shown[N] : FVector2D(-1e9, -1e9);
		}
	};

	void LayTaxiway(ARoadNetworkActor* Actor, const FVector2D& A, const FVector2D& B)
	{
		IRoadEditTarget* Target = Actor;
		const int32 First = Target->PlaceNode(A);
		const int32 Second = Target->PlaceNode(B);
		Target->ConnectNodes(First, Second, ERoadKind::Taxiway, INDEX_NONE);
	}

	/** LayTaxiway past the strip judge - see TestTool::ConnectUnjudged for when that is honest. */
	void LayTaxiwayUnjudged(ARoadNetworkActor* Actor, const FVector2D& A, const FVector2D& B)
	{
		const int32 First = Actor->PlaceNode(A);
		const int32 Second = Actor->PlaceNode(B);
		TestTool::ConnectUnjudged(*Actor, First, Second);
	}

	/** A cleared network with a stand definition, the Stand tool selected, and the step set. */
	bool Begin(FStandSession& Out, EGridStep Step)
	{
		if (Out.TestWorld.World == nullptr || Out.TestWorld.Actor == nullptr) { return false; }
		Out.TestWorld.Actor->ClearNetwork();
		Out.TestWorld.Actor->StandDefinition = UEntityDefinition::MakeStandTransient();

		const TConstArrayView<FToolRegistration> Registry = ToolRegistry();
		int32 Index = INDEX_NONE;
		for (int32 I = 0; I < Registry.Num(); ++I)
		{
			if (Registry[I].Id == FName(TEXT("Stand"))) { Index = I; }
		}
		if (Index == INDEX_NONE) { return false; }
		Out.Session.SelectTool(Index);

		Out.Tunables = Out.TestWorld.Actor->MakeTunables(10000.0);
		// FORCED, not inherited: these are the column and relation the neighbour guide lives in,
		// and a moved default would silently stop the repro measuring anything.
		Out.Tunables.GuideSources.bCollinear = true;
		Out.Tunables.GuideSources.bStand = true;
		Out.Tunables.GuideSources.GridStep = Step;
		return Out.Session.GetActiveTool() != nullptr;
	}

	/** The narrowest reachable entrance at least a Code C stand wide - see StandPlotToolTest. */
	double CodeCWidth()
	{
		const double Floor = IcaoCode::StandWidthForLetter(EIcaoCode::C);
		if (Floor <= PlotGesture::MinFrontageUu) { return PlotGesture::MinFrontageUu; }
		return PlotGesture::MinFrontageUu
			+ FMath::CeilToDouble((Floor - PlotGesture::MinFrontageUu) / PlotGesture::FrontageStepUu)
			* PlotGesture::FrontageStepUu;
	}

	/** The largest Y of any live stand's outline - its back edge, for a stand north of its road. */
	double BackEdgeY(const ARoadNetworkActor* Actor)
	{
		double Best = -1e18;
		for (const FEntityInstance& Entity : Actor->Network->GetEntities())
		{
			if (!Entity.bAlive || !Entity.IsStand()) { continue; }
			for (const FVector2D& P : Entity.Outline) { Best = FMath::Max(Best, P.Y); }
		}
		return Best;
	}

	bool IsOnGridLine(double Value, double Step)
	{
		return FMath::IsNearlyEqual(Value, FMath::RoundToDouble(Value / Step) * Step, 1e-6);
	}
}

/**
 * THE REPRO. Two parallel taxiways 7.3 m apart in Y, on different bay-grid phases: stand 1
 * north of the first at Code C depth, stand 2 north of the second dragged to a depth that would
 * leave its back edge 1.5 m short of stand 1's. The neighbour's back edge wins - with the grid
 * off AND on - and without the Stand column (the control) it does not line up, which is what
 * shows the guide, not luck, did it.
 *
 * WITHIN 1e-6 uu, NOT BITWISE: the depth is a projection and Far + Inward * Depth re-adds it, so
 * an ulp can move. Bitwise is the surface weld's contract, not this one's.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandGridNeighbourBackEdgeTest,
	"Airside.Tool.StandGrid.NeighbourBackEdgeLinesUp",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandGridNeighbourBackEdgeTest::RunTest(const FString& Parameters)
{
	const double Width = StandGridFixture::CodeCWidth();
	const double Depth = IcaoCode::StandDepthForLetter(EIcaoCode::C);

	struct FCase { const TCHAR* Name; EGridStep Step; bool bStandColumn; bool bExpectFlush; };
	const FCase Cases[] = {
		{ TEXT("grid off"), EGridStep::Off, true, true },
		{ TEXT("grid 5 m"), EGridStep::FiveMetres, true, true },
		{ TEXT("control: Stand column off"), EGridStep::Off, false, false },
	};

	for (const FCase& Case : Cases)
	{
		StandGridFixture::FStandSession S;
		if (!TestTrue(Case.Name, StandGridFixture::Begin(S, Case.Step))) { return false; }
		S.Tunables.GuideSources.bStand = Case.bStandColumn;
		ARoadNetworkActor* Actor = S.TestWorld.Actor;

		StandGridFixture::LayTaxiway(Actor, FVector2D(-20000.0, 0.0), FVector2D(0.0, 0.0));
		// A LAYOUT THAT PREDATES THE STRIP (stage 3, 2026-09-29): this test is about the guides over it, not about laying it - see TestTool::ConnectUnjudged.
		StandGridFixture::LayTaxiwayUnjudged(Actor, FVector2D(1234.0, -730.0), FVector2D(20000.0, -730.0));

		// Stand 1, with the grid off whatever the case, so its edge is the same in every case.
		const EGridStep Asked = S.Tunables.GuideSources.GridStep;
		S.Tunables.GuideSources.GridStep = EGridStep::Off;
		S.Click(FVector2D(-5500.0, 1000.0));
		const FVector2D Anchor1 = S.Corner(0, FVector2D(-5500.0, 1000.0));
		S.Click(Anchor1 + FVector2D(Width, 0.0));
		S.Click(Anchor1 + FVector2D(Width, Depth));
		S.Tool()->OnCommit(S.At(Anchor1));
		const double Back1 = StandGridFixture::BackEdgeY(Actor);
		if (!TestTrue(*FString::Printf(TEXT("%s: stand 1 placed"), Case.Name), Back1 > 0.0)) { return false; }
		S.Tunables.GuideSources.GridStep = Asked;

		// Stand 2, off the second taxiway, whose kerb line is 730 uu further south.
		S.Click(FVector2D(1500.0, 270.0));
		const FVector2D Anchor2 = S.Corner(0, FVector2D(1500.0, 270.0));
		// ON THE SECOND TAXIWAY'S KERB, 730 uu south of the first's - the offset the report is
		// about. Not "Y < 0": a taxiway's half-width is more than 7.3 m, so both kerbs are north
		// of the origin.
		if (!TestTrue(*FString::Printf(TEXT("%s: stand 2 anchored on the second taxiway's kerb (%.1f vs %.1f)"),
			Case.Name, Anchor2.Y, Anchor1.Y), FMath::IsNearlyEqual(Anchor2.Y, Anchor1.Y - 730.0, 1e-6))) { return false; }
		S.Click(Anchor2 + FVector2D(Width, 0.0));
		const FVector2D Far2 = S.Corner(1, Anchor2);
		const FVector2D DepthCursor(Far2.X, Back1 - 150.0);
		const double Back2 = S.Corner(2, DepthCursor).Y;

		if (Case.bExpectFlush)
		{
			TestTrue(*FString::Printf(TEXT("%s: stand 2's back edge is ON stand 1's (%.6f vs %.6f)"),
				Case.Name, Back2, Back1), FMath::IsNearlyEqual(Back2, Back1, 1e-6));
		}
		else
		{
			TestTrue(*FString::Printf(TEXT("%s: without the guide it is NOT (%.3f vs %.3f)"),
				Case.Name, Back2, Back1), FMath::Abs(Back2 - Back1) > 1.0);
		}
	}
	return true;
}

/**
 * NO NEIGHBOUR, GRID 5 M: anchor and frontage on world X lines, back edge on a world Y line -
 * the WORLD's, not the kerb's, which sits a road half-width off any grid line.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandGridOnWorldLinesTest,
	"Airside.Tool.StandGrid.CornersOnWorldLines",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandGridOnWorldLinesTest::RunTest(const FString& Parameters)
{
	StandGridFixture::FStandSession S;
	if (!TestTrue(TEXT("stand tool, 5 m"), StandGridFixture::Begin(S, EGridStep::FiveMetres))) { return false; }
	// A CODE C TAXIWAY, since the clearance strip (2026-09-28): a stand's entrance now sits half
	// the letter's span plus its clearance off the centreline, whatever the pavement width -
	// 40 m for the default E, a whole 5 m step, which would put the control below ON a line.
	// C's is 22.5 m, off every 5 m line, so the control still discriminates.
	S.TestWorld.Actor->Profile = URoadProfile::MakeTransient(1600.0, 1067.0);
	StandGridFixture::LayTaxiway(S.TestWorld.Actor, FVector2D(-10000.0, 0.0), FVector2D(10000.0, 0.0));

	TestEqual(TEXT("idle: the stand tool asks for the grid itself"), S.At(FVector2D(1234.0, 1000.0)).GridFrame.StepUu, 500.0);

	S.Click(FVector2D(1234.0, 1000.0));
	const FVector2D Anchor = S.Corner(0, FVector2D(1234.0, 1000.0));
	TestTrue(TEXT("anchor on a world X line"), StandGridFixture::IsOnGridLine(Anchor.X, 500.0));

	const FVector2D FarCursor = Anchor + FVector2D(4321.0, 0.0);
	const FVector2D Far = S.Corner(1, FarCursor);
	TestTrue(TEXT("frontage end on a world X line"), StandGridFixture::IsOnGridLine(Far.X, 500.0));
	S.Click(FarCursor);

	const FVector2D Back = S.Corner(2, Far + FVector2D(0.0, 3777.0));
	TestTrue(TEXT("back corner on a world Y line"), StandGridFixture::IsOnGridLine(Back.Y, 500.0));
	TestFalse(TEXT("which the kerb-relative 0.5 m step would not have given (control)"),
		StandGridFixture::IsOnGridLine(Anchor.Y, 500.0));
	return true;
}

/**
 * TWO SEGMENTS, TWO BAY-GRID PHASES, ONE WORLD GRID. A road split at X = 1234: with the grid off
 * the second segment's anchors sit on 1234 + k * 500 (its own phase, the report's cause); on,
 * both segments' anchors are world multiples of 500.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandGridAnchorPhaseTest,
	"Airside.Tool.StandGrid.AnchorIgnoresSegmentPhase",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandGridAnchorPhaseTest::RunTest(const FString& Parameters)
{
	StandGridFixture::FStandSession S;
	if (!TestTrue(TEXT("stand tool"), StandGridFixture::Begin(S, EGridStep::Off))) { return false; }
	ARoadNetworkActor* Actor = S.TestWorld.Actor;
	StandGridFixture::LayTaxiway(Actor, FVector2D(-10000.0, 0.0), FVector2D(1234.0, 0.0));
	// A SEPARATE NODE at 1234, as it always was - the phase is the point - so the second piece
	// is not joined to the first and sits in its strip.
	// A LAYOUT THAT PREDATES THE STRIP (stage 3, 2026-09-29): this test is about the guides over it, not about laying it - see TestTool::ConnectUnjudged.
	StandGridFixture::LayTaxiwayUnjudged(Actor, FVector2D(1234.0, 0.0), FVector2D(10000.0, 0.0));

	const URoadNetwork& Network = *Actor->Network;
	auto Taxiways = [](const URoadNetwork& N, FRoadSegmentId Id) { return PlotGesture::IsTaxiway(N, Id); };
	// AT THE KERB: these pin the grid along the frontage, not the strip - 0 states that.
	auto AtKerb = [](const URoadNetwork&, FRoadSegmentId) { return 0.0; };
	const FVector2D OnSecond(4100.0, 1000.0);

	PlotGesture::FAnchor Off;
	if (!TestTrue(TEXT("grid off anchors"), PlotGesture::AnchorAt(Network, OnSecond, Taxiways, AtKerb, Off))) { return false; }
	TestFalse(TEXT("control: grid off, the second segment's own phase is off the world grid"),
		StandGridFixture::IsOnGridLine(Off.Corner.X, 500.0));

	PlotGesture::FAnchor On;
	if (!TestTrue(TEXT("grid on anchors"), PlotGesture::AnchorAt(Network, OnSecond, Taxiways, AtKerb, On, GridSnap::FGridFrame::World(500.0)))) { return false; }
	TestTrue(TEXT("grid on: a world X line"), StandGridFixture::IsOnGridLine(On.Corner.X, 500.0));
	TestEqual(TEXT("and still off the kerb, as the bay grid was"), On.Corner.Y, Off.Corner.Y);
	return true;
}

/**
 * NEAR A SEGMENT'S END THE ANCHOR TAKES THE LAST LINE ON THE ROAD rather than refusing (review,
 * 2026-09-27), and a segment shorter than a step, holding no line, refuses.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandGridAnchorNearEndTest,
	"Airside.Tool.StandGrid.AnchorNearEndStaysOnRoad",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandGridAnchorNearEndTest::RunTest(const FString& Parameters)
{
	StandGridFixture::FStandSession S;
	if (!TestTrue(TEXT("stand tool"), StandGridFixture::Begin(S, EGridStep::Off))) { return false; }
	StandGridFixture::LayTaxiway(S.TestWorld.Actor, FVector2D(0.0, 0.0), FVector2D(9700.0, 0.0));
	// A LAYOUT THAT PREDATES THE STRIP (stage 3, 2026-09-29): this test is about the guides over it, not about laying it - see TestTool::ConnectUnjudged.
	StandGridFixture::LayTaxiwayUnjudged(S.TestWorld.Actor, FVector2D(100.0, 5000.0), FVector2D(900.0, 5000.0));

	const URoadNetwork& Network = *S.TestWorld.Actor->Network;
	auto Taxiways = [](const URoadNetwork& N, FRoadSegmentId Id) { return PlotGesture::IsTaxiway(N, Id); };
	// AT THE KERB: these pin the grid along the frontage, not the strip - 0 states that.
	auto AtKerb = [](const URoadNetwork&, FRoadSegmentId) { return 0.0; };

	PlotGesture::FAnchor Anchor;
	if (!TestTrue(TEXT("10 m grid: the nearest line (10000) is past the end, the anchor still takes one"),
		PlotGesture::AnchorAt(Network, FVector2D(9900.0, 1000.0), Taxiways, AtKerb, Anchor, GridSnap::FGridFrame::World(1000.0)))) { return false; }
	TestTrue(TEXT("the last line on the road, X = 9000"), FMath::IsNearlyEqual(Anchor.Corner.X, 9000.0, 1e-6));

	TestFalse(TEXT("an 8 m segment between two 10 m lines has no anchor"),
		PlotGesture::AnchorAt(Network, FVector2D(500.0, 6000.0), Taxiways, AtKerb, Anchor, GridSnap::FGridFrame::World(1000.0)));
	return true;
}

/**
 * GRID ON KEEPS THE FRONTAGE FLOOR: a cursor a few metres from the anchor, which used to round
 * to the anchor's own line and pin a zero-length entrance (review, 2026-09-27).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandGridFrontageFloorTest,
	"Airside.Tool.StandGrid.FrontageKeepsItsFloor",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandGridFrontageFloorTest::RunTest(const FString& Parameters)
{
	StandGridFixture::FStandSession S;
	if (!TestTrue(TEXT("stand tool, 5 m"), StandGridFixture::Begin(S, EGridStep::FiveMetres))) { return false; }
	StandGridFixture::LayTaxiway(S.TestWorld.Actor, FVector2D(-10000.0, 0.0), FVector2D(10000.0, 0.0));

	S.Click(FVector2D(1000.0, 1000.0));
	const FVector2D Anchor = S.Corner(0, FVector2D(1000.0, 1000.0));
	for (const double Reach : { 100.0, -100.0, 0.0 })
	{
		const FVector2D Far = S.Corner(1, Anchor + FVector2D(Reach, 0.0));
		TestTrue(*FString::Printf(TEXT("reach %.0f: entrance at least the floor (%.0f)"), Reach, FMath::Abs(Far.X - Anchor.X)),
			FMath::Abs(Far.X - Anchor.X) >= PlotGesture::MinFrontageUu - 1e-6);
		TestTrue(*FString::Printf(TEXT("reach %.0f: and on a world line"), Reach), StandGridFixture::IsOnGridLine(Far.X, 500.0));
	}
	return true;
}

/**
 * GRID OFF, THE DEPTH IGNORES ANGULAR GUIDES. A cursor 3 degrees off the entrance line is within
 * "along the entrance"'s angular tolerance; followed, it projected the depth to zero (review,
 * 2026-09-27). The depth must be the raw 0.5 m step, as before the stand had a guide anchor.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandGridDepthIgnoresAngularTest,
	"Airside.Tool.StandGrid.DepthIgnoresAngularGuides",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandGridDepthIgnoresAngularTest::RunTest(const FString& Parameters)
{
	StandGridFixture::FStandSession S;
	if (!TestTrue(TEXT("stand tool, off"), StandGridFixture::Begin(S, EGridStep::Off))) { return false; }
	S.Tunables.GuideSources.bExtending = true;
	S.Tunables.GuideSources.bParallel = true;
	S.Tunables.GuideSources.bWorld = true;
	StandGridFixture::LayTaxiway(S.TestWorld.Actor, FVector2D(-10000.0, 0.0), FVector2D(10000.0, 0.0));

	S.Click(FVector2D(1000.0, 1000.0));
	const FVector2D Anchor = S.Corner(0, FVector2D(1000.0, 1000.0));
	const double Width = StandGridFixture::CodeCWidth();
	S.Click(Anchor + FVector2D(Width, 0.0));
	const FVector2D Far = S.Corner(1, Anchor);

	const FVector2D Cursor = Far + FVector2D(2000.0, 100.0);
	TestTrue(TEXT("precondition: an angular guide holds here"), S.At(Cursor).Guide.Of(SnapGuide::EFit::Angular) != nullptr);
	const double Depth = S.Corner(2, Cursor).Y - Far.Y;
	TestTrue(*FString::Printf(TEXT("depth is the cursor's 1 m, not zero (%.3f)"), Depth), FMath::IsNearlyEqual(Depth, 100.0, 1e-6));
	return true;
}

/**
 * A DIAGONAL TAXIWAY UNDER FOLLOW - grid-follows-snap design, the report that asked for it. The
 * stand tool names its taxiway (DescribeGridLine), so the grid lies along it from the first
 * hover: anchor and frontage on the cross lines, back edge a whole number of steps off the
 * centreline and parallel to it. With World the same clicks land on world lines instead.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandGridDiagonalFollowsTest,
	"Airside.Tool.StandGrid.DiagonalTaxiwayFollows",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandGridDiagonalFollowsTest::RunTest(const FString& Parameters)
{
	const FVector2D Dir(FMath::Cos(PI / 6.0), FMath::Sin(PI / 6.0));
	const FVector2D Perp(-Dir.Y, Dir.X);
	const FVector2D RoadA = Dir * -10000.0 + Perp * 1234.0;

	StandGridFixture::FStandSession S;
	if (!TestTrue(TEXT("stand tool, 5 m"), StandGridFixture::Begin(S, EGridStep::FiveMetres))) { return false; }
	S.Tunables.GuideSources.GridOrientation = EGridOrientation::Follow;
	// A CODE C TAXIWAY, since the clearance strip (2026-09-28): a stand's entrance now sits half
	// the letter's span plus its clearance off the centreline, whatever the pavement width -
	// 40 m for the default E, a whole 5 m step, which would put the control below ON a line.
	// C's is 22.5 m, off every 5 m line, so the control still discriminates.
	S.TestWorld.Actor->Profile = URoadProfile::MakeTransient(1600.0, 1067.0);
	StandGridFixture::LayTaxiway(S.TestWorld.Actor, RoadA, RoadA + Dir * 20000.0);

	const FVector2D Hover = RoadA + Dir * 11234.0 + Perp * 1000.0;
	const FToolContext Idle = S.At(Hover);
	TestTrue(TEXT("idle: the grid already lies along the taxiway"), FMath::IsNearlyEqual(Idle.GridFrame.AxisDegrees(), 30.0, 1e-6));

	// Along the road, measured from the world origin's cross line; across it, from the centreline.
	auto AlongOf = [&Dir](const FVector2D& P) { return FVector2D::DotProduct(P, Dir); };
	auto OffCentre = [&Perp, &RoadA](const FVector2D& P) { return FVector2D::DotProduct(P - RoadA, Perp); };

	S.Click(Hover);
	const FVector2D Anchor = S.Corner(0, Hover);
	TestTrue(TEXT("anchor on a cross line"), StandGridFixture::IsOnGridLine(AlongOf(Anchor), 500.0));

	const FVector2D FarCursor = Anchor + Dir * 4321.0;
	const FVector2D Far = S.Corner(1, FarCursor);
	TestTrue(TEXT("frontage end on a cross line"), StandGridFixture::IsOnGridLine(AlongOf(Far), 500.0));
	S.Click(FarCursor);

	const FVector2D Back = S.Corner(2, Far + Perp * 3777.0);
	TestTrue(TEXT("back corner a whole number of steps off the centreline"), StandGridFixture::IsOnGridLine(OffCentre(Back), 500.0));
	TestTrue(TEXT("the back edge runs along the taxiway"), FMath::Abs(FVector2D::DotProduct(Back - Far, Dir)) < 1e-6);
	TestFalse(TEXT("control: the kerb is not on a line - the half-width is not whole steps"),
		StandGridFixture::IsOnGridLine(OffCentre(Anchor), 500.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandGridNearerTaxiwayWinsTest,
	"Airside.Tool.StandGrid.NearerTaxiwayWinsInsideTwoStrips",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandGridNearerTaxiwayWinsTest::RunTest(const FString& Parameters)
{
	// A JUNCTION: the cursor stands inside BOTH taxiways' strips, 3 m from one centreline and
	// 5 m from the other. The nearer one must win - a reach counted from the frontage and
	// clamped at zero called both "0 away" and let array order decide (review, 2026-09-28).
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FRoadSegmentId Near = Net->AddSegment(Net->AddNode(FVector2D(-10000.0, 0.0)), Net->AddNode(FVector2D(10000.0, 0.0)),
		FVector2D::ZeroVector, URoadProfile::MakeTransient(2400.0, 1600.0));
	Net->AddSegment(Net->AddNode(FVector2D(6000.0, -10000.0)), Net->AddNode(FVector2D(6000.0, 10000.0)),
		FVector2D(6000.0, 0.0), URoadProfile::MakeTransient(2400.0, 1600.0));
	auto Taxiways = [](const URoadNetwork& N, FRoadSegmentId Id) { return PlotGesture::IsTaxiway(N, Id); };
	auto Strip = [](const URoadNetwork& N, FRoadSegmentId Id) { return TaxiwayStrip::StripWidthOf(N, Id); };

	FRoadSegmentId Road;
	double T = 0.0;
	if (!TestTrue(TEXT("a road is found"), PlotGesture::NearestRoad(*Net, FVector2D(5500.0, 300.0), Taxiways, Strip, Road, T))) { return false; }
	TestTrue(TEXT("the nearer taxiway wins, whatever order they were laid in"), Road == Near);
	return true;
}

#endif
