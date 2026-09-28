#include "SettingsPanelWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/SizeBox.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "RoadBuildLog.h"
#include "UI/UiButton.h"
#include "UI/UiDropdown.h"
#include "UI/UiRadioGroup.h"
#include "UI/UiRow.h"
#include "UI/UiSlider.h"
#include "UI/UiToggle.h"
#include "UI/UiWindowHost.h"
#include "UIStyle.h"

#define LOCTEXT_NAMESPACE "SettingsPanel"

namespace
{
	/** The spec's 420 px. */
	constexpr float DialogWidth = 420.0f;

	/** What the dropdown shows for the engine's -1 (a custom mix): High, the engine's default preset. */
	constexpr int32 CustomShowsAs = 2;
}

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
	SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	SetShown(false);
	UVerticalBox* Root = Cast<UVerticalBox>(EnsureContentRoot(TEXT("SettingsContent")));
	if (Root == nullptr)
	{
		return;   // a Blueprint supplied its own layout; it binds nothing here yet
	}
	USizeBox* Width = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	Width->SetWidthOverride(DialogWidth);
	Root->AddChildToVerticalBox(Width);
	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Width->SetContent(Column);

	// THE NAMES ARE THE TESTS' HANDLES (SettingsPanelTest finds each control by name and drives it
	// through the engine's own event).
	UiScale = WidgetTree->ConstructWidget<UUiSlider>(UUiSlider::StaticClass(), TEXT("UiScale"));
	UiScale->Build(Style, 0.75f, 1.5f, 0.05f, 2, INVTEXT("x"));
	UiScale->OnValueCommitted.AddDynamic(this, &USettingsPanelWidget::HandleUiScale);

	Graphics = WidgetTree->ConstructWidget<UUiDropdown>(UUiDropdown::StaticClass(), TEXT("Graphics"));
	Graphics->Build(Style, { LOCTEXT("Low", "Low"), LOCTEXT("Medium", "Medium"), LOCTEXT("High", "High"), LOCTEXT("Epic", "Epic") });
	Graphics->OnSelectionChanged.AddDynamic(this, &USettingsPanelWidget::HandleGraphics);

	PanSpeed = WidgetTree->ConstructWidget<UUiSlider>(UUiSlider::StaticClass(), TEXT("PanSpeed"));
	PanSpeed->Build(Style, 0.5f, 2.0f, 0.1f, 1, INVTEXT("x"));
	PanSpeed->OnValueChanged.AddDynamic(this, &USettingsPanelWidget::HandlePanSpeed);

	ZoomSpeed = WidgetTree->ConstructWidget<UUiSlider>(UUiSlider::StaticClass(), TEXT("ZoomSpeed"));
	ZoomSpeed->Build(Style, 0.5f, 2.0f, 0.1f, 1, INVTEXT("x"));
	ZoomSpeed->OnValueChanged.AddDynamic(this, &USettingsPanelWidget::HandleZoomSpeed);

	// LEFT FIRST, as a map reads - index 0 is EDriveSide::Left (HandleDriveSide).
	DriveSide = WidgetTree->ConstructWidget<UUiRadioGroup>(UUiRadioGroup::StaticClass(), TEXT("DriveSide"));
	DriveSide->Build(Style, { LOCTEXT("Left", "Left"), LOCTEXT("Right", "Right") });
	DriveSide->OnSelectionChanged.AddDynamic(this, &USettingsPanelWidget::HandleDriveSide);

	GridSnap = WidgetTree->ConstructWidget<UUiToggle>(UUiToggle::StaticClass(), TEXT("GridSnap"));
	GridSnap->Build(Style);
	GridSnap->OnToggled.AddDynamic(this, &USettingsPanelWidget::HandleGridSnap);

	UUiButton* Reset = WidgetTree->ConstructWidget<UUiButton>(UUiButton::StaticClass(), TEXT("ResetLayout"));
	Reset->SetLabel(LOCTEXT("ResetLayout", "Reset window layout"));
	Reset->Build(Style, EUiButtonKind::Secondary);
	Reset->OnClicked.AddDynamic(this, &USettingsPanelWidget::HandleResetLayout);

	AddCaption(Style, *Column, LOCTEXT("Display", "DISPLAY"));
	AddRow(Style, *Column, LOCTEXT("UiScaleLabel", "UI scale"), *UiScale);
	AddRow(Style, *Column, LOCTEXT("GraphicsLabel", "Graphics"), *Graphics);
	AddCaption(Style, *Column, LOCTEXT("Camera", "CAMERA"));
	AddRow(Style, *Column, LOCTEXT("PanLabel", "Pan speed"), *PanSpeed);
	AddRow(Style, *Column, LOCTEXT("ZoomLabel", "Zoom speed"), *ZoomSpeed);
	AddCaption(Style, *Column, LOCTEXT("Driving", "DRIVING"));
	AddRow(Style, *Column, LOCTEXT("DriveLabel", "Traffic drives on"), *DriveSide);
	AddCaption(Style, *Column, LOCTEXT("Building", "BUILDING"));
	AddRow(Style, *Column, LOCTEXT("GridLabel", "Grid snap on start"), *GridSnap);
	AddCaption(Style, *Column, LOCTEXT("Windows", "WINDOWS"));
	Column->AddChildToVerticalBox(Reset)->SetHorizontalAlignment(HAlign_Left);

	// THE FOOTER: Cancel then Save, right-aligned, Save the one Accent on the dialog.
	UHorizontalBox* Footer = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	UUiButton* CancelButton = WidgetTree->ConstructWidget<UUiButton>(UUiButton::StaticClass(), TEXT("Cancel"));
	CancelButton->SetLabel(LOCTEXT("Cancel", "Cancel"));
	CancelButton->Build(Style, EUiButtonKind::Secondary);
	CancelButton->OnClicked.AddDynamic(this, &USettingsPanelWidget::HandleCancel);
	UUiButton* SaveButton = WidgetTree->ConstructWidget<UUiButton>(UUiButton::StaticClass(), TEXT("Save"));
	SaveButton->SetLabel(LOCTEXT("Save", "Save"));
	SaveButton->Build(Style, EUiButtonKind::Primary);
	SaveButton->OnClicked.AddDynamic(this, &USettingsPanelWidget::HandleSave);
	Footer->AddChildToHorizontalBox(WidgetTree->ConstructWidget<USpacer>(USpacer::StaticClass()))->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	Footer->AddChildToHorizontalBox(CancelButton);
	Footer->AddChildToHorizontalBox(SaveButton)->SetPadding(FMargin(Style.RowGap, 0.0f, 0.0f, 0.0f));
	Column->AddChildToVerticalBox(Footer)->SetPadding(FMargin(0.0f, 16.0f, 0.0f, 0.0f));
}

void USettingsPanelWidget::AddCaption(const UUIStyle& Style, UVerticalBox& Column, const FText& Text)
{
	UTextBlock* Caption = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Caption->SetText(Text);
	Style.ApplyText(*Caption, EUITextRole::Heading, Style.InkMuted);
	// The first caption sits under the title bar's hairline; the rest open a new group.
	const float Above = Column.GetChildrenCount() == 0 ? 0.0f : 12.0f;
	Column.AddChildToVerticalBox(Caption)->SetPadding(FMargin(0.0f, Above, 0.0f, 4.0f));
}

void USettingsPanelWidget::AddRow(const UUIStyle& Style, UVerticalBox& Column, const FText& Label, UWidget& Control)
{
	UUiRow* Row = WidgetTree->ConstructWidget<UUiRow>(UUiRow::StaticClass());
	Row->Build(Style, FMargin(10.0f, 6.0f));
	UHorizontalBox* Line = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Row->SetContent(Line);
	UTextBlock* Text = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Text->SetText(Label);
	Style.ApplyText(*Text, EUITextRole::Label, Style.Ink);
	UHorizontalBoxSlot* TextSlot = Line->AddChildToHorizontalBox(Text);
	TextSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	TextSlot->SetVerticalAlignment(VAlign_Center);
	Line->AddChildToHorizontalBox(&Control)->SetVerticalAlignment(VAlign_Center);
	Column.AddChildToVerticalBox(Row)->SetPadding(FMargin(0.0f, 0.0f, 0.0f, Style.RowGap));
}

void USettingsPanelWidget::Open()
{
	if (!Sink.IsValid())
	{
		UE_LOG(LogRoadBuild, Warning, TEXT("Settings: no settings to edit - not opened"));
		return;
	}
	Snapshot = Sink->Read();
	Current = Snapshot;
	LoadControls();
	bShowing = true;
	SetShown(true);
	UE_LOG(LogRoadBuild, Log, TEXT("Settings: opened (UI %.2f, pan %.1f, zoom %.1f, graphics %d, drive %s, grid on start %s)"),
		Current.UIScale, Current.PanSpeedScale, Current.ZoomSpeedScale, Current.GraphicsQuality,
		Current.DriveSide == EDriveSide::Left ? TEXT("left") : TEXT("right"), Current.bGridSnapOnStart ? TEXT("on") : TEXT("off"));
}

void USettingsPanelWidget::LoadControls()
{
	// NO EVENTS: every Set* here is the 4a controls' code path, which raises nothing - so loading
	// the dialog never applies a value (Review Focus 2).
	// ENFORCED BY: AirportMgr.Settings.Panel.OpenAppliesNothing.
	if (UiScale != nullptr) { UiScale->SetValue(Current.UIScale); }
	if (PanSpeed != nullptr) { PanSpeed->SetValue(Current.PanSpeedScale); }
	if (ZoomSpeed != nullptr) { ZoomSpeed->SetValue(Current.ZoomSpeedScale); }
	if (Graphics != nullptr) { Graphics->SetSelected(Current.GraphicsQuality < 0 ? CustomShowsAs : Current.GraphicsQuality); }
	if (DriveSide != nullptr) { DriveSide->SetSelected(Current.DriveSide == EDriveSide::Left ? 0 : 1); }
	if (GridSnap != nullptr) { GridSnap->SetOn(Current.bGridSnapOnStart); }
}

void USettingsPanelWidget::ApplyCurrent(const TCHAR* Field, const FString& Value)
{
	UE_LOG(LogRoadBuild, Log, TEXT("Settings: %s -> %s"), Field, *Value);
	if (Sink.IsValid())
	{
		Sink->Apply(Current);
	}
}

void USettingsPanelWidget::HandleUiScale(float Value)
{
	Current.UIScale = Value;
	ApplyCurrent(TEXT("UI scale"), FString::SanitizeFloat(Value));
}

void USettingsPanelWidget::HandlePanSpeed(float Value)
{
	Current.PanSpeedScale = Value;
	ApplyCurrent(TEXT("pan speed"), FString::SanitizeFloat(Value));
}

void USettingsPanelWidget::HandleZoomSpeed(float Value)
{
	Current.ZoomSpeedScale = Value;
	ApplyCurrent(TEXT("zoom speed"), FString::SanitizeFloat(Value));
}

void USettingsPanelWidget::HandleGraphics(int32 Index)
{
	Current.GraphicsQuality = Index;
	ApplyCurrent(TEXT("graphics"), FString::FromInt(Index));
}

void USettingsPanelWidget::HandleDriveSide(int32 Index)
{
	Current.DriveSide = Index == 0 ? EDriveSide::Left : EDriveSide::Right;
	ApplyCurrent(TEXT("drive side"), Index == 0 ? TEXT("left") : TEXT("right"));
}

void USettingsPanelWidget::HandleGridSnap(bool bOn)
{
	Current.bGridSnapOnStart = bOn;
	ApplyCurrent(TEXT("grid snap on start"), bOn ? TEXT("on") : TEXT("off"));
}

void USettingsPanelWidget::HandleResetLayout()
{
	if (UUiWindowHost* WindowHost = GetWindowHost())
	{
		WindowHost->ResetLayout();
		UE_LOG(LogRoadBuild, Log, TEXT("Settings: window layout reset"));
	}
}

void USettingsPanelWidget::HandleCancel()
{
	Cancel();
}

void USettingsPanelWidget::HandleSave()
{
	SaveAndClose();
}

void USettingsPanelWidget::Cancel()
{
	if (!bShowing)
	{
		return;
	}
	if (Sink.IsValid())
	{
		// RESTORED AND RE-SAVED. A window-layout save while the dialog was open (dragging this very
		// window: FUserSettingsLayoutStore::Write saves the whole settings object) wrote the LIVE
		// values to disk; saving the snapshot puts the file back to what the player kept.
		// ENFORCED BY: AirportMgr.Settings.Panel.CancelRestoresEverything.
		Sink->Apply(Snapshot);
		Sink->Save();
	}
	UE_LOG(LogRoadBuild, Log, TEXT("Settings: cancelled - every value restored"));
	Close();
}

void USettingsPanelWidget::SaveAndClose()
{
	if (!bShowing)
	{
		return;
	}
	if (Sink.IsValid())
	{
		Sink->Save();
	}
	UE_LOG(LogRoadBuild, Log, TEXT("Settings: saved"));
	Close();
}

void USettingsPanelWidget::Close()
{
	bShowing = false;
	SetShown(false);
}

void USettingsPanelWidget::Toggle()
{
	if (bShowing)
	{
		Cancel();
	}
	else
	{
		Open();
	}
}

void USettingsPanelWidget::OnWindowClosedByPlayer()
{
	// THE CLOSE IS CANCEL, and bShowing must follow - or the next Escape "opens" it hidden.
	Cancel();
}

#undef LOCTEXT_NAMESPACE
