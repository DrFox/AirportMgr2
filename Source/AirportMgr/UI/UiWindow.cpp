#include "UI/UiWindow.h"

#include "Blueprint/WidgetTree.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Components/Border.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "RoadBuildLog.h"
#include "UI/UiButton.h"
#include "UI/UiClicks.h"
#include "UI/UiWindowHost.h"
#include "UIStyle.h"

void UUiWindow::Build(const UUIStyle& Style, const FUiWindowSpec& Spec, UWidget& Content, UUiWindowHost& InHost)
{
	Host = &InHost;
	Id = Spec.Id;

	// THE OUTER FRAME IS A SHADOW - a translucent rounded box one pixel wider on three sides and
	// three below, so a white card lifts off grass and concrete alike. Slate has no drop shadow;
	// padding on a darker box is the cheapest thing that reads as one (the spike's recipe).
	UBorder* Frame = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("WindowFrame"));
	Frame->SetBrush(FSlateRoundedBoxBrush(Style.Shadow, Style.WindowRadius + 1.0f));
	Frame->SetPadding(FMargin(1.0f, 1.0f, 1.0f, 3.0f));
	// NOT hit-testable itself: UiClicks treats a root that hits ITSELF as one that may cover the
	// screen and gives the click back. The card inside is what takes - and eats - a press.
	Frame->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	WidgetTree->RootWidget = Frame;

	UBorder* Card = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("WindowCard"));
	Card->SetBrush(FSlateRoundedBoxBrush(Style.Surface, Style.WindowRadius));
	Card->SetPadding(FMargin(0.0f));
	Card->SetClipping(EWidgetClipping::ClipToBounds);
	Frame->SetContent(Card);

	UOverlay* Layers = WidgetTree->ConstructWidget<UOverlay>(UOverlay::StaticClass());
	Card->SetContent(Layers);
	UVerticalBox* Chrome = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	UOverlaySlot* ChromeSlot = Layers->AddChildToOverlay(Chrome);
	ChromeSlot->SetHorizontalAlignment(HAlign_Fill);
	ChromeSlot->SetVerticalAlignment(VAlign_Fill);

	// VISIBLE with a transparent brush: a Border hit-tests but handles nothing, so a press on the
	// title bubbles to NativeOnMouseButtonDown, which starts the drag. The close button handles
	// its own press first and never gets there.
	UBorder* Bar = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("WindowTitleBar"));
	Bar->SetBrushColor(FLinearColor::Transparent);
	Bar->SetPadding(FMargin(Style.CardPadding.Left, 6.0f, 6.0f, 6.0f));
	Bar->SetVisibility(ESlateVisibility::Visible);
	Chrome->AddChildToVerticalBox(Bar);
	TitleBar = Bar;

	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Bar->SetContent(Row);
	UTextBlock* Title = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("WindowTitle"));
	Title->SetText(Spec.Title);
	Style.ApplyText(*Title, EUITextRole::Title, Style.Ink);
	UHorizontalBoxSlot* TitleSlot = Row->AddChildToHorizontalBox(Title);
	TitleSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	TitleSlot->SetVerticalAlignment(VAlign_Center);

	if (Spec.bClosable)
	{
		// A Ghost UUiButton: no fill until hovered, so the close reads as an affordance rather
		// than a second button competing with the panel's own verbs.
		CloseButton = WidgetTree->ConstructWidget<UUiButton>(UUiButton::StaticClass(), TEXT("WindowClose"));
		CloseButton->SetLabel(FText::FromString(FString(TEXT("×"))));
		CloseButton->Build(Style, EUiButtonKind::Ghost, EUiButtonLayout::Inline, false);
		CloseButton->OnClicked.AddDynamic(this, &UUiWindow::HandleClose);
		UHorizontalBoxSlot* CloseSlot = Row->AddChildToHorizontalBox(CloseButton);
		CloseSlot->SetVerticalAlignment(VAlign_Center);
		CloseSlot->SetPadding(FMargin(12.0f, 0.0f, 0.0f, 0.0f));
	}

	// The hairline under the title. A SizeBox, not UImage::SetDesiredSizeOverride, which drew a
	// 15 px band in the spike: the height has to be imposed from outside the brush.
	USizeBox* Rule = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("WindowRule"));
	Rule->SetHeightOverride(1.0f);
	UBorder* RuleFill = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	RuleFill->SetBrushColor(Style.Rule);
	RuleFill->SetPadding(FMargin(0.0f));
	Rule->SetContent(RuleFill);
	Chrome->AddChildToVerticalBox(Rule)->SetHorizontalAlignment(HAlign_Fill);

	// A SCROLL BOX, so a window resized smaller than its panel scrolls instead of cropping.
	// Auto-sized, it is exactly the panel's size. Its bar is VISIBLE, which SScrollBox draws only
	// when the content overflows: Collapsed hid content below the fold with no sign it was there
	// (final review 2026-09-28). ENFORCED BY: AirportMgr.UI.WindowHost.OverflowIsSignalledAndOffersNeverShrink.
	UScrollBox* Scroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass(), TEXT("WindowScroll"));
	Scroll->SetScrollBarVisibility(ESlateVisibility::Visible);
	Chrome->AddChildToVerticalBox(Scroll)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	UBorder* Pad = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	Pad->SetBrushColor(FLinearColor::Transparent);
	// UUIStyle::CardPadding, not a literal: see its own comment (issue #192).
	Pad->SetPadding(Style.CardPadding);
	Pad->SetContent(&Content);
	Scroll->AddChild(Pad);

	if (Spec.bResizable)
	{
		// The grip: a small muted square in the corner, over the content. Visible so it
		// hit-tests; NativeOnMouseButtonDown checks it before the title bar.
		UImage* GripImage = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("WindowGrip"));
		GripImage->SetBrush(FSlateRoundedBoxBrush(Style.InkMuted * FLinearColor(1.0f, 1.0f, 1.0f, 0.45f), 2.0f));
		GripImage->SetDesiredSizeOverride(FVector2D(9.0, 9.0));
		GripImage->SetVisibility(ESlateVisibility::Visible);
		UOverlaySlot* GripSlot = Layers->AddChildToOverlay(GripImage);
		GripSlot->SetHorizontalAlignment(HAlign_Right);
		GripSlot->SetVerticalAlignment(VAlign_Bottom);
		GripSlot->SetPadding(FMargin(0.0f, 0.0f, 4.0f, 4.0f));
		Grip = GripImage;
	}
	SetVisibility(ESlateVisibility::SelfHitTestInvisible);
}

void UUiWindow::HandleClose()
{
	if (Host != nullptr)
	{
		Host->CloseByPlayer(Id);
	}
}

FReply UUiWindow::NativeOnPreviewMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	if (Host != nullptr)
	{
		Host->BringToFront(Id);
	}
	return Super::NativeOnPreviewMouseButtonDown(InGeometry, InMouseEvent);
}

FReply UUiWindow::NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	const FReply Reply = Super::NativeOnMouseButtonDown(InGeometry, InMouseEvent);
	if (!Reply.IsEventHandled() && Host != nullptr && InMouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
	{
		const FVector2D At = InMouseEvent.GetScreenSpacePosition();
		// The grip first: it sits inside the card, over the content.
		EUiWindowGesture Wanted = EUiWindowGesture::None;
		if (Grip != nullptr && Grip->GetCachedGeometry().IsUnderLocation(At))
		{
			Wanted = EUiWindowGesture::Resize;
		}
		else if (TitleBar != nullptr && TitleBar->GetCachedGeometry().IsUnderLocation(At))
		{
			Wanted = EUiWindowGesture::Move;
		}
		if (Wanted != EUiWindowGesture::None)
		{
			BeginGesture(Wanted, Host->ToLocal(At));
			return FReply::Handled().CaptureMouse(TakeWidget());
		}
	}
	return UiClicks::EatUnhandled(*this, WidgetTree != nullptr ? WidgetTree->RootWidget.Get() : nullptr, Reply, TEXT("down"));
}

FReply UUiWindow::NativeOnMouseButtonDoubleClick(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	return UiClicks::EatUnhandled(*this, WidgetTree != nullptr ? WidgetTree->RootWidget.Get() : nullptr,
		Super::NativeOnMouseButtonDoubleClick(InGeometry, InMouseEvent), TEXT("double-click"));
}

FReply UUiWindow::NativeOnMouseMove(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	if (Gesture == EUiWindowGesture::None || Host == nullptr)
	{
		return Super::NativeOnMouseMove(InGeometry, InMouseEvent);
	}
	UpdateGesture(Host->ToLocal(InMouseEvent.GetScreenSpacePosition()));
	return FReply::Handled();
}

FReply UUiWindow::NativeOnMouseButtonUp(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	if (Gesture == EUiWindowGesture::None || InMouseEvent.GetEffectingButton() != EKeys::LeftMouseButton)
	{
		return Super::NativeOnMouseButtonUp(InGeometry, InMouseEvent);
	}
	EndGesture(TEXT("released"));
	return FReply::Handled().ReleaseMouseCapture();
}

void UUiWindow::NativeOnMouseCaptureLost(const FCaptureLostEvent& CaptureLostEvent)
{
	Super::NativeOnMouseCaptureLost(CaptureLostEvent);
	if (Gesture != EUiWindowGesture::None)
	{
		EndGesture(TEXT("capture lost"));   // CaptureLostEndsTheGesture
	}
}

void UUiWindow::BeginGesture(EUiWindowGesture Kind, FVector2D HostLocal)
{
	if (Host == nullptr)
	{
		return;
	}
	const FBox2D Rect = Host->WindowRect(Id);
	Gesture = Kind;
	GestureStartMouse = HostLocal;
	GestureStartTopLeft = Rect.Min;
	GestureStartSize = Rect.GetSize();
	UE_LOG(LogRoadBuild, Log, TEXT("Window %s: %s begins at (%.0f, %.0f) size (%.0f, %.0f)"), *Id.ToString(),
		Kind == EUiWindowGesture::Move ? TEXT("move") : TEXT("resize"),
		Rect.Min.X, Rect.Min.Y, GestureStartSize.X, GestureStartSize.Y);
}

void UUiWindow::UpdateGesture(FVector2D HostLocal)
{
	if (Host == nullptr)
	{
		return;
	}
	const FVector2D Delta = HostLocal - GestureStartMouse;
	if (Gesture == EUiWindowGesture::Move)
	{
		Host->MoveWindow(Id, GestureStartTopLeft + Delta);
	}
	else if (Gesture == EUiWindowGesture::Resize)
	{
		Host->ResizeWindow(Id, GestureStartSize + Delta);
	}
}

void UUiWindow::EndGesture(const TCHAR* Why)
{
	Gesture = EUiWindowGesture::None;
	if (Host != nullptr)
	{
		const FBox2D Rect = Host->WindowRect(Id);
		UE_LOG(LogRoadBuild, Log, TEXT("Window %s: gesture ends (%s) at (%.0f, %.0f) size (%.0f, %.0f)"), *Id.ToString(),
			Why, Rect.Min.X, Rect.Min.Y, Rect.GetSize().X, Rect.GetSize().Y);
	}
}
