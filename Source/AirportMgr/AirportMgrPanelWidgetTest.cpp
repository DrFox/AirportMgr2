#include "CoreMinimal.h"
#include "AirportMgrPanelWidget.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "BuildBarWidget.h"
#include "InspectorWidget.h"
#include "Misc/AutomationTest.h"
#include "OfferInboxWidget.h"
#include "Testing/AirsideTestWorld.h"
#include "ToastStackWidget.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * UAirportMgrPanelWidget itself (issue #90's shared base, #187's shared PanelStyle) has no
 * test by name - each of the four subclasses is tested for its OWN content, and nothing
 * checks the base's own two promises: BuildOnce runs EXACTLY ONCE per widget however many
 * times Slate calls Initialize, and PanelStyle is resolved through the ONE path every
 * subclass now shares rather than four private copies that could drift (issue #194).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAirportMgrPanelWidgetSharedBaseTest,
	"AirportMgr.Panels.SharedBaseBuildsOnce",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAirportMgrPanelWidgetSharedBaseTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	if (!TestNotNull(TEXT("a world"), TestWorld.World))
	{
		return false;
	}

	UBuildBarWidget* Bar = CreateWidget<UBuildBarWidget>(TestWorld.World, UBuildBarWidget::StaticClass());
	UInspectorWidget* Inspector = CreateWidget<UInspectorWidget>(TestWorld.World, UInspectorWidget::StaticClass());
	UOfferInboxWidget* Offers = CreateWidget<UOfferInboxWidget>(TestWorld.World, UOfferInboxWidget::StaticClass());
	UToastStackWidget* Toasts = CreateWidget<UToastStackWidget>(TestWorld.World, UToastStackWidget::StaticClass());
	if (!TestNotNull(TEXT("the bar builds with no asset"), Bar)
		|| !TestNotNull(TEXT("the inspector builds with no asset"), Inspector)
		|| !TestNotNull(TEXT("the offer inbox builds with no asset"), Offers)
		|| !TestNotNull(TEXT("the toast stack builds with no asset"), Toasts))
	{
		return false;
	}

	// EVERY SUBCLASS BUILDS THROUGH THE SHARED BASE: CreateWidget already ran Initialize once
	// (UAirportMgrPanelWidget::Initialize is sealed - no subclass overrides it), so each must
	// already show exactly one BuildOnce call.
	TestEqual(TEXT("the bar's BuildOnce ran exactly once"), Bar->BuildOnceCallCountForTest(), 1);
	TestEqual(TEXT("the inspector's BuildOnce ran exactly once"), Inspector->BuildOnceCallCountForTest(), 1);
	TestEqual(TEXT("the offer inbox's BuildOnce ran exactly once"), Offers->BuildOnceCallCountForTest(), 1);
	TestEqual(TEXT("the toast stack's BuildOnce ran exactly once"), Toasts->BuildOnceCallCountForTest(), 1);

	// PANELSTYLE IS RESOLVED, and through the ONE path: all four hold the SAME style object,
	// not four independent UAirsideSettings::ResolveStyle() calls that happen to agree today
	// and could silently diverge tomorrow - see the class comment's own account of issue #187.
	if (TestNotNull(TEXT("the bar resolved a style"), Bar->PanelStyleForTest()))
	{
		TestEqual(TEXT("the inspector shares the bar's style object"),
			Inspector->PanelStyleForTest(), Bar->PanelStyleForTest());
		TestEqual(TEXT("the offer inbox shares it too"),
			Offers->PanelStyleForTest(), Bar->PanelStyleForTest());
		TestEqual(TEXT("the toast stack shares it too"),
			Toasts->PanelStyleForTest(), Bar->PanelStyleForTest());
	}

	// A SECOND Initialize() MUST NOT RE-RUN BuildOnce. Slate can call Initialize again on a
	// widget it re-parents; the bBuilt guard is what stops a second WidgetTree::ConstructWidget
	// pass from duplicating whatever the first one built - this is the guard's only test.
	Bar->Initialize();
	TestEqual(TEXT("a second Initialize() call does not re-run the bar's BuildOnce"),
		Bar->BuildOnceCallCountForTest(), 1);

	return true;
}

/**
 * A PRESS ON A PANEL'S CHROME STOPS AT THE PANEL (2026-09-28 report: a click on the bar where
 * there is no button started a road under it). Driven through the widget's own Slate widget -
 * SObjectWidget, what the viewport's hit test actually reaches - rather than the protected
 * UMG override, so the forwarding is under test too. A default FGeometry/FPointerEvent is enough:
 * the rule does not look at where the press is, only whether anything took it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAirportMgrPanelChromeEatsClicksTest,
	"AirportMgr.Panels.ChromeEatsClicks",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAirportMgrPanelChromeEatsClicksTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }

	UBuildBarWidget* Bar = CreateWidget<UBuildBarWidget>(TestWorld.World, UBuildBarWidget::StaticClass());
	if (!TestNotNull(TEXT("the bar builds with no asset"), Bar)) { return false; }
	const TSharedRef<SWidget> Slate = Bar->TakeWidget();

	TestTrue(TEXT("a press on the bar's chrome is handled, so it never reaches the game viewport"),
		Slate->OnMouseButtonDown(FGeometry(), FPointerEvent()).IsEventHandled());
	TestTrue(TEXT("and so is a double-click - Slate's second press"),
		Slate->OnMouseButtonDoubleClick(FGeometry(), FPointerEvent()).IsEventHandled());
	TestFalse(TEXT("the release is left alone, so a drag begun on the ground can still end"),
		Slate->OnMouseButtonUp(FGeometry(), FPointerEvent()).IsEventHandled());

	// THE CONTROL: a root that hits ITSELF may cover the screen, and must not swallow the game.
	UWidget* Root = Bar->WidgetTree != nullptr ? Bar->WidgetTree->RootWidget.Get() : nullptr;
	if (!TestNotNull(TEXT("the bar has a root"), Root)) { return false; }
	Root->SetVisibility(ESlateVisibility::Visible);
	TestFalse(TEXT("a hit-testable root keeps Slate's answer - a click through, not every click eaten"),
		Slate->OnMouseButtonDown(FGeometry(), FPointerEvent()).IsEventHandled());
	return true;
}

#endif
