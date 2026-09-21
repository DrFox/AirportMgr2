#include "AirportMgrPanelWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/VerticalBox.h"
#include "RoadBuildController.h"
#include "Styling/SlateBrush.h"
#include "UIStyle.h"

bool UAirportMgrPanelWidget::Initialize()
{
	const bool bOk = Super::Initialize();
	if (!bOk || bBuilt || HasAnyFlags(RF_ClassDefaultObject) || WidgetTree == nullptr)
	{
		return bOk;
	}
	bBuilt = true;
	const UUIStyle& Style = *UAirportMgrUISettings::ResolveStyle();   // never null
	// CACHED BEFORE BuildOnce RUNS, not after: a subclass's own BuildOnce (UInspectorWidget's,
	// ULedgerPanelWidget's) reads PanelStyle from inside its own EnsureSlots, called from here.
	PanelStyle = &Style;
	BuildOnce(Style);
	return bOk;
}

ARoadBuildController* UAirportMgrPanelWidget::Controller() const
{
	if (APlayerController* Owning = GetOwningPlayer())
	{
		return Cast<ARoadBuildController>(Owning);
	}
	return GetWorld() ? Cast<ARoadBuildController>(GetWorld()->GetFirstPlayerController()) : nullptr;
}

UPanelWidget* UAirportMgrPanelWidget::EnsureCardRoot(FName CardName, const FAnchors& Anchors,
	FVector2D Alignment, FVector2D Position, bool bRounded)
{
	UVerticalBox* Column = nullptr;
	if (WidgetTree->RootWidget == nullptr)
	{
		const UUIStyle* Style = UAirportMgrUISettings::ResolveStyle();   // never null

		UCanvasPanel* Root = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("PanelRoot"));
		WidgetTree->RootWidget = Root;

		UBorder* CardBorder = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), CardName);
		if (bRounded)
		{
			CardBorder->SetBrush(FSlateRoundedBoxBrush(Style->PanelDark, Style->CornerRadius));
		}
		else
		{
			CardBorder->SetBrushColor(Style->PanelDark);
		}
		// UUIStyle::CardPadding, not a literal here: see its own comment (issue #192).
		CardBorder->SetPadding(Style->CardPadding);

		UCanvasPanelSlot* CardSlot = Root->AddChildToCanvas(CardBorder);
		CardSlot->SetAnchors(Anchors);
		CardSlot->SetAlignment(Alignment);
		CardSlot->SetAutoSize(true);
		CardSlot->SetPosition(Position);

		Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
		CardBorder->SetContent(Column);
	}
	// An asset already supplied a root: BindWidgetOptional has filled every slot it offers,
	// and a code-built card here would replace the designer's own layout - Column stays null
	// for that path, same as before.
	//
	// FOUND BY NAME EITHER WAY, and cached in CardWidget - a Blueprint restyle names its own card to
	// match (UInspectorWidget's class comment states the convention), so this is the one place
	// that has to know the name at all. See CardWidget and SetCardShown's own comments (issue #187):
	// two subclasses used to do this same FindWidget themselves, one of them every tick.
	CardWidget = WidgetTree != nullptr ? WidgetTree->FindWidget(CardName) : nullptr;
	return Column;
}

void UAirportMgrPanelWidget::SetCardShown(bool bShown)
{
	if (CardWidget == nullptr)
	{
		return;
	}
	const ESlateVisibility Wanted = bShown ? ESlateVisibility::Visible : ESlateVisibility::Collapsed;
	if (CardWidget->GetVisibility() != Wanted)
	{
		CardWidget->SetVisibility(Wanted);
	}
}
