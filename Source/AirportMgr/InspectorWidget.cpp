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
#include "Model/InspectFacts.h"
#include "Model/RoadAgent.h"
#include "Present/RoadNetworkActor.h"
#include "RoadBuildController.h"

DEFINE_LOG_CATEGORY_STATIC(LogInspector, Log, All);

ARoadBuildController* UInspectorWidget::Controller() const
{
	if (APlayerController* Owning = GetOwningPlayer())
	{
		return Cast<ARoadBuildController>(Owning);
	}
	return GetWorld() ? Cast<ARoadBuildController>(GetWorld()->GetFirstPlayerController()) : nullptr;
}

bool UInspectorWidget::Initialize()
{
	const bool bOk = Super::Initialize();
	if (!bOk || bBuilt || HasAnyFlags(RF_ClassDefaultObject) || WidgetTree == nullptr)
	{
		return bOk;
	}
	bBuilt = true;
	EnsureSlots();
	if (DepartButton != nullptr) { DepartButton->OnClicked.AddDynamic(this, &UInspectorWidget::HandleDepart); }
	if (FollowButton != nullptr) { FollowButton->OnClicked.AddDynamic(this, &UInspectorWidget::HandleFollow); }
	SetVisibility(ESlateVisibility::Collapsed);
	return bOk;
}

void UInspectorWidget::EnsureSlots()
{
	// Code-built chrome only where the asset gave none - the same rule as the bar. A
	// bottom-left card: title, facts, status, then the two verbs in a row.
	UVerticalBox* Column = nullptr;
	if (WidgetTree->RootWidget == nullptr)
	{
		UCanvasPanel* Root = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("InspectorRoot"));
		WidgetTree->RootWidget = Root;
		UBorder* Card = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("InspectorCard"));
		Card->SetBrushColor(PanelTint);
		Card->SetPadding(FMargin(12.0f, 10.0f));
		UCanvasPanelSlot* CardSlot = Root->AddChildToCanvas(Card);
		CardSlot->SetAnchors(FAnchors(0.0f, 1.0f, 0.0f, 1.0f));
		CardSlot->SetAlignment(FVector2D(0.0, 1.0));
		CardSlot->SetAutoSize(true);
		CardSlot->SetPosition(FVector2D(12.0, -BottomOffset));
		Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("InspectorColumn"));
		Card->SetContent(Column);
		UE_LOG(LogInspector, Log, TEXT("No inspector asset: building the code-only panel"));
	}

	auto Text = [&](TObjectPtr<UTextBlock>& Field, const TCHAR* Name)
	{
		if (Field != nullptr) { return; }
		Field = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), Name);
		FSlateFontInfo Font = Field->GetFont();
		Font.Size = FontSize;
		Field->SetFont(Font);
		Field->SetAutoWrapText(true);
		Field->SetMinDesiredWidth(static_cast<float>(PanelWidth));
		if (Column != nullptr) { Column->AddChildToVerticalBox(Field)->SetPadding(FMargin(0.0f, 2.0f)); }
	};
	Text(TitleText, TEXT("TitleText"));
	Text(FactsText, TEXT("FactsText"));
	Text(StatusText, TEXT("StatusText"));

	UHorizontalBox* Row = nullptr;
	if (Column != nullptr && (DepartButton == nullptr || FollowButton == nullptr))
	{
		Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("InspectorVerbs"));
		Column->AddChildToVerticalBox(Row)->SetPadding(FMargin(0.0f, 8.0f, 0.0f, 0.0f));
	}
	auto Button = [&](TObjectPtr<UButton>& Field, const TCHAR* Name, const TCHAR* Caption)
	{
		if (Field != nullptr) { return; }
		Field = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), Name);
		UTextBlock* Label = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		Label->SetText(FText::FromString(Caption));
		FSlateFontInfo Font = Label->GetFont();
		Font.Size = FontSize;
		Label->SetFont(Font);
		Field->SetContent(Label);
		Field->SetBackgroundColor(ButtonTint);
		if (Row != nullptr) { Row->AddChildToHorizontalBox(Field)->SetPadding(FMargin(0.0f, 0.0f, 8.0f, 0.0f)); }
	};
	Button(DepartButton, TEXT("DepartButton"), TEXT("Depart"));
	Button(FollowButton, TEXT("FollowButton"), TEXT("Follow (C)"));
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
		SetVisibility(ESlateVisibility::Collapsed);
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
			SetVisibility(ESlateVisibility::Collapsed);
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
	}
	else
	{
		FStandFacts S;
		if (Target->GetNetwork() == nullptr || !InspectFacts::DescribeStand(Target->GetGroundTraffic(), *Target->GetNetwork(), Selection.Id, S))
		{
			SetVisibility(ESlateVisibility::Collapsed);
			bDepartEnabled = false;
			return;
		}
		Title = FString::Printf(TEXT("Stand %d"), S.Index);
		Facts = FString::Printf(TEXT("Code %s (%.0f m span)\n%d service anchors\n%s"),
			*S.SizeClass, S.DesignWingspan / 100.0, S.AnchorCount,
			S.bReachable ? TEXT("Reachable by taxiway") : TEXT("NOT reachable - no taxiway joins it"));
		Status = S.OccupantAgent != 0 ? FString::Printf(TEXT("Occupied by aircraft #%d"), S.OccupantAgent) : FString(TEXT("Empty"));
		bDepartEnabled = false;
	}

	if (TitleText != nullptr) { TitleText->SetText(FText::FromString(Title)); }
	if (FactsText != nullptr) { FactsText->SetText(FText::FromString(Facts)); }
	if (StatusText != nullptr) { StatusText->SetText(FText::FromString(Status)); }
	if (DepartButton != nullptr)
	{
		DepartButton->SetVisibility(bAircraft ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
		DepartButton->SetIsEnabled(bDepartEnabled);
		DepartButton->SetBackgroundColor(bDepartEnabled ? ButtonTint : DisabledTint);
	}
	if (FollowButton != nullptr)
	{
		FollowButton->SetVisibility(bAircraft ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}
	// The root itself takes no clicks - an empty canvas that swallowed them would eat the
	// tool's clicks under the panel - but its children (the buttons) do.
	SetVisibility(ESlateVisibility::SelfHitTestInvisible);
}

void UInspectorWidget::RunActionById(FName Id)
{
	ARoadBuildController* C = Controller();
	if (C == nullptr)
	{
		UE_LOG(LogInspector, Warning, TEXT("Inspector %s ignored: no controller"), *Id.ToString());
		return;
	}
	for (const FBuildAction& A : BuildActions())
	{
		if (A.Id == Id)
		{
			if (A.IsEnabled(*C))
			{
				UE_LOG(LogInspector, Log, TEXT("Inspector: %s"), *Id.ToString());
				A.Execute(*C);
			}
			return;
		}
	}
	UE_LOG(LogInspector, Warning, TEXT("Inspector: no action named %s"), *Id.ToString());
}

void UInspectorWidget::HandleDepart() { RunActionById(TEXT("selection.depart")); }
void UInspectorWidget::HandleFollow() { RunActionById(TEXT("selection.follow")); }

bool UInspectorWidget::IsShownForTest() const { return GetVisibility() != ESlateVisibility::Collapsed; }
bool UInspectorWidget::IsDepartEnabledForTest() const { return bDepartEnabled; }
FString UInspectorWidget::TitleForTest() const { return TitleText != nullptr ? TitleText->GetText().ToString() : FString(); }
