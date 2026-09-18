#include "BuildHudLayer.h"

#include "Blueprint/UserWidget.h"
#include "BuildBarWidget.h"
#include "GameFramework/PlayerController.h"
#include "InspectorWidget.h"
#include "LedgerPanelWidget.h"
#include "OfferInboxWidget.h"
#include "RoadBuildLog.h"
#include "ToastStackWidget.h"

template<class T>
T* UBuildHudLayer::CreateConfiguredWidget(APlayerController& Owner, TSubclassOf<T> ConfiguredClass,
	int32 ZOrder, const TCHAR* DisplayName, const TCHAR* PropertyName)
{
	const TSubclassOf<T> WidgetClass =
		ConfiguredClass != nullptr ? ConfiguredClass : TSubclassOf<T>(T::StaticClass());
	T* Widget = CreateWidget<T>(&Owner, WidgetClass);
	if (Widget != nullptr)
	{
		Widget->AddToViewport(ZOrder);
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
	Inspector = CreateConfiguredWidget<UInspectorWidget>(Owner, InspectorClass, 1,
		TEXT("Inspector"), TEXT("InspectorClass"));
	OfferInbox = CreateConfiguredWidget<UOfferInboxWidget>(Owner, OfferInboxClass, 1,
		TEXT("Offer inbox"), TEXT("OfferInboxClass"));
	LedgerPanel = CreateConfiguredWidget<ULedgerPanelWidget>(Owner, LedgerPanelClass, 1,
		TEXT("Ledger panel"), TEXT("LedgerPanelClass"));
	ToastStack = CreateConfiguredWidget<UToastStackWidget>(Owner, ToastStackClass, 2,
		TEXT("Toast stack"), TEXT("ToastStackClass"));
}
