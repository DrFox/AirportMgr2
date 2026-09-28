#include "UI/UiScrim.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "UIStyle.h"

void UUiScrim::Build(const UUIStyle& Style)
{
	UBorder* Sheet = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("ScrimSheet"));
	Sheet->SetBrushColor(Style.Scrim);
	WidgetTree->RootWidget = Sheet;
	// VISIBLE, not SelfHitTestInvisible like every other root in the UI: catching what misses the
	// dialog is this widget's whole job.
	SetVisibility(ESlateVisibility::Collapsed);
}

FReply UUiScrim::NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	return FReply::Handled();
}

FReply UUiScrim::NativeOnMouseButtonDoubleClick(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	return FReply::Handled();
}

FReply UUiScrim::NativeOnMouseWheel(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	return FReply::Handled();
}
