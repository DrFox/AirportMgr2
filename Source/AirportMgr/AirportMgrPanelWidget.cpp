#include "AirportMgrPanelWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/VerticalBox.h"
#include "OpsRuntimeResolver.h"
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

UOpsRuntime* UAirportMgrPanelWidget::OpsRuntime() const
{
	return OpsRuntimeResolver::Resolve(GetWorld());
}

UPanelWidget* UAirportMgrPanelWidget::EnsureContentRoot(FName ContentName)
{
	if (WidgetTree->RootWidget != nullptr)
	{
		// An asset already supplied a root: BindWidgetOptional has filled every slot it offers,
		// and a code-built root here would replace the designer's own layout.
		return nullptr;
	}
	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), ContentName);
	WidgetTree->RootWidget = Column;
	return Column;
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

void UAirportMgrPanelWidget::OnShownChanged(bool /*bShown*/)
{
}

void UAirportMgrPanelWidget::Toggle()
{
	if (Host != nullptr)
	{
		// THE HOST'S STATE, and the request follows it so an IsShown answered without the host (after a detach) is not stale.
		bShownRequested = Host->Toggle(WindowId);
		return;
	}
	SetShown(!bShownRequested);
}

void UAirportMgrPanelWidget::SetShown(bool bShown)
{
	const bool bWas = bShownRequested;
	bShownRequested = bShown;
	if (Host != nullptr)
	{
		Host->SetShown(WindowId, bShown);   // which tells the panel, through OnShownChanged, if the window changed
	}
	else if (bWas != bShown)
	{
		// NO HOST TO TELL US: the headless test and any unhosted panel hear their own change, so the hook means the same with or without a window.
		OnShownChanged(bShown);
	}
}

void UAirportMgrPanelWidget::SetWindowBadge(const FText& Badge)
{
	if (Host != nullptr)
	{
		Host->SetBadge(WindowId, Badge);
	}
}

bool UAirportMgrPanelWidget::IsShown() const
{
	return Host != nullptr ? Host->IsShown(WindowId) : bShownRequested;
}

bool UAirportMgrPanelWidget::IsFolded() const
{
	return Host != nullptr && Host->IsCollapsed(WindowId);
}

void UAirportMgrPanelWidget::ForgetPlayerClose()
{
	if (Host != nullptr)
	{
		Host->ForgetDismissal(WindowId);
	}
}
