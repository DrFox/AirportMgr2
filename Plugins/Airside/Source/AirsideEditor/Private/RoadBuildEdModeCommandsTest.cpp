#include "CoreMinimal.h"
#include "InputCoreTypes.h"
#include "Misc/AutomationTest.h"
#include "RoadBuildEdModeCommands.h"
#include "Tool/BuildSession.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The one seam that can still drift: ToolCommandsInOrder() is a hand-written UI_COMMAND list
 * beside ToolRegistry(), and nothing but opening the mode used to compare them.
 *
 * IN AirsideEditor, not AirsideTests, deliberately. AirsideTests depends on Airside alone -
 * Model/Present/Tool are tested with no editor module loaded - and making it depend on an
 * editor module to reach one class would invert the direction the whole plugin is built on.
 * A test that lives with the code it guards costs nothing and needs no new dependency.
 *
 * This test would have failed for the whole slice in which the registry carried Road and Fuel
 * depot and this list did not: key 9 was unbound, so it left the previous tool running and a
 * player drew a TAXIWAY - an Aircraft-only guideline that no service lane can join.
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

#endif
