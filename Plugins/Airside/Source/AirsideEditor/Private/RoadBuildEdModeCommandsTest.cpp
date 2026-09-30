#include "CoreMinimal.h"
#include "Framework/Commands/InputBindingManager.h"
#include "InputCoreTypes.h"
#include "Misc/AutomationTest.h"
#include "RoadBuildEdMode.h"
#include "RoadBuildEdModeCommands.h"
#include "Tool/BuildSession.h"
#include "Tool/SnapToggleRegistry.h"

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
		// key to compare there; only Edit (M) has one to check against drift. NO KEY IS NOT NO
		// WAY IN: whether a keyless command is DRAWN is EveryCommandIsReachable's question below -
		// this test stopping here is how #304 closed with Remove/Insert still unreachable (#440).
		if (Registry[Index].Key != EKeys::Invalid)
		{
			TestEqual(*FString::Printf(TEXT("verb %d is on the same key as at runtime"), Index),
				Command->GetDefaultChord(EMultipleKeyBindingIndex::Primary).Key.GetFName(),
				Registry[Index].Key.GetFName());
		}
	}

	return true;
}

/**
 * EVERY COMMAND IS REACHABLE: drawn as a button by the toolkit's own palette consumer, or bound to
 * a default chord - issue #440's pin, and the one VerbCommandsMatchRegistry above never was. That
 * test asserts the commands EXIST and are named after the registry; its own comment on Remove and
 * Insert ("EKeys::Invalid ... no key to compare there") is where closed #304 stopped looking. A
 * keyless command in no palette has neither a key nor a button, and IsVerbActive was lighting a
 * toggle nothing drew.
 *
 * WHAT THE CONSUMER READS, not what is declared (CLAUDE.md "Check where a list is CONSUMED"):
 * URoadBuildEdMode::DrawnPalettesForTest runs the engine's FModeToolkit::BuildToolPalette for
 * each name the toolkit's GetToolPaletteNames gives, into a real toolbar builder - so a palette
 * that GetModeCommands declares and GetToolPaletteNames never asks for (this mode's first bug of
 * this shape, the stock toolkit names none) draws nothing here too.
 *
 * WRITTEN RED FIRST (2026-09-30), against one Tools palette: "Remove is reachable" and "Insert is
 * reachable" failed, and so did every keyless snap toggle's line. RED AGAIN once it enumerated the
 * binding context: a scratch keyless command registered in RegisterCommands and put in no list or
 * palette failed both the control count and its own "is reachable" line, and was then removed.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadBuildEdModeEveryCommandReachableTest,
	"Airside.Editor.EveryCommandIsReachable",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadBuildEdModeEveryCommandReachableTest::RunTest(const FString& Parameters)
{
	if (!TestTrue(TEXT("the command set is registered"), FRoadBuildEdModeCommands::IsRegistered()))
	{
		return false;
	}

	// THE CASE THE PALETTE EXISTS FOR, asserted rather than assumed: were every verb to gain a
	// chord, the "or a chord" half would pass this test on its own and the palette half would be
	// measuring nothing (memory: a-green-test-may-measure-nothing).
	bool bKeylessVerb = false;
	for (const FBuildVerbRegistration& Verb : BuildVerbRegistry())
	{
		bKeylessVerb |= !Verb.Key.IsValid();
	}
	TestTrue(TEXT("BuildVerbRegistry has a keyless entry (Remove/Insert are EKeys::Invalid) - the palette's case"), bKeylessVerb);

	URoadBuildEdMode* Mode = NewObject<URoadBuildEdMode>(GetTransientPackage());
	if (!TestNotNull(TEXT("an ed mode"), Mode))
	{
		return false;
	}
	// CreateToolkit alone, as ToolCommandBindingSurvivesRegisterTool does: Enter() needs a viewport.
	Mode->CreateToolkit();
	const TArray<TPair<FName, TArray<TSharedPtr<const FUICommandInfo>>>> Drawn = Mode->DrawnPalettesForTest();
	TestEqual(TEXT("the toolkit asks for every palette the command set declares"),
		Drawn.Num(), FRoadBuildEdModeCommands::Palettes().Num());

	// EVERY COMMAND THE BINDING CONTEXT HOLDS, from the input binding manager - not a list typed
	// here (#468's review). A hand-built list is the #304 shape one more time: a UI_COMMAND added to
	// RegisterCommands and appended to no palette and no list would pass a test that only walks the
	// lists it was told about. The context is what the editor itself registers and binds keys from.
	const FRoadBuildEdModeCommands& Commands = FRoadBuildEdModeCommands::Get();
	TArray<TSharedPtr<FUICommandInfo>> Every;
	FInputBindingManager::Get().GetCommandInfosFromContext(Commands.GetContextName(), Every);

	// CONTROL: the lists this class hands out cover the same commands, so a count that moves means a
	// command registered with no accessor - which the loop below then shows is also undrawn.
	const int32 Listed = Commands.ToolCommandsInOrder().Num() + Commands.VerbCommandsInOrder().Num()
		+ 2 /* Build, CancelGesture */ + Commands.SnapCommandsInOrder().Num();
	TestEqual(TEXT("control: the binding context holds exactly the commands this class's lists name"),
		Every.Num(), Listed);
	TestEqual(TEXT("control: every snap toggle has a command to walk"),
		Commands.SnapCommandsInOrder().Num(), SnapToggleRegistry().Num());

	for (const TSharedPtr<FUICommandInfo>& Command : Every)
	{
		if (!TestTrue(TEXT("a registered command"), Command.IsValid()))
		{
			continue;
		}
		const FString Name = Command->GetCommandName().ToString();
		int32 Buttons = 0;
		for (const TPair<FName, TArray<TSharedPtr<const FUICommandInfo>>>& Palette : Drawn)
		{
			for (const TSharedPtr<const FUICommandInfo>& Button : Palette.Value)
			{
				Buttons += Button == Command ? 1 : 0;
			}
		}
		const bool bChord = Command->GetDefaultChord(EMultipleKeyBindingIndex::Primary).IsValidChord();

		// THE PIN: a button or a key, or the player cannot reach it at all.
		TestTrue(*FString::Printf(TEXT("%s is reachable: drawn in a palette, or bound to a default chord"), *Name),
			Buttons > 0 || bChord);
		// AND THE PALETTES' OWN PROMISE (FRoadBuildEdModeCommands::Palettes): one button each, so
		// no control is reachable only by a key the player has to already know.
		TestEqual(*FString::Printf(TEXT("%s is drawn in exactly one palette"), *Name), Buttons, 1);
	}

	return true;
}

#endif
