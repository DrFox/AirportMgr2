#include "CoreMinimal.h"
#include "BuildActions.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "RoadBuildController.h"
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
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("a world"), World)) { return false; }
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };

	ARoadBuildController* C = World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }

	int32 RanCount = 0;
	FBuildAction Action;
	Action.Id = FName(TEXT("test.action"));
	Action.Execute = [&RanCount](ARoadBuildController&) { ++RanCount; };
	Action.IsActive = [](const ARoadBuildController&) { return false; };

	Action.IsEnabled = [](const ARoadBuildController&) { return false; };
	TestFalse(TEXT("TryRun refuses a disabled action"), Action.TryRun(*C, TEXT("Test")));
	TestEqual(TEXT("Execute did not run while disabled"), RanCount, 0);

	Action.IsEnabled = [](const ARoadBuildController&) { return true; };
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

/**
 * ONE TOGGLE PER SOURCE, WALKED FROM THE ENUM. Spec section 9's
 * AirportMgr.Actions.SnapTogglesAreInTheRegistry: a source added in a later stage without a
 * button is a guide the player cannot switch off, and nothing else would say so.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSnapTogglesAreInTheRegistryTest,
	"AirportMgr.Actions.SnapTogglesAreInTheRegistry",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSnapTogglesAreInTheRegistryTest::RunTest(const FString& Parameters)
{
	const TArray<TPair<SnapGuide::ESource, const TCHAR*>> Expected = {
		{ SnapGuide::ESource::Extending,  TEXT("snap.extending")  },
		{ SnapGuide::ESource::PointAlign, TEXT("snap.pointalign") },
		{ SnapGuide::ESource::Aligned,    TEXT("snap.aligned")    },
		{ SnapGuide::ESource::Collinear,  TEXT("snap.collinear")  },
		{ SnapGuide::ESource::Parallel,   TEXT("snap.parallel")   },
		{ SnapGuide::ESource::Runway,     TEXT("snap.runway")     },
		{ SnapGuide::ESource::World,      TEXT("snap.world")      },
		{ SnapGuide::ESource::Offset,     TEXT("snap.offset")     } };

	// THE TABLE ABOVE IS ITSELF A SECOND LIST, so it is checked against the enum's own size
	// first - otherwise a source added to ESource could be missed by this test as easily as by
	// the registry, which is the failure the test exists to prevent.
	TestEqual(TEXT("every ESource value is covered by this test's own table"),
		Expected.Num(), static_cast<int32>(SnapGuide::ESource::Offset) + 1);

	for (const TPair<SnapGuide::ESource, const TCHAR*>& Pair : Expected)
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

	// AND THE SECTION HAS A NAME. ActionSectionName indexes SectionNames by the enum, so a row
	// added in the wrong slot renames two sections at once and the static_assert cannot see it.
	// SENTENCE CASE, like every other row - "Time", "Tools", "Game".
	TestEqual(TEXT("the Snap section is named"),
		FString(ActionSectionName(EActionSection::Snap)), FString(TEXT("Snap")));

	return true;
}

#endif
