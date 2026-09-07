#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "BuildActions.h"
#include "BuildBarWidget.generated.h"

class UButton;
class UPanelWidget;
class UTextBlock;
class UBuildBarWidget;
class ARoadBuildController;

/**
 * One button on the bar and the action it runs. A UObject because UButton::OnClicked is a
 * dynamic delegate and binds only to a UFUNCTION on a UObject; a lambda cannot bind.
 * Holds the action's INDEX into BuildActions(), never a name to look up.
 */
UCLASS()
class UBuildBarEntry : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY() int32 ActionIndex = INDEX_NONE;
	UPROPERTY() TObjectPtr<UButton> Button;
	UPROPERTY() TObjectPtr<UTextBlock> Label;
	UPROPERTY() TWeakObjectPtr<UBuildBarWidget> Owner;

	UFUNCTION() void HandleClicked();
};

/**
 * The bottom bar. Sections are Blueprint-authored panels bound by name; their CONTENTS are
 * generated here from BuildActions(), so the asset never lists an action and a new action
 * appears the moment it is registered. Any slot the asset lacks is built in code, so the
 * bar works with no asset at all (AirportMgr.Actions.BarBuildsFromRegistry proves it).
 *
 * State is POLLED each tick rather than subscribed: the fifteen booleans come from four
 * owners (session, controller, actor, runtime), and fifteen reads a frame cost nothing next
 * to four subscriptions and their lifetime rules.
 *
 * TO RESTYLE IN THE DESIGNER: make a Widget Blueprint with this class as parent, give it
 * any layout you like, and name the panels you want filled TimeSection, ToolsSection,
 * EditSection, AircraftSection, SelectionSection, GameSection (any UPanelWidget; a HorizontalBox gets padded
 * slots) plus TextBlocks ClockText and NotificationText. Set it as BuildBarClass on the
 * controller (DefaultGame.ini, [/Script/AirportMgr.RoadBuildController]). Sections you
 * leave out are built in code and a warning names them. Authoring that asset from Python
 * was tried and is not possible on this engine build: UWidgetBlueprint::WidgetTree is not
 * a scriptable property, so the code-built bar is the default look.
 */
UCLASS()
class AIRPORTMGR_API UBuildBarWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	// Slots the Blueprint may supply. Optional: a missing one is created in code.
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UPanelWidget> TimeSection;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UPanelWidget> ToolsSection;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UPanelWidget> EditSection;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UPanelWidget> AircraftSection;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UPanelWidget> SelectionSection;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UPanelWidget> GameSection;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> ClockText;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> NotificationText;

	// Style knobs a Blueprint subclass overrides without a build.
	UPROPERTY(EditAnywhere, Category = "Bar|Style") FLinearColor NormalTint = FLinearColor(0.18f, 0.20f, 0.24f);
	UPROPERTY(EditAnywhere, Category = "Bar|Style") FLinearColor ActiveTint = FLinearColor(0.95f, 0.75f, 0.20f);
	UPROPERTY(EditAnywhere, Category = "Bar|Style") FLinearColor DisabledTint = FLinearColor(0.10f, 0.10f, 0.12f);
	UPROPERTY(EditAnywhere, Category = "Bar|Style") FMargin ButtonPadding = FMargin(10.0f, 6.0f);
	UPROPERTY(EditAnywhere, Category = "Bar|Style") int32 FontSize = 12;
	UPROPERTY(EditAnywhere, Category = "Bar|Style") FLinearColor BarTint = FLinearColor(0.06f, 0.07f, 0.09f, 0.92f);
	UPROPERTY(EditAnywhere, Category = "Bar|Style", meta = (ClampMin = "24.0")) double BarHeight = 56.0;

	/** Runs an action by registry index on the owning controller. Called by entries. */
	void RunAction(int32 ActionIndex);

	int32 ButtonCountForTest(EActionSection Section) const;
	bool HasRootWidgetForTest() const;

	/**
	 * Builds the bar right after the base class has bound the asset's slots.
	 *
	 * Initialize rather than NativeOnInitialized: UUserWidget::Initialize only calls the
	 * latter when a PLAYER CONTEXT is valid (or a Blueprint class opts in), so a bar created
	 * from a world with no player - which is what a headless test does - would never build
	 * and would silently pass through empty. Overriding the one call every creation path
	 * makes keeps the build unconditional.
	 */
	virtual bool Initialize() override;

protected:
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

private:
	UPROPERTY() TArray<TObjectPtr<UBuildBarEntry>> Entries;
	bool bBuilt = false;

	ARoadBuildController* Controller() const;
	UPanelWidget* SectionPanel(EActionSection Section) const;
	void EnsureSlots();
	void BuildButtons();
	void RefreshState();
	void RefreshClock();

	UFUNCTION() void OnNotification(const FString& Text);
};
