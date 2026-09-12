#include "CoreMinimal.h"
#include "Components/TextBlock.h"
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

/**
 * THE SEAM ApplyText REPLACES, EXERCISED. Eleven call sites used to build this
 * fallback/size/letter-spacing/colour recipe by hand (issue #89); this proves the one
 * function that now does it actually reaches a real UTextBlock, per role, not just that the
 * per-role UPROPERTYs exist and are never read - the "declared but never consumed" bug
 * CLAUDE.md names three times, in a new place.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUIStyleApplyTextTest,
	"AirportMgr.UI.ApplyTextSetsRoleFontAndColour",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUIStyleApplyTextTest::RunTest(const FString& Parameters)
{
	const UUIStyle* Style = UAirportMgrUISettings::ResolveStyle();
	if (!TestNotNull(TEXT("a style"), Style)) { return false; }

	UTextBlock* Text = NewObject<UTextBlock>(GetTransientPackage());
	if (!TestNotNull(TEXT("a text block to paint"), Text)) { return false; }

	Style->ApplyText(*Text, EUITextRole::Heading, Style->TextMuted);
	TestEqual(TEXT("Heading takes the style's HeadingSize"), Text->GetFont().Size, Style->HeadingSize);
	TestEqual(TEXT("Heading is widely spaced, so it reads as a heading and not a short label"),
		Text->GetFont().LetterSpacing, 120);
	TestEqual(TEXT("colour is the passed-in one, not baked into the role"),
		Text->GetColorAndOpacity().GetSpecifiedColor(), Style->TextMuted);

	Style->ApplyText(*Text, EUITextRole::Label, Style->Text);
	TestEqual(TEXT("Label takes the style's LabelSize, distinct from Heading"),
		Text->GetFont().Size, Style->LabelSize);
	TestEqual(TEXT("Label carries no extra letter-spacing"), Text->GetFont().LetterSpacing, 0);

	Style->ApplyText(*Text, EUITextRole::Clock, Style->Text);
	TestEqual(TEXT("Clock takes its own size, kept apart from Title"), Text->GetFont().Size, Style->ClockSize);
	return true;
}

#endif
