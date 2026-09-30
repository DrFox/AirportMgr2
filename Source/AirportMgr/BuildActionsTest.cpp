#include "CoreMinimal.h"
#include "UIStyle.h"
#include "UI/UiMenuButton.h"
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
#include "Entities/EntityDefinition.h"
#include "BuildActions.h"
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
		TestFalse(TEXT("no runway selected: the flip is disabled"), C->CanFlipSelectedRunway());
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
		// world axes. See BuildActions.cpp, and ERelation::Parallel's own comment.
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
	UBuildHudLayer* Hud = C->GetHudForTest();
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
		UBuildHudLayer* Hud = C->GetHudForTest();
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
 * CTRL CHORDS WAIT TOO (4b final review, Important 6): Ctrl+Z behind an open dialog would undo the
 * airport. Both key handlers ask the one predicate, so this test covers the chord handler, which a
 * headless test cannot drive (it polls WasInputKeyJustPressed).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBuildActionsModalChordTest, "AirportMgr.Actions.ChordsWaitUnderModal",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildActionsModalChordTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	ARoadBuildController* C = BuildActionsModalTest::SpawnWithSettings(TestWorld);
	if (!TestNotNull(TEXT("a controller with Settings"), C)) { return false; }
	const FBuildAction* Undo = FindAction(FName(TEXT("edit.undo")));
	const FBuildAction* Settings = FindAction(SettingsActionId());
	if (!TestTrue(TEXT("undo and settings are registered"), Undo != nullptr && Settings != nullptr)) { return false; }
	TestTrue(TEXT("control: undo is a Ctrl chord"), Undo->bRequiresCtrl);
	TestFalse(TEXT("no modal: undo runs"), C->KeyWaitsForModal(*Undo));
	C->ToggleSettings();
	TestTrue(TEXT("under the modal: Ctrl+Z waits"), C->KeyWaitsForModal(*Undo));
	TestFalse(TEXT("and Settings' own key does not"), C->KeyWaitsForModal(*Settings));
	return true;
}

/**
 * THE GRID'S ORIENTATION TOGGLE, ON H - the one snap toggle with a key, by the player's request
 * (grid-follows-snap design): it is switched mid-gesture, which is the NO KEYS rule's own test.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGridOrientButtonIsInTheRegistryTest,
	"AirportMgr.Actions.GridOrientButtonIsOnH",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGridOrientButtonIsInTheRegistryTest::RunTest(const FString& Parameters)
{
	const FBuildAction* Action = FindAction(FName(TEXT("snap.gridorient")));
	if (!TestNotNull(TEXT("snap.gridorient is registered"), Action)) { return false; }
	TestEqual(TEXT("in the Snap section"), Action->Section, EActionSection::Snap);
	TestTrue(TEXT("on H"), Action->Key == EKeys::H);
	TestFalse(TEXT("no Ctrl"), Action->bRequiresCtrl);
	TestTrue(TEXT("FindAction(H) is this action - the binding loop reads the same table"),
		FindAction(EKeys::H, false) == Action);
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
	for (const TCHAR* Id : { TEXT("selection.buy_module"), TEXT("selection.buy_vehicle"), TEXT("selection.sell_vehicle") })
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
		Actor->MinimumRunwayLength = 100.0;
		Actor->PlaceRunway(FVector2D(0.0, -50000.0), FVector2D(6000.0, -50000.0), TestProfiles::Runway());
		UEntityDefinition* StandDef = UEntityDefinition::MakeStandTransient();
		Actor->Network->PlaceEntity(StandDef, StandDef->Anchors, FVector2D(0.0, 90000.0), 0.0, 3600.0, StandDef->PoseRole, StandDef->Trucks);
		UOpsRuntime* Runtime = NewObject<UOpsRuntime>();
		Runtime->Attach(Actor);
		for (int32 Tick = 0; Tick < 3; ++Tick) { Runtime->Tick(0.0); }
		return Runtime;
	}

	/** An accepted flight on Runtime's board, its lead long enough never to come due in a test. */
	UFlight* AirportActionAccepted(UOpsRuntime& Runtime, ARoadNetworkActor& Actor)
	{
		UFlight* Flight = NewObject<UFlight>(GetTransientPackage());
		Flight->Airframe.Wingspan = 3400.0;
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
	TestEqual(TEXT("dismissed: the flight is still coming"), Flight->Phase, EFlightPhase::Accepted);

	// ARMED, THEN CONFIRMED.
	Menu->BuildMenu();
	Chosen = Menu->ChosenCountForTest();
	Menu->Choose(0);
	Menu->Choose(0);
	ChooseIfChosen(Chosen);
	Runtime->Tick(0.0);
	TestEqual(TEXT("confirmed: closed"), Runtime->GetAirport()->Status(), EAirportStatus::ClosedByPlayer);
	TestEqual(TEXT("confirmed: the flight is cancelled"), Flight->Phase, EFlightPhase::Cancelled);

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
	Ground->Phase = EFlightPhase::TaxiIn;
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
	return true;
}

#endif
