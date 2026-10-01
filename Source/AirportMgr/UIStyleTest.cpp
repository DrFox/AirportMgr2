#include "CoreMinimal.h"
#include "Components/TextBlock.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/AutomationTest.h"
#include "UI/UiButton.h"
#include "UIStyle.h"
#include "BuildActions.h"

#if WITH_DEV_AUTOMATION_TESTS

// NAMED, NOT ANONYMOUS: the module is a unity build.
namespace UIStyleTestFixture
{
	/**
	 * THE CONFIGURED STYLE ASSET MUST BE THERE (2026-09-30 review). Three tests below had a half that skipped - AddInfo and
	 * return true - when DA_UIStyle lacked a button material or the Inter faces, so a missing or emptied asset left them GREEN
	 * having asserted nothing (when the last skipped, EditingTheStyleReachesTheControlFill ran zero assertions). The asset is
	 * committed and Config/DefaultGame.ini names it, so its absence is a defect in the content, not a state to excuse.
	 */
	bool RequireConfigured(FAutomationTestBase& Test, bool bConfigured, const TCHAR* What)
	{
		return Test.TestTrue(FString::Printf(TEXT("DA_UIStyle is configured with %s - without it this half measures nothing"), What), bConfigured);
	}
}

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
	TestTrue(TEXT("Well colour is not transparent"), Style->Well.A > 0.0f);
	TestTrue(TEXT("Ink colour is not transparent"), Style->Ink.A > 0.0f);
	TestTrue(TEXT("Button size is a usable hit target"), Style->ButtonSize >= 32.0f);
	return true;
}

/**
 * Walks the REGISTRY, not a hand-written list, so an action added without an icon fails
 * here rather than rendering as a blank square nobody notices. Same shape as
 * AirportOps.Model.SimClock.SpeedLadderCoversEveryRung.
 *
 * A MISSING ICON MAP FAILS (2026-10): it used to AddInfo and pass, a green run that checked no action's icon.
 * DA_UIStyle is committed content and DefaultGame.ini names it (UIStyleTestFixture::RequireConfigured).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUIStyleIconsTest,
	"AirportMgr.UI.EveryActionResolvesAnIcon",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUIStyleIconsTest::RunTest(const FString& Parameters)
{
	const UUIStyle* Style = UAirportMgrUISettings::ResolveStyle();
	if (!UIStyleTestFixture::RequireConfigured(*this, Style != nullptr && Style->IconsByActionId.Num() > 0, TEXT("an icon map"))) { return false; }

	// The three time controls are drawn as geometric glyphs and need no texture. Exempting
	// them by SECTION rather than by name means adding a fourth time control does not need
	// this test edited.
	//
	// THE SNAP TOGGLES ARE EXEMPT FOR THE SAME REASON, added with them in stage 3: they are
	// WORD buttons - "Extending", "Parallel" - and UBuildBarWidget draws the label whenever
	// IconFor returns null, which is exactly what the time controls rely on. Eight glyphs that
	// each had to say "the line an existing segment lies on" would be worse than the words.
	//
	// BOTH GUIDE SECTIONS, since 2026-09-20: the toggles split into a relation row and a
	// reference row, and "Snap to" is word buttons for the same reason "Snap" is.
	//
	// IconsByActionId is authored CONTENT, so an action with no entry is not a code bug - it
	// is a button that draws its label, and this test is the one place that decides which of
	// those two a section is.
	for (const FBuildAction& Action : BuildActions())
	{
		if (Action.Section == EActionSection::Time
			|| Action.Section == EActionSection::Snap
			|| Action.Section == EActionSection::SnapTo
			// INSPECTOR-ONLY rows draw no bar button, so they need no icon (facility spec §6 deviation 8).
			|| Action.bInspectorOnly)
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

	// A FRESH TEXT BLOCK PER ROLE, matching every real call site (each constructs its own
	// UTextBlock and applies exactly one role to it, once). Reusing one across roles would
	// make a later ApplyText's "fall back to the widget's own font" path read back whatever
	// the PREVIOUS call left on it - a test artifact no production call site can hit.
	auto Fresh = [&]() -> UTextBlock*
	{
		return NewObject<UTextBlock>(GetTransientPackage());
	};

	UTextBlock* Heading = Fresh();
	if (!TestNotNull(TEXT("a text block to paint"), Heading)) { return false; }
	Style->ApplyText(*Heading, EUITextRole::Heading, Style->InkMuted);
	TestEqual(TEXT("Heading takes the style's HeadingSize"), Heading->GetFont().Size, Style->HeadingSize);
	TestEqual(TEXT("Heading is widely spaced, so it reads as a heading and not a short label"),
		Heading->GetFont().LetterSpacing, 120);
	TestEqual(TEXT("colour is the passed-in one, not baked into the role"),
		Heading->GetColorAndOpacity().GetSpecifiedColor(), Style->InkMuted);

	UTextBlock* Label = Fresh();
	if (!TestNotNull(TEXT("a text block to paint"), Label)) { return false; }
	Style->ApplyText(*Label, EUITextRole::Label, Style->Ink);
	TestEqual(TEXT("Label takes the style's LabelSize, distinct from Heading"),
		Label->GetFont().Size, Style->LabelSize);
	// ONLY HEADING IS TRACKED OUT. The per-asset TitleFont/LabelFont this used to defer to are
	// gone (nothing ever set them - UI library step 1); Inter's own spacing is 0, and a Label
	// that inherited Heading's 120 would read as a heading.
	TestEqual(TEXT("Label is not tracked out - only Heading is"), Label->GetFont().LetterSpacing, 0);

	UTextBlock* Clock = Fresh();
	if (!TestNotNull(TEXT("a text block to paint"), Clock)) { return false; }
	Style->ApplyText(*Clock, EUITextRole::Clock, Style->Ink);
	TestEqual(TEXT("Clock takes its own size, kept apart from Title"), Clock->GetFont().Size, Style->ClockSize);
	return true;
}

namespace UIStyleContrast
{
	/** WCAG relative luminance of a LINEAR colour - the slots are stored linear already. */
	double Luminance(const FLinearColor& C) { return 0.2126 * C.R + 0.7152 * C.G + 0.0722 * C.B; }
	double Ratio(const FLinearColor& A, const FLinearColor& B)
	{
		const double LA = Luminance(A), LB = Luminance(B);
		return (FMath::Max(LA, LB) + 0.05) / (FMath::Min(LA, LB) + 0.05);
	}
}

/**
 * THE WHITE GROUND IS ONLY SAFE IF EVERY INK STILL READS ON IT. The old palette was cream on
 * slate; flipping the ground to white flips which slot is the ink, and PanelDark was quietly
 * doing two jobs (a surface AND the text on yellow) - so each ink is pinned against the surface
 * it actually lands on. 4.5:1 is WCAG AA for body text, 3:1 for the muted headings.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUIStyleContrastTest,
	"AirportMgr.UI.EveryInkReadsOnItsSurface",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUIStyleContrastTest::RunTest(const FString& Parameters)
{
	using namespace UIStyleContrast;
	for (const UUIStyle* Style : { GetDefault<UUIStyle>(), UAirportMgrUISettings::ResolveStyle() })
	{
		if (!TestNotNull(TEXT("a style"), Style)) { return false; }
		TestTrue(TEXT("Ink on Surface >= 4.5"), Ratio(Style->Ink, Style->Surface) >= 4.5);
		TestTrue(TEXT("Ink on Well >= 4.5"), Ratio(Style->Ink, Style->Well) >= 4.5);
		TestTrue(TEXT("InkOnAccent on Accent >= 4.5"), Ratio(Style->InkOnAccent, Style->Accent) >= 4.5);
		TestTrue(TEXT("InkMuted on Surface >= 3"), Ratio(Style->InkMuted, Style->Surface) >= 3.0);
		TestTrue(TEXT("InkMuted on Well >= 3"), Ratio(Style->InkMuted, Style->Well) >= 3.0);
		TestTrue(TEXT("Surface and Well are distinguishable"), !Style->Surface.Equals(Style->Well));
		// A Secondary button's DETAIL (a variant's second line, a Land row's refusal) is InkMuted
		// on Control - the one pair the first cut of this test missed (final review 2026-09-28).
		TestTrue(TEXT("InkMuted on Control >= 3"), Ratio(Style->InkMuted, Style->Control) >= 3.0);
		// Every filled kind's label, through the rule that picks it, at the 3:1 a button label needs.
		for (const EUiButtonKind Kind : { EUiButtonKind::Primary, EUiButtonKind::Secondary, EUiButtonKind::Danger })
		{
			const FUiButtonLook Look = UUiButton::LookFor(*Style, Kind, true, false);
			TestTrue(FString::Printf(TEXT("kind %d label on its fill >= 3"), static_cast<int32>(Kind)), Ratio(Look.Ink, Look.Fill) >= 3.0);
		}
	}
	return true;
}

/**
 * THE FILL IS WHITE, ALWAYS - the caller's SetBackgroundColor is the one place a state colour is
 * chosen (the rule UOfferInboxWidget::MakeAnswerButton's comment states). A fill that carried
 * its own colour would multiply with that choice and every button would draw the wrong shade.
 * And a style with no material (the CDO: a fresh checkout) must still round its corners.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUIStyleControlFillTest,
	"AirportMgr.UI.ControlFillIsWhiteAndAlwaysRounded",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUIStyleControlFillTest::RunTest(const FString& Parameters)
{
	const UUIStyle* Cdo = GetDefault<UUIStyle>();
	const FSlateBrush Fallback = Cdo->ControlFill();
	TestEqual(TEXT("no material: a rounded box"), Fallback.DrawAs, ESlateBrushDrawType::RoundedBox);
	TestTrue(TEXT("no material: corner is ControlRadius"),
		FMath::IsNearlyEqual(static_cast<float>(Fallback.OutlineSettings.CornerRadii.X), Cdo->ControlRadius));
	TestEqual(TEXT("no material: tint is white"), Fallback.TintColor.GetSpecifiedColor(), FLinearColor::White);

	const UUIStyle* Style = UAirportMgrUISettings::ResolveStyle();
	if (!UIStyleTestFixture::RequireConfigured(*this, Style != Cdo && !Style->ButtonMaterial.IsNull(), TEXT("a ButtonMaterial"))) { return false; }
	const FSlateBrush Fill = Style->ControlFill();
	TestEqual(TEXT("material: drawn as an image"), Fill.DrawAs, ESlateBrushDrawType::Image);
	TestNotNull(TEXT("material: has a resource"), Fill.GetResourceObject());
	TestEqual(TEXT("material: tint is white"), Fill.TintColor.GetSpecifiedColor(), FLinearColor::White);
	TestTrue(TEXT("the SAME instance twice - one cached MID, not one per button"),
		Style->ControlFill().GetResourceObject() == Fill.GetResourceObject());
	return true;
}

/**
 * INTER, OR THE ENGINE FONT - NEVER NOTHING. The composite is built from two faces the style
 * asset points at; a style without them (the CDO) must still draw text in the widget's own
 * font rather than an empty FSlateFontInfo, which renders nothing at all.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUIStyleInterTest,
	"AirportMgr.UI.TextDrawsInInterWithWeightByRole",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUIStyleInterTest::RunTest(const FString& Parameters)
{
	UTextBlock* Plain = NewObject<UTextBlock>();
	GetDefault<UUIStyle>()->ApplyText(*Plain, EUITextRole::Body, FLinearColor::Black);
	TestTrue(TEXT("no faces: still a valid font"), Plain->GetFont().HasValidFont());

	const UUIStyle* Style = UAirportMgrUISettings::ResolveStyle();
	if (!UIStyleTestFixture::RequireConfigured(*this, !Style->FontRegular.IsNull() && !Style->FontSemiBold.IsNull(), TEXT("the Inter Regular and SemiBold faces"))) { return false; }
	UTextBlock* Title = NewObject<UTextBlock>();
	Style->ApplyText(*Title, EUITextRole::Title, Style->Ink);
	UTextBlock* Body = NewObject<UTextBlock>();
	Style->ApplyText(*Body, EUITextRole::Body, Style->Ink);
	TestTrue(TEXT("Title uses the composite"), Title->GetFont().CompositeFont.IsValid());
	TestEqual(TEXT("Title is SemiBold"), Title->GetFont().TypefaceFontName, FName(TEXT("SemiBold")));
	TestEqual(TEXT("Body is Regular"), Body->GetFont().TypefaceFontName, FName(TEXT("Regular")));
	TestTrue(TEXT("one composite for the whole UI"), Title->GetFont().CompositeFont == Body->GetFont().CompositeFont);
	return true;
}

/**
 * ONE DIMMING, NOT TWO. Slate multiplies every DISABLED widget's content alpha by 0.45 on top of
 * whatever colour it was given (SlateElementPixelShader.usf, DrawDisabledEffect). UUiButton::LookFor
 * already dims a disabled label to InkMuted, so with the engine's pass left on, a refused Land
 * row's reason - text the player must read - rendered about 1.5:1 on its fill (PIE capture
 * 2026-09-28: (175,183,190) on (220,224,228)). The cvar strips the effect for project content only.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUIDisabledEffectOffTest,
	"AirportMgr.UI.DisabledTextIsDimmedOnceNotTwice",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUIDisabledEffectOffTest::RunTest(const FString& Parameters)
{
	const IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("Slate.ApplyDisabledEffectOnWidgets"));
	if (!TestNotNull(TEXT("the engine still has the cvar"), CVar)) { return false; }
	TestFalse(TEXT("Slate's own disabled dimming is off - LookFor's InkMuted is the only one"), CVar->GetBool());
	return true;
}

/**
 * THE STYLE IS EDITED IN A DETAILS PANEL - that is the whole point of the asset - so its caches
 * must not outlive an edit. ControlFill's dynamic instance used to keep the radius it was built
 * with until an editor restart, while UUiRow read the new ControlRadius at once: rows and buttons
 * disagreeing in the same frame (final review 2026-09-28).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUIStyleEditInvalidatesTest,
	"AirportMgr.UI.EditingTheStyleReachesTheControlFill",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUIStyleEditInvalidatesTest::RunTest(const FString& Parameters)
{
#if WITH_EDITOR
	const UUIStyle* Configured = UAirportMgrUISettings::ResolveStyle();
	if (!UIStyleTestFixture::RequireConfigured(*this, !Configured->ButtonMaterial.IsNull(), TEXT("a ButtonMaterial"))) { return false; }
	UUIStyle* Style = NewObject<UUIStyle>();
	Style->ButtonMaterial = Configured->ButtonMaterial;
	Style->ControlRadius = 5.0f;
	auto RadiusOf = [](const FSlateBrush& Brush)
	{
		float R = -1.0f;
		if (const UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(Brush.GetResourceObject()))
		{
			MID->GetScalarParameterValue(FName(TEXT("RadiusPx")), R);
		}
		return R;
	};
	TestEqual(TEXT("built with the radius it had"), RadiusOf(Style->ControlFill()), 5.0f);
	Style->ControlRadius = 9.0f;
	FProperty* Prop = UUIStyle::StaticClass()->FindPropertyByName(TEXT("ControlRadius"));
	FPropertyChangedEvent Changed(Prop);
	Style->PostEditChangeProperty(Changed);
	TestEqual(TEXT("an edit reaches the fill without a restart"), RadiusOf(Style->ControlFill()), 9.0f);
#endif
	return true;
}
#endif
