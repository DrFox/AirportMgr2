#pragma once

#include "CoreMinimal.h"
#include "AirportMgrPanelWidget.h"
#include "Blueprint/UserWidget.h"
#include "Tool/Selection.h"
#include "InspectorWidget.generated.h"

class ARoadBuildController;
class ARoadNetworkActor;
class UButton;
class UTextBlock;
class UUIStyle;

/**
 * The inspector: what the selected aircraft or stand is doing, and the verbs for it.
 *
 * The same recipe as UBuildBarWidget: a C++ base that builds a working panel with no asset,
 * and BindWidgetOptional slots a Widget Blueprint fills to restyle it. Set the Blueprint
 * as InspectorClass on the controller.
 *
 * POLLED each tick from InspectFacts, never subscribed: the bar's enabled states were
 * event-driven once and went stale, and a panel that shows speed needs every frame anyway.
 * It reads FAgentFacts / FStandFacts and never FRoadAgent, so M3's UFlight fills the same
 * struct and this file does not change.
 *
 * Its buttons run rows of BuildActions() by id, so the panel, the bar and the C key are one
 * list (spec §6.2).
 */
UCLASS()
class AIRPORTMGR_API UInspectorWidget : public UAirportMgrPanelWidget
{
	GENERATED_BODY()

public:
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> TitleText;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> FactsText;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> StatusText;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UButton> DepartButton;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UButton> FollowButton;

	UPROPERTY(EditAnywhere, Category = "Inspector|Style") FLinearColor PanelTint = FLinearColor(0.06f, 0.07f, 0.09f, 0.92f);
	UPROPERTY(EditAnywhere, Category = "Inspector|Style") FLinearColor ButtonTint = FLinearColor(0.18f, 0.20f, 0.24f);
	UPROPERTY(EditAnywhere, Category = "Inspector|Style") FLinearColor DisabledTint = FLinearColor(0.10f, 0.10f, 0.12f);
	UPROPERTY(EditAnywhere, Category = "Inspector|Style") int32 FontSize = 12;
	UPROPERTY(EditAnywhere, Category = "Inspector|Style") double PanelWidth = 300.0;
	/** Distance above the bottom edge, so it clears the build bar. */
	UPROPERTY(EditAnywhere, Category = "Inspector|Style") double BottomOffset = 72.0;

	/**
	 * Re-reads the facts for Selection over Target and repaints. What NativeTick calls with
	 * the controller's target and selection; public so a headless test can drive it with
	 * no controller.
	 */
	void Refresh(const ARoadNetworkActor* Target, const FSelection& Selection);

	bool IsShownForTest() const;
	bool IsDepartEnabledForTest() const;
	FString TitleForTest() const;

protected:
	/** Builds the panel's chrome and binds its two verbs. See
	 *  UAirportMgrPanelWidget::Initialize for why this runs from Initialize. */
	virtual void BuildOnce(const UUIStyle& Style) override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

private:
	bool bDepartEnabled = false;

	void EnsureSlots();
	void RunActionById(FName Id);

	UFUNCTION() void HandleDepart();
	UFUNCTION() void HandleFollow();
};
