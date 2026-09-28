#include "UI/UiWindowHost.h"

#include "AirportMgrPanelWidget.h"
#include "Blueprint/WidgetTree.h"
#include "BuildBarWidget.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "RoadBuildLog.h"
#include "UI/UiWindow.h"
#include "UI/WindowSnap.h"
#include "UIStyle.h"

bool UUiWindowHost::Initialize()
{
	const bool bOk = Super::Initialize();
	if (!bOk || Canvas != nullptr || HasAnyFlags(RF_ClassDefaultObject) || WidgetTree == nullptr)
	{
		return bOk;
	}
	Style = UAirportMgrUISettings::ResolveStyle();   // never null
	Canvas = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("WindowCanvas"));
	WidgetTree->RootWidget = Canvas;
	// Click-transparent: empty screen between windows still reaches the game.
	SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	return bOk;
}

UUiWindow* UUiWindowHost::AddWindow(UAirportMgrPanelWidget& Panel)
{
	FUiWindowSpec Spec;
	if (Canvas == nullptr || !Panel.WantsWindow(Spec))
	{
		return nullptr;
	}
	if (Find(Spec.Id) != nullptr)
	{
		// ONE ID, ONE WINDOW: every call here finds a window by id, and step 3 persists by it.
		UE_LOG(LogRoadBuild, Error, TEXT("Window %s: a second panel (%s) claimed an id already hosted - not added"),
			*Spec.Id.ToString(), *Panel.GetName());
		return nullptr;
	}
	UUiWindow* Window = CreateWidget<UUiWindow>(this, UUiWindow::StaticClass());
	Window->Build(*Style, Spec, Panel, *this);

	UCanvasPanelSlot* CanvasSlot = Canvas->AddChildToCanvas(Window);
	CanvasSlot->SetAutoSize(true);
	switch (Spec.Anchor)
	{
	case EUiWindowAnchor::TopRight:
		CanvasSlot->SetAnchors(FAnchors(1.0f, 0.0f));
		CanvasSlot->SetAlignment(FVector2D(1.0, 0.0));
		CanvasSlot->SetPosition(FVector2D(-Spec.Offset.X, Spec.Offset.Y));
		break;
	case EUiWindowAnchor::AboveBarLeft:
		CanvasSlot->SetAnchors(FAnchors(0.0f, 1.0f));
		CanvasSlot->SetAlignment(FVector2D(0.0, 1.0));
		CanvasSlot->SetPosition(FVector2D(Spec.Offset.X, -(BarHeight() + Spec.Offset.Y)));
		break;
	default:
		CanvasSlot->SetAnchors(FAnchors(0.0f, 0.0f));
		CanvasSlot->SetAlignment(FVector2D::ZeroVector);
		CanvasSlot->SetPosition(Spec.Offset);
		break;
	}
	CanvasSlot->SetZOrder(++TopZ);

	FUiWindowEntry& E = Windows.AddDefaulted_GetRef();
	E.Window = Window;
	E.Panel = &Panel;
	E.Slot = CanvasSlot;
	E.Spec = Spec;
	Apply(E);
	UE_LOG(LogRoadBuild, Log, TEXT("Window %s: hosted (%s)"), *Spec.Id.ToString(), *Panel.GetName());
	Panel.AttachToHost(*this, Spec.Id);   // applies whatever the panel already asked for
	return Window;
}

FUiWindowEntry* UUiWindowHost::Find(FName Id)
{
	return Windows.FindByPredicate([Id](const FUiWindowEntry& E) { return E.Spec.Id == Id; });
}

const FUiWindowEntry* UUiWindowHost::Find(FName Id) const
{
	return Windows.FindByPredicate([Id](const FUiWindowEntry& E) { return E.Spec.Id == Id; });
}

void UUiWindowHost::Apply(FUiWindowEntry& E)
{
	const ESlateVisibility Wanted = (E.bWanted && !E.bUserClosed)
		? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed;
	// SetVisibility has no early-out of its own; panels ask every tick.
	if (E.Window != nullptr && E.Window->GetVisibility() != Wanted)
	{
		E.Window->SetVisibility(Wanted);
		UE_LOG(LogRoadBuild, Log, TEXT("Window %s: %s"), *E.Spec.Id.ToString(),
			Wanted == ESlateVisibility::Collapsed ? TEXT("hidden") : TEXT("shown"));
	}
}

void UUiWindowHost::SetShown(FName Id, bool bShown)
{
	if (FUiWindowEntry* E = Find(Id))
	{
		if (!bShown)
		{
			E->bUserClosed = false;   // the panel hid it itself: its next show is a fresh one
		}
		E->bWanted = bShown;
		Apply(*E);
	}
}

bool UUiWindowHost::IsShown(FName Id) const
{
	const FUiWindowEntry* E = Find(Id);
	return E != nullptr && E->bWanted && !E->bUserClosed;
}

void UUiWindowHost::ForgetDismissal(FName Id)
{
	if (FUiWindowEntry* E = Find(Id))
	{
		E->bUserClosed = false;
		Apply(*E);
	}
}

void UUiWindowHost::CloseByPlayer(FName Id)
{
	if (FUiWindowEntry* E = Find(Id))
	{
		E->bUserClosed = true;
		Apply(*E);
		UE_LOG(LogRoadBuild, Log, TEXT("Window %s: closed by the player"), *Id.ToString());
		if (E->Panel != nullptr)
		{
			E->Panel->OnWindowClosedByPlayer();
		}
	}
}

void UUiWindowHost::BringToFront(FName Id)
{
	if (FUiWindowEntry* E = Find(Id))
	{
		// Already on top: left alone, or every frame of a drag would re-number it.
		if (E->Slot != nullptr && E->Slot->GetZOrder() != TopZ)
		{
			E->Slot->SetZOrder(++TopZ);
		}
	}
}

void UUiWindowHost::DockAbove(const UBuildBarWidget* Bar)
{
	DockBar = Bar;
	UE_LOG(LogRoadBuild, Log, TEXT("Window host: %s"), Bar != nullptr ? TEXT("docked above the build bar") : TEXT("no bar to dock above"));
}

double UUiWindowHost::BarHeight() const
{
	return DockBar != nullptr ? DockBar->LiveHeight() : 0.0;
}

FBox2D UUiWindowHost::Bounds() const
{
	return FBox2D(FVector2D::ZeroVector, FVector2D(ViewSize.X, FMath::Max(0.0, ViewSize.Y - BarHeight())));
}

FVector2D UUiWindowHost::SizeOf(const FUiWindowEntry& E) const
{
	if (E.Slot == nullptr || E.Window == nullptr)
	{
		return FVector2D::ZeroVector;
	}
	return E.Slot->GetAutoSize() ? E.Window->GetDesiredSize() : E.Slot->GetSize();
}

FVector2D UUiWindowHost::TopLeftOf(const FUiWindowEntry& E) const
{
	if (E.Slot == nullptr)
	{
		return FVector2D::ZeroVector;
	}
	// Computed, not read off cached geometry: anchor point + position - alignment * size is what
	// SConstraintCanvas lays out, and it holds before the first paint and in a headless test.
	const FAnchors A = E.Slot->GetAnchors();
	return FVector2D(A.Minimum.X * ViewSize.X, A.Minimum.Y * ViewSize.Y)
		+ E.Slot->GetPosition() - E.Slot->GetAlignment() * SizeOf(E);
}

FBox2D UUiWindowHost::WindowRect(FName Id) const
{
	const FUiWindowEntry* E = Find(Id);
	if (E == nullptr)
	{
		return FBox2D(ForceInit);
	}
	const FVector2D TL = TopLeftOf(*E);
	return FBox2D(TL, TL + SizeOf(*E));
}

TArray<FBox2D> UUiWindowHost::OthersThan(FName Id) const
{
	TArray<FBox2D> Out;
	for (const FUiWindowEntry& E : Windows)
	{
		if (E.Spec.Id != Id && E.bWanted && !E.bUserClosed)   // SnapsOnlyToShownWindows
		{
			Out.Add(WindowRect(E.Spec.Id));
		}
	}
	return Out;
}

void UUiWindowHost::Place(FUiWindowEntry& E)
{
	if (E.bPlaced || E.Slot == nullptr)
	{
		return;
	}
	const FVector2D TL = TopLeftOf(E);
	E.Slot->SetAnchors(FAnchors(0.0f, 0.0f));
	E.Slot->SetAlignment(FVector2D::ZeroVector);
	E.Slot->SetPosition(TL);
	E.bPlaced = true;
}

void UUiWindowHost::MoveWindow(FName Id, FVector2D ProposedTopLeft)
{
	FUiWindowEntry* E = Find(Id);
	if (E == nullptr || E->Slot == nullptr)
	{
		return;
	}
	Place(*E);
	const TArray<FBox2D> Others = OthersThan(Id);
	E->Slot->SetPosition(WindowSnap::Place(ProposedTopLeft, SizeOf(*E), Bounds(), Others, Style->SnapDistance, Style->WindowMargin));
}

void UUiWindowHost::ResizeWindow(FName Id, FVector2D ProposedSize)
{
	FUiWindowEntry* E = Find(Id);
	if (E == nullptr || E->Slot == nullptr || !E->Spec.bResizable)
	{
		return;
	}
	Place(*E);
	const FVector2D TL = E->Slot->GetPosition();
	const TArray<FBox2D> Others = OthersThan(Id);
	const FVector2D Size = WindowSnap::Resize(TL, ProposedSize, Style->WindowMinSize, Bounds(), Others,
		Style->SnapDistance, Style->WindowMargin);
	E->Slot->SetAutoSize(false);
	E->Slot->SetSize(Size);
}

void UUiWindowHost::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	if (MyGeometry.GetLocalSize().X > 0.0 && MyGeometry.GetLocalSize().Y > 0.0)
	{
		ViewSize = MyGeometry.GetLocalSize();
	}
	TickWindows(InDeltaTime);
}

void UUiWindowHost::TickWindows(float DeltaTime)
{
	for (FUiWindowEntry& E : Windows)
	{
		if (E.Panel != nullptr)
		{
			E.Panel->RunPanelTick(DeltaTime);
		}
		if (E.Slot == nullptr)
		{
			continue;
		}
		if (!E.bPlaced && E.Spec.Anchor == EUiWindowAnchor::AboveBarLeft)
		{
			// RIDES THE BAR'S TOP EDGE as the bar grows and shrinks (the inspector's old DockAbove,
			// 2026-09-27: a fixed offset left it over the bar's left-hand sections). Written only
			// when it changed, so an unchanged bar writes nothing.
			const FVector2D Want(E.Spec.Offset.X, -(BarHeight() + E.Spec.Offset.Y));
			if (!E.Slot->GetPosition().Equals(Want))
			{
				E.Slot->SetPosition(Want);
			}
		}
		else if (E.bPlaced)
		{
			// A PLACED WINDOW STAYS REACHABLE when the view shrinks under it (ReclampsWhenTheViewShrinks):
			// a pure clamp, no snapping - nobody is dragging.
			const FVector2D At = E.Slot->GetPosition();
			const FVector2D Clamped = WindowSnap::Place(At, SizeOf(E), Bounds(), TConstArrayView<FBox2D>(), 0.0, 0.0);
			if (!Clamped.Equals(At))
			{
				E.Slot->SetPosition(Clamped);
			}
		}
	}
}

FVector2D UUiWindowHost::ToLocal(FVector2D ScreenPosition) const
{
	return GetCachedGeometry().AbsoluteToLocal(ScreenPosition);
}

UUiWindow* UUiWindowHost::WindowForTest(FName Id) const
{
	const FUiWindowEntry* E = Find(Id);
	return E != nullptr ? E->Window.Get() : nullptr;
}

int32 UUiWindowHost::ZOrderForTest(FName Id) const
{
	const FUiWindowEntry* E = Find(Id);
	return E != nullptr && E->Slot != nullptr ? E->Slot->GetZOrder() : INDEX_NONE;
}

double UUiWindowHost::WindowClearanceForTest(FName Id) const
{
	const FUiWindowEntry* E = Find(Id);
	return E != nullptr && E->Slot != nullptr ? -E->Slot->GetPosition().Y : 0.0;
}
