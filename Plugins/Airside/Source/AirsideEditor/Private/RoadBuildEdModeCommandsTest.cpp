#include "CoreMinimal.h"
#include "InputCoreTypes.h"
#include "Misc/AutomationTest.h"
#include "RoadBuildEdModeCommands.h"
#include "Tool/BuildSession.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * ToolCommandsInOrder() is now BUILT FROM ToolRegistry() (issue #105 item 10), replacing a
 * hand-written UI_COMMAND list that used to sit beside it and could drift silently. This test
 * predates that move and stays as the regression guard: it still fails if RegisterCommands
 * ever goes back to a hand-typed list, or a future entry skips FUICommandInfo::MakeCommandInfo.
 *
 * IN AirsideEditor, not AirsideTests, deliberately. AirsideTests depends on Airside alone -
 * Model/Present/Tool are tested with no editor module loaded - and making it depend on an
 * editor module to reach one class would invert the direction the whole plugin is built on.
 * A test that lives with the code it guards costs nothing and needs no new dependency.
 *
 * This test would have failed for the whole slice in which the registry carried Road and Fuel
 * depot and the old hand-written list did not: key 9 was unbound, so it left the previous
 * tool running and a player drew a TAXIWAY - an Aircraft-only guideline no service lane can
 * join.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadBuildEdModeCommandsTest,
	"Airside.Editor.ToolCommandsMatchRegistry",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadBuildEdModeCommandsTest::RunTest(const FString& Parameters)
{
	// Registered by FAirsideEditorModule::StartupModule. Asserted rather than assumed,
	// because Get() on an unregistered TCommands asserts inside the engine and the failure
	// would read as an engine crash rather than as this module not having started.
	if (!TestTrue(TEXT("the command set is registered"), FRoadBuildEdModeCommands::IsRegistered()))
	{
		return false;
	}

	const TArray<TSharedPtr<FUICommandInfo>> ToolCommands =
		FRoadBuildEdModeCommands::Get().ToolCommandsInOrder();
	const TConstArrayView<FToolRegistration> Registry = ToolRegistry();

	// COUNT FIRST, because URoadBuildEdMode::Enter walks Min() of the two: a short command
	// list does not fail loudly, it silently drops the tools past its end.
	if (!TestEqual(TEXT("every registry tool has an editor command"),
		ToolCommands.Num(), Registry.Num()))
	{
		return false;
	}

	for (int32 Index = 0; Index < Registry.Num(); ++Index)
	{
		const TSharedPtr<FUICommandInfo>& Command = ToolCommands[Index];
		if (!TestTrue(*FString::Printf(TEXT("tool %d has a command"), Index), Command.IsValid()))
		{
			continue;
		}

		// BY LABEL, not by count: seven of each, wrongly paired, still passes the count.
		// The label is the human-authored UI_COMMAND string, so it says which tool the
		// palette THINKS index Index is, independent of the registry's FKey and Make lambda.
		TestEqual(*FString::Printf(TEXT("tool %d's command names the registry's tool"), Index),
			Command->GetLabel().ToString(), Registry[Index].Name.ToString());

		// THE SAME KEY IN BOTH DRIVERS. A tool that changed number between the editor and
		// play would be worse than having no shortcut at all - and an editor key bound to
		// nothing is worse still, because it leaves the previous tool drawing.
		//
		// The DEFAULT chord, not the active one: the active chord carries whatever the user
		// has since rebound in Editor Preferences, and this is asserting what the code
		// declares, not what one machine's saved keybindings happen to say.
		// Compared as FNames because TestEqual has no FKey overload, and a bool assertion
		// would report "expected true, got false" without saying WHICH key it found.
		TestEqual(*FString::Printf(TEXT("tool %d is on the same key as at runtime"), Index),
			Command->GetDefaultChord(EMultipleKeyBindingIndex::Primary).Key.GetFName(),
			Registry[Index].Key.GetFName());
	}

	return true;
}

/**
 * THE TWIN OF FRoadBuildEdModeCommandsTest ABOVE, for BuildVerbRegistry() instead of
 * ToolRegistry() - issue #304. VerbCommandsInOrder() is BUILT FROM BuildVerbRegistry() the
 * identical way ToolCommandsInOrder() is built from ToolRegistry() (same MakeCommandInfo loop,
 * same reason: a hand-typed second list can drift, and here there had never been a FIRST list at
 * all - `grep GestureMode AirsideEditor/Private/*.cpp` was 0 before this.
 *
 * WRITTEN RED FIRST: before FRoadBuildEdModeCommands carried VerbCommands or RegisterCommands
 * looped over BuildVerbRegistry(), this test did not compile - VerbCommandsInOrder() and
 * BuildVerbRegistry() did not exist. That is this table's own version of "the list nothing
 * consumes yet", the same failure mode ToolCommandsMatchRegistry's own comment describes for
 * key 9 going unbound.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadBuildEdModeVerbCommandsTest,
	"Airside.Editor.VerbCommandsMatchRegistry",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadBuildEdModeVerbCommandsTest::RunTest(const FString& Parameters)
{
	if (!TestTrue(TEXT("the command set is registered"), FRoadBuildEdModeCommands::IsRegistered()))
	{
		return false;
	}

	const TArray<TSharedPtr<FUICommandInfo>> VerbCommands =
		FRoadBuildEdModeCommands::Get().VerbCommandsInOrder();
	const TConstArrayView<FBuildVerbRegistration> Registry = BuildVerbRegistry();

	if (!TestEqual(TEXT("every registry verb has an editor command"),
		VerbCommands.Num(), Registry.Num()))
	{
		return false;
	}

	for (int32 Index = 0; Index < Registry.Num(); ++Index)
	{
		const TSharedPtr<FUICommandInfo>& Command = VerbCommands[Index];
		if (!TestTrue(*FString::Printf(TEXT("verb %d has a command"), Index), Command.IsValid()))
		{
			continue;
		}

		TestEqual(*FString::Printf(TEXT("verb %d's command names the registry's verb"), Index),
			Command->GetLabel().ToString(), Registry[Index].Name.ToString());

		// Remove/Insert are EKeys::Invalid - a HELD Ctrl/Shift, not a chord - so there is no
		// key to compare there; only Edit (M) has one to check against drift.
		if (Registry[Index].Key != EKeys::Invalid)
		{
			TestEqual(*FString::Printf(TEXT("verb %d is on the same key as at runtime"), Index),
				Command->GetDefaultChord(EMultipleKeyBindingIndex::Primary).Key.GetFName(),
				Registry[Index].Key.GetFName());
		}
	}

	return true;
}

#endif
