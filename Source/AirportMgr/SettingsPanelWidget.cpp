#include "SettingsPanelWidget.h"

#include "RoadBuildLog.h"

#define LOCTEXT_NAMESPACE "SettingsPanel"

bool USettingsPanelWidget::WantsWindow(FUiWindowSpec& Out) const
{
	Out.Id = TEXT("settings");
	Out.Title = LOCTEXT("SettingsWindow", "Settings");
	Out.Anchor = EUiWindowAnchor::Centre;
	Out.Offset = FVector2D::ZeroVector;
	Out.bModal = true;
	Out.bResizable = false;
	return true;
}

void USettingsPanelWidget::BuildOnce(const UUIStyle& Style)
{
	EnsureContentRoot(TEXT("SettingsContent"));
	SetShown(false);
}

void USettingsPanelWidget::Toggle()
{
	bShowing = !bShowing;
	SetShown(bShowing);
	UE_LOG(LogRoadBuild, Log, TEXT("Settings: %s"), bShowing ? TEXT("opened") : TEXT("closed"));
}

void USettingsPanelWidget::OnWindowClosedByPlayer()
{
	if (bShowing)
	{
		Toggle();
	}
}

#undef LOCTEXT_NAMESPACE
