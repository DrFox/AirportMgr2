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
	Centre,         // the middle of the screen - a dialog, where the eye already is
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
	/** A title-bar button (and a double-click on the title) folds the window to its title bar.
	 *  Off by default: a folded ledger or inspector is one more state for a panel nobody asked to fold. */
	UPROPERTY() bool bCollapsible = false;
	/** A dialog: while shown, the host lays a scrim under it that swallows every press elsewhere. */
	UPROPERTY() bool bModal = false;
	UPROPERTY() EUiWindowAnchor Anchor = EUiWindowAnchor::TopLeft;
	/** From the anchored corner, inward; for AboveBarLeft, Y is the gap above the bar's top. */
	UPROPERTY() FVector2D Offset = FVector2D(12.0, 12.0);
};
