#pragma once

#include "CoreMinimal.h"
#include "AirportMgrPanelWidget.h"
#include "SettingsPanelWidget.generated.h"

/**
 * The player's Settings (UI library step 4b, spec section 3): a MODAL window, opened by the
 * game.settings action (the gear on the bar, and Escape).
 */
UCLASS()
class AIRPORTMGR_API USettingsPanelWidget : public UAirportMgrPanelWidget
{
	GENERATED_BODY()

public:
	/** Open if closed, cancel if open - what the game.settings action does. */
	void Toggle();
	bool IsShowing() const { return bShowing; }

	/** Centred, modal, closable (the close is Cancel), not resizable - a form, not a list. */
	virtual bool WantsWindow(FUiWindowSpec& Out) const override;
	virtual void OnWindowClosedByPlayer() override;

protected:
	virtual void BuildOnce(const UUIStyle& Style) override;

private:
	bool bShowing = false;
};
