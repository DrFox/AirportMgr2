#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "UIStyle.h"
#include "BuildActions.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE LESSON FROM ResolveDefaultScenario, which returned null when nothing was configured.
 * Every caller guarded with `if (...)`, so the "built-in defaults" its log promised were
 * applied to nothing, and the project silently ran on a clock that started at midnight for
 * weeks. A resolver that can return null is a resolver whose defaults never arrive.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUIStyleResolvesTest,
	"AirportMgr.UI.StyleResolverIsNeverNull",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUIStyleResolvesTest::RunTest(const FString& Parameters)
{
	const UUIStyle* Style = UAirportMgrUISettings::ResolveStyle();
	if (!TestNotNull(TEXT("ResolveStyle never returns null, even with no asset configured"), Style))
	{
		return false;
	}

	// The CDO's own declared defaults must be usable, not zeroes: a style of transparent
	// black would render a bar that is technically present and invisible.
	TestTrue(TEXT("Panel colour is not transparent"), Style->Panel.A > 0.0f);
	TestTrue(TEXT("Text colour is not transparent"), Style->Text.A > 0.0f);
	TestTrue(TEXT("Button size is a usable hit target"), Style->ButtonSize >= 32.0f);
	return true;
}

/**
 * Walks the REGISTRY, not a hand-written list, so an action added without an icon fails
 * here rather than rendering as a blank square nobody notices. Same shape as
 * AirportOps.Model.SimClock.SpeedLadderCoversEveryRung.
 *
 * Skipped when no style asset is configured: the CDO carries no icon map, and failing then
 * would make a fresh checkout fail for want of content rather than for a defect.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUIStyleIconsTest,
	"AirportMgr.UI.EveryActionResolvesAnIcon",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUIStyleIconsTest::RunTest(const FString& Parameters)
{
	const UUIStyle* Style = UAirportMgrUISettings::ResolveStyle();
	if (Style == nullptr || Style->IconsByActionId.Num() == 0)
	{
		AddInfo(TEXT("No style asset configured; icon coverage not checked"));
		return true;
	}

	// The three time controls are drawn as geometric glyphs and need no texture. Exempting
	// them by SECTION rather than by name means adding a fourth time control does not need
	// this test edited.
	for (const FBuildAction& Action : BuildActions())
	{
		if (Action.Section == EActionSection::Time)
		{
			continue;
		}
		TestTrue(*FString::Printf(TEXT("action '%s' has an icon mapped, so the bar has "
			"something to draw for it"), *Action.Id.ToString()),
			Style->IconsByActionId.Contains(Action.Id));
	}
	return true;
}

#endif
