#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "UI/UiRow.h"
#include "UIStyle.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * A ROW IS A WELL, NOT A SLAB. The offer cards were drawn in Panel over a PanelDark ground; on a
 * white window the row must be the recessed Well, rounded like a control, or rows and window
 * merge into one surface and an offer reads as a line of text instead of a thing to answer.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiRowTest, "AirportMgr.UI.Row.IsARoundedWell",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUiRowTest::RunTest(const FString& Parameters)
{
	const UUIStyle& S = *GetDefault<UUIStyle>();
	UUiRow* Row = NewObject<UUiRow>();
	Row->Build(S, FMargin(10.0f, 8.0f));
	const FSlateBrush& Brush = Row->Background;
	TestEqual(TEXT("rounded"), Brush.DrawAs, ESlateBrushDrawType::RoundedBox);
	TestEqual(TEXT("Well"), Brush.TintColor.GetSpecifiedColor(), S.Well);
	TestTrue(TEXT("control radius"), FMath::IsNearlyEqual(static_cast<float>(Brush.OutlineSettings.CornerRadii.X), S.ControlRadius));
	TestTrue(TEXT("padding as asked"), Row->GetPadding() == FMargin(10.0f, 8.0f));
	return true;
}

#endif
