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
	/**
	 * A window the PLAYER OPENS AND CLOSES with a key or a bar button - the ledger, the alerts, Land, settings: its close button is the same
	 * toggle, so the host hides it plainly (UUiWindowHost::Toggle flips it, CloseByPlayer clears it) and the next press of the key opens it.
	 * An UNtoggled window's close STICKS (FUiWindowEntry::bUserClosed) while its panel keeps asking to show - the inspector asks every tick
	 * while anything is selected, and a close that did not stick would reopen it at once. The host owns this; it was a bShowing on each of
	 * four panels, resynced by an OnWindowClosedByPlayer override that carried the same comment ("THE CLOSE BUTTON IS THE TOGGLE -
	 * bShowing must agree") four times, and a fifth toggled window would have copied it again (#447).
	 * ENFORCED BY: AirportMgr.UI.WindowHost.ToggleIsTheHostsAndCloseIsToggle, Check-Architecture rule 4's 'panel shown-state is the host's' row
	 */
	UPROPERTY() bool bToggled = false;
	UPROPERTY() EUiWindowAnchor Anchor = EUiWindowAnchor::TopLeft;
	/** From the anchored corner, inward; for AboveBarLeft, Y is the gap above the bar's top. */
	UPROPERTY() FVector2D Offset = FVector2D(12.0, 12.0);
};
