#include "AlertsPanelWidget.h"

#include "Blueprint/WidgetTree.h"
#include "BuildHudLayer.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Engine/Texture2D.h"
#include "Model/OpsAlerts.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Model/OpsEvents.h"
#include "Present/OpsRuntime.h"
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
		Panel->GoTo(Key, *Controller);
	}
}

void UAlertRowEntry::HandleCancelClick()
{
	// THROUGH THE PANEL, which asks its own runtime (OpsRuntime(), the resolver's answer) - not a controller verb, whose public surface
	// is a closed list, and not a UOpsRuntimeSubsystem::Get of this widget's own. The flight is the row's own key.
	// ENFORCED BY: Check-Architecture rule 55 (the subsystem is called from the resolver alone), rule 54 (the controller's closed list)
	if (UAlertsPanelWidget* Panel = Owner.Get())
	{
		Panel->OnCancelClicked(Key);
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

	WarningIcon = Style.IconWarning.LoadSynchronous();

	// THE RUNTIME'S EVENTS, as the toast stack binds them - AND WHAT IT ALREADY HOLDS: the model publishes
	// only changes, so an alert raised before this panel existed would never reach it (stage 2 review).
	if (UOpsRuntime* Runtime = OpsRuntime())
	{
		// NO CATCH-UP LOOP any more (#445): the model already holds whatever was raised before this panel existed, and the first paint
		// reads it - the loop existed because a mirror only heard changes.
		BindTo(*Runtime->GetEvents(), *Runtime->GetAlerts());
	}
	else
	{
		// Not an error: a headless test binds its own events. Said so a silent list in PIE has a line.
		UE_LOG(LogRoadBuild, Log, TEXT("No ops runtime: the alerts window is up but subscribed to nothing"));
	}
}

void UAlertsPanelWidget::BindTo(UOpsEvents& Events, const UOpsAlerts& InModel)
{
	Model = &InModel;
	Events.OnAlertRaised.AddUniqueDynamic(this, &UAlertsPanelWidget::OnAlertRaised);
	Events.OnAlertCleared.AddUniqueDynamic(this, &UAlertsPanelWidget::OnAlertCleared);
	Events.OnAlertChanged.AddUniqueDynamic(this, &UAlertsPanelWidget::OnAlertChanged);
	Events.OnAlertsReset.AddUniqueDynamic(this, &UAlertsPanelWidget::OnAlertsReset);
	bRowsDirty = true;
}

const TArray<FOpsAlert>& UAlertsPanelWidget::GetAlerts() const
{
	static const TArray<FOpsAlert> None;
	const UOpsAlerts* Live = Model.Get();
	return Live != nullptr ? Live->GetAlerts() : None;
}

// THE FOUR EVENTS ONLY INVALIDATE (#445). They used to upsert into, remove from and empty a copy of the list; the copy is gone, and what
// the rows show is the model's at the moment they are painted. A raise after a load, a change of words and a clear are the same thing
// to this window: the set or its text moved, look again.
void UAlertsPanelWidget::OnAlertRaised(const FOpsAlert& Alert)
{
	bRowsDirty = true;
}

void UAlertsPanelWidget::OnAlertCleared(const FOpsAlertKey& Key)
{
	bRowsDirty = true;
}

void UAlertsPanelWidget::OnAlertChanged(const FOpsAlertKey& Key)
{
	bRowsDirty = true;
}

void UAlertsPanelWidget::OnAlertsReset()
{
	bRowsDirty = true;
}

bool UAlertsPanelWidget::WantsWindow(FUiWindowSpec& Out) const
{
	Out.Id = TEXT("alerts");
	Out.Title = LOCTEXT("AlertsWindow", "Alerts");
	Out.Anchor = EUiWindowAnchor::TopLeft;
	Out.Offset = FVector2D(12.0, 12.0);
	Out.bToggled = true;   // the bar's Alerts button and its close are one toggle - the host's
	return true;
}

void UAlertsPanelWidget::OnShownChanged(bool bShown)
{
	if (bShown)
	{
		// PAINTED ON OPEN, not a frame later - the ledger panel's reason.
		PaintRows();
	}
}

void UAlertsPanelWidget::TickPanel(float DeltaTime)
{
	if (IsShown() && bRowsDirty)
	{
		PaintRows();
	}
}

bool UAlertsPanelWidget::Go(int32 Index, ARoadBuildController& Controller)
{
	const TArray<FOpsAlert>& Alerts = GetAlerts();
	return Alerts.IsValidIndex(Index) && GoTo(Alerts[Index].Key, Controller);
}

bool UAlertsPanelWidget::GoTo(const FOpsAlertKey& Key, ARoadBuildController& Controller)
{
	const FOpsAlert* Found = GetAlerts().FindByPredicate([&Key](const FOpsAlert& A) { return A.Key == Key; });
	if (Found == nullptr)
	{
		return false;
	}
	const FOpsAlert& Alert = *Found;
	if (Alert.Focus.Kind == EAlertFocusKind::None)
	{
		// NO PLACE IN THE WORLD. Overdrawn's "where" is the ledger; an airline that cannot come has no one
		// place - its row says what to build, and the offer window is already on screen.
		if (Alert.Key.Kind == EAlertKind::Overdrawn)
		{
			UBuildHudLayer* Hud = Controller.GetHud();
			if (Hud != nullptr && !Hud->IsWindowShowing(EHudWindow::Ledger))
			{
				Hud->ToggleWindow(EHudWindow::Ledger);
			}
			return true;
		}
		return false;
	}
	return Controller.SelectAndFocus(Alert.Focus);
}

bool UAlertsPanelWidget::OnCancelClicked(const FOpsAlertKey& Key)
{
	UOpsRuntime* Runtime = OpsRuntime();
	if (Runtime == nullptr)
	{
		UE_LOG(LogRoadBuild, Warning, TEXT("Alerts: Cancel flight clicked on %s %d with no ops runtime - nothing cancelled"),
			*UEnum::GetValueAsString(Key.Kind), Key.Id);
		return false;
	}
	return CancelFlightOf(Key, *Runtime);
}

bool UAlertsPanelWidget::IsCancelBoundForTest(const FOpsAlertKey& Key) const
{
	for (const TObjectPtr<UAlertRowEntry>& Entry : Entries)
	{
		if (Entry != nullptr && Entry->Key == Key)
		{
			return Entry->CancelButton != nullptr
				&& Entry->CancelButton->OnClicked.Contains(Entry.Get(), GET_FUNCTION_NAME_CHECKED(UAlertRowEntry, HandleCancelClick));
		}
	}
	return false;
}

bool UAlertsPanelWidget::ClickCancelForTest(const FOpsAlertKey& Key)
{
	for (const TObjectPtr<UAlertRowEntry>& Entry : Entries)
	{
		if (Entry != nullptr && Entry->Key == Key && Entry->CancelButton != nullptr)
		{
			Entry->CancelButton->OnClicked.Broadcast();
			return true;
		}
	}
	return false;
}

bool UAlertsPanelWidget::CancelFlightOf(const FOpsAlertKey& Key, UOpsRuntime& Runtime)
{
	const FOpsAlert* Found = GetAlerts().FindByPredicate([&Key](const FOpsAlert& A) { return A.Key == Key; });
	if (Found == nullptr || Found->Key.Kind != EAlertKind::FlightCannotLand)
	{
		UE_LOG(LogRoadBuild, Log, TEXT("Alerts: cancel of %s %d refused: %s"), *UEnum::GetValueAsString(Key.Kind), Key.Id,
			Found == nullptr ? TEXT("that alert has cleared") : TEXT("only a flight that cannot land offers one"));
		return false;
	}
	return Runtime.CancelFlight(Found->Key.Id);
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
	// READ HERE, FROM THE MODEL, every paint (#445) - the rows are a view of it, not of a list this window kept.
	const TArray<FOpsAlert>& Alerts = GetAlerts();
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

		// THE WARNING ICON, spec §3's row - the same texture a Warning toast shows, so the two read as one kind.
		if (WarningIcon != nullptr)
		{
			UImage* Icon = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass());
			Icon->SetBrushFromTexture(WarningIcon);
			Icon->SetDesiredSizeOverride(FVector2D(18.0, 18.0));
			Icon->SetColorAndOpacity(Style.Warning);
			UHorizontalBoxSlot* IconSlot = Line->AddChildToHorizontalBox(Icon);
			IconSlot->SetVerticalAlignment(VAlign_Center);
			IconSlot->SetPadding(FMargin(0.0f, 0.0f, 8.0f, 0.0f));
		}

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
		Entry->Key = Alert.Key;
		GoButton->OnClicked.AddDynamic(Entry, &UAlertRowEntry::HandleClick);
		Entries.Add(Entry);
		Line->AddChildToHorizontalBox(GoButton);

		// A FLIGHT THAT CAN NEVER LAND has no aeroplane for the card's Unstick to act on, so its way out is here (#442): the
		// fix is the player's airport, or this. Secondary, like Go - a cancel costs the airline's goodwill, and the button
		// that does is not the row's primary action.
		if (Alert.Key.Kind == EAlertKind::FlightCannotLand)
		{
			UUiButton* CancelButton = WidgetTree->ConstructWidget<UUiButton>(UUiButton::StaticClass());
			CancelButton->SetLabel(LOCTEXT("CancelFlight", "Cancel flight"));
			CancelButton->Build(Style, EUiButtonKind::Secondary);
			CancelButton->SetState(true, false);
			CancelButton->OnClicked.AddDynamic(Entry, &UAlertRowEntry::HandleCancelClick);
			Entry->CancelButton = CancelButton;
			UHorizontalBoxSlot* CancelSlot = Line->AddChildToHorizontalBox(CancelButton);
			CancelSlot->SetPadding(FMargin(6.0f, 0.0f, 0.0f, 0.0f));
		}

		UUiRow* Row = WidgetTree->ConstructWidget<UUiRow>(UUiRow::StaticClass());
		Row->Build(Style, FMargin(8.0f, 4.0f));
		Row->SetContent(Line);
		UVerticalBoxSlot* RowSlot = RowColumn->AddChildToVerticalBox(Row);
		RowSlot->SetPadding(FMargin(0.0f, 0.0f, 0.0f, Style.RowGap));
	}
}

#undef LOCTEXT_NAMESPACE
