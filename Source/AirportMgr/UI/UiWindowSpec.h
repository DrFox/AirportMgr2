#pragma once

#include "CoreMinimal.h"
#include "UiWindowSpec.generated.h"

/** Where a window sits until the player moves it. */
UENUM()
enum class EUiWindowAnchor : uint8
{
	TopLeft,
	TopRight,
	AboveBarLeft,   // bottom-left, riding the bar's top edge as the bar grows (the inspector)
};

/**
 * How a panel wants its window (UAirportMgrPanelWidget::WantsWindow). Defaults are the panel's
 * - its placement lived in its own TopOffset/BarGap properties before windows, and still does.
 */
USTRUCT()
struct FUiWindowSpec
{
	GENERATED_BODY()

	UPROPERTY() FName Id;
	UPROPERTY() FText Title;
	UPROPERTY() bool bClosable = true;
	UPROPERTY() bool bResizable = true;
	UPROPERTY() EUiWindowAnchor Anchor = EUiWindowAnchor::TopLeft;
	/** From the anchored corner, inward; for AboveBarLeft, Y is the gap above the bar's top. */
	UPROPERTY() FVector2D Offset = FVector2D(12.0, 12.0);
};
