#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "UiScrim.generated.h"

class UUIStyle;

/**
 * The dim sheet under a modal window (UI library step 4b, spec section 2 Modal). Full-screen and
 * Visible, so every press that misses the dialog lands HERE rather than on another window or the
 * world - and stops: press, double-click and wheel are all handled. The wheel too, unlike a
 * window's chrome (UAirportMgrPanelWidget's rule): zooming the airport behind an open dialog reads
 * as the dialog leaking.
 *
 * Keys are not a Slate concern here - they reach the game viewport whatever is on screen, so the
 * controller asks UUiWindowHost::IsModalOpen before acting on one.
 */
UCLASS()
class AIRPORTMGR_API UUiScrim : public UUserWidget
{
	GENERATED_BODY()

public:
	void Build(const UUIStyle& Style);

protected:
	virtual FReply NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnMouseButtonDoubleClick(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnMouseWheel(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
};
