#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Entities/EntityDefinition.h"
#include "Present/RoadNetworkActor.h"
#include "Solve/IcaoCode.h"
#include "Testing/AirsideTestWorld.h"
#include "Tool/BuildSession.h"
#include "Tool/RoadEditTarget.h"
#include "Tool/SnapGuideChain.h"

#if WITH_DEV_AUTOMATION_TESTS

// A LEAF NAME, not the bare "Airside.Tool.BuildSession" this used to be: UE 5.8's automation tree turns a bare name
// into a GROUP node the moment a dotted child registers and silently drops its own RunTest. The bare name never ran
// after SelectFromCode (#413, 2026-09-29) registered under it, and three of its checks (registry keys unique,
// per-index display names, RecordPlaneHit) are covered by nothing else - the tool sources, BuildVerbRegistryTest and
// FreeStartGuideTest cite this test by name as their pin. Check-Architecture rule 42 (test-name-prefix) now fails
// on the shape; see memory unreal-automation-test-tree-drops-bare-parent.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildSessionTest,
	"Airside.Tool.BuildSession.RegistryAndSession",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildSessionTest::RunTest(const FString& Parameters)
{
	const TConstArrayView<FToolRegistration> Registry = ToolRegistry();

	// 1. Every key reachable, and every key reaching exactly one tool - the two failure
	// modes CLAUDE.md records this project has shipped: a tool with no key, and (had two
	// entries shared a key) a key whose meaning depended on registration order.
	for (int32 Outer = 0; Outer < Registry.Num(); ++Outer)
	{
		for (int32 Inner = Outer + 1; Inner < Registry.Num(); ++Inner)
		{
			TestFalse(
				FString::Printf(TEXT("registry entries %d and %d must not share a key"), Outer, Inner),
				Registry[Outer].Key == Registry[Inner].Key);
		}
	}

	// 2. FBuildSession is built FROM the registry, not a second list that merely happens to
	// match it today.
	FBuildSession Session;
	TestEqual(TEXT("a session holds exactly as many tools as the registry lists"),
		Session.NumTools(), Registry.Num());
	TestEqual(TEXT("a fresh session opens in registry index 0 (Select)"), Session.GetActiveToolIndex(), 0);

	// 3. Selecting index i yields the tool the registry SAYS lives at i, by name - the
	// check that would have caught the editor module carrying a shorter, silently
	// re-numbered copy of this list before issue #33.
	for (int32 Index = 0; Index < Registry.Num(); ++Index)
	{
		Session.SelectTool(Index);
		const IBuildTool* Active = Session.GetActiveTool();
		if (!TestNotNull(FString::Printf(TEXT("index %d selects a real tool"), Index), Active))
		{
			continue;
		}

		TestTrue(
			FString::Printf(TEXT("tool %d's own display name agrees with its registration"), Index),
			Active->GetDisplayName().EqualTo(Registry[Index].Name));
	}

	// 4. RecordPlaneHit/LastPlaneHit - the one shared home for the fallback that used to be
	// ARoadBuildController::LastPlaneHit and URoadBuildEditorTool::HoverPosition separately
	// (issue #92). Pinned directly rather than only through MakeContext's fallback branch,
	// so a future change to that branch cannot stop exercising the pair without a test
	// noticing.
	{
		FBuildSession PlaneHitSession;
		TestTrue(TEXT("a fresh session's last plane hit is the origin"),
			PlaneHitSession.LastPlaneHit().Equals(FVector2D::ZeroVector, 1e-6));

		PlaneHitSession.RecordPlaneHit(FVector2D(1234.0, -500.0));
		TestTrue(TEXT("RecordPlaneHit's value reads back exactly"),
			PlaneHitSession.LastPlaneHit().Equals(FVector2D(1234.0, -500.0), 1e-6));
	}

	// 5. WHICH TOOLS GUIDE A GESTURE THAT HAS NOT STARTED, and it is FOUR of the nine - ruled
	// 2026-09-20: "a lot of the tools would benefit from snapping to guides before the first
	// place of the road. It doesnt make sense for all of them, but some it does."
	//
	// FIVE UNTIL 2026-09-23: the stand was one, for lining a row of stands up by their stop
	// marks. The drawn stand (FStandPlotTool) anchors its first click on a taxiway's step grid,
	// as the depot does on a service road's, so a free-start guide there would be drawn and
	// then not obeyed - the stand left this list for the reason the depot was never on it.
	//
	// THE LIST IS WRITTEN HERE, BY ID, AND NOT ASKED OF THE TOOLS. Walking the registry and
	// comparing each tool's WantsFreeStartGuides() against itself would pass however the
	// virtual was answered - the vacuous shape this file's item 2 exists to avoid. Naming the
	// four is what makes a fifth tool opting in, or one of these quietly dropping out, a
	// failure rather than a change nobody notices.
	{
		const TSet<FName> FreeStarts = {
			FName(TEXT("Taxiway")),   // in line with an existing one, or a gap from a pair
			FName(TEXT("Apron")),     // an outline started flush with a road edge
			FName(TEXT("Runway")),    // a threshold in line with another runway
			FName(TEXT("Road")),      // the same argument as Taxiway; one class, two entries
		};

		FBuildSession IdleSession;
		for (int32 Index = 0; Index < Registry.Num(); ++Index)
		{
			IdleSession.SelectTool(Index);
			const IBuildTool* Active = IdleSession.GetActiveTool();
			if (Active == nullptr) { continue; }

			const bool bExpected = FreeStarts.Contains(Registry[Index].Id);
			TestEqual(
				*FString::Printf(TEXT("'%s' answers the free-start question as ruled"),
					*Registry[Index].Id.ToString()),
				Active->WantsFreeStartGuides(), bExpected);

			// AND THE ANSWER REACHES THE ANCHOR. The virtual is a declaration; this is the
			// behaviour, and the two are only connected because every opted-in override
			// DELEGATES to the base instead of returning false. An override that forgot lands
			// exactly here - see IBuildTool::DescribeGuideAnchor.
			//
			// NULL NETWORK AND NULL TARGET ON PURPOSE: the first click of a session has
			// neither, and a tool must answer this without looking at either.
			FGuideAnchor Anchor;
			const bool bOffered = Active->DescribeGuideAnchor(nullptr, nullptr, Anchor);
			TestEqual(
				*FString::Printf(TEXT("'%s' offers an idle anchor exactly when it opted in"),
					*Registry[Index].Id.ToString()),
				bOffered, bExpected);

			// FLAGGED, because the flag is what FBuildSession::MakeContext branches on to put
			// the cursor in Origin. An anchor offered without it would swing around (0,0).
			TestEqual(
				*FString::Printf(TEXT("'%s' marks that idle anchor as a free start"),
					*Registry[Index].Id.ToString()),
				Anchor.bFreeStart, bExpected);
		}
	}

	return true;
}

/**
 * EVERY REGISTERED TOOL HAS A CLEARANCE-STRIP DECISION (strip stage 3; spec: "a test
 * enumerates the placement tools and asserts each one's preview refuses a footprint inside a
 * strip, so a new tool that forgets the query goes red"). LISTS THAT MUST AGREE: the table
 * below and ToolRegistry() are checked by NAME, both ways - a registration missing from the
 * table fails naming it, so no tool ships without a ruling; a table row naming no registered
 * tool fails too, so a renamed tool cannot leave a stale row passing for it.
 *
 * Each Judged row then asks ITS evaluator - the one the tool's readout and the facade's commit
 * both call (WhySegmentRefused for Taxiway/Road, WhyStandRefused for Stand, WhyPlotRefused for
 * FuelDepot) - to refuse a footprint 20 m off a 24 m taxiway, and to pass a control 60 m off.
 * The tool-level wiring of each is Airside.Tool.RoadRefusedInsideStrip,
 * Airside.Present.StandPlot.RefusedInsideStrip and Airside.Tool.PlotPlace.RefusedInsideStrip.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEveryPlacementToolHonoursTheStripTest,
	"Airside.Tool.EveryPlacementToolHonoursTheStrip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FEveryPlacementToolHonoursTheStripTest::RunTest(const FString& Parameters)
{
	enum class EStripCoverage : uint8 { Judged, ExemptApron, ExemptRunway, PlacesNoPavement };
	struct FRow { const TCHAR* Id; EStripCoverage Coverage; };
	const FRow Table[] = {
		{ TEXT("Taxiway"),         EStripCoverage::Judged },            // Task 4
		{ TEXT("Road"),            EStripCoverage::Judged },            // Task 4
		{ TEXT("Stand"),           EStripCoverage::Judged },            // #390
		{ TEXT("FuelDepot"),       EStripCoverage::Judged },            // Task 5
		{ TEXT("Apron"),           EStripCoverage::ExemptApron },       // plan ruling 1: aircraft pavement
		{ TEXT("Runway"),          EStripCoverage::ExemptRunway },      // plan ruling 2: its own strip rules
		{ TEXT("Select"),          EStripCoverage::PlacesNoPavement },
		{ TEXT("Guideline"),       EStripCoverage::PlacesNoPavement },  // routing links on existing pavement
		{ TEXT("HoldingPosition"), EStripCoverage::PlacesNoPavement },  // a mark on an existing node
	};
	const auto Find = [&Table](FName Id) -> const FRow*
	{
		for (const FRow& Row : Table)
		{
			if (Id == FName(Row.Id)) { return &Row; }
		}
		return nullptr;
	};

	const TConstArrayView<FToolRegistration> Registry = ToolRegistry();
	for (const FToolRegistration& Tool : Registry)
	{
		TestNotNull(*FString::Printf(TEXT("registered tool '%s' has a clearance-strip decision in this table"),
			*Tool.Id.ToString()), Find(Tool.Id));
	}
	for (const FRow& Row : Table)
	{
		TestTrue(*FString::Printf(TEXT("table row '%s' names a registered tool"), Row.Id),
			Registry.ContainsByPredicate([&Row](const FToolRegistration& Tool) { return Tool.Id == FName(Row.Id); }));
	}

	// THE JUDGED ROWS, each through its own evaluator on one field: a 24 m taxiway along y 0,
	// keep-out 40 m.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	Actor->ClearNetwork();
	Actor->StandDefinition = UEntityDefinition::MakeStandTransient();
	IRoadEditTarget* Target = Actor;
	Target->ConnectNodes(Target->PlaceNode(FVector2D(-30000.0, 0.0)), Target->PlaceNode(FVector2D(30000.0, 0.0)),
		ERoadKind::Taxiway, INDEX_NONE);

	const auto SegmentWhy = [Actor, Target](ERoadKind Kind, double Y)
	{
		const int32 From = Target->PlaceNode(FVector2D(-5000.0, Y));
		FRoadSnapResult To;
		To.Position = FVector2D(5000.0, Y);
		return Target->WhySegmentRefused(From, To, Kind, INDEX_NONE);
	};
	const auto Box = [](double NearY, double Wd, double Dp)
	{
		return TArray<FVector2D>{ { 0.0, NearY }, { Wd, NearY }, { Wd, NearY + Dp }, { 0.0, NearY + Dp } };
	};
	const double StandWd = IcaoCode::StandWidthForLetter(EIcaoCode::C);
	const double StandDp = IcaoCode::StandDepthForLetter(EIcaoCode::C);

	for (const FRow& Row : Table)
	{
		if (Row.Coverage != EStripCoverage::Judged) { continue; }
		const FName Id(Row.Id);
		FString Inside, Clear;
		if (Id == FName(TEXT("Taxiway")))
		{
			// 60 m clears the far end's own 40 m strip against the existing one's pavement too.
			Inside = SegmentWhy(ERoadKind::Taxiway, 2000.0);
			Clear = SegmentWhy(ERoadKind::Taxiway, 6000.0);
		}
		else if (Id == FName(TEXT("Road")))
		{
			Inside = SegmentWhy(ERoadKind::ServiceRoad, 2000.0);
			Clear = SegmentWhy(ERoadKind::ServiceRoad, 6000.0);
		}
		else if (Id == FName(TEXT("Stand")))
		{
			Inside = Target->WhyStandRefused(Box(2000.0, StandWd, StandDp), EPavement::Tarmac);
			Clear = Target->WhyStandRefused(Box(6000.0, StandWd, StandDp), EPavement::Tarmac);
		}
		else if (Id == FName(TEXT("FuelDepot")))
		{
			Inside = Target->WhyPlotRefused(Box(2000.0, 2000.0, 1600.0));
			Clear = Target->WhyPlotRefused(Box(6000.0, 2000.0, 1600.0));
		}
		else
		{
			AddError(FString::Printf(TEXT("Judged row '%s' has no refusal case here - add one"), Row.Id));
			continue;
		}
		TestTrue(FString::Printf(TEXT("%s: 20 m off a taxiway is refused for the strip (said: %s)"), Row.Id, *Inside),
			Inside.Contains(TEXT("clearance strip")));
		TestFalse(FString::Printf(TEXT("%s: 60 m off is not refused for the strip (said: %s)"), Row.Id, *Clear),
			Clear.Contains(TEXT("clearance strip")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildSessionSelectFromCodeTest,
	"Airside.Tool.BuildSession.SelectFromCode",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildSessionSelectFromCodeTest::RunTest(const FString& Parameters)
{
	// AN ALERT'S "GO" SELECTS ITS SUBJECT (ops alerts spec 2026-09-29 §3) - the first writer of the
	// selection that is not the select tool's hover and click. The inspector shows whatever is selected.
	FBuildSession Session;
	TestFalse(TEXT("a fresh session selects nothing"), Session.GetSelection().IsSet());
	Session.Select(ESelectionKind::Stand, 3);
	TestEqual(TEXT("selected from code, the kind reads back"), Session.GetSelection().Kind, ESelectionKind::Stand);
	TestEqual(TEXT("and the id"), Session.GetSelection().Id, 3);
	Session.Select(ESelectionKind::None, 0);
	TestFalse(TEXT("and None clears it"), Session.GetSelection().IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildSessionSelectToolIndexTest,
	"Airside.Tool.BuildSession.SelectIsTheRegistryIndexTheSessionNames",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildSessionSelectToolIndexTest::RunTest(const FString& Parameters)
{
	// #448: "Select is registry index 0" was a bare 0 at several sites; FBuildSession::SelectToolIndex is the one name, and it is the
	// Select tool. A registry reordered so that index 0 is something else goes red HERE, not as a session that opens on a road tool.
	const TConstArrayView<FToolRegistration> Registry = ToolRegistry();
	if (!TestTrue(TEXT("the named index is in the registry"), Registry.IsValidIndex(FBuildSession::SelectToolIndex))) { return false; }
	TestEqual(TEXT("and it is the Select tool"), Registry[FBuildSession::SelectToolIndex].Id, FName(TEXT("Select")));

	FBuildSession Session;
	TestEqual(TEXT("a fresh session opens on it"), Session.GetActiveToolIndex(), FBuildSession::SelectToolIndex);
	Session.Select(ESelectionKind::Stand, 3);
	Session.SelectTool(1);
	TestFalse(TEXT("picking another tool closes the selection"), Session.GetSelection().IsSet());

	// THE TWO LEVELS OF A CANCEL, apart (#448): CancelStage abandons only a part-drawn stage and never puts the tool down; the right-click
	// cancel is CancelStage and then, with nothing to abandon, the put-down. An idle build tool has no stage.
	if (!TestEqual(TEXT("setup: a build tool is lit"), Session.GetActiveToolIndex(), 1)) { return false; }
	if (!TestTrue(TEXT("setup: and idle"), Session.GetActiveTool() != nullptr && Session.GetActiveTool()->IsIdle())) { return false; }
	TestFalse(TEXT("CancelStage on an idle tool abandons nothing"), Session.CancelStage(FToolContext()));
	TestEqual(TEXT("and leaves the tool lit - it is not a put-down"), Session.GetActiveToolIndex(), 1);
	Session.CancelActiveGesture(FToolContext());
	TestEqual(TEXT("the right-click cancel of an idle build tool returns to Select"), Session.GetActiveToolIndex(), FBuildSession::SelectToolIndex);
	return true;
}

#endif
