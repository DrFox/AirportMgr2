#include "InspectorWidget.h"

#include "Blueprint/WidgetTree.h"
#include "BuildActions.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Model/FuelService.h"
#include "Model/InspectFacts.h"
#include "Model/RoadAgent.h"
#include "Present/OpsRuntime.h"
#include "Present/OpsRuntimeSubsystem.h"
#include "Present/RoadNetworkActor.h"
#include "RoadBuildController.h"
#include "UIStyle.h"

DEFINE_LOG_CATEGORY_STATIC(LogInspector, Log, All);

namespace
{
	/**
	 * Shows or hides the card - the visible chrome - BY NAME rather than through a bound slot:
	 * the code-built card is named InspectorCard, and a Blueprint restyle names its own card
	 * the same to get the hide-when-nothing-selected behaviour. A free function rather than a
	 * member so this fix was a function-body change Live Coding could apply.
	 */
	void ShowInspectorCard(UWidgetTree* Tree, bool bShown)
	{
		UWidget* Card = Tree != nullptr ? Tree->FindWidget(TEXT("InspectorCard")) : nullptr;
		if (Card == nullptr)
		{
			return;
		}
		Card->SetVisibility(bShown ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}

	bool IsInspectorCardShown(UWidgetTree* Tree)
	{
		UWidget* Card = Tree != nullptr ? Tree->FindWidget(TEXT("InspectorCard")) : nullptr;
		return Card != nullptr && Card->GetVisibility() != ESlateVisibility::Collapsed;
	}
}

void UInspectorWidget::BuildOnce(const UUIStyle& Style)
{
	// Cached for Refresh, which runs every tick: without this it called ResolveStyle() (a
	// TSoftObjectPtr::LoadSynchronous) itself just to recolour one button.
	CachedStyle = &Style;

	EnsureSlots(&Style);
	if (DepartButton != nullptr) { DepartButton->OnClicked.AddDynamic(this, &UInspectorWidget::HandleDepart); }
	if (FollowButton != nullptr) { FollowButton->OnClicked.AddDynamic(this, &UInspectorWidget::HandleFollow); }
	// SelfHitTestInvisible, not Collapsed: see UAirportMgrPanelWidget::BuildOnce for why an
	// otherwise-empty panel must stay this way. Only the CARD hides; the root stays laid out.
	SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	ShowInspectorCard(WidgetTree, false);
}

void UInspectorWidget::EnsureSlots(const UUIStyle* Style)
{
	// FOUND ONCE, POSITIONALLY. BuildActions.cpp's own comment says these two rows exist so
	// the panel, the bar and the C key are one list (spec §6.2); walking the Selection section
	// in the order it is built - depart, then follow - is what lets this panel ask for "its"
	// two verbs without retyping their ids (issue #91).
	const TConstArrayView<FBuildAction> Actions = BuildActions();
	int32 SelectionSeen = 0;
	for (int32 Index = 0; Index < Actions.Num(); ++Index)
	{
		if (Actions[Index].Section != EActionSection::Selection) { continue; }
		if (SelectionSeen == 0) { DepartActionIndex = Index; }
		else if (SelectionSeen == 1) { FollowActionIndex = Index; }
		++SelectionSeen;
	}

	// Code-built chrome only where the asset gave none - the same skeleton the offer inbox
	// uses (EnsureCardRoot, issue #90). A bottom-left card: title, facts, status, then the two
	// verbs in a row.
	UVerticalBox* Column = Cast<UVerticalBox>(EnsureCardRoot(TEXT("InspectorCard"),
		FAnchors(0.0f, 1.0f, 0.0f, 1.0f), FVector2D(0.0, 1.0), FVector2D(12.0, -BottomOffset), true));
	if (Column != nullptr)
	{
		UE_LOG(LogInspector, Log, TEXT("No inspector asset: building the code-only panel"));
	}

	auto Text = [&](TObjectPtr<UTextBlock>& Field, const TCHAR* Name, EUITextRole Role, FLinearColor Colour)
	{
		if (Field != nullptr) { return; }
		Field = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), Name);
		Style->ApplyText(*Field, Role, Colour);
		Field->SetAutoWrapText(true);
		Field->SetMinDesiredWidth(static_cast<float>(PanelWidth));
		if (Column != nullptr) { Column->AddChildToVerticalBox(Field)->SetPadding(FMargin(0.0f, 2.0f)); }
	};
	Text(TitleText, TEXT("TitleText"), EUITextRole::Title, Style->Text);
	Text(FactsText, TEXT("FactsText"), EUITextRole::Body, Style->TextMuted);
	Text(StatusText, TEXT("StatusText"), EUITextRole::Body, Style->TextMuted);

	UHorizontalBox* Row = nullptr;
	if (Column != nullptr && (DepartButton == nullptr || FollowButton == nullptr))
	{
		Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("InspectorVerbs"));
		Column->AddChildToVerticalBox(Row)->SetPadding(FMargin(0.0f, 8.0f, 0.0f, 0.0f));
	}

	// LABEL FROM Action.Label, KEY IN THE TOOLTIP - the bar's own convention
	// (UBuildBarWidget::BuildButtons), so a verb reads the same wherever it appears.
	auto Button = [&](TObjectPtr<UButton>& Field, const TCHAR* Name, int32 ActionIndex, TObjectPtr<UTextBlock>* OutLabel)
	{
		if (Field != nullptr || !Actions.IsValidIndex(ActionIndex)) { return; }
		const FBuildAction& Action = Actions[ActionIndex];
		Field = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), Name);
		UTextBlock* Label = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		Label->SetText(Action.Label);
		Style->ApplyText(*Label, EUITextRole::Label, Style->Text);
		Field->SetContent(Label);
		Field->SetBackgroundColor(Style->Button);
		if (OutLabel != nullptr) { *OutLabel = Label; }
		if (Action.Key.IsValid())
		{
			Field->SetToolTipText(FText::FromString(FString::Printf(TEXT("%s  (%s%s)"),
				*Action.Label.ToString(), Action.bRequiresCtrl ? TEXT("Ctrl+") : TEXT(""),
				*Action.Key.GetDisplayName().ToString())));
		}
		else
		{
			Field->SetToolTipText(Action.Label);
		}
		if (Row != nullptr) { Row->AddChildToHorizontalBox(Field)->SetPadding(FMargin(0.0f, 0.0f, 8.0f, 0.0f)); }
	};
	// DepartLabel is HELD, not re-found: Refresh recolours it every tick when Depart's
	// enabled state changes, and UBuildBarEntry holds its own Label for the same reason.
	Button(DepartButton, TEXT("DepartButton"), DepartActionIndex, &DepartLabel);
	Button(FollowButton, TEXT("FollowButton"), FollowActionIndex, nullptr);
}

void UInspectorWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	if (const ARoadBuildController* C = Controller())
	{
		Refresh(C->GetTarget(), C->GetSelection());
	}
}

void UInspectorWidget::Refresh(const ARoadNetworkActor* Target, const FSelection& Selection)
{
	if (Target == nullptr || !Selection.IsSet())
	{
		ShowInspectorCard(WidgetTree, false);
		bDepartEnabled = false;
		return;
	}

	FString Title, Facts, Status;
	bool bAircraft = false;
	if (Selection.Kind == ESelectionKind::Aircraft)
	{
		FAgentFacts F;
		if (Target->GetGroundTraffic() == nullptr || !InspectFacts::DescribeAgent(*Target->GetGroundTraffic(), Target->GetNetwork(), Selection.Id, F))
		{
			ShowInspectorCard(WidgetTree, false);
			bDepartEnabled = false;
			return;
		}
		bAircraft = true;
		Title = FString::Printf(TEXT("%s  #%d"), *F.TypeName, F.Id);
		// m/s and knots side by side: the sim's unit and the one a pilot reads.
		Facts = FString::Printf(TEXT("Heading %03.0f\nSpeed %.1f m/s (%.0f kt)\nAltitude %.0f m\nTo %s\nEngine %s"),
			F.HeadingDegrees, F.GroundSpeed / 100.0, F.GroundSpeed / 100.0 * 1.94384, F.Altitude / 100.0,
			*F.Destination, F.bEngineRunning ? TEXT("running") : TEXT("off"));
		Status = F.Status;
		bDepartEnabled = F.bCanDepart;

		// THE FUEL LINE, from the layer that knows what fuel is. Reached through the ops
		// subsystem rather than through Target, because the airport actor is Airside's and
		// must not carry a pointer to a service it is forbidden to know about - see
		// FAgentFacts::Fuel, the field this fills and DescribeAgent deliberately leaves empty.
		if (const UOpsRuntime* Runtime = UOpsRuntimeSubsystem::Get(GetWorld()))
		{
			if (const UFuelService* Fuel = Runtime->GetFuelService())
			{
				F.Fuel = Fuel->DescribeAgent(F.Id);
			}
		}
		if (!F.Fuel.IsEmpty())
		{
			Facts += FString::Printf(TEXT("\nFuel %s"), *F.Fuel);
		}
	}
	else
	{
		FStandFacts S;
		if (Target->GetNetwork() == nullptr || !InspectFacts::DescribeStand(Target->GetGroundTraffic(), *Target->GetNetwork(), Selection.Id, S))
		{
			ShowInspectorCard(WidgetTree, false);
			bDepartEnabled = false;
			return;
		}
		// AN ENTITY, NOT ALWAYS A STAND, since the fuel slice. PoseRole is what tells the two
		// apart (see FStandFacts::PoseRole); a stand's own card is unchanged.
		if (S.PoseRole == EServiceRole::Aircraft)
		{
			Title = FString::Printf(TEXT("Stand %d"), S.Index);
			Facts = FString::Printf(TEXT("Code %s (%.0f m span)\n%d service anchors\n%s"),
				*S.SizeClass, S.DesignWingspan / 100.0, S.AnchorCount,
				S.bReachable ? TEXT("Reachable by taxiway") : TEXT("NOT reachable - no taxiway joins it"));
			Status = S.OccupantAgent == 0 ? FString(TEXT("Empty"))
				: S.bOccupantParked ? FString::Printf(TEXT("Occupied by aircraft #%d"), S.OccupantAgent)
				: FString::Printf(TEXT("Reserved for aircraft #%d"), S.OccupantAgent);
		}
		else
		{
			// bReachable is the pose node having line on it, which for a depot means a
			// SERVICE ROAD within its lead-in reach. The message names the fix rather than
			// the symptom: the road is the thing the player goes and draws.
			Title = FString::Printf(TEXT("Fuel depot %d"), S.Index);
			Facts = S.bReachable
				? FString(TEXT("On a service road"))
				: FString(TEXT("Fuel depot: not on a road"));
			Status = S.bReachable ? TEXT("Ready") : TEXT("Cannot dispatch");
		}
		bDepartEnabled = false;
	}

	if (TitleText != nullptr) { TitleText->SetText(FText::FromString(Title)); }
	if (FactsText != nullptr) { FactsText->SetText(FText::FromString(Facts)); }
	if (StatusText != nullptr) { StatusText->SetText(FText::FromString(Status)); }
	if (DepartButton != nullptr)
	{
		// Cached in BuildOnce, not re-resolved here: Refresh runs every tick.
		const UUIStyle* Style = CachedStyle != nullptr ? CachedStyle.Get() : UAirportMgrUISettings::ResolveStyle();
		DepartButton->SetVisibility(bAircraft ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
		DepartButton->SetIsEnabled(bDepartEnabled);

		// THE BAR'S OWN RULE (UBuildBarWidget::RefreshState): the BUTTON stays Style->Button
		// always: disabled dims a button by darkening it under Button, never by brightening
		// it, and TextMuted is a LABEL slot (it is lighter than Button on purpose, for text
		// over a dark ground) - painting a disabled background with it made Depart look
		// MORE prominent while taxiing than while parked, backwards from the intent. Only the
		// CAPTION follows enabled state, exactly as the bar's icon/label content does.
		DepartButton->SetBackgroundColor(Style->Button);
		if (DepartLabel != nullptr)
		{
			DepartLabel->SetColorAndOpacity(FSlateColor(bDepartEnabled ? Style->Text : Style->TextMuted));
		}
	}
	if (FollowButton != nullptr)
	{
		FollowButton->SetVisibility(bAircraft ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}
	ShowInspectorCard(WidgetTree, true);
}

void UInspectorWidget::RunAction(int32 ActionIndex)
{
	ARoadBuildController* C = Controller();
	if (C == nullptr)
	{
		UE_LOG(LogInspector, Warning, TEXT("Inspector click %d ignored: no controller"), ActionIndex);
		return;
	}
	const TConstArrayView<FBuildAction> Actions = BuildActions();
	if (!Actions.IsValidIndex(ActionIndex))
	{
		// Should not happen - EnsureSlots finds both indices from the registry itself - but a
		// missing row degrades to a no-op rather than a crash, same as the old by-id lookup.
		UE_LOG(LogInspector, Warning, TEXT("Inspector click %d ignored: no such action"), ActionIndex);
		return;
	}
	if (!Actions[ActionIndex].IsEnabled(*C))
	{
		return;
	}
	UE_LOG(LogInspector, Log, TEXT("Inspector: %s"), *Actions[ActionIndex].Id.ToString());
	Actions[ActionIndex].Execute(*C);
}

void UInspectorWidget::HandleDepart() { RunAction(DepartActionIndex); }
void UInspectorWidget::HandleFollow() { RunAction(FollowActionIndex); }

bool UInspectorWidget::IsShownForTest() const { return IsInspectorCardShown(WidgetTree); }
bool UInspectorWidget::IsDepartEnabledForTest() const { return bDepartEnabled; }
FString UInspectorWidget::TitleForTest() const { return TitleText != nullptr ? TitleText->GetText().ToString() : FString(); }
FLinearColor UInspectorWidget::DepartLabelColourForTest() const
{
	return DepartLabel != nullptr ? DepartLabel->GetColorAndOpacity().GetSpecifiedColor() : FLinearColor::Black;
}
