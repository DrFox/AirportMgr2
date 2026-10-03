#include "CoreMinimal.h"
#include "UIStyle.h"
#include "UI/UiMenuButton.h"
#include "Content/AirsideSettings.h"
#include "Testing/AirsideTestGraph.h"
#include "Profiles/RoadProfile.h"
#include "Present/RoadNetworkActor.h"
#include "Present/OpsRuntime.h"
#include "Present/AirsideTraffic.h"
#include "Model/RoadNetwork.h"
#include "Model/GroundTraffic.h"
#include "Model/FlightBoard.h"
#include "Model/Flight.h"
#include "Model/Airport.h"
#include "Model/DeparturePlanner.h"
#include "Model/InspectFacts.h"
#include "Model/OpsAlerts.h"
#include "Model/RoadGuideline.h"
#include "Model/RunwayFacts.h"
#include "Entities/EntityDefinition.h"
#include "AlertsPanelWidget.h"
#include "BuildActions.h"
#include "BuildBarWidget.h"
#include "LandAircraftPanelWidget.h"
#include "LedgerPanelWidget.h"
#include "OpsRuntimeResolver.h"
#include "Tool/SnapGuideSettings.h"
#include "Tool/SnapToggleRegistry.h"
#include "BuildHudLayer.h"
#include "Entities/EntityDefinition.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "PlayerSettings.h"
#include "SettingsPanelWidget.h"
#include "UI/UiWindowHost.h"
#include "Misc/AutomationTest.h"
#include "RoadBuildController.h"
#include "Testing/AirsideTestWorld.h"
#include "Tool/BuildSession.h"
#include "Tool/Selection.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildActionsRegistryTest,
	"AirportMgr.Actions.RegistryIsComplete",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildActionsRegistryTest::RunTest(const FString& Parameters)
{
	// THE ONE-LIST CHECK. Keys, banner and bar all read this table, so a defect here is a
	// key that goes nowhere or a button with no handler - the bug this project has shipped
	// three times through lists that were supposed to agree.
	const TConstArrayView<FBuildAction> Actions = BuildActions();
	TestTrue(TEXT("the registry is not empty"), Actions.Num() > 0);

	TSet<FName> Ids;
	TSet<FString> Chords;
	for (const FBuildAction& A : Actions)
	{
		TestFalse(*FString::Printf(TEXT("%s has an id"), *A.Id.ToString()), A.Id.IsNone());
		TestFalse(*FString::Printf(TEXT("%s has a label"), *A.Id.ToString()), A.Label.IsEmpty());
		TestTrue(*FString::Printf(TEXT("%s has an executor"), *A.Id.ToString()), static_cast<bool>(A.Execute));
		TestTrue(*FString::Printf(TEXT("%s has an IsActive query"), *A.Id.ToString()), static_cast<bool>(A.IsActive));
		TestTrue(*FString::Printf(TEXT("%s has an IsEnabled query"), *A.Id.ToString()), static_cast<bool>(A.IsEnabled));
		TestFalse(*FString::Printf(TEXT("%s id is unique"), *A.Id.ToString()), Ids.Contains(A.Id));
		Ids.Add(A.Id);
		if (A.Key.IsValid())
		{
			const FString Chord = FString::Printf(TEXT("%s%s"), A.bRequiresCtrl ? TEXT("Ctrl+") : TEXT(""), *A.Key.ToString());
			TestFalse(*FString::Printf(TEXT("%s key %s is unique"), *A.Id.ToString(), *Chord), Chords.Contains(Chord));
			Chords.Add(Chord);
		}
	}

	// Every registered tool is an action exactly once, in the Tools section, with its key.
	for (const FToolRegistration& Tool : ToolRegistry())
	{
		int32 Found = 0;
		for (const FBuildAction& A : Actions)
		{
			if (A.Section == EActionSection::Tools && A.Key == Tool.Key) { ++Found; }
		}
		TestEqual(*FString::Printf(TEXT("tool %s appears once as an action"), *Tool.Name.ToString()), Found, 1);
	}

	// The inspector's verbs are rows of THIS table, so the panel's buttons and the C key
	// cannot diverge. Depart is bar/panel only (no key); Follow took the old Watch key.
	TestTrue(TEXT("selection.depart is registered"), Actions.ContainsByPredicate([](const FBuildAction& A) { return A.Id == FName(TEXT("selection.depart")) && A.Section == EActionSection::Selection && !A.Key.IsValid(); }));
	TestTrue(TEXT("selection.follow is registered on C"), Actions.ContainsByPredicate([](const FBuildAction& A) { return A.Id == FName(TEXT("selection.follow")) && A.Key == EKeys::C; }));
	TestFalse(TEXT("aircraft.watch is gone - one verb for following"), Actions.ContainsByPredicate([](const FBuildAction& A) { return A.Id == FName(TEXT("aircraft.watch")); }));
	// The runway card's verb (spec 2026-09-28-runway-in-use): a Selection row, found by the
	// inspector BY ID, and like Depart it has no key - a key that reversed whatever runway was
	// selected is a misclick away from sending the next arrival the other way.
	TestTrue(TEXT("selection.runway_in_use is registered, keyless, in Selection"), Actions.ContainsByPredicate([](const FBuildAction& A)
		{ return A.Id == FName(TEXT("selection.runway_in_use")) && A.Section == EActionSection::Selection && !A.Key.IsValid() && A.DynamicLabel; }));
	// ITS NEIGHBOUR, the runway's mode (2026-09-29): keyless for the flip's reason, captioned live.
	TestTrue(TEXT("selection.runway_use is registered, keyless, in Selection"), Actions.ContainsByPredicate([](const FBuildAction& A)
		{ return A.Id == FName(TEXT("selection.runway_use")) && A.Section == EActionSection::Selection && !A.Key.IsValid() && A.DynamicLabel; }));

	// THE DEPOT CARD'S FUEL ROW (2026-10-03): three rows the inspector finds by id - a rename here is dead buttons on the card.
	for (const TCHAR* Fuel : { TEXT("selection.fuel_spot"), TEXT("selection.fuel_contract_up"), TEXT("selection.fuel_contract_cancel") })
	{
		TestTrue(FString::Printf(TEXT("%s is registered, keyless, inspector-only, in Selection"), Fuel), Actions.ContainsByPredicate([Fuel](const FBuildAction& A)
			{ return A.Id == FName(Fuel) && A.Section == EActionSection::Selection && !A.Key.IsValid() && A.bInspectorOnly; }));
	}

	// Every section has at least one action - an empty section on the bar is a layout with
	// nothing in it, which reads as a bug.
	for (uint8 S = 0; S < static_cast<uint8>(EActionSection::Count); ++S)
	{
		const EActionSection Section = static_cast<EActionSection>(S);
		TestTrue(*FString::Printf(TEXT("section %s has actions"), ActionSectionName(Section)),
			Actions.ContainsByPredicate([Section](const FBuildAction& A) { return A.Section == Section; }));
	}

	// FindAction is the lookup OnActionKey/OnCtrlActionKey now use instead of scanning by
	// hand - a bug in it is a key silently going nowhere, same class of bug as the rest of
	// this test.
	const FBuildAction* Depart = FindAction(FName(TEXT("selection.depart")));
	if (TestNotNull(TEXT("FindAction(Id) finds selection.depart"), Depart))
	{
		TestEqual(TEXT("found by id has the right section"), Depart->Section, EActionSection::Selection);
	}
	TestNull(TEXT("FindAction(Id) is null for an id nothing registers"), FindAction(FName(TEXT("no.such.action"))));

	const FBuildAction* Follow = FindAction(EKeys::C, false);
	if (TestNotNull(TEXT("FindAction(Key) finds selection.follow on C"), Follow))
	{
		TestEqual(TEXT("found by key is selection.follow"), Follow->Id, FName(TEXT("selection.follow")));
	}
	TestNull(TEXT("FindAction(Key) respects the Ctrl requirement"), FindAction(EKeys::C, true));
	// Insert, not Escape: Escape opens Settings since UI library step 4b.
	TestNull(TEXT("FindAction(Key) is null for an unbound key"), FindAction(EKeys::Insert, false));

	return true;
}

/**
 * TryRun IS THE GATE. Before this, OnActionKey called Execute directly and never asked
 * IsEnabled - a key could fire undo with nothing to undo, or land with no runway, while the
 * bar and the inspector (which called IsEnabled by hand) refused the same click. This test
 * fails if TryRun ever forgets to check IsEnabled, in either direction, regardless of which
 * concrete action the real table happens to contain.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildActionTryRunTest,
	"AirportMgr.Actions.TryRunGatesOnEnabled",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildActionTryRunTest::RunTest(const FString& Parameters)
{
	// No ARoadNetworkActor: this test only needs a world to spawn a controller in (#189).
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }

	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }

	int32 RanCount = 0;
	FBuildAction Action;
	Action.Id = FName(TEXT("test.action"));
	// FBuildActionContext&, not a bare ARoadBuildController& - issue #191 gave every action's
	// Execute/IsActive/IsEnabled that context instead, so this generic TryRun test (which
	// exercises none of the fields on it) matches the real signature.
	Action.Execute = [&RanCount](FBuildActionContext&) { ++RanCount; };
	Action.IsActive = [](const FBuildActionContext&) { return false; };

	Action.IsEnabled = [](const FBuildActionContext&) { return false; };
	TestFalse(TEXT("TryRun refuses a disabled action"), Action.TryRun(*C, TEXT("Test")));
	TestEqual(TEXT("Execute did not run while disabled"), RanCount, 0);

	Action.IsEnabled = [](const FBuildActionContext&) { return true; };
	TestTrue(TEXT("TryRun runs an enabled action"), Action.TryRun(*C, TEXT("Test")));
	TestEqual(TEXT("Execute ran exactly once while enabled"), RanCount, 1);

	// THE RUNWAY FLIP WITH NOTHING SELECTED is refused by its own gate, not by a crash in
	// FlipSelectedRunway reaching for a runway that is not there.
	const FBuildAction* Flip = FindAction(FName(TEXT("selection.runway_in_use")));
	if (TestNotNull(TEXT("the runway flip is registered"), Flip))
	{
		const FBuildActionContext Ctx(*C);
		TestFalse(TEXT("no runway selected: the flip is disabled"), Flip->IsEnabled(Ctx));
		TestFalse(TEXT("and TryRun refuses it"), Flip->TryRun(*C, TEXT("Test")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFeeLeverIsInTheOneListTest,
	"AirportMgr.Actions.FeeLeverIsInTheOneList",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFeeLeverIsInTheOneListTest::RunTest(const FString& Parameters)
{
	// THROUGH BuildActions AND NOT BESIDE IT. The bar, the key bindings and the inspector all
	// read this one table, so a fee button added straight to the widget would exist on the bar
	// and nowhere else - the "lists that must agree" failure this codebase has shipped three
	// times. This test is what makes that a rule rather than an intention.
	const FBuildAction* Up = FindAction(FName(TEXT("game.feeup")));
	const FBuildAction* Down = FindAction(FName(TEXT("game.feedown")));

	if (!TestNotNull(TEXT("the fee can be raised from the one action list"), Up)) { return false; }
	if (!TestNotNull(TEXT("and lowered from it"), Down)) { return false; }

	TestEqual(TEXT("both sit in the Game section, beside save and load"),
		Up->Section, EActionSection::Game);
	TestEqual(TEXT("and so does the other"), Down->Section, EActionSection::Game);

	// NO KEYS, deliberately: a mis-hit that silently repriced every future offer is worse than
	// a click that has to be aimed at.
	TestFalse(TEXT("raising the fee has no key binding"), Up->Key.IsValid());
	TestFalse(TEXT("nor does lowering it"), Down->Key.IsValid());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDriveSideIsInTheOneListTest,
	"AirportMgr.Actions.DriveSideIsInTheOneList",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDriveSideIsInTheOneListTest::RunTest(const FString& Parameters)
{
	// Through the one table for the fee lever's reason (spec 2026-09-23 §2): a toggle added
	// straight to the widget would be a button nothing else knew about.
	const FBuildAction* Side = FindAction(FName(TEXT("game.driveside")));
	if (!TestNotNull(TEXT("the drive side can be flipped from the one action list"), Side)) { return false; }
	TestEqual(TEXT("it sits in the Game section, an airport-wide setting"), Side->Section, EActionSection::Game);
	// NO KEY: a mis-hit would re-lane the whole airport.
	TestFalse(TEXT("it has no key binding"), Side->Key.IsValid());
	return true;
}

/**
 * ONE BUTTON PER ROW AND PER COLUMN, WALKED FROM BOTH ENUMS. A row or column with no button is
 * a guide the player cannot switch, and nothing else would say so.
 *
 * REPLACES SnapTogglesAreInTheRegistry, which walked ESource - an enum that no longer exists,
 * and whose single axis is the defect the grid was built to remove. That test could never have
 * caught the 2026-09-20 report: it checked that every SOURCE had a button, and the bug was that
 * a source referenced a runway no button governed.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGuideGridIsInTheRegistryTest,
	"AirportMgr.Actions.GuideGridIsInTheRegistry",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGuideGridIsInTheRegistryTest::RunTest(const FString& Parameters)
{
	const TArray<TPair<SnapGuide::ERelation, const TCHAR*>> Relations = {
		{ SnapGuide::ERelation::Extending,   TEXT("snap.extending")   },
		{ SnapGuide::ERelation::LevelWith,   TEXT("snap.levelwith")   },
		// THE ID IS NOT THE ENUM'S NAME, and this is the one row where they differ: the button
		// reads "Direction" because the row offers a direction AND its perpendicular AND the
		// world axes. See SnapToggleRegistry.cpp, and ERelation::Parallel's own comment.
		{ SnapGuide::ERelation::Parallel,    TEXT("snap.direction")   },
		{ SnapGuide::ERelation::Collinear,   TEXT("snap.collinear")   },
		{ SnapGuide::ERelation::AngledFrom,  TEXT("snap.angledfrom")  },
		{ SnapGuide::ERelation::MatchingGap, TEXT("snap.matchinggap") } };

	// THISGESTURE IS ABSENT ON PURPOSE, and the count below is what keeps that deliberate: it
	// is checked against the enum MINUS ONE, so a seventh reference added without a button
	// still fails here. See the 2026-09-20 guide-grid design section 7 for why that one has no
	// switch. Six buttons since the Road column became Taxiway and ServiceRoad that same day.
	const TArray<TPair<SnapGuide::EReference, const TCHAR*>> References = {
		{ SnapGuide::EReference::Taxiway,     TEXT("snapto.taxiway")     },
		{ SnapGuide::EReference::ServiceRoad, TEXT("snapto.serviceroad") },
		{ SnapGuide::EReference::Runway,      TEXT("snapto.runway")      },
		{ SnapGuide::EReference::Apron,       TEXT("snapto.apron")       },
		{ SnapGuide::EReference::Stand,       TEXT("snapto.stand")       },
		{ SnapGuide::EReference::World,       TEXT("snapto.world")       } };

	// THE TABLES ABOVE ARE THEMSELVES SECOND LISTS, so each is checked against its enum's own
	// size first - otherwise a row added to ERelation could be missed by this test as easily as
	// by the registry, which is the failure the test exists to prevent.
	TestEqual(TEXT("every ERelation value is covered by this test's own table"),
		Relations.Num(), static_cast<int32>(SnapGuide::ERelation::MatchingGap) + 1);
	TestEqual(TEXT("every EReference but ThisGesture is covered"),
		References.Num(), static_cast<int32>(SnapGuide::EReference::World));

	for (const TPair<SnapGuide::ERelation, const TCHAR*>& Pair : Relations)
	{
		const FBuildAction* Action = FindAction(FName(Pair.Value));
		if (!TestNotNull(*FString::Printf(TEXT("%s is registered"), Pair.Value), Action))
		{
			continue;
		}
		TestEqual(*FString::Printf(TEXT("%s sits in the Snap section"), Pair.Value),
			Action->Section, EActionSection::Snap);
		TestTrue(*FString::Printf(TEXT("%s can be executed"), Pair.Value),
			static_cast<bool>(Action->Execute));
		TestTrue(*FString::Printf(TEXT("%s reports whether it is lit"), Pair.Value),
			static_cast<bool>(Action->IsActive));
	}

	for (const TPair<SnapGuide::EReference, const TCHAR*>& Pair : References)
	{
		const FBuildAction* Action = FindAction(FName(Pair.Value));
		if (!TestNotNull(*FString::Printf(TEXT("%s is registered"), Pair.Value), Action))
		{
			continue;
		}
		TestEqual(*FString::Printf(TEXT("%s sits in the Snap to section"), Pair.Value),
			Action->Section, EActionSection::SnapTo);
		TestTrue(*FString::Printf(TEXT("%s can be executed"), Pair.Value),
			static_cast<bool>(Action->Execute));
		TestTrue(*FString::Printf(TEXT("%s reports whether it is lit"), Pair.Value),
			static_cast<bool>(Action->IsActive));
	}

	// AND BOTH SECTIONS HAVE A NAME. ActionSectionName indexes SectionNames by the enum, so a
	// row added in the wrong slot renames two sections at once and the static_assert cannot see
	// it. SENTENCE CASE, like every other row - "Time", "Tools", "Game".
	TestEqual(TEXT("the Snap section is named"),
		FString(ActionSectionName(EActionSection::Snap)), FString(TEXT("Snap")));
	TestEqual(TEXT("the Snap to section is named"),
		FString(ActionSectionName(EActionSection::SnapTo)), FString(TEXT("Snap to")));

	return true;
}

/**
 * THE KEY THAT WENT NOWHERE, issue #192. FindAction(Key, bRequiresCtrl) required an EXACT
 * match, so pressing a plain tool key (One, Taxiway, bRequiresCtrl=false) while Ctrl was held
 * for an unrelated reason (FRoadDrawTool's own "ctrl to remove" click modifier) matched
 * nothing and silently did nothing - CLAUDE.md's own name for this class of bug. Drives
 * ARoadBuildController::RunActionForKey through OnActionKeyForTest with a SYNTHETIC Ctrl flag,
 * since a headless test has no viewport to hold a real key down.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FActionKeyFallsBackWithoutCtrlTest,
	"AirportMgr.Actions.KeyFallsBackWhenCtrlHasNoExactMatch",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FActionKeyFallsBackWithoutCtrlTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }

	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }

	// One is the Taxiway tool's key (index 1 in ToolRegistry, bRequiresCtrl=false) - see
	// BuildSession.cpp's own comment on the key order. FindAction(One, true) matches nothing:
	// this is the exact miss the fallback exists for.
	TestNull(TEXT("no action requires Ctrl on the tool key One"), FindAction(EKeys::One, true));
	const FBuildAction* Plain = FindAction(EKeys::One, false);
	if (!TestNotNull(TEXT("One selects a tool without Ctrl"), Plain)) { return false; }

	TestNotEqual(TEXT("starts on a different tool than One selects"),
		C->GetActiveToolIndex(), 1);

	// THE REPORTED CASE: Ctrl held (a modifier meant for something else) plus the plain tool
	// key. Pre-fix, RunActionForKey's exact match failed and nothing ran - this must now fall
	// back to the same action FindAction(One, false) found above.
	C->OnActionKeyForTest(EKeys::One, /*bCtrl=*/true);
	TestEqual(TEXT("key 1 with Ctrl held still selects the tool"), C->GetActiveToolIndex(), 1);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildActionsEditModeIsInTheOneListTest,
	"AirportMgr.Actions.EditModeIsInTheOneList",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildActionsEditModeIsInTheOneListTest::RunTest(const FString& Parameters)
{
	// THROUGH THE TABLE, which is what makes the key binding, the bar button and the startup
	// banner one list - see BuildActions()'s own comment and the three times this project
	// shipped a key that went nowhere. A toggle wired straight into SetupInputComponent would
	// work at the keyboard and leave no button, which is exactly how the mode would stay
	// undiscoverable - the complaint this whole feature exists to answer.
	const FBuildAction* Edit = FindAction(FName(TEXT("edit.editmode")));
	if (!TestNotNull(TEXT("the Edit mode toggle is in BuildActions"), Edit)) { return false; }

	TestTrue(TEXT("it sits in the Edit section, not on the tool row - it is the other axis "
				  "and must not read as a tenth tool"),
		Edit->Section == EActionSection::Edit);

	// M, NOT E: Q/E is camera turn, polled every frame. A binding that rotated the view while
	// toggling the mode would be a bug nothing else in this suite would catch.
	TestTrue(TEXT("bound to M"), Edit->Key == EKeys::M);
	TestFalse(TEXT("and needs no modifier"), Edit->bRequiresCtrl);

	TestNotNull(TEXT("it can be run"), (void*)(bool)Edit->Execute);
	TestNotNull(TEXT("it lights when the mode is on"), (void*)(bool)Edit->IsActive);
	TestNotNull(TEXT("and greys when the lit tool exposes nothing"), (void*)(bool)Edit->IsEnabled);

	// NO OTHER ACTION MAY SHARE THE KEY. FindAction(FKey, bool) returns the first match, so a
	// collision would silently give one of the two actions away.
	int32 OnM = 0;
	for (const FBuildAction& Action : BuildActions())
	{
		OnM += (Action.Key == EKeys::M) ? 1 : 0;
	}
	TestEqual(TEXT("M belongs to exactly one action"), OnM, 1);
	return true;
}

/**
 * THE GRID BUTTON: one cycling toggle in the Snap section, bar-only, with a caption that follows
 * the step. Its behaviour (the cycle, the figures) is Airside.Tool.GridSnap.CycleAndFigures; this
 * pins that the player has a button to reach it at all.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGridButtonIsInTheRegistryTest,
	"AirportMgr.Actions.GridButtonIsInTheRegistry",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGridButtonIsInTheRegistryTest::RunTest(const FString& Parameters)
{
	const FBuildAction* Action = FindAction(FName(TEXT("snap.grid")));
	if (!TestNotNull(TEXT("snap.grid is registered"), Action)) { return false; }
	TestEqual(TEXT("in the Snap section"), Action->Section, EActionSection::Snap);
	TestFalse(TEXT("bar-only: no key, by the snap toggles' NO KEYS rule"), Action->Key.IsValid());
	TestTrue(TEXT("can be executed"), static_cast<bool>(Action->Execute));
	TestTrue(TEXT("reports whether it is lit"), static_cast<bool>(Action->IsActive));
	TestTrue(TEXT("has a caption that follows the step"), static_cast<bool>(Action->DynamicLabel));
	return true;
}

/**
 * SETTINGS IS IN THE ONE LIST (spec section 3): the bar's gear and Escape come from game.settings,
 * so neither can exist without the other - the "key that goes nowhere" this project shipped three
 * times. Escape because it is what a player presses for a game's settings; in PIE the editor's
 * Stop takes it first (memory: unreal-escape-stops-pie), and the gear is the way in there.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBuildActionsSettingsTest, "AirportMgr.Actions.SettingsOnEscape",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildActionsSettingsTest::RunTest(const FString& Parameters)
{
	const FBuildAction* Settings = FindAction(SettingsActionId());
	if (!TestNotNull(TEXT("game.settings is in BuildActions"), Settings)) { return false; }
	TestEqual(TEXT("in the Game section, with save and load"), Settings->Section, EActionSection::Game);
	const FBuildAction* OnEscape = FindAction(EKeys::Escape, false);
	if (!TestNotNull(TEXT("Escape runs an action"), OnEscape)) { return false; }
	TestEqual(TEXT("and it is Settings"), OnEscape->Id, SettingsActionId());
	return true;
}

/**
 * KEYS WAIT UNDER A MODAL (spec section 2, Modal; plan 4b Review Focus 3): a tool key pressed
 * while Settings is open does nothing - the player is in a form, not building - and Escape still
 * reaches Settings, which closes. Driven through the controller's own key path, with a real host
 * and Settings panel standing in for the hud CreateAll would build (it needs a local player).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBuildActionsModalKeysTest, "AirportMgr.Actions.KeysIgnoredUnderModal",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildActionsModalKeysTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }
	UBuildHudLayer* Hud = C->GetHud();
	Hud->WindowHost = CreateWidget<UUiWindowHost>(TestWorld.World, UUiWindowHost::StaticClass());
	Hud->SettingsPanel = CreateWidget<USettingsPanelWidget>(TestWorld.World, USettingsPanelWidget::StaticClass());
	if (!TestTrue(TEXT("a host and a Settings panel"), Hud->WindowHost != nullptr && Hud->SettingsPanel != nullptr)) { return false; }
	Hud->WindowHost->AddWindow(*Hud->SettingsPanel);
	Hud->SettingsPanel->SetSink(MakeShared<FMemoryPlayerSettingsSink>());

	const int32 Before = C->GetActiveToolIndex();
	TestNotEqual(TEXT("starts on a different tool than One selects"), Before, 1);
	C->OnActionKeyForTest(EKeys::Escape, false);
	TestTrue(TEXT("Escape opens Settings"), C->IsSettingsShowing());
	TestTrue(TEXT("as a modal"), C->IsModalOpen());
	C->OnActionKeyForTest(EKeys::One, false);
	TestEqual(TEXT("a tool key under the modal does nothing"), C->GetActiveToolIndex(), Before);
	C->OnActionKeyForTest(EKeys::One, true);
	TestEqual(TEXT("nor with Ctrl held"), C->GetActiveToolIndex(), Before);
	C->OnActionKeyForTest(EKeys::Escape, false);
	TestFalse(TEXT("Escape closes it again"), C->IsSettingsShowing());
	C->OnActionKeyForTest(EKeys::One, false);
	TestEqual(TEXT("control: with it closed the same key selects the tool"), C->GetActiveToolIndex(), 1);
	return true;
}

namespace BuildActionsModalTest
{
	/** A controller with a real host and Settings panel standing in for CreateAll's hud. */
	ARoadBuildController* SpawnWithSettings(FAirsideTestWorld& TestWorld)
	{
		ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
		if (C == nullptr) { return nullptr; }
		UBuildHudLayer* Hud = C->GetHud();
		Hud->WindowHost = CreateWidget<UUiWindowHost>(TestWorld.World, UUiWindowHost::StaticClass());
		Hud->SettingsPanel = CreateWidget<USettingsPanelWidget>(TestWorld.World, USettingsPanelWidget::StaticClass());
		if (Hud->WindowHost == nullptr || Hud->SettingsPanel == nullptr) { return nullptr; }
		Hud->WindowHost->AddWindow(*Hud->SettingsPanel);
		Hud->SettingsPanel->SetSink(MakeShared<FMemoryPlayerSettingsSink>());
		return C;
	}
}

/**
 * THE MOUSE WAITS UNDER A MODAL TOO (4b final review, Important 5): a drag begun before Escape kept
 * extending behind the scrim and built on release. Opening Settings drops a held press, and a
 * press while it is open starts nothing.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBuildActionsModalMouseTest, "AirportMgr.Actions.MouseWaitsUnderModal",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildActionsModalMouseTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	ARoadBuildController* C = BuildActionsModalTest::SpawnWithSettings(TestWorld);
	if (!TestNotNull(TEXT("a controller with Settings"), C)) { return false; }
	C->PressPrimaryForTest(FVector2D(100.0, 100.0));
	TestTrue(TEXT("control: a press is held"), C->IsPrimaryPressedForTest());
	C->OnActionKeyForTest(EKeys::Escape, false);
	TestTrue(TEXT("Settings opened"), C->IsModalOpen());
	TestFalse(TEXT("opening it dropped the held press"), C->IsPrimaryPressedForTest());
	C->PressPrimaryForTest(FVector2D(100.0, 100.0));
	TestFalse(TEXT("a press under the modal starts nothing"), C->IsPrimaryPressedForTest());
	C->OnActionKeyForTest(EKeys::Escape, false);
	C->PressPrimaryForTest(FVector2D(100.0, 100.0));
	TestTrue(TEXT("control: closed, a press is held again"), C->IsPrimaryPressedForTest());
	return true;
}

/**
 * OPENING SETTINGS IS NOT A RIGHT-CLICK (#448). ToggleSettings ran OnCancelGesture - the two-level cancel - whenever there was a
 * target, so the moment the dialog opened the Select tool cleared the selection and an idle build tool was put down to Select.
 * FBuildActionsModalMouseTest above has NO target, so that branch never ran under test. Only a drag in flight is dropped.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBuildActionsSettingsKeepsTheToolTest, "AirportMgr.Actions.OpeningSettingsKeepsToolAndSelection",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildActionsSettingsKeepsTheToolTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("an actor"), Actor)) { return false; }
	// FDepotForSelectionTest's setup: a spawned actor's network is only placeable into after a clear.
	Actor->ClearNetwork();
	UEntityDefinition* StandDef = UEntityDefinition::MakeStandTransient();
	const FEntityInstanceId Stand = Actor->Network->PlaceEntity(StandDef, StandDef->Anchors, FVector2D(20000.0, 0.0), 0.0, 0.0, StandDef->PoseRole, 0);
	if (!TestTrue(TEXT("setup: a stand to select"), Stand.IsSet())) { return false; }
	ARoadBuildController* C = BuildActionsModalTest::SpawnWithSettings(TestWorld);
	if (!TestNotNull(TEXT("a controller with Settings"), C)) { return false; }
	C->SetTargetForTest(Actor);

	// A BUILD TOOL LIT, IDLE: the state one right-click puts down. Opening Settings must not.
	C->SelectTool(1);
	if (!TestEqual(TEXT("setup: a build tool is lit"), C->GetActiveToolIndex(), 1)) { return false; }
	if (!TestTrue(TEXT("setup: and idle, the state a cancel would drop to Select"), C->GetActiveTool() != nullptr && C->GetActiveTool()->IsIdle())) { return false; }
	C->OnActionKeyForTest(EKeys::Escape, false);
	if (!TestTrue(TEXT("Settings opened"), C->IsSettingsShowing())) { return false; }
	TestEqual(TEXT("opening Settings left the lit build tool lit - it is not a right-click"), C->GetActiveToolIndex(), 1);
	C->OnActionKeyForTest(EKeys::Escape, false);
	if (!TestFalse(TEXT("Escape closed it again"), C->IsSettingsShowing())) { return false; }

	// THE SELECT TOOL WITH A SELECTION: a cancel would clear it.
	C->SelectTool(0);
	FSelection Selected;
	Selected.Kind = ESelectionKind::Stand;
	Selected.Id = Stand.Index;
	C->SelectForTest(Selected);
	// The tool binds the session's selection on a tick, as PlayerTick's does - and only then is it not idle.
	C->GetActiveTool()->Tick(C->MakeToolContext());
	if (!TestFalse(TEXT("setup: the Select tool holds a selection, so a cancel would have something to clear"), C->GetActiveTool()->IsIdle())) { return false; }
	C->OnActionKeyForTest(EKeys::Escape, false);
	if (!TestTrue(TEXT("Settings opened again"), C->IsSettingsShowing())) { return false; }
	TestEqual(TEXT("opening Settings kept the selection's kind"), C->GetSelection().Kind, Selected.Kind);
	TestEqual(TEXT("and its id"), C->GetSelection().Id, Selected.Id);
	TestEqual(TEXT("and the Select tool stayed lit"), C->GetActiveToolIndex(), 0);
	return true;
}

/**
 * OPENING SETTINGS STILL DROPS A DRAG IN FLIGHT (4b final review, Important 5) - the half of the old behaviour that was right, which
 * #448's fix must not take with the half that was wrong. A drag begun before Escape would keep extending behind the scrim and build on
 * release; the tool's stage is abandoned through CancelStage and the press is let go. The CONTROL is the same state with the press NOT
 * yet a drag: a staged tool is left as it was, because opening a dialog abandons nothing but a drag.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBuildActionsSettingsDropsADragTest, "AirportMgr.Actions.OpeningSettingsDropsADragInFlight",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildActionsSettingsDropsADragTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("an actor"), Actor)) { return false; }
	Actor->PlaceNode(FVector2D(0.0, 30000.0));
	ARoadBuildController* C = BuildActionsModalTest::SpawnWithSettings(TestWorld);
	if (!TestNotNull(TEXT("a controller with Settings"), C)) { return false; }
	C->SetTargetForTest(Actor);

	// A TOOL MID-STAGE: the taxiway tool's first click fixes a start and leaves it part-drawn, which is what a cancel abandons.
	auto Stage = [&]() -> bool
	{
		C->SelectTool(1);
		IBuildTool* Tool = C->GetActiveTool();
		if (Tool == nullptr) { return false; }
		Tool->OnClick(C->MakeToolContext());
		return !Tool->IsIdle();
	};
	if (!TestTrue(TEXT("setup: the taxiway tool is part-drawn after a click"), Stage())) { return false; }

	// CONTROL - A PRESS THAT HAS NOT BECOME A DRAG: dropped, but the staged chain is kept.
	C->PressPrimaryForTest(FVector2D(100.0, 100.0));
	if (!TestTrue(TEXT("setup: a press is held"), C->IsPrimaryPressedForTest())) { return false; }
	C->OnActionKeyForTest(EKeys::Escape, false);
	if (!TestTrue(TEXT("Settings opened"), C->IsSettingsShowing())) { return false; }
	TestFalse(TEXT("control: the held press was let go"), C->IsPrimaryPressedForTest());
	TestFalse(TEXT("control: but a press that never became a drag abandons nothing - the staged chain is kept"), C->GetActiveTool()->IsIdle());
	TestEqual(TEXT("control: and the tool is still lit"), C->GetActiveToolIndex(), 1);
	C->OnActionKeyForTest(EKeys::Escape, false);
	if (!TestFalse(TEXT("Escape closed it again"), C->IsSettingsShowing())) { return false; }

	// THE DRAG: pressed, then moved past the threshold.
	C->PressPrimaryForTest(FVector2D(100.0, 100.0));
	C->MovePrimaryForTest(FVector2D(100.0 + C->DragThresholdPixels * 4.0, 100.0));
	if (!TestTrue(TEXT("setup: the press is held and travelling"), C->IsPrimaryPressedForTest())) { return false; }
	C->OnActionKeyForTest(EKeys::Escape, false);
	if (!TestTrue(TEXT("Settings opened over the drag"), C->IsSettingsShowing())) { return false; }
	TestFalse(TEXT("the drag's press was let go"), C->IsPrimaryPressedForTest());
	TestTrue(TEXT("and the tool's stage was abandoned through the tool's own cancel"), C->GetActiveTool()->IsIdle());
	TestEqual(TEXT("with the tool still lit - only the drag was dropped"), C->GetActiveToolIndex(), 1);
	return true;
}

/**
 * #448: THE WINDOW VERBS TOGGLE THE HUD LAYER. Four Toggle/IsShowing pairs on the controller became ONE toggle on UBuildHudLayer, which the
 * bar's rows reach through FBuildActionContext::Hud - so this is the seam that goes red if a row is left bound to nothing, or to the wrong
 * window. Each row's Execute opens its window, its IsActive lights while open, and the second Execute closes it. (Ledger, alerts and Land
 * go through the HUD directly; Settings through the controller, which adds the drag drop - the tests above.)
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWindowVerbsToggleTheHudTest, "AirportMgr.Actions.WindowVerbsToggleTheHudLayer",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FWindowVerbsToggleTheHudTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	ARoadBuildController* C = BuildActionsModalTest::SpawnWithSettings(TestWorld);
	if (!TestNotNull(TEXT("a controller with Settings"), C)) { return false; }
	UBuildHudLayer* Hud = C->GetHud();
	// A headless controller builds no HUD widgets: the panels are put where CreateAll would have put them.
	Hud->LedgerPanel = CreateWidget<ULedgerPanelWidget>(TestWorld.World, ULedgerPanelWidget::StaticClass());
	Hud->AlertsPanel = CreateWidget<UAlertsPanelWidget>(TestWorld.World, UAlertsPanelWidget::StaticClass());
	Hud->LandPanel = CreateWidget<ULandAircraftPanelWidget>(TestWorld.World, ULandAircraftPanelWidget::StaticClass());
	if (!TestTrue(TEXT("setup: the three panels"), Hud->LedgerPanel != nullptr && Hud->AlertsPanel != nullptr && Hud->LandPanel != nullptr)) { return false; }

	struct FRow { const TCHAR* Id; EHudWindow Window; };
	const FRow Rows[] = { { TEXT("game.ledger"), EHudWindow::Ledger }, { TEXT("game.alerts"), EHudWindow::Alerts },
		{ TEXT("aircraft.land"), EHudWindow::Land }, { TEXT("game.settings"), EHudWindow::Settings } };
	for (const FRow& Row : Rows)
	{
		const FBuildAction* Action = FindAction(FName(Row.Id));
		if (!TestNotNull(*FString::Printf(TEXT("%s is registered"), Row.Id), Action)) { continue; }
		FBuildActionContext Ctx(*C);
		TestFalse(*FString::Printf(TEXT("%s: closed to start"), Row.Id), Hud->IsWindowShowing(Row.Window));
		Action->Execute(Ctx);
		TestTrue(*FString::Printf(TEXT("%s: Execute opens its window"), Row.Id), Hud->IsWindowShowing(Row.Window));
		TestTrue(*FString::Printf(TEXT("%s: and the button lights while it is open"), Row.Id), Action->IsActive(Ctx));
		Action->Execute(Ctx);
		TestFalse(*FString::Printf(TEXT("%s: the second Execute closes it"), Row.Id), Hud->IsWindowShowing(Row.Window));
	}
	// A HUD WITH NO PANEL FOR A WINDOW is inert, not a crash: the editor mode and a bare test build none.
	UBuildHudLayer* Bare = NewObject<UBuildHudLayer>(GetTransientPackage());
	Bare->ToggleWindow(EHudWindow::Ledger);
	TestFalse(TEXT("no panel: nothing to open"), Bare->IsWindowShowing(EHudWindow::Ledger));
	return true;
}

/**
 * #448: ONE RESOLVER FOR THE OPS RUNTIME. A headless test's world has no game instance, so a test stands a runtime in for it
 * (OpsRuntimeResolver::SetOverrideForTest) - and EVERY reader must see it: the action context, the bar, and the resolver itself. They
 * did not: the controller's depot verbs saw a controller-held override while the context, the bar (UseForTest's own runtime) and nine widgets
 * asked the subsystem, so handing one place a runtime left the rest with none.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOneResolverForTheOpsRuntimeTest, "AirportMgr.Actions.OneResolverForTheOpsRuntime",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOneResolverForTheOpsRuntimeTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("an actor"), Actor)) { return false; }
	Actor->PlaceNode(FVector2D(0.0, 0.0));
	UOpsRuntime* Runtime = NewObject<UOpsRuntime>();
	Runtime->Attach(Actor);
	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	UBuildBarWidget* Bar = CreateWidget<UBuildBarWidget>(TestWorld.World, UBuildBarWidget::StaticClass());
	if (!TestTrue(TEXT("setup: a controller and a bar"), C != nullptr && Bar != nullptr)) { return false; }
	Bar->UseForTest(C);

	// CONTROL: before the override, a world with no game instance has no runtime for anyone.
	TestNull(TEXT("control: the resolver finds none"), OpsRuntimeResolver::Resolve(TestWorld.World));
	TestNull(TEXT("control: the action context finds none"), FBuildActionContext(*C).Runtime);
	Bar->RefreshBalanceForTest();
	const FString NoLedger = Bar->BalanceTextForTest().ToString();
	TestTrue(TEXT("control: the bar shows no balance (the fallback, or nothing yet)"), NoLedger.IsEmpty() || NoLedger.Contains(TEXT("no ledger")));

	OpsRuntimeResolver::SetOverrideForTest(TestWorld.World, Runtime);
	TestTrue(TEXT("the resolver answers the override"), OpsRuntimeResolver::Resolve(TestWorld.World) == Runtime);
	TestTrue(TEXT("the action context sees the same runtime"), FBuildActionContext(*C).Runtime == Runtime);
	Bar->RefreshBalanceForTest();
	const FString Shown = Bar->BalanceTextForTest().ToString();
	TestTrue(TEXT("and the bar reads its ledger: a balance is shown, not the fallback"), !Shown.IsEmpty() && !Shown.Contains(TEXT("no ledger")));

	// PER WORLD: another world is not given this world's runtime.
	FAirsideTestWorld Other(/*bSpawnActor=*/false);
	TestNull(TEXT("a different world resolves none"), OpsRuntimeResolver::Resolve(Other.World));

	OpsRuntimeResolver::SetOverrideForTest(TestWorld.World, nullptr);
	TestNull(TEXT("clearing the override gives the world its own answer back"), OpsRuntimeResolver::Resolve(TestWorld.World));
	TestNull(TEXT("a null world resolves none"), OpsRuntimeResolver::Resolve(nullptr));
	return true;
}

/**
 * #448 (PR review): THE MOVED SELECTION VERBS ACT ON THEIR OWNERS THROUGH THE CONTEXT. Depart, the runway flip, the runway mode and the
 * unstick gate left the controller for BuildActions.cpp, where each reads Ctx.Selection and acts on Ctx.Target / Ctx.Runtime; the
 * controller's own tests of them went with the forwarders, and nothing else ran them through a row. Each is driven here by TryRun against
 * a selection set the way a click sets it, and the EFFECT is read off the owner - the actor's runway facts, the agent's phase - not off a
 * log line. Two worlds: a runway field for the runway verbs, and a parked aircraft for depart and unstick.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSelectionVerbsActOnTheirOwnersTest, "AirportMgr.Actions.SelectionVerbsActOnTheirOwners",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSelectionVerbsActOnTheirOwnersTest::RunTest(const FString& Parameters)
{
	const FBuildAction* Flip = FindAction(FName(TEXT("selection.runway_in_use")));
	const FBuildAction* Mode = FindAction(FName(TEXT("selection.runway_use")));
	const FBuildAction* Unstick = FindAction(FName(TEXT("selection.unstick")));
	const FBuildAction* Depart = FindAction(FName(TEXT("selection.depart")));
	if (!TestTrue(TEXT("setup: the four rows are registered"), Flip != nullptr && Mode != nullptr && Unstick != nullptr && Depart != nullptr)) { return false; }

	// --- THE RUNWAY VERBS, on a field with one runway ---
	{
		FAirsideTestWorld TestWorld;
		ARoadNetworkActor* Actor = TestWorld.Actor;
		if (!TestNotNull(TEXT("an actor"), Actor)) { return false; }
		Actor->PlaceNode(FVector2D(-300000.0, -300000.0));
		FTestAirport::Build(UAirsideSettings::ResolveDefaultAirframe(), FTestAirportOptions(), Actor->Network);
		URoadNetwork& Net = *Actor->Network;
		int32 Segment = INDEX_NONE;
		for (int32 Index = 0; Index < Net.GetSegments().Num() && Segment == INDEX_NONE; ++Index)
		{
			const FRoadSegmentId Id = Net.SegmentIdAt(Index);
			Segment = Id.IsSet() && Net.IsRunwaySegment(Id) ? Index : INDEX_NONE;
		}
		if (!TestTrue(TEXT("setup: a runway segment"), Segment != INDEX_NONE)) { return false; }
		ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
		if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }
		C->SetTargetForTest(Actor);
		FSelection Selected;
		Selected.Kind = ESelectionKind::Runway;
		Selected.Id = Segment;
		C->SelectForTest(Selected);
		const FBuildActionContext Ctx(*C);

		FRunwayCardFacts Before;
		if (!TestTrue(TEXT("setup: the runway describes"), InspectFacts::DescribeRunway(Net, Segment, Before))) { return false; }
		TestTrue(TEXT("the flip and the mode are enabled with a runway selected"), Flip->IsEnabled(Ctx) && Mode->IsEnabled(Ctx));
		TestEqual(TEXT("the flip is captioned with the end it would change TO"), Flip->DynamicLabel(Ctx).ToString(), FString::Printf(TEXT("Use %02d"), Before.Other));
		TestEqual(TEXT("the mode is captioned with the current one"), Mode->DynamicLabel(Ctx).ToString(), FString(TEXT("Mixed ops")));

		// THE FLIP: the runway in use becomes the end it was not.
		TestTrue(TEXT("the flip runs"), Flip->TryRun(*C, TEXT("Test")));
		FRunwayCardFacts AfterFlip;
		if (!TestTrue(TEXT("the runway still describes"), InspectFacts::DescribeRunway(Net, Segment, AfterFlip))) { return false; }
		TestEqual(TEXT("the flip put the OTHER end in use"), AfterFlip.InUse, Before.Other);
		TestEqual(TEXT("and the old end is now the other"), AfterFlip.Other, Before.InUse);
		TestEqual(TEXT("the flip left the mode alone"), AfterFlip.Use, Before.Use);
		TestEqual(TEXT("the caption follows to the end it would now change to"), Flip->DynamicLabel(Ctx).ToString(), FString::Printf(TEXT("Use %02d"), AfterFlip.Other));

		// THE MODE: mixed -> arrivals only (RunwayUse::Next), and the flip untouched by it.
		TestTrue(TEXT("the mode runs"), Mode->TryRun(*C, TEXT("Test")));
		FRunwayCardFacts AfterMode;
		if (!TestTrue(TEXT("the runway still describes, again"), InspectFacts::DescribeRunway(Net, Segment, AfterMode))) { return false; }
		TestTrue(TEXT("the mode stepped to RunwayUse::Next"), AfterMode.Use == RunwayUse::Next(AfterFlip.Use));
		TestEqual(TEXT("the mode left the end in use alone"), AfterMode.InUse, AfterFlip.InUse);
		TestEqual(TEXT("and its caption is the new mode"), Mode->DynamicLabel(Ctx).ToString(), FString(TEXT("Arrivals only")));

		// A SELECTION THAT IS NO RUNWAY: both refused, nothing changed.
		Selected.Kind = ESelectionKind::Stand;
		C->SelectForTest(Selected);
		TestFalse(TEXT("a stand is selected: the flip is refused"), Flip->TryRun(*C, TEXT("Test")));
		TestFalse(TEXT("and the mode"), Mode->TryRun(*C, TEXT("Test")));
	}

	// --- DEPART AND UNSTICK, on a parked aircraft ---
	{
		FAirsideTestWorld TestWorld;
		ARoadNetworkActor* Actor = TestWorld.Actor;
		if (!TestNotNull(TEXT("an actor"), Actor)) { return false; }
		Actor->PlaceNode(FVector2D(-400000.0, -400000.0));
		// DepartAgentTest's graph, its runway lengthened for the content airframe: a runway split at (0,0), a stand A, a junction J where the
		// lead-in meets a taxiway, and the far arm E the pushback needs.
		URoadNetwork& Net = *Actor->Network;
		URoadProfile* Runway = TestProfiles::Runway();
		const FRoadNodeId RA = Net.AddNode(FVector2D(-300000.0, 0.0));
		const FRoadNodeId RM = Net.AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId RB = Net.AddNode(FVector2D(300000.0, 0.0));
		Net.AddStraightSegment(RA, RM, Runway);
		Net.AddStraightSegment(RM, RB, Runway);
		const FGuidelineNodeId Stand = TestGraph::Node(Net, 0.0, -20000.0);
		const FGuidelineNodeId Junction = TestGraph::Node(Net, 0.0, -10000.0);
		const FGuidelineNodeId OnStrip = TestGraph::Node(Net, 0.0, 0.0);
		const FGuidelineNodeId FarArm = TestGraph::Node(Net, 20000.0, -10000.0);
		TestGraph::FJoinOptions Authored;
		Authored.bDerived = false;
		TestGraph::Join(Net, Stand, Junction, Authored);
		TestGraph::Join(Net, Junction, OnStrip, Authored);
		TestGraph::Join(Net, Junction, FarArm, Authored);
		// The hand-authored graph is this fixture's graph (DepartAgentForwardersTest's reason): stamped current, or the planners wait for a release that never comes.
		Net.MarkGuidelinesDerived();
		if (!TestTrue(TEXT("setup: an aircraft dispatched to the stand"),
			Actor->DispatchAgent(TestGraph::Probe(Net, OnStrip, Stand, ETraversalClass::Aircraft), UAirsideSettings::ResolveDefaultAirframe()))) { return false; }
		const int32 Id = Actor->GetTraffic()->GetNewestAgentId();
		for (int32 I = 0; I < 20000 && Actor->GetTraffic()->LastAgentPhaseForTest() != EAgentPhase::Parked; ++I) { Actor->Tick(1.0f / 30.0f); }
		if (!TestEqual(TEXT("setup: it parks"), Actor->GetTraffic()->LastAgentPhaseForTest(), EAgentPhase::Parked)) { return false; }

		// THE RUNTIME, stood in for this world: the unstick gate asks it.
		UOpsRuntime* Runtime = NewObject<UOpsRuntime>();
		Runtime->Attach(Actor);
		OpsRuntimeResolver::SetOverrideForTest(TestWorld.World, Runtime);

		ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
		ARoadBuildController* Other = TestWorld.World->SpawnActor<ARoadBuildController>();
		if (!TestTrue(TEXT("controllers spawned"), C != nullptr && Other != nullptr)) { return false; }
		C->SetTargetForTest(Actor);
		Other->SetTargetForTest(Actor);
		FSelection Aircraft;
		Aircraft.Kind = ESelectionKind::Aircraft;
		Aircraft.Id = Id;
		FSelection AStand = Aircraft;
		AStand.Kind = ESelectionKind::Stand;   // the SAME id, read as a stand's entity index: the kind decides, not the number
		C->SelectForTest(Aircraft);
		Other->SelectForTest(AStand);

		// UNSTICK'S GATE: an agent is selected and the runtime allows despawn.
		TestTrue(TEXT("unstick is enabled with an aircraft selected"), Unstick->IsEnabled(FBuildActionContext(*C)));
		TestFalse(TEXT("and not with a stand selected, whatever its index"), Unstick->IsEnabled(FBuildActionContext(*Other)));
		TestFalse(TEXT("depart is not enabled with a stand selected"), Depart->IsEnabled(FBuildActionContext(*Other)));

		// DEPART: the actor's own DepartAgent runs with the selected id. (The facts cache is per controller per frame, and C is asked first here.)
		TestTrue(TEXT("depart runs with a parked aircraft selected"), Depart->TryRun(*C, TEXT("Test")));
		const FRoadAgent* Agent = Actor->GetTraffic()->GetModel()->FindAgent(Id);
		if (!TestNotNull(TEXT("the aircraft is still there"), Agent)) { return false; }
		TestTrue(TEXT("and it left its stand"), Agent->Phase != EAgentPhase::Parked);
	}
	return true;
}

/**
 * CTRL CHORDS WAIT TOO (4b final review, Important 6): Ctrl+Z behind an open dialog would undo the airport.
 *
 * BOTH KEY HANDLERS REACH ONE FUNCTION. OnCtrlActionKey polls WasInputKeyJustPressed, which a headless test cannot
 * drive, and hands the key it found to RunActionForKey with Ctrl held; Check-Architecture's rule
 * `chord-handler-shares-the-gate` pins that call (and that the handler runs nothing itself). This drives that shared
 * handler with the real chord and asserts the EFFECT - an undoable edit survives Ctrl+Z under the modal and is undone
 * once it closes. (2026-09-30 review: it used to assert only what KeyWaitsForModal returned, a predicate the chord handler
 * could stop asking while this stayed green.)
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBuildActionsModalChordTest, "AirportMgr.Actions.ChordsWaitUnderModal",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildActionsModalChordTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("an actor"), Actor)) { return false; }
	ARoadBuildController* C = BuildActionsModalTest::SpawnWithSettings(TestWorld);
	if (!TestNotNull(TEXT("a controller with Settings"), C)) { return false; }
	C->SetTargetForTest(Actor);
	const FBuildAction* Undo = FindAction(FName(TEXT("edit.undo")));
	if (!TestNotNull(TEXT("undo is registered"), Undo)) { return false; }
	TestTrue(TEXT("control: undo is a Ctrl chord on Z"), Undo->bRequiresCtrl && Undo->Key == EKeys::Z);

	// AN EDIT TO UNDO, so "Ctrl+Z ran" is something this can see.
	Actor->PlaceNode(FVector2D(0.0, 0.0));
	if (!TestTrue(TEXT("setup: there is an edit to undo"), C->CanUndo())) { return false; }

	C->ToggleSettings();
	if (!TestTrue(TEXT("setup: Settings is open as a modal"), C->IsModalOpen())) { return false; }
	C->OnActionKeyForTest(EKeys::Z, true);
	TestTrue(TEXT("under the modal: Ctrl+Z waits - the edit is still there to undo"), C->CanUndo());

	C->ToggleSettings();
	if (!TestFalse(TEXT("setup: Settings closed again"), C->IsModalOpen())) { return false; }
	C->OnActionKeyForTest(EKeys::Z, true);
	TestFalse(TEXT("control: with no modal the same chord undoes the edit"), C->CanUndo());
	return true;
}

/**
 * THE GRID'S ORIENTATION TOGGLE, BAR-ONLY SINCE 2026-09-30. It was on H (grid-follows-snap design,
 * 2026-09-28, the one snap toggle with a key); the owner dropped the key once the snap rows became
 * both drivers' one table (#440), because the level editor's H is Toggle Selected Hierarchy
 * Visibility and the Road Build mode would have taken it. H must now run nothing at all in PIE.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGridOrientButtonIsInTheRegistryTest,
	"AirportMgr.Actions.GridOrientButtonHasNoKey",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGridOrientButtonIsInTheRegistryTest::RunTest(const FString& Parameters)
{
	const FBuildAction* Action = FindAction(FName(TEXT("snap.gridorient")));
	if (!TestNotNull(TEXT("snap.gridorient is registered"), Action)) { return false; }
	TestEqual(TEXT("in the Snap section"), Action->Section, EActionSection::Snap);
	TestFalse(TEXT("bar-only: no key, by the owner's 2026-09-30 ruling"), Action->Key.IsValid());
	TestFalse(TEXT("no Ctrl"), Action->bRequiresCtrl);
	TestNull(TEXT("H runs no action in PIE - the binding loop reads the same table, so no row claims it"),
		FindAction(EKeys::H, false));
	TestTrue(TEXT("can be executed"), static_cast<bool>(Action->Execute));
	TestTrue(TEXT("reports whether it is lit"), static_cast<bool>(Action->IsActive));
	TestTrue(TEXT("has a caption that follows the orientation"), static_cast<bool>(Action->DynamicLabel));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFacilityVerbsRegisteredTest,
	"AirportMgr.Actions.FacilityVerbsRegistered",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFacilityVerbsRegisteredTest::RunTest(const FString& Parameters)
{
	// BY NAME (CLAUDE.md "lists that must agree"): the inspector finds these rows by id, so a rename here
	// would leave the depot card with dead buttons and nothing red.
	for (const TCHAR* Id : { TEXT("selection.buy_module"), TEXT("selection.buy_vehicle"), TEXT("selection.sell_vehicle"),
		TEXT("selection.fuel_spot"), TEXT("selection.fuel_contract_up"), TEXT("selection.fuel_contract_cancel") })
	{
		const FBuildAction* Action = FindAction(FName(Id));
		if (!TestNotNull(*FString::Printf(TEXT("%s is registered"), Id), Action)) { continue; }
		TestEqual(*FString::Printf(TEXT("%s is a Selection verb"), Id), Action->Section, EActionSection::Selection);
		TestFalse(*FString::Printf(TEXT("%s has no key - a key that spent money on whatever was selected is a misclick"), Id), Action->Key.IsValid());
		TestTrue(*FString::Printf(TEXT("%s is inspector-only - acting on a selection belongs to the inspector"), Id), Action->bInspectorOnly);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDepotForSelectionTest,
	"AirportMgr.Actions.SelectionNamesItsDepot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDepotForSelectionTest::RunTest(const FString& Parameters)
{
	// THE ONE selection->depot WALK (ruling C4): the card's verbs and the ghost reveal both ask it, so what
	// counts as "the selected depot" is decided here once. An UNPLOTTED depot still counts - the card sells
	// its vehicles; narrowing to plotted yards is the reveal's own business.
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor"), Actor)) { return false; }
	// PlotKitContentTest's setup: a spawned actor's network is only placeable into after a clear.
	Actor->ClearNetwork();
	if (!TestNotNull(TEXT("a network"), Actor->Network.Get())) { return false; }
	URoadNetwork& Net = *Actor->Network;
	UEntityDefinition* DepotDef = UEntityDefinition::MakeFuelDepotTransient();
	FEntityPlacement Placement;
	Placement.Definition = DepotDef;
	Placement.Anchors = DepotDef->Anchors;
	Placement.Position = FVector2D(1000.0, 0.0);
	Placement.Heading = UE_DOUBLE_HALF_PI;
	Placement.PoseRole = EServiceRole::Fuel;
	const FEntityInstanceId Depot = Net.PlaceEntity(Placement);
	UEntityDefinition* StandDef = UEntityDefinition::MakeStandTransient();
	const FEntityInstanceId Stand = Net.PlaceEntity(StandDef, StandDef->Anchors, FVector2D(20000.0, 0.0), 0.0, 0.0, StandDef->PoseRole, 0);
	if (!TestTrue(TEXT("setup: a depot and a stand"), Depot.IsSet() && Stand.IsSet())) { return false; }

	FSelection Sel;
	Sel.Kind = ESelectionKind::Stand;
	Sel.Id = Depot.Index;
	TestTrue(TEXT("a selected depot is named, plotted or not"), ARoadBuildController::DepotForSelection(Actor, Sel) == Depot);
	Sel.Id = Stand.Index;
	TestFalse(TEXT("a selected stand is no depot"), ARoadBuildController::DepotForSelection(Actor, Sel).IsSet());
	Sel.Id = Depot.Index;
	Sel.Kind = ESelectionKind::Aircraft;
	TestFalse(TEXT("an aircraft id that happens to equal the depot's index names nothing"), ARoadBuildController::DepotForSelection(Actor, Sel).IsSet());
	Sel.Kind = ESelectionKind::Stand;
	TestFalse(TEXT("and no target names nothing"), ARoadBuildController::DepotForSelection(nullptr, Sel).IsSet());
	return true;
}

namespace
{
	/** An attached runtime over a field with a runway and one stand - the airport the bar's close verb acts on. */
	UOpsRuntime* AirportActionRuntime(FAirsideTestWorld& World)
	{
		ARoadNetworkActor* Actor = World.Actor;
		const int32 A = Actor->PlaceNode(FVector2D(0.0, 30000.0));
		const int32 B = Actor->PlaceNode(FVector2D(20000.0, 30000.0));
		Actor->ConnectNodes(A, B);
		// A FIELD AN ARRIVAL CAN USE - runway, exit, taxiway, stand (#431): an accept is the arrival plan's now, so a strip nothing can land on, or a stand nothing reaches, accepts nothing.
		FTestAirport::Build(UAirsideSettings::ResolveDefaultAirframe(), FTestAirportOptions(), Actor->Network);
		UOpsRuntime* Runtime = NewObject<UOpsRuntime>();
		Runtime->Attach(Actor);
		for (int32 Tick = 0; Tick < 3; ++Tick) { Runtime->Tick(0.0); }
		return Runtime;
	}

	/** An accepted flight on Runtime's board, its lead long enough never to come due in a test. */
	UFlight* AirportActionAccepted(UOpsRuntime& Runtime, ARoadNetworkActor& Actor)
	{
		UFlight* Flight = NewObject<UFlight>(GetTransientPackage());
		// THE CONTENT DEFAULT, what AirportActionRuntime's field is sized for.
		Flight->Airframe = UAirsideSettings::ResolveDefaultAirframe();
		Flight->LeadTimeSeconds = 1.0e7;
		Runtime.GetFlightBoard()->AddOffer(*Runtime.GetClock(), Flight);
		return Runtime.GetFlightBoard()->Accept(*Actor.GetTraffic()->GetModel(), *Actor.Network, *Runtime.GetClock(), *Flight) ? Flight : nullptr;
	}
}

/**
 * CLOSING CONFIRMS AT THE CURSOR; DISMISSING CANCELS NOTHING (spec 2026-09-29-ops-batch3 §3). The bar's game.airport
 * is a menu verb - UUiMenuButton, the inspector Unstick's popup at the button the player clicked - whose Close line is
 * a bConfirm line: the first click arms it and re-captions it with what will be lost; only the second closes.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirportCloseConfirmsTest, "AirportMgr.Actions.AirportCloseConfirms",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAirportCloseConfirmsTest::RunTest(const FString& Parameters)
{
	const FBuildAction* Action = FindAction(FName(TEXT("game.airport")));
	if (!TestNotNull(TEXT("the airport verb is registered"), Action)) { return false; }
	TestTrue(TEXT("it is a menu verb"), static_cast<bool>(Action->MenuItems) && static_cast<bool>(Action->Choose));
	TestFalse(TEXT("with no key: a close is a misclick away from cancelling every flight"), Action->Key.IsValid());

	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor"), TestWorld.Actor)) { return false; }
	UOpsRuntime* Runtime = AirportActionRuntime(TestWorld);
	UFlight* Flight = AirportActionAccepted(*Runtime, *TestWorld.Actor);
	if (!TestNotNull(TEXT("an accepted flight"), Flight)) { return false; }
	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }
	// THE RUNTIME HANDED IN: a test world has no game instance, so the context's own lookup finds none.
	FBuildActionContext Ctx(*C);
	Ctx.Runtime = Runtime;

	const TArray<FUiMenuItem> OpenLines = Action->MenuItems(Ctx);
	if (!TestEqual(TEXT("two lines, always: Close then Open"), OpenLines.Num(), 2)) { return false; }
	TestEqual(TEXT("the first closes"), OpenLines[0].Label.ToString(), FString(TEXT("Close airport")));
	TestTrue(TEXT("and is enabled while open"), OpenLines[0].bEnabled);
	TestFalse(TEXT("the second, Open, is greyed while open"), OpenLines[1].bEnabled);
	TestTrue(TEXT("and must be confirmed"), OpenLines[0].bConfirm);
	TestEqual(TEXT("the confirm says what will be lost"), OpenLines[0].ConfirmLabel.ToString(), FString(TEXT("Close? 1 flight will be cancelled")));

	const UUIStyle* Style = UAirportMgrUISettings::ResolveStyle();
	if (!TestNotNull(TEXT("a style"), Style)) { return false; }
	UUiMenuButton* Menu = CreateWidget<UUiMenuButton>(TestWorld.World, UUiMenuButton::StaticClass());
	Menu->Build(*Style, Action->Label);
	Menu->Items = [Action, &Ctx]() { return Action->MenuItems(Ctx); };
	// WHAT THE BAR DOES WITH A CHOICE: OnChosen is a dynamic delegate a lambda cannot bind, so the count stands in.
	auto ChooseIfChosen = [&](int32 Before)
	{
		if (Menu->ChosenCountForTest() > Before) { Action->Choose(Ctx, Menu->LastChosenForTest()); }
	};

	// ARMED, THEN DISMISSED: a click away closes the popup, and nothing reaches the verb.
	Menu->BuildMenu();
	int32 Chosen = Menu->ChosenCountForTest();
	Menu->Choose(0);
	ChooseIfChosen(Chosen);
	TestEqual(TEXT("the first click arms the line"), Menu->ArmedForTest(), 0);
	Menu->HandleOpenChanged(false);
	Runtime->Tick(0.0);
	TestEqual(TEXT("dismissed: still open"), Runtime->GetAirport()->Status(), EAirportStatus::Open);
	TestEqual(TEXT("dismissed: the flight is still coming"), Flight->GetPhase(), EFlightPhase::Accepted);

	// ARMED, THEN CONFIRMED.
	Menu->BuildMenu();
	Chosen = Menu->ChosenCountForTest();
	Menu->Choose(0);
	Menu->Choose(0);
	ChooseIfChosen(Chosen);
	Runtime->Tick(0.0);
	TestEqual(TEXT("confirmed: closed"), Runtime->GetAirport()->Status(), EAirportStatus::ClosedByPlayer);
	TestEqual(TEXT("confirmed: the flight is cancelled"), Flight->GetPhase(), EFlightPhase::Cancelled);

	// REOPENING IS NOT CONFIRMED: nothing is lost by it.
	const TArray<FUiMenuItem> ClosedLines = Action->MenuItems(Ctx);
	if (!TestEqual(TEXT("closed: the same two lines"), ClosedLines.Num(), 2)) { return false; }
	TestFalse(TEXT("Close greyed"), ClosedLines[0].bEnabled);
	TestEqual(TEXT("the second reopens"), ClosedLines[1].Label.ToString(), FString(TEXT("Open airport")));
	TestTrue(TEXT("enabled"), ClosedLines[1].bEnabled);
	TestFalse(TEXT("on one click"), ClosedLines[1].bConfirm);
	Menu->BuildMenu();
	Chosen = Menu->ChosenCountForTest();
	Menu->Choose(1);
	ChooseIfChosen(Chosen);
	TestEqual(TEXT("reopened"), Runtime->GetAirport()->Status(), EAirportStatus::Open);
	return true;
}

/** The caption is the status whenever it is not simply open - what the player reads on the bar. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirportStatusCaptionTest, "AirportMgr.Actions.AirportStatusCaption",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAirportStatusCaptionTest::RunTest(const FString& Parameters)
{
	const FBuildAction* Action = FindAction(FName(TEXT("game.airport")));
	if (!TestNotNull(TEXT("the airport verb is registered"), Action)) { return false; }
	if (!TestTrue(TEXT("with a caption that follows the status"), static_cast<bool>(Action->DynamicLabel))) { return false; }
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor"), TestWorld.Actor)) { return false; }
	UOpsRuntime* Runtime = AirportActionRuntime(TestWorld);
	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }
	FBuildActionContext Ctx(*C);
	Ctx.Runtime = Runtime;

	TestEqual(TEXT("open: the verb"), Action->DynamicLabel(Ctx).ToString(), FString(TEXT("Close airport")));
	TestFalse(TEXT("and not lit"), Action->IsActive(Ctx));
	Runtime->SetAirportClosed(true);
	TestEqual(TEXT("closed and empty"), Action->DynamicLabel(Ctx).ToString(), FString(TEXT("Closed")));
	// ACCENT MEANS ARMED AND NOTHING ELSE (UBuildBarWidget, whole-stack review M6): a closed airport is not an armed
	// tool, and lighting it for one would teach the player that accent means two things. The caption is the signal.
	TestFalse(TEXT("closed: NOT lit - the caption says it"), Action->IsActive(Ctx));
	UFlight* Ground = NewObject<UFlight>(GetTransientPackage());
	Ground->SetPhaseForTest(EFlightPhase::TaxiIn);
	Runtime->GetFlightBoard()->AddOffer(*Runtime->GetClock(), Ground);
	TestEqual(TEXT("closed with an aircraft still on the ground"), Action->DynamicLabel(Ctx).ToString(), FString(TEXT("Closed (draining: 1)")));

	Runtime->SetAirportClosed(false);
	for (int32 Index = TestWorld.Actor->Network->GetSegments().Num() - 1; Index >= 0 && UAirport::HasRunway(*TestWorld.Actor->Network); --Index)
	{
		TestWorld.Actor->DeleteSegment(Index);
	}
	Runtime->Tick(0.0);
	TestEqual(TEXT("no runway"), Action->DynamicLabel(Ctx).ToString(), FString(TEXT("No runway")));
	return true;
}

/**
 * CHOOSE ACTS ON THE LINE SHOWN, never a toggle (review M9): line 0 is always Close and line 1 always Open, each
 * enabled only when it applies. A stale popup's Close chosen on an airport that is already closed must not reopen it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirportChooseLineTest, "AirportMgr.Actions.AirportChooseActsOnTheLine",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAirportChooseLineTest::RunTest(const FString& Parameters)
{
	const FBuildAction* Action = FindAction(FName(TEXT("game.airport")));
	if (!TestNotNull(TEXT("the airport verb is registered"), Action)) { return false; }
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor"), TestWorld.Actor)) { return false; }
	UOpsRuntime* Runtime = AirportActionRuntime(TestWorld);
	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }
	FBuildActionContext Ctx(*C);
	Ctx.Runtime = Runtime;

	Runtime->SetAirportClosed(true);
	Action->Choose(Ctx, 0);
	TestEqual(TEXT("Close on a closed airport leaves it closed"), Runtime->GetAirport()->Status(), EAirportStatus::ClosedByPlayer);
	Action->Choose(Ctx, 1);
	TestEqual(TEXT("Open opens it"), Runtime->GetAirport()->Status(), EAirportStatus::Open);
	Action->Choose(Ctx, 1);
	TestEqual(TEXT("Open on an open airport leaves it open"), Runtime->GetAirport()->Status(), EAirportStatus::Open);
	return true;
}

/**
 * EXECUTE NEVER CLOSES (review M7): it is the door with no confirm - TryRun, a key were one bound - so on an open
 * airport it does nothing; only the popup's confirmed Close line closes.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirportExecuteNeverClosesTest, "AirportMgr.Actions.AirportExecuteNeverCloses",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAirportExecuteNeverClosesTest::RunTest(const FString& Parameters)
{
	const FBuildAction* Action = FindAction(FName(TEXT("game.airport")));
	if (!TestNotNull(TEXT("the airport verb is registered"), Action)) { return false; }
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor"), TestWorld.Actor)) { return false; }
	UOpsRuntime* Runtime = AirportActionRuntime(TestWorld);
	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }
	FBuildActionContext Ctx(*C);
	Ctx.Runtime = Runtime;
	Action->Execute(Ctx);
	TestEqual(TEXT("Execute on an open airport leaves it open"), Runtime->GetAirport()->Status(), EAirportStatus::Open);
	Runtime->SetAirportClosed(true);
	Action->Execute(Ctx);
	TestEqual(TEXT("and on a closed one reopens it"), Runtime->GetAirport()->Status(), EAirportStatus::Open);
	return true;
}

/** TryChoose IS THE GATE for a menu verb's line, as TryRun is for Execute: a disabled action's line does not run. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTryChooseGatesTest, "AirportMgr.Actions.TryChooseGatesOnEnabled",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTryChooseGatesTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }
	int32 Chosen = INDEX_NONE;
	FBuildAction Action;
	Action.Id = FName(TEXT("test.menu"));
	Action.Execute = [](FBuildActionContext&) {};
	Action.IsActive = [](const FBuildActionContext&) { return false; };
	Action.Choose = [&Chosen](FBuildActionContext&, int32 Line) { Chosen = Line; };
	Action.IsEnabled = [](const FBuildActionContext&) { return false; };
	TestFalse(TEXT("TryChoose refuses a disabled action"), Action.TryChoose(*C, 1, TEXT("Test")));
	TestEqual(TEXT("and its line did not run"), Chosen, static_cast<int32>(INDEX_NONE));
	Action.IsEnabled = [](const FBuildActionContext&) { return true; };
	TestTrue(TEXT("TryChoose runs an enabled action's line"), Action.TryChoose(*C, 1, TEXT("Test")));
	TestEqual(TEXT("that line"), Chosen, 1);
	return true;
}

/** RULING I1: Land is greyed while the airport is not open - a closed airport admits no arrivals, the debug one too. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandGreyedWhileClosedTest, "AirportMgr.Actions.LandGreyedWhileClosed",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FLandGreyedWhileClosedTest::RunTest(const FString& Parameters)
{
	const FBuildAction* Land = FindAction(FName(TEXT("aircraft.land")));
	if (!TestNotNull(TEXT("Land is registered"), Land)) { return false; }
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor"), TestWorld.Actor)) { return false; }
	UOpsRuntime* Runtime = AirportActionRuntime(TestWorld);
	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }
	C->SetTargetForTest(TestWorld.Actor);
	FBuildActionContext Ctx(*C);
	Ctx.Runtime = Runtime;
	TestTrue(TEXT("CONTROL: open with a runway, Land is enabled"), Land->IsEnabled(Ctx));
	Runtime->SetAirportClosed(true);
	TestFalse(TEXT("closed: Land is greyed"), Land->IsEnabled(Ctx));
	Runtime->SetAirportClosed(false);
	// NO RUNTIME, NOTHING TO LAND THROUGH (#431): the board-less fallback that made "no runtime" look like a working Land
	// is gone, so the button no longer offers a click the controller would only refuse.
	Ctx.Runtime = nullptr;
	TestFalse(TEXT("no runtime: Land is greyed"), Land->IsEnabled(Ctx));
	return true;
}

/**
 * THE BAR'S SNAP ROWS COME FROM THE PLUGIN'S TABLE (issue #440), not a copy typed here: one row
 * per SnapToggleRegistry() entry, under its id, in its group's section, on its key, with its
 * caption - and no Snap/Snap to row the table does not have. Then the seam itself: TryRun on each
 * row, through a real controller with a real airport, changes the airport's FSnapGuideSettings
 * exactly as the registry's Apply does (ARoadBuildController::ApplySnapToggle, the door that
 * replaced eight proxies), and the row lights from the airport. The editor's twin is
 * Airside.Editor.SnapCommandsReachTheAirport; a toggle added to the table reaches both or neither.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSnapRowsComeFromTheRegistryTest,
	"AirportMgr.Actions.SnapRowsComeFromTheRegistry",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSnapRowsComeFromTheRegistryTest::RunTest(const FString& Parameters)
{
	const TConstArrayView<FSnapToggleRegistration> Registry = SnapToggleRegistry();
	int32 SnapRows = 0;
	for (const FBuildAction& Action : BuildActions())
	{
		SnapRows += (Action.Section == EActionSection::Snap || Action.Section == EActionSection::SnapTo) ? 1 : 0;
	}
	TestEqual(TEXT("every Snap and Snap to row is a registry toggle - none typed here beside the table"),
		SnapRows, Registry.Num());

	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world with an airport"), TestWorld.Actor)) { return false; }
	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }

	// NO AIRPORT FIRST (BuildActions.cpp's snap loop; #468's review): before SetTargetForTest this
	// controller has no target. A row stays ENABLED (inert, not illegal - ApplySnapToggle logs the
	// no-op), is NOT lit, and a caption row reads its fixed Name rather than claiming a state no
	// airport holds - the one behaviour #440 changed on purpose ("Grid: world" used to show here).
	const FSnapGuideSettings Untouched = TestWorld.Actor->GuideSources;
	{
		const FBuildActionContext NoAirport(*C);
		TestNull(TEXT("control: the controller has no airport yet"), NoAirport.Target);
		for (const FSnapToggleRegistration& Toggle : Registry)
		{
			const FString Id = Toggle.Id.ToString();
			const FBuildAction* Row = FindAction(Toggle.Id);
			if (!TestNotNull(*FString::Printf(TEXT("%s has a bar row"), *Id), Row)) { continue; }
			TestTrue(*FString::Printf(TEXT("%s: no airport, still enabled"), *Id), Row->IsEnabled(NoAirport));
			TestFalse(*FString::Printf(TEXT("%s: no airport, not lit"), *Id), Row->IsActive(NoAirport));
			if (Row->DynamicLabel)
			{
				TestEqual(*FString::Printf(TEXT("%s: no airport, the caption is the fixed name"), *Id),
					Row->DynamicLabel(NoAirport).ToString(), Toggle.Name.ToString());
			}
			TestTrue(*FString::Printf(TEXT("%s: no airport, it still runs (and logs the no-op)"), *Id), Row->TryRun(*C, TEXT("Test")));
		}
	}
	TestTrue(TEXT("with no airport, no toggle reached the level's settings"),
		FSnapGuideSettings::StaticStruct()->CompareScriptStruct(&Untouched, &TestWorld.Actor->GuideSources, PPF_None));

	C->SetTargetForTest(TestWorld.Actor);

	UScriptStruct* SettingsStruct = FSnapGuideSettings::StaticStruct();
	for (const FSnapToggleRegistration& Toggle : Registry)
	{
		const FString Id = Toggle.Id.ToString();
		const FBuildAction* Row = FindAction(Toggle.Id);
		if (!TestNotNull(*FString::Printf(TEXT("%s has a bar row under the registry's own id"), *Id), Row)) { continue; }
		TestEqual(*FString::Printf(TEXT("%s sits in its group's section"), *Id), Row->Section,
			Toggle.Group == ESnapToggleGroup::SnapTo ? EActionSection::SnapTo : EActionSection::Snap);
		TestTrue(*FString::Printf(TEXT("%s is on the registry's key"), *Id), Row->Key == Toggle.Key);
		TestEqual(*FString::Printf(TEXT("%s reads the registry's name"), *Id), Row->Label.ToString(), Toggle.Name.ToString());
		TestTrue(*FString::Printf(TEXT("%s has a caption exactly when the registry gives one"), *Id),
			static_cast<bool>(Row->DynamicLabel) == static_cast<bool>(Toggle.DynamicLabel));

		FSnapGuideSettings Expected = TestWorld.Actor->GuideSources;
		Toggle.Apply(Expected);
		TestTrue(*FString::Printf(TEXT("%s runs"), *Id), Row->TryRun(*C, TEXT("Test")));
		TestTrue(*FString::Printf(TEXT("%s changed the airport exactly as the registry's Apply does"), *Id),
			SettingsStruct->CompareScriptStruct(&Expected, &TestWorld.Actor->GuideSources, PPF_None));
		const FBuildActionContext Ctx(*C);
		TestTrue(*FString::Printf(TEXT("%s is lit from the airport's own settings"), *Id),
			Row->IsActive(Ctx) == Toggle.IsActive(TestWorld.Actor->GuideSources));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunwayRowsDescribeOncePerFrameTest, "AirportMgr.Actions.RunwayRowsDescribeOncePerFrame",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayRowsDescribeOncePerFrameTest::RunTest(const FString& Parameters)
{
	// #446 PIN: with a runway selected the bar asked InspectFacts::DescribeRunway four times a tick - IsEnabled and DynamicLabel, for the two
	// runway rows (the flip and the mode). The controller answers all four from ONE describe per frame now (SelectedRunwayFactsThisFrame).
	// Driven through the REAL bar tick (RefreshStateForTest, the function a frame runs), and counted at DescribeRunway itself, so a row that
	// bypassed the cache would be seen whoever it is.
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("an actor"), Actor)) { return false; }
	Actor->PlaceNode(FVector2D(-300000.0, -300000.0));
	FTestAirport::Build(UAirsideSettings::ResolveDefaultAirframe(), FTestAirportOptions(), Actor->Network);
	URoadNetwork& Net = *Actor->Network;
	int32 Segment = INDEX_NONE;
	for (int32 Index = 0; Index < Net.GetSegments().Num() && Segment == INDEX_NONE; ++Index)
	{
		const FRoadSegmentId Id = Net.SegmentIdAt(Index);
		Segment = Id.IsSet() && Net.IsRunwaySegment(Id) ? Index : INDEX_NONE;
	}
	if (!TestTrue(TEXT("setup: a runway segment"), Segment != INDEX_NONE)) { return false; }
	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }
	C->SetTargetForTest(Actor);
	C->InitInputSystem();
	UBuildBarWidget* Bar = CreateWidget<UBuildBarWidget>(TestWorld.World, UBuildBarWidget::StaticClass());
	if (!TestNotNull(TEXT("the bar"), Bar)) { return false; }

	FSelection Selected;
	Selected.Kind = ESelectionKind::Runway;
	Selected.Id = Segment;
	C->SelectForTest(Selected);

	C->RetireFrameCachesForTest();   // a frame of its own: the per-frame answers turn over with the frame, which a headless test never advances
	const int32 Start = InspectFacts::DescribeRunwayCountForTest();
	Bar->RefreshStateForTest(*C);
	TestEqual(TEXT("one bar tick with a runway selected describes it ONCE, not once per IsEnabled and DynamicLabel of two rows"),
		InspectFacts::DescribeRunwayCountForTest() - Start, 1);
	Bar->RefreshStateForTest(*C);
	TestEqual(TEXT("a second tick in the same frame describes nothing more"), InspectFacts::DescribeRunwayCountForTest() - Start, 1);
	C->RetireFrameCachesForTest();   // the next frame
	Bar->RefreshStateForTest(*C);
	TestEqual(TEXT("the next frame describes again, once - the answer is per frame, not for ever"), InspectFacts::DescribeRunwayCountForTest() - Start, 2);

	// NO RUNWAY SELECTED: nothing to describe, however many rows ask.
	C->SelectForTest(FSelection());
	C->RetireFrameCachesForTest();   // the next frame
	Bar->RefreshStateForTest(*C);
	TestEqual(TEXT("with nothing selected the bar describes no runway"), InspectFacts::DescribeRunwayCountForTest() - Start, 2);

	// A FLIP RETIRES THE FRAME'S ANSWER: the verb changes the network, the next reader in the SAME frame must see the flip - or the second
	// runway verb would act on the end that was in use before the first.
	C->SelectForTest(Selected);
	C->RetireFrameCachesForTest();   // the next frame
	FRunwayCardFacts Before;
	if (!TestTrue(TEXT("setup: the selected runway describes"), C->SelectedRunwayFactsThisFrame(Before))) { return false; }
	const FBuildAction* Flip = FindAction(FName(TEXT("selection.runway_in_use")));
	if (!TestNotNull(TEXT("the flip row"), Flip)) { return false; }
	TestTrue(TEXT("the flip runs"), Flip->TryRun(*C, TEXT("Test")));
	FRunwayCardFacts After;
	if (!TestTrue(TEXT("and the runway still describes"), C->SelectedRunwayFactsThisFrame(After))) { return false; }
	TestEqual(TEXT("the next reader in the same frame sees the flipped end, not the answer from before it"), After.InUse, Before.Other);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUndoDropsASelectionTest, "AirportMgr.Actions.UndoDropsASelectionOfThePlacementItRemoved",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUndoDropsASelectionTest::RunTest(const FString& Parameters)
{
	// #446 PIN: select a stand, undo its placement, and the selection clears rather than retargeting. The selection is a slot INDEX; undoing the
	// placement takes the slot, and a stand placed next is issued the same index - so until the select tool's next Tick (a frame later) the
	// selection named the newcomer. Through the real seam: the actor's Undo announces a replaced network, the controller hands it to the session
	// (OnNetworkReplaced), and the session drops a selection the network no longer holds - with NO Tick in between.
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("an actor"), Actor)) { return false; }
	Actor->ClearNetwork();
	Actor->StandDefinition = UEntityDefinition::MakeStandTransient();
	IRoadEditTarget* Edit = Actor;
	const int32 Stand = Edit->PlaceStand(FVector2D(0.0, 0.0), 0.0);
	if (!TestTrue(TEXT("setup: a stand placed"), Stand != INDEX_NONE)) { return false; }
	if (!TestTrue(TEXT("setup: and the placement is undoable"), Actor->CanUndo())) { return false; }

	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }
	C->SetTargetForTest(Actor);
	FAlertFocus Focus;
	Focus.Kind = EAlertFocusKind::Entity;
	Focus.Id = Stand;
	Focus.Point = FVector2D::ZeroVector;
	if (!TestTrue(TEXT("setup: the stand is selected, as an alert's Go selects it"), C->SelectAndFocus(Focus))) { return false; }
	if (!TestEqual(TEXT("setup: the selection names the stand"), C->GetSelection().Kind, ESelectionKind::Stand)) { return false; }
	TestTrue(TEXT("and records its slot's generation"), C->GetSelection().Generation != 0);

	if (!TestTrue(TEXT("the undo runs"), Actor->Undo())) { return false; }
	TestFalse(TEXT("undoing the placement drops the selection on the spot - no tick - rather than waiting to be retargeted"), C->GetSelection().IsSet());

	const int32 Again = Edit->PlaceStand(FVector2D(9000.0, 0.0), 0.0);
	TestTrue(TEXT("a stand placed next is issued the slot the undone one had - the very index a stale selection would have named"), Again == Stand);
	TestFalse(TEXT("and is not the selection"), C->GetSelection().IsSet());
	return true;
}

#endif
