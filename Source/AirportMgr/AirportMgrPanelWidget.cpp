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
	BuildOnce(*UAirportMgrUISettings::ResolveStyle());   // never null
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
	if (WidgetTree->RootWidget != nullptr)
	{
		// An asset already supplied a root: BindWidgetOptional has filled every slot it
		// offers, and a code-built card here would replace the designer's own layout.
		return nullptr;
	}
	const UUIStyle* Style = UAirportMgrUISettings::ResolveStyle();   // never null

	UCanvasPanel* Root = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("PanelRoot"));
	WidgetTree->RootWidget = Root;

	UBorder* Card = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), CardName);
	if (bRounded)
	{
		Card->SetBrush(FSlateRoundedBoxBrush(Style->PanelDark, Style->CornerRadius));
	}
	else
	{
		Card->SetBrushColor(Style->PanelDark);
	}
	Card->SetPadding(FMargin(12.0f, 10.0f));

	UCanvasPanelSlot* CardSlot = Root->AddChildToCanvas(Card);
	CardSlot->SetAnchors(Anchors);
	CardSlot->SetAlignment(Alignment);
	CardSlot->SetAutoSize(true);
	CardSlot->SetPosition(Position);

	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Card->SetContent(Column);
	return Column;
}
