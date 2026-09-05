#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Tool/BuildSession.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildSessionTest,
	"Airside.Tool.BuildSession",
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

	return true;
}

#endif
