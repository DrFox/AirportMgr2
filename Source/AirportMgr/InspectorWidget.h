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

	// PanelTint/ButtonTint/DisabledTint/FontSize are GONE (issue #91) - EVERY COLOUR AND FONT
	// COMES FROM UUIStyle now, the rule UBuildBarWidget's own header already states. Only
	// metrics this panel alone needs (its width, how far it floats) stay as knobs.
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

	/**
	 * INDICES INTO BuildActions(), never ids to look up - the same reason UBuildBarEntry
	 * holds one. Found once, in EnsureSlots, by walking the Selection section positionally:
	 * BuildActions.cpp adds selection.depart then selection.follow so the panel, the bar and
	 * the C key stay one list (its own comment, spec §6.2) - the pair this panel needs is
	 * exactly those two rows, in that order.
	 */
	int32 DepartActionIndex = INDEX_NONE;
	int32 FollowActionIndex = INDEX_NONE;

	void EnsureSlots();
	void RunAction(int32 ActionIndex);

	UFUNCTION() void HandleDepart();
	UFUNCTION() void HandleFollow();
};
