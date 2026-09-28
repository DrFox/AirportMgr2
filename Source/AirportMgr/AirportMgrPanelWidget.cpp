#include "AirportMgrPanelWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/VerticalBox.h"
#include "RoadBuildController.h"
#include "RoadBuildLog.h"
#include "Styling/SlateBrush.h"
#include "UI/UiClicks.h"
#include "UI/UiWindowHost.h"
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
	++BuildOnceCalls;   // See BuildOnceCallCountForTest - must read 1 even across a re-Initialize.
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
		// PanelStyle, not another ResolveStyle() call (issue #309): Initialize sets it before
		// BuildOnce runs, and EnsureCardRoot is only ever called FROM BuildOnce (or something
		// it calls), so the resolve two lines above in Initialize has always already happened
		// by the time this runs - this was a second LoadSynchronous of the same asset for no
		// reason, not a per-frame cost (EnsureCardRoot runs once, guarded by bBuilt).
		const UUIStyle* Style = PanelStyle;

		UCanvasPanel* Root = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("PanelRoot"));
		WidgetTree->RootWidget = Root;

		UBorder* CardBorder = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), CardName);
		if (bRounded)
		{
			CardBorder->SetBrush(FSlateRoundedBoxBrush(Style->Surface, Style->WindowRadius, Style->Rule, 1.0f));
		}
		else
		{
			CardBorder->SetBrushColor(Style->Surface);
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

FReply UAirportMgrPanelWidget::NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	return UiClicks::EatUnhandled(*this, WidgetTree != nullptr ? WidgetTree->RootWidget : nullptr,
		Super::NativeOnMouseButtonDown(InGeometry, InMouseEvent), TEXT("down"));
}

FReply UAirportMgrPanelWidget::NativeOnMouseButtonDoubleClick(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	return UiClicks::EatUnhandled(*this, WidgetTree != nullptr ? WidgetTree->RootWidget : nullptr,
		Super::NativeOnMouseButtonDoubleClick(InGeometry, InMouseEvent), TEXT("double-click"));
}

bool UAirportMgrPanelWidget::WantsWindow(FUiWindowSpec& Out) const
{
	return false;
}

void UAirportMgrPanelWidget::AttachToHost(UUiWindowHost& InHost, FName InId)
{
	Host = &InHost;
	WindowId = InId;
	InHost.SetShown(WindowId, bShownRequested);   // whatever BuildOnce already asked for
}

void UAirportMgrPanelWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	// HOSTED PANELS ARE TICKED BY THE HOST, hidden or not - see RunPanelTick. Doing it here too
	// would tick a visible hosted panel twice. ENFORCED BY: AirportMgr.UI.WindowHost.TicksHiddenPanelsOnce.
	if (Host == nullptr)
	{
		RunPanelTick(InDeltaTime);
	}
}

void UAirportMgrPanelWidget::RunPanelTick(float DeltaTime)
{
	++PanelTicks;
	TickPanel(DeltaTime);
}

void UAirportMgrPanelWidget::TickPanel(float DeltaTime)
{
}

void UAirportMgrPanelWidget::OnWindowClosedByPlayer()
{
}

void UAirportMgrPanelWidget::SetShown(bool bShown)
{
	bShownRequested = bShown;
	if (Host != nullptr)
	{
		Host->SetShown(WindowId, bShown);
	}
}

bool UAirportMgrPanelWidget::IsShown() const
{
	return Host != nullptr ? Host->IsShown(WindowId) : bShownRequested;
}

void UAirportMgrPanelWidget::ForgetPlayerClose()
{
	if (Host != nullptr)
	{
		Host->ForgetDismissal(WindowId);
	}
}
