#include "BuildHudLayer.h"

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
	// CODE-ONLY, no *Class hook: the other panels' hooks predate windows (see SettingsPanel's comment).
	SettingsPanel = CreateWidget<USettingsPanelWidget>(&Owner, USettingsPanelWidget::StaticClass());
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
	for (UAirportMgrPanelWidget* Panel : TArray<UAirportMgrPanelWidget*>{ Inspector, OfferInbox, LedgerPanel, LandPanel, SettingsPanel })
	{
		if (Panel != nullptr)
		{
			WindowHost->AddWindow(*Panel);
		}
	}
	WindowHost->DockAbove(BuildBar);
}
