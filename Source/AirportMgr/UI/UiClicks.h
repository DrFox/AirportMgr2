#pragma once

#include "CoreMinimal.h"
#include "Input/Reply.h"

class UUserWidget;
class UWidget;

namespace UiClicks
{
	/**
	 * Handled when the press can only have come from Widget's own pixels (a Border is hit-testable
	 * but handles nothing, so the press would bubble on to the game viewport as a click on the
	 * ground). A Widget or Root that hits ITSELF may cover the screen, so Slate's answer stands for
	 * those - better a click through than every click in the game swallowed. Logs at the boundary.
	 * Shared by UAirportMgrPanelWidget (the bar) and UUiWindow. ENFORCED BY: AirportMgr.Panels.ChromeEatsClicks,
	 * AirportMgr.UI.Window.ChromeEatsClicks.
	 */
	AIRPORTMGR_API FReply EatUnhandled(const UUserWidget& Widget, const UWidget* Root, FReply Reply, const TCHAR* What);
}
