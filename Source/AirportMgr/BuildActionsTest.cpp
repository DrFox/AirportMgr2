#include "CoreMinimal.h"
#include "BuildActions.h"
#include "Misc/AutomationTest.h"
#include "RoadBuildController.h"
#include "Testing/AirsideTestWorld.h"
#include "Tool/BuildSession.h"

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
	TestNull(TEXT("FindAction(Key) is null for an unbound key"), FindAction(EKeys::Escape, false));

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

#endif
