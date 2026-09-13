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

	return true;
}

#endif
