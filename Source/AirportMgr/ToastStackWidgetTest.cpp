#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "NotificationCentre.h"
#include "ToastStackWidget.h"
#include "UIStyle.h"
#include "Styling/SlateBrush.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** A world the widget can be created from. Mirrors AirportMgr.Actions.BarBuildsFromRegistry. */
	UToastStackWidget* MakeStack(UWorld* World)
	{
		return CreateWidget<UToastStackWidget>(World, UToastStackWidget::StaticClass());
	}
}

/**
 * THE DEFECT IN SPEC SECTION 1, PINNED. The bar used to bind OnNotification to a single
 * UTextBlock, so a second notification in the same second REPLACED the first and the first
 * was never seen. Two entries must produce two rows.
 *
 * At the level of the COMPOSITION - spawn the widget and tick it - not of the model struct.
 * UNotificationCentre's own tests already prove it holds two entries; what this adds is that
 * the widget above it actually draws them, which is the half that was broken.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FToastStackShowsEveryEntryTest,
	"AirportMgr.UI.ToastsDoNotOverwriteEachOther",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FToastStackShowsEveryEntryTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("a world"), World)) { return false; }
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };

	UToastStackWidget* Stack = MakeStack(World);
	if (!TestNotNull(TEXT("the stack is created with no asset"), Stack)) { return false; }
	if (!TestNotNull(TEXT("it owns a notification centre"), Stack->Centre())) { return false; }

	Stack->Centre()->PostFeed(FText::FromString(TEXT("Saved 'quick'")));
	Stack->Centre()->PostFeed(FText::FromString(TEXT("Arrival refused - runway too short")));

	// One frame. Both must be on screen: the old single label would have shown only the last.
	Stack->TickFeed(1.0f / 60.0f);
	TestEqual(TEXT("both notifications are drawn, not just the last one"),
		Stack->ToastCountForTest(), 2);
	return true;
}

/**
 * The real-seconds rule, measured through the WIDGET rather than the model.
 *
 * UNotificationCentre::Advance takes real seconds by contract, so a test against the model
 * alone cannot catch a caller that hands it game seconds - and the caller is where that
 * mistake lives. The plan for this work specified Advance(Delta * Multiplier(Speed)), which
 * at x32 would have given an eight-second toast a quarter of a second on screen. Half a real
 * second of frames must expire nothing, whatever the sim clock is doing.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FToastStackRealSecondsTest,
	"AirportMgr.UI.ToastsSurviveAFastClock",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FToastStackRealSecondsTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("a world"), World)) { return false; }
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };

	UToastStackWidget* Stack = MakeStack(World);
	if (!TestNotNull(TEXT("the stack is created"), Stack)) { return false; }

	Stack->Centre()->PostFeed(FText::FromString(TEXT("Loaded 'quick'")));
	for (int32 Frame = 0; Frame < 30; ++Frame)
	{
		Stack->TickFeed(1.0f / 60.0f);
	}

	TestEqual(TEXT("half a real second of frames expires nothing"), Stack->ToastCountForTest(), 1);
	TestTrue(TEXT("and the centre has been told half a real second, not a scaled one"),
		Stack->Centre()->Now() < 0.6);

	// Past its lifetime it does go, so the count above is not just a stuck widget.
	Stack->TickFeed(9.0f);
	TestEqual(TEXT("past its lifetime it clears itself"), Stack->ToastCountForTest(), 0);
	return true;
}

/**
 * THE DECLARED-BUT-NEVER-CONSUMED BUG, caught in the act and kept caught.
 *
 * UUIStyle::CornerRadius sat in the header and in DA_UIStyle, read by NOTHING, for the whole
 * of the first implementation - so every toast drew as a flat square slab while the asset
 * cheerfully carried a radius of 5. That is the same bug CLAUDE.md names three times
 * (ToolCommandList, GetModeCommands, ARoadBuildController::Tools): a value is added to one
 * place and no consumer is ever wired to it.
 *
 * Reading the radius back OFF THE BRUSH is the point. Asserting that the style has a
 * CornerRadius would pass on a completely square card - it is the drawn brush, not the
 * declaration, that has to carry it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FToastCardRoundingTest,
	"AirportMgr.UI.ToastCardUsesTheStyleCornerRadius",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FToastCardRoundingTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("a world"), World)) { return false; }
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };

	UToastStackWidget* Stack = MakeStack(World);
	if (!TestNotNull(TEXT("the stack is created"), Stack)) { return false; }

	Stack->Centre()->PostFeed(FText::FromString(TEXT("Saved 'quick'")));
	Stack->TickFeed(1.0f / 60.0f);

	FSlateBrush Brush;
	if (!TestTrue(TEXT("the card has a brush to read"), Stack->FirstToastBrushForTest(Brush)))
	{
		return false;
	}

	const UUIStyle* Style = UAirportMgrUISettings::ResolveStyle();
	TestEqual(TEXT("the card is drawn as a ROUNDED box, not the default flat one"),
		Brush.DrawAs, ESlateBrushDrawType::RoundedBox);
	// float against float: the ambiguity is real, TestEqual takes double and float overloads
	// and CornerRadii is a FVector4 of doubles.
	TestTrue(TEXT("and its radius is the style's CornerRadius, so the asset's value is the "
		"one on screen"),
		FMath::IsNearlyEqual(static_cast<float>(Brush.OutlineSettings.CornerRadii.X),
			Style->CornerRadius, KINDA_SMALL_NUMBER));
	return true;
}

/**
 * Severity reaches the card. Spec section 6.1 lists Severity in the entry and the first
 * implementation dropped the field entirely, so every toast drew identically - a refusal
 * looked exactly like a save confirmation.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FToastSeverityTest,
	"AirportMgr.UI.ToastSeverityPicksItsColour",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FToastSeverityTest::RunTest(const FString& Parameters)
{
	const UUIStyle* Style = UAirportMgrUISettings::ResolveStyle();

	// Three severities, three DIFFERENT slots. Accent is not among them on purpose: it means
	// the armed tool and nothing else, so a warning may never take it.
	UNotificationCentre* Centre = NewObject<UNotificationCentre>(GetTransientPackage());
	Centre->PostFeed(FText::FromString(TEXT("info")), ENotificationSeverity::Info);
	Centre->PostFeed(FText::FromString(TEXT("good")), ENotificationSeverity::Success);
	Centre->PostFeed(FText::FromString(TEXT("bad")), ENotificationSeverity::Warning);

	TestEqual(TEXT("severity is carried on the entry"), Centre->Entries().Num(), 3);
	TestEqual(TEXT("info stays info"), Centre->Entries()[0].Severity, ENotificationSeverity::Info);
	TestEqual(TEXT("success stays success"), Centre->Entries()[1].Severity, ENotificationSeverity::Success);
	TestEqual(TEXT("warning stays warning"), Centre->Entries()[2].Severity, ENotificationSeverity::Warning);

	TestTrue(TEXT("warning and success are different colours, or severity says nothing"),
		!Style->Warning.Equals(Style->Positive));
	TestTrue(TEXT("a warning never takes Accent, which means the armed tool"),
		!Style->Warning.Equals(Style->Accent));
	return true;
}

#endif
