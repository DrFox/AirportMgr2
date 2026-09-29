#include "AlertsPanelWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Model/OpsEvents.h"
#include "Present/OpsRuntime.h"
#include "Present/OpsRuntimeSubsystem.h"
#include "RoadBuildController.h"
#include "RoadBuildLog.h"
#include "UI/UiButton.h"
#include "UI/UiRow.h"
#include "UIStyle.h"

#define LOCTEXT_NAMESPACE "AlertsPanel"

void UAlertRowEntry::HandleClick()
{
	UAlertsPanelWidget* Panel = Owner.Get();
	ARoadBuildController* Controller = Panel != nullptr ? Cast<ARoadBuildController>(Panel->GetOwningPlayer()) : nullptr;
	if (Panel != nullptr && Controller != nullptr)
	{
		Panel->Go(Index, *Controller);
	}
}

void UAlertsPanelWidget::BuildOnce(const UUIStyle& Style)
{
	if (UVerticalBox* Column = Cast<UVerticalBox>(EnsureContentRoot(TEXT("AlertsCard"))))
	{
		if (RowColumn == nullptr)
		{
			RowColumn = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("RowColumn"));
			Column->AddChildToVerticalBox(RowColumn);
		}
	}
	// SelfHitTestInvisible on the ROOT; the WINDOW hides (SetShown) - see UAirportMgrPanelWidget::BuildOnce.
	SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	SetShown(false);

	// THE RUNTIME'S EVENTS, as the toast stack binds them. A headless test has no runtime and binds its own.
	if (UOpsRuntime* Runtime = UOpsRuntimeSubsystem::Get(GetWorld()))
	{
		BindTo(*Runtime->GetEvents());
	}
}

void UAlertsPanelWidget::BindTo(UOpsEvents& Events)
{
	Events.OnAlertRaised.AddUniqueDynamic(this, &UAlertsPanelWidget::OnAlertRaised);
	Events.OnAlertCleared.AddUniqueDynamic(this, &UAlertsPanelWidget::OnAlertCleared);
	Events.OnAlertsReset.AddUniqueDynamic(this, &UAlertsPanelWidget::OnAlertsReset);
}

void UAlertsPanelWidget::OnAlertRaised(const FOpsAlert& Alert)
{
	// UPSERT BY KEY: a re-raise after a load names a problem already listed (if the reset was missed, or
	// arrives after), and must refresh its row, not add a second.
	if (FOpsAlert* Existing = Alerts.FindByPredicate([&Alert](const FOpsAlert& A) { return A.Key == Alert.Key; }))
	{
		*Existing = Alert;
	}
	else
	{
		Alerts.Add(Alert);
	}
	bRowsDirty = true;
}

void UAlertsPanelWidget::OnAlertCleared(const FOpsAlertKey& Key)
{
	Alerts.RemoveAll([&Key](const FOpsAlert& A) { return A.Key == Key; });
	bRowsDirty = true;
}

void UAlertsPanelWidget::OnAlertsReset()
{
	Alerts.Reset();
	bRowsDirty = true;
}

void UAlertsPanelWidget::Toggle()
{
	bShowing = !bShowing;
	SetShown(bShowing);
	if (bShowing)
	{
		// PAINTED ON OPEN, not a frame later - the ledger panel's reason.
		PaintRows();
	}
}

bool UAlertsPanelWidget::WantsWindow(FUiWindowSpec& Out) const
{
	Out.Id = TEXT("alerts");
	Out.Title = LOCTEXT("AlertsWindow", "Alerts");
	Out.Anchor = EUiWindowAnchor::TopLeft;
	Out.Offset = FVector2D(12.0, 12.0);
	return true;
}

void UAlertsPanelWidget::OnWindowClosedByPlayer()
{
	// THE CLOSE BUTTON IS THE TOGGLE - bShowing must agree, or the bar lights a window nobody can see.
	if (bShowing)
	{
		Toggle();
	}
}

void UAlertsPanelWidget::TickPanel(float DeltaTime)
{
	if (bShowing && bRowsDirty)
	{
		PaintRows();
	}
}

bool UAlertsPanelWidget::Go(int32 Index, ARoadBuildController& Controller)
{
	if (!Alerts.IsValidIndex(Index))
	{
		return false;
	}
	const FOpsAlert& Alert = Alerts[Index];
	if (Alert.Focus.Kind == EAlertFocusKind::None)
	{
		// NO PLACE IN THE WORLD. Overdrawn's "where" is the ledger; an airline that cannot come has no one
		// place - its row says what to build, and the offer window is already on screen.
		if (Alert.Key.Kind == EAlertKind::Overdrawn)
		{
			if (!Controller.IsLedgerShowing())
			{
				Controller.ToggleLedger();
			}
			return true;
		}
		return false;
	}
	return Controller.SelectAndFocus(Alert.Focus);
}

void UAlertsPanelWidget::PaintRows()
{
	bRowsDirty = false;
	if (RowColumn == nullptr || PanelStyle == nullptr)
	{
		return;
	}
	const UUIStyle& Style = *PanelStyle;
	RowColumn->ClearChildren();
	Entries.Reset();
	if (Alerts.Num() == 0)
	{
		UTextBlock* None = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		None->SetText(LOCTEXT("NoAlerts", "Nothing needs you right now."));
		Style.ApplyText(*None, EUITextRole::Label, Style.InkMuted);
		RowColumn->AddChildToVerticalBox(None);
		return;
	}
	for (int32 Index = 0; Index < Alerts.Num(); ++Index)
	{
		const FOpsAlert& Alert = Alerts[Index];
		UHorizontalBox* Line = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());

		UTextBlock* Text = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		Text->SetText(Alert.Text);
		Text->SetAutoWrapText(true);
		Style.ApplyText(*Text, EUITextRole::Label, Style.Warning);
		UHorizontalBoxSlot* TextSlot = Line->AddChildToHorizontalBox(Text);
		TextSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		TextSlot->SetVerticalAlignment(VAlign_Center);
		TextSlot->SetPadding(FMargin(0.0f, 0.0f, 8.0f, 0.0f));

		const bool bCanGo = Alert.Focus.Kind != EAlertFocusKind::None || Alert.Key.Kind == EAlertKind::Overdrawn;
		UUiButton* GoButton = WidgetTree->ConstructWidget<UUiButton>(UUiButton::StaticClass());
		GoButton->SetLabel(LOCTEXT("Go", "Go"));
		GoButton->Build(Style, EUiButtonKind::Secondary);
		GoButton->SetState(bCanGo, false);
		UAlertRowEntry* Entry = NewObject<UAlertRowEntry>(this);
		Entry->Owner = this;
		Entry->Index = Index;
		GoButton->OnClicked.AddDynamic(Entry, &UAlertRowEntry::HandleClick);
		Entries.Add(Entry);
		Line->AddChildToHorizontalBox(GoButton);

		UUiRow* Row = WidgetTree->ConstructWidget<UUiRow>(UUiRow::StaticClass());
		Row->Build(Style, FMargin(8.0f, 4.0f));
		Row->SetContent(Line);
		UVerticalBoxSlot* RowSlot = RowColumn->AddChildToVerticalBox(Row);
		RowSlot->SetPadding(FMargin(0.0f, 0.0f, 0.0f, Style.RowGap));
	}
}

#undef LOCTEXT_NAMESPACE
