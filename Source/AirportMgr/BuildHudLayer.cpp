#include "BuildHudLayer.h"
#include "AlertsPanelWidget.h"

#include "ArrivalsPanelWidget.h"
#include "Blueprint/UserWidget.h"
#include "BuildBarWidget.h"
#include "GameFramework/PlayerController.h"
#include "InspectorWidget.h"
#include "LandAircraftPanelWidget.h"
#include "LedgerPanelWidget.h"
#include "OfferInboxWidget.h"
#include "SettingsPanelWidget.h"
#include "RoadBuildLog.h"
#include "ToastStackWidget.h"
#include "UI/UiLayoutStore.h"
#include "UI/UiWindowHost.h"

template<class T>
T* UBuildHudLayer::CreateConfiguredWidget(APlayerController& Owner, TSubclassOf<T> ConfiguredClass,
	int32 ZOrder, const TCHAR* DisplayName, const TCHAR* PropertyName)
{
	const TSubclassOf<T> WidgetClass =
		ConfiguredClass != nullptr ? ConfiguredClass : TSubclassOf<T>(T::StaticClass());
	T* Widget = CreateWidget<T>(&Owner, WidgetClass);
	if (Widget != nullptr)
	{
		if (ZOrder != INDEX_NONE)
		{
			Widget->AddToViewport(ZOrder);
		}
		UE_LOG(LogRoadBuild, Log, TEXT("%s: %s"), DisplayName,
			ConfiguredClass != nullptr ? *ConfiguredClass->GetName()
				: *FString::Printf(TEXT("code-only (no %s configured)"), PropertyName));
	}
	return Widget;
}

void UBuildHudLayer::CreateAll(APlayerController& Owner)
{
	BuildBar = CreateConfiguredWidget<UBuildBarWidget>(Owner, BuildBarClass, 0,
		TEXT("Build bar"), TEXT("BuildBarClass"));
	WindowHost = CreateWidget<UUiWindowHost>(&Owner, UUiWindowHost::StaticClass());
	if (WindowHost != nullptr)
	{
		WindowHost->AddToViewport(1);
		// THE PLAYER'S FILE, here and only here - every test's host has none (IUiLayoutStore's
		// comment). ENFORCED BY: Check-Architecture rule 30 (layout-store-wired).
		WindowHost->SetLayoutStore(MakeShared<FUserSettingsLayoutStore>());
	}
	// INDEX_NONE: not added to the viewport - WireWindows hands each to the window host.
	Inspector = CreateConfiguredWidget<UInspectorWidget>(Owner, InspectorClass, INDEX_NONE,
		TEXT("Inspector"), TEXT("InspectorClass"));
	OfferInbox = CreateConfiguredWidget<UOfferInboxWidget>(Owner, OfferInboxClass, INDEX_NONE,
		TEXT("Offer inbox"), TEXT("OfferInboxClass"));
	LedgerPanel = CreateConfiguredWidget<ULedgerPanelWidget>(Owner, LedgerPanelClass, INDEX_NONE,
		TEXT("Ledger panel"), TEXT("LedgerPanelClass"));
	LandPanel = CreateConfiguredWidget<ULandAircraftPanelWidget>(Owner, LandPanelClass, INDEX_NONE,
		TEXT("Land panel"), TEXT("LandPanelClass"));
	// CODE-ONLY, no *Class hook, like Settings: split out of the inbox after windows existed.
	ArrivalsPanel = CreateWidget<UArrivalsPanelWidget>(&Owner, UArrivalsPanelWidget::StaticClass());
	// CODE-ONLY, no *Class hook: the other panels' hooks predate windows (see SettingsPanel's comment).
	SettingsPanel = CreateWidget<USettingsPanelWidget>(&Owner, USettingsPanelWidget::StaticClass());
	// CODE-ONLY, no *Class hook, like Arrivals and Settings. IN WireWindows TOO - a list that must agree.
	// ENFORCED BY: AirportMgr.UI.Alerts.HudHostsItAsAWindow
	AlertsPanel = CreateWidget<UAlertsPanelWidget>(&Owner, UAlertsPanelWidget::StaticClass());
	ToastStack = CreateConfiguredWidget<UToastStackWidget>(Owner, ToastStackClass, 2,
		TEXT("Toast stack"), TEXT("ToastStackClass"));
	WireWindows();
}

void UBuildHudLayer::WireWindows()
{
	if (WindowHost == nullptr)
	{
		return;
	}
	for (UAirportMgrPanelWidget* Panel : TArray<UAirportMgrPanelWidget*>{ Inspector, OfferInbox, ArrivalsPanel, LedgerPanel, LandPanel, AlertsPanel, SettingsPanel })
	{
		if (Panel != nullptr)
		{
			WindowHost->AddWindow(*Panel);
		}
	}
	WindowHost->DockAbove(BuildBar);
}

namespace
{
	/** The words of each window's open/close line, in EHudWindow order - ONE TABLE whose static_assert fails a window added to the
	 *  enum with no name, the way ActionSectionName's does. The first three are what the controller's own lines said. */
	constexpr const TCHAR* HudWindowNames[] = { TEXT("Ledger panel"), TEXT("Alerts window"), TEXT("Land panel"), TEXT("Settings") };
	static_assert(UE_ARRAY_COUNT(HudWindowNames) == static_cast<int32>(EHudWindow::Count),
		"Every EHudWindow needs a name in HudWindowNames");
}

namespace
{
	/**
	 * THE ONE LIST of which panel each EHudWindow is - ToggleWindow and IsWindowShowing both read it, where each was its own four-case switch
	 * over the same four panels (#447). A fifth window adds one case here. Null for a window this layer has no panel for.
	 */
	UAirportMgrPanelWidget* PanelFor(const UBuildHudLayer& Layer, EHudWindow Window)
	{
		switch (Window)
		{
		case EHudWindow::Ledger:   return Layer.LedgerPanel;
		case EHudWindow::Alerts:   return Layer.AlertsPanel;
		case EHudWindow::Land:     return Layer.LandPanel;
		case EHudWindow::Settings: return Layer.SettingsPanel;
		default:                   return nullptr;
		}
	}
}

void UBuildHudLayer::ToggleWindow(EHudWindow Window)
{
	UAirportMgrPanelWidget* Panel = PanelFor(*this, Window);
	if (Panel == nullptr)
	{
		return;
	}
	// THE PANEL'S TOGGLE, which is the host's for a hosted window (UAirportMgrPanelWidget::Toggle) - and Settings' own, which begins or
	// cancels an edit - and the ANSWER is the host's too: nothing here keeps a second copy of whether it is open.
	Panel->Toggle();
	const bool bOpen = Panel->IsShown();
	// ONE LINE FOR ALL FOUR, in the words the controller's three used ("Ledger panel opened", "Land panel closed") - and the alerts
	// window's count, which is what tells a player the badge and the window agree.
	const FString Count = Window == EHudWindow::Alerts ? FString::Printf(TEXT(" (%d alert(s))"), AlertCount()) : FString();
	UE_LOG(LogRoadBuild, Log, TEXT("%s %s%s"), HudWindowNames[static_cast<int32>(Window)], bOpen ? TEXT("opened") : TEXT("closed"), *Count);
}

bool UBuildHudLayer::IsWindowShowing(EHudWindow Window) const
{
	const UAirportMgrPanelWidget* Panel = PanelFor(*this, Window);
	return Panel != nullptr && Panel->IsShown();
}

int32 UBuildHudLayer::AlertCount() const
{
	return AlertsPanel != nullptr ? AlertsPanel->AlertCount() : 0;
}

void UBuildHudLayer::OpenUnstickMenu()
{
	if (Inspector != nullptr)
	{
		Inspector->OpenUnstickMenu();
	}
}
