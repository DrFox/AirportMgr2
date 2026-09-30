#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Entities/EntityDefinition.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Solve/IcaoCode.h"
#include "Testing/AirsideTestWorld.h"
#include "Tool/BuildSession.h"
#include "Tool/SelectTool.h"
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

namespace
{
	/** Counts what FBuildSession::OnSelectionChanged says, and keeps the last Old and New. Prefixed for the unity build. */
	struct FSessionSelectionSpy
	{
		int32 Count = 0;
		FSelection Old;
		FSelection New;

		void Listen(FBuildSession& Session)
		{
			Session.OnSelectionChanged().AddLambda([this](const FSelection& InOld, const FSelection& InNew)
			{
				++Count;
				Old = InOld;
				New = InNew;
			});
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildSessionSelectionChangedTest,
	"Airside.Tool.BuildSession.SelectionChangedFiresOncePerChange",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildSessionSelectionChangedTest::RunTest(const FString& Parameters)
{
	// #446 PIN: the inspector learned of a selection change by comparing (Kind, Id) with the last it had seen, every tick. The session announces
	// it now - ONCE per real change, from EVERY writer, and never for a re-select of what is already selected. Each writer is exercised: a
	// code select, the same select again, a deselect, a tool's click and cancel through the CONTEXT'S door, a build tool being lit, and a replaced network.
	FBuildSession Session;
	FSessionSelectionSpy Spy;
	Spy.Listen(Session);

	Session.Select(ESelectionKind::Stand, 3);
	TestEqual(TEXT("selecting from code is one change"), Spy.Count, 1);
	TestEqual(TEXT("it says what it was - nothing"), Spy.Old.Kind, ESelectionKind::None);
	TestEqual(TEXT("and what it is - the stand"), Spy.New.Kind, ESelectionKind::Stand);
	TestEqual(TEXT("with its id"), Spy.New.Id, 3);

	Session.Select(ESelectionKind::Stand, 3);
	TestEqual(TEXT("selecting what is ALREADY selected is not a change"), Spy.Count, 1);

	Session.Select(ESelectionKind::Stand, 4);
	TestEqual(TEXT("another stand is a change"), Spy.Count, 2);
	TestEqual(TEXT("which names the one it replaced"), Spy.Old.Id, 3);

	Session.Select(ESelectionKind::Runway, 4);
	TestEqual(TEXT("the same id under another KIND is a change (the kind decides what the number means)"), Spy.Count, 3);

	Session.Select(ESelectionKind::None, 9);
	TestEqual(TEXT("deselecting is a change"), Spy.Count, 4);
	TestEqual(TEXT("and nothing carries no id, however the caller spelled it"), Session.GetSelection().Id, 0);
	Session.Select(ESelectionKind::None, 0);
	TestEqual(TEXT("deselecting nothing is not"), Spy.Count, 4);

	// THE TOOLS WRITE THROUGH THE CONTEXT MakeContext HANDS THEM: an unwired door would write the selection and announce nothing - the
	// very silence this seam replaces - with every assertion above still green.
	const FToolContext Context = Session.MakeContext(nullptr, FVector2D::ZeroVector, FBuildSessionTunables());
	TestTrue(TEXT("the context's door is the session's own announcement"), Context.OnSelectionChanged == &Session.OnSelectionChanged());
	FSelectTool Select;
	FToolContext Click = Context;
	Click.HoverAgent = 7;
	Select.OnClick(Click);
	TestEqual(TEXT("a click on an aircraft is a change"), Spy.Count, 5);
	TestEqual(TEXT("to that aircraft"), Spy.New.Id, 7);
	Select.OnClick(Click);
	TestEqual(TEXT("a second click on the same aircraft is not"), Spy.Count, 5);
	Select.OnCancel(Context);
	TestEqual(TEXT("the tool's cancel deselects, once"), Spy.Count, 6);
	Select.OnCancel(Context);
	TestEqual(TEXT("and cancelling with nothing selected announces nothing"), Spy.Count, 6);

	// LIGHTING A BUILD TOOL CLOSES THE SELECTION (FBuildSession::SelectTool) - a write the tools do not make.
	Session.Select(ESelectionKind::Aircraft, 2);
	TestEqual(TEXT("setup: selected again"), Spy.Count, 7);
	Session.SelectTool(1);
	TestEqual(TEXT("lighting a build tool deselects, and says so"), Spy.Count, 8);
	TestFalse(TEXT("the selection is gone"), Session.GetSelection().IsSet());

	// A NETWORK WITH NO HISTORY IN COMMON clears it (Discarding); an undo's Adopted leaves a selection its network still holds.
	Session.Select(ESelectionKind::Aircraft, 2);
	Session.OnNetworkReplaced(Context, ENetworkReplace::Adopted);
	TestTrue(TEXT("an aircraft's selection survives an Adopted replacement - an agent id is not a slot"), Session.GetSelection().IsSet());
	Session.OnNetworkReplaced(Context, ENetworkReplace::Discarding);
	TestFalse(TEXT("a Discarding replacement clears it"), Session.GetSelection().IsSet());
	TestEqual(TEXT("once"), Spy.Count, 10);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildSessionContextsShareTheDoorTest,
	"Airside.Tool.BuildSession.EveryContextCarriesTheSelectionDoor",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildSessionContextsShareTheDoorTest::RunTest(const FString& Parameters)
{
	// BOTH DRIVERS BUILD THEIR CONTEXTS THROUGH THE SESSION (PIE's MakeToolContext, the editor's GetFrameContext), so the door rides on every one
	// of them - the cached frame context included. A context that carried the selection pointer and not the announcement would let a
	// tool change the selection silently, in one driver only (#446, and rule 47's shape: the drivers must not differ).
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor"), TestWorld.Actor)) { return false; }
	FBuildSession Session;
	FSessionSelectionSpy Spy;
	Spy.Listen(Session);

	const FBuildSessionTunables Tunables;
	const FBuildInputState Input;
	const FToolContext Frame = Session.GetFrameContext(TestWorld.Actor, FVector2D(10.0, 20.0), Tunables, Input);
	TestTrue(TEXT("a frame context carries the session's selection"), Frame.Selection == &Session.GetSelection());
	TestTrue(TEXT("and its announcement"), Frame.OnSelectionChanged == &Session.OnSelectionChanged());
	TestTrue(TEXT("a write through it is heard"), Frame.SetSelection(FSelectTool::MakeSelection(TestWorld.Actor->GetNetwork(), ESelectionKind::Aircraft, 5)));
	TestEqual(TEXT("once"), Spy.Count, 1);

	const FToolContext Fresh = Session.MakeContext(TestWorld.Actor, FVector2D::ZeroVector, Tunables, Input);
	TestTrue(TEXT("a fresh context carries it too"), Fresh.OnSelectionChanged == &Session.OnSelectionChanged());

	// A TEST'S OWN CONTEXT, with a bare FSelection and no session, still writes - and has nobody to tell.
	FSelection Bare;
	FToolContext Loose;
	Loose.Selection = &Bare;
	TestTrue(TEXT("a context with no announcement still writes the selection"), Loose.SetSelection(FSelectTool::MakeSelection(nullptr, ESelectionKind::Aircraft, 6)));
	TestEqual(TEXT("into the FSelection it points at"), Bare.Id, 6);
	FToolContext Nowhere;
	TestFalse(TEXT("and one with no selection at all writes nothing"), Nowhere.SetSelection(FSelectTool::MakeSelection(nullptr, ESelectionKind::Aircraft, 6)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildSessionStaleSelectionDropTest,
	"Airside.Tool.BuildSession.AdoptedNetworkDropsAStaleSelection",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildSessionStaleSelectionDropTest::RunTest(const FString& Parameters)
{
	// #446 PIN: select a stand, undo its placement, and the selection clears rather than retargeting. The undo is a Memento restore of the
	// network - here RestoreFrom a snapshot taken before the stand existed, which is exactly what an undo of the placement does - and the
	// session's answer to it is OnNetworkReplaced(Adopted). Before #446 that phase left the selection alone and the select tool's NEXT
	// Tick cleared it, so a stand placed into the freed slot before that tick was the selection.
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("an actor"), Actor)) { return false; }
	Actor->PlaceNode(FVector2D(0.0, 40000.0));
	URoadNetwork& Net = *Actor->Network;
	URoadNetwork* Before = DuplicateObject<URoadNetwork>(&Net, GetTransientPackage());
	if (!TestNotNull(TEXT("a snapshot of the network before the placement"), Before)) { return false; }

	UEntityDefinition* StandDef = UEntityDefinition::MakeStandTransient();
	const FEntityInstanceId Stand = Net.PlaceEntity(StandDef, StandDef->Anchors, FVector2D(5000.0, 0.0), 0.0);
	if (!TestTrue(TEXT("setup: a stand placed"), Stand.IsSet())) { return false; }

	FBuildSession Session;
	FSessionSelectionSpy Spy;
	Spy.Listen(Session);
	Session.Select(ESelectionKind::Stand, Stand.Index, &Net);
	if (!TestEqual(TEXT("setup: the stand is selected, with its slot's generation"), Session.GetSelection().Generation, Stand.Generation)) { return false; }
	const FToolContext Context = Session.MakeContext(Actor, FVector2D::ZeroVector, FBuildSessionTunables());

	Session.OnNetworkReplaced(Context, ENetworkReplace::Adopted);
	TestTrue(TEXT("an Adopted replacement that touched nothing selected leaves the selection"), Session.GetSelection().IsSet());
	TestEqual(TEXT("and announces nothing"), Spy.Count, 1);

	Net.RestoreFrom(*Before);
	Session.OnNetworkReplaced(Context, ENetworkReplace::Adopted);
	TestFalse(TEXT("the undo of the placement took the stand's slot: the selection is dropped, NOT retargeted"), Session.GetSelection().IsSet());
	TestEqual(TEXT("and dropped once, announced"), Spy.Count, 2);
	return true;
}

#endif
