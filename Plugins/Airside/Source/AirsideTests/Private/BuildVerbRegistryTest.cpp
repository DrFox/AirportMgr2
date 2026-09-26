#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Tool/BuildSession.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * ISSUE #304: `grep GestureMode AirsideEditor/Private/*.cpp` was 0 - nothing in the editor
 * module could reach Remove/Insert/Edit at all, though BuildActions.cpp had carried them as bar
 * rows since the mode was unified onto FBuildSession. BuildVerbRegistry() is the ONE table both
 * BuildActions.cpp and FRoadBuildEdModeCommands::RegisterCommands now read - this test pins the
 * registry's own behaviour, independent of either driver, the same way Airside.Tool.BuildSession
 * pins ToolRegistry() before either driver touches it.
 *
 * IN AirsideTests, not AirsideEditor: this table lives in Airside, which AirsideTests already
 * depends on alone - see Airside.Editor.ToolCommandsMatchRegistry's own comment for why the
 * editor-facing parity check (label/key against the registry) still has to live in the editor
 * module instead of here.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildVerbRegistryTest,
	"Airside.Tool.BuildVerbRegistry",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildVerbRegistryTest::RunTest(const FString& Parameters)
{
	const TConstArrayView<FBuildVerbRegistration> Registry = BuildVerbRegistry();

	// 1. EXACTLY the sticky EGestureMode trio, by id - Build/Cancel/Guidelines are deliberately
	// NOT here (see FBuildVerbRegistration's own header comment for why). A fourth sticky mode
	// landing in the registry with no name added to this set is what this pins; a mode landing
	// OUTSIDE the registry is what Airside.Editor.VerbCommandsMatchRegistry pins instead.
	const TSet<FName> Expected = { FName(TEXT("Remove")), FName(TEXT("Insert")), FName(TEXT("EditMode")) };
	TestEqual(TEXT("exactly the sticky EGestureMode trio"), Registry.Num(), Expected.Num());
	for (const FBuildVerbRegistration& Verb : Registry)
	{
		TestTrue(*FString::Printf(TEXT("'%s' is one of the expected verbs"), *Verb.Id.ToString()),
			Expected.Contains(Verb.Id));
	}

	// 2. No two verbs share a key - the same failure Airside.Tool.BuildSession pins for
	// ToolRegistry(), applied here. EKeys::Invalid entries (Remove/Insert, which rely on a HELD
	// Ctrl/Shift rather than a chord) are exempt, the same way tool-less bar rows are elsewhere.
	for (int32 Outer = 0; Outer < Registry.Num(); ++Outer)
	{
		if (Registry[Outer].Key == EKeys::Invalid) { continue; }
		for (int32 Inner = Outer + 1; Inner < Registry.Num(); ++Inner)
		{
			if (Registry[Inner].Key == EKeys::Invalid) { continue; }
			TestFalse(*FString::Printf(TEXT("verbs %d and %d must not share a key"), Outer, Inner),
				Registry[Outer].Key == Registry[Inner].Key);
		}
	}

	// 3. Apply ACTUALLY flips FBuildSession's gesture mode, and IsActive agrees once it has -
	// the behaviour this table exists to make reachable from a second driver (the editor mode),
	// pinned directly rather than only through either driver's own composition test.
	FBuildSession Session;
	Session.SelectTool(1);   // Taxiway: has edit handles, so Edit's IsEnabled can read true below.

	for (const FBuildVerbRegistration& Verb : Registry)
	{
		Session.ToggleGestureMode(EGestureMode::Build);   // a known starting point, like ClickModifierTest's own.

		FToolContext Context;
		Verb.Apply(Session, Context);

		const EGestureMode ExpectedMode =
			Verb.Id == FName(TEXT("Remove")) ? EGestureMode::Remove :
			Verb.Id == FName(TEXT("Insert")) ? EGestureMode::Insert : EGestureMode::Edit;
		TestEqual(*FString::Printf(TEXT("'%s'.Apply enters its own mode"), *Verb.Id.ToString()),
			Session.GetGestureMode(), ExpectedMode);
		TestTrue(*FString::Printf(TEXT("'%s'.IsActive agrees once entered"), *Verb.Id.ToString()),
			Verb.IsActive(Session));
	}

	// 4. Edit's IsEnabled reads FToolRegistration::EditHandles - greyed when the lit tool
	// exposes nothing (Select, index 0) and lit when it does (Taxiway, index 1) - the exact rule
	// ARoadBuildController::ActiveToolHasEditHandles already applies for PIE's own bar, now
	// reachable from the registry directly rather than only through a controller method.
	const FBuildVerbRegistration* Edit = nullptr;
	for (const FBuildVerbRegistration& Verb : Registry)
	{
		if (Verb.Id == FName(TEXT("EditMode")))
		{
			Edit = &Verb;
			break;
		}
	}
	if (TestNotNull(TEXT("Edit is in the registry"), Edit))
	{
		Session.SelectTool(0);   // Select: no edit handles.
		TestFalse(TEXT("greyed when the lit tool exposes nothing"), Edit->IsEnabled(Session));

		Session.SelectTool(1);   // Taxiway: AirsideNode handles.
		TestTrue(TEXT("lit when the lit tool exposes handles"), Edit->IsEnabled(Session));
	}

	return true;
}

#endif
