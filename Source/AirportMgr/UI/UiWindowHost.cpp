#include "UI/UiWindowHost.h"

#include "AirportMgrPanelWidget.h"
#include "Blueprint/WidgetTree.h"
#include "BuildBarWidget.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "RoadBuildLog.h"
#include "UI/UiScrim.h"
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
	UUiScrim* Sheet = WidgetTree->ConstructWidget<UUiScrim>(UUiScrim::StaticClass(), TEXT("ModalScrim"));
	Sheet->Build(*Style);
	UCanvasPanelSlot* SheetSlot = Canvas->AddChildToCanvas(Sheet);
	SheetSlot->SetAnchors(FAnchors(0.0f, 0.0f, 1.0f, 1.0f));
	SheetSlot->SetOffsets(FMargin(0.0f));
	Scrim = Sheet;
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
	CanvasSlot->SetZOrder(++TopZ);

	FUiWindowEntry& E = Windows.AddDefaulted_GetRef();
	E.Window = Window;
	E.Panel = &Panel;
	E.Slot = CanvasSlot;
	E.Spec = Spec;
	ApplyDefaultPlacement(E);
	Apply(E);
	UE_LOG(LogRoadBuild, Log, TEXT("Window %s: hosted (%s)"), *Spec.Id.ToString(), *Panel.GetName());
	Panel.AttachToHost(*this, Spec.Id);   // applies whatever the panel already asked for
	return Window;
}

void UUiWindowHost::ApplyDefaultPlacement(FUiWindowEntry& E)
{
	UCanvasPanelSlot* CanvasSlot = E.Slot;
	if (CanvasSlot == nullptr)
	{
		return;
	}
	CanvasSlot->SetAutoSize(true);
	switch (E.Spec.Anchor)
	{
	case EUiWindowAnchor::TopRight:
		CanvasSlot->SetAnchors(FAnchors(1.0f, 0.0f));
		CanvasSlot->SetAlignment(FVector2D(1.0, 0.0));
		CanvasSlot->SetPosition(FVector2D(-E.Spec.Offset.X, E.Spec.Offset.Y));
		break;
	case EUiWindowAnchor::Centre:
		CanvasSlot->SetAnchors(FAnchors(0.5f, 0.5f));
		CanvasSlot->SetAlignment(FVector2D(0.5, 0.5));
		CanvasSlot->SetPosition(E.Spec.Offset);
		break;
	case EUiWindowAnchor::AboveBarLeft:
		CanvasSlot->SetAnchors(FAnchors(0.0f, 1.0f));
		CanvasSlot->SetAlignment(FVector2D(0.0, 1.0));
		CanvasSlot->SetPosition(FVector2D(E.Spec.Offset.X, -(BarHeight() + E.Spec.Offset.Y)));
		break;
	default:
		CanvasSlot->SetAnchors(FAnchors(0.0f, 0.0f));
		CanvasSlot->SetAlignment(FVector2D::ZeroVector);
		CanvasSlot->SetPosition(E.Spec.Offset);
		break;
	}
	E.bPlaced = false;
}

void UUiWindowHost::SetLayoutStore(TSharedPtr<IUiLayoutStore> InStore)
{
	LayoutStore = MoveTemp(InStore);
}

void UUiWindowHost::CommitPlacement(FName Id)
{
	FUiWindowEntry* E = Find(Id);
	if (E == nullptr || !LayoutStore.IsValid() || !E->bPlaced || E->Slot == nullptr)
	{
		return;
	}
	FUiWindowPlacement P;
	P.TopLeft = E->Slot->GetPosition();   // placed: top-left anchored, so position IS the top-left
	P.bSized = !E->Slot->GetAutoSize();
	P.Size = P.bSized ? E->Slot->GetSize() : FVector2D::ZeroVector;
	LayoutStore->Write(Id, P);
	UE_LOG(LogRoadBuild, Log, TEXT("Window %s: layout saved at (%.0f, %.0f)%s"), *Id.ToString(), P.TopLeft.X, P.TopLeft.Y,
		P.bSized ? *FString::Printf(TEXT(" size (%.0f, %.0f)"), P.Size.X, P.Size.Y) : TEXT(""));
}

void UUiWindowHost::RestoreSavedLayout()
{
	if (!LayoutStore.IsValid())
	{
		return;
	}
	for (FUiWindowEntry& E : Windows)
	{
		const TOptional<FUiWindowPlacement> Saved = LayoutStore->Read(E.Spec.Id);
		if (!Saved.IsSet() || E.Slot == nullptr)
		{
			continue;
		}
		// A SIZE ONLY FOR A WINDOW THAT CAN BE RESIZED, and never below the minimum: a saved size
		// for Offers (unresizable since step 2) would freeze a window meant to grow with its offers.
		const bool bUseSize = Saved->bSized && E.Spec.bResizable;
		const FVector2D Size = bUseSize
			? FVector2D(FMath::Max(Saved->Size.X, static_cast<double>(Style->WindowMinSize.X)),
				FMath::Max(Saved->Size.Y, static_cast<double>(Style->WindowMinSize.Y)))
			: SizeOf(E);
		// OFF SCREEN -> THE DEFAULT, not a clamp: a layout from a bigger monitor clamped into this
		// one's corner is a place the player never chose (Review Focus 1 of the step 3 plan). An
		// AUTO-SIZED window is judged at least at the minimum size: hidden, it measures 0x0, and a
		// top-left alone "fitted" an ultrawide's layout that was then clamped into the corner.
		// ENFORCED BY: AirportMgr.UI.WindowHost.AutoSizedRestoreIsJudgedAtMinimumSize.
		const FVector2D FitSize(FMath::Max(Size.X, static_cast<double>(Style->WindowMinSize.X)),
			FMath::Max(Size.Y, static_cast<double>(Style->WindowMinSize.Y)));
		const FBox2D B = Bounds();
		const bool bFits = Saved->TopLeft.X >= B.Min.X && Saved->TopLeft.Y >= B.Min.Y
			&& Saved->TopLeft.X + FitSize.X <= B.Max.X && Saved->TopLeft.Y + FitSize.Y <= B.Max.Y;
		if (!bFits)
		{
			UE_LOG(LogRoadBuild, Log, TEXT("Window %s: saved layout (%.0f, %.0f) is off this screen - default placement"),
				*E.Spec.Id.ToString(), Saved->TopLeft.X, Saved->TopLeft.Y);
			continue;
		}
		E.Slot->SetAnchors(FAnchors(0.0f, 0.0f));
		E.Slot->SetAlignment(FVector2D::ZeroVector);
		E.Slot->SetPosition(Saved->TopLeft);
		if (bUseSize)
		{
			E.Slot->SetAutoSize(false);
			E.Slot->SetSize(Size);
		}
		E.bPlaced = true;
		UE_LOG(LogRoadBuild, Log, TEXT("Window %s: layout restored from settings at (%.0f, %.0f)"),
			*E.Spec.Id.ToString(), Saved->TopLeft.X, Saved->TopLeft.Y);
	}
}

void UUiWindowHost::ResetLayout()
{
	if (LayoutStore.IsValid())
	{
		LayoutStore->Clear();
	}
	for (FUiWindowEntry& E : Windows)
	{
		ApplyDefaultPlacement(E);
	}
	UE_LOG(LogRoadBuild, Log, TEXT("Window host: layout reset to defaults"));
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
		if (E.Spec.bModal)
		{
			UpdateScrim(E);
		}
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
	// THE SAVED LAYOUT IS JUDGED ONCE THE VIEW AND THE BAR ARE REAL, not in AddWindow: "does it still
	// fit" needs the view's real size (NativeTick has only just read it) and the bar's real height.
	// The bar's first measure runs its wrap box at SWrapBox's 100-unit starting width, so frame 1's
	// height reads several sections tall, and judging then threw away every window saved just above
	// the bar (final review 2026-09-28). So: two equal readings in a row, or 30 ticks at most.
	// ENFORCED BY: AirportMgr.UI.WindowHost.RestoreWaitsForTheBarToSettle.
	if (!bLayoutRestored)
	{
		const double Height = BarHeight();
		++RestoreWaitTicks;
		const bool bSettled = DockBar == nullptr || (RestoreWaitTicks > 1 && Height == LastBarHeightSeen);
		if (bSettled || RestoreWaitTicks >= 30)
		{
			bLayoutRestored = true;
			UE_LOG(LogRoadBuild, Log, TEXT("Window host: judging the saved layout after %d tick(s) - view (%.0f, %.0f), bar %.0f%s"),
				RestoreWaitTicks, ViewSize.X, ViewSize.Y, Height, bSettled ? TEXT("") : TEXT(" (bar never settled)"));
			RestoreSavedLayout();
		}
		LastBarHeightSeen = Height;
	}
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
		if (E.Window != nullptr)
		{
			E.Window->SetMaxHeight(E.Slot->GetAutoSize() ? MaxAutoHeight(E) : 0.0);
		}
	}
}

double UUiWindowHost::MaxAutoHeight(const FUiWindowEntry& E) const
{
	// AN AUTO-SIZED WINDOW STOPS AT THE SCREEN'S EDGE and scrolls (its body is a UScrollBox), rather
	// than growing off it: Offers is unresizable and grows with its offers, and eight of them ran
	// past the bar (2026-09-29). Its top edge is anchor + position - alignment * H, so with the
	// anchor and position fixed each bound on H is linear: the top stays a margin below the view's
	// top (alignment > 0) and the bottom a margin above the bar (alignment < 1).
	// ENFORCED BY: AirportMgr.UI.WindowHost.AutoSizedWindowIsCappedAboveTheBar.
	const FBox2D B = Bounds();
	const double Margin = Style != nullptr ? Style->WindowMargin : 0.0;
	const double Edge = E.Slot->GetAnchors().Minimum.Y * ViewSize.Y + E.Slot->GetPosition().Y;
	const double Align = E.Slot->GetAlignment().Y;
	double Cap = TNumericLimits<double>::Max();
	if (Align > 0.0)
	{
		Cap = FMath::Min(Cap, (Edge - B.Min.Y - Margin) / Align);
	}
	if (Align < 1.0)
	{
		Cap = FMath::Min(Cap, (B.Max.Y - Margin - Edge) / (1.0 - Align));
	}
	// Never below the minimum: a window squeezed to nothing hides its content outright.
	const double Floor = Style != nullptr ? Style->WindowMinSize.Y : 0.0;
	return FMath::Max(Cap, Floor);
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

bool UUiWindowHost::IsModalOpen() const
{
	return Windows.ContainsByPredicate([](const FUiWindowEntry& E) { return E.Spec.bModal && E.bWanted && !E.bUserClosed; });
}

void UUiWindowHost::UpdateScrim(const FUiWindowEntry& Changed)
{
	if (Scrim == nullptr)
	{
		return;
	}
	const bool bUp = IsModalOpen();
	Scrim->SetVisibility(bUp ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	if (bUp && Changed.bWanted && !Changed.bUserClosed && Changed.Slot != nullptr)
	{
		// ABOVE EVERYTHING, THEN THE DIALOG ABOVE IT: the scrim covers every window opened before,
		// and the dialog stays the one thing on screen that takes a press.
		if (UCanvasPanelSlot* SheetSlot = Cast<UCanvasPanelSlot>(Scrim->Slot))
		{
			SheetSlot->SetZOrder(++TopZ);
		}
		Changed.Slot->SetZOrder(++TopZ);
	}
	UE_LOG(LogRoadBuild, Log, TEXT("Window host: modal %s %s"), *Changed.Spec.Id.ToString(), bUp ? TEXT("up") : TEXT("down"));
}
