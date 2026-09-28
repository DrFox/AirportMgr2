#include "UI/UiRow.h"

#include "Brushes/SlateRoundedBoxBrush.h"
#include "UIStyle.h"

void UUiRow::Build(const UUIStyle& Style, const FMargin& InPadding)
{
	SetBrush(FSlateRoundedBoxBrush(Style.Well, Style.ControlRadius));
	SetPadding(InPadding);
}
