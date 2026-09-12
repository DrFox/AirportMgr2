#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "BuildActions.h"
#include "BuildBarWidget.generated.h"

class UButton;
class UImage;
class UPanelWidget;
class UTextBlock;
class UBuildBarWidget;
class ARoadBuildController;
class UUIStyle;

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
	/** Null for the time controls, which are glyphs and carry no texture. */
	UPROPERTY() TObjectPtr<UImage> Icon;
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
 * EVERY COLOUR, FONT AND METRIC COMES FROM UUIStyle, resolved through
 * UAirportMgrUISettings::ResolveStyle, which is never null. The per-widget NormalTint /
 * ActiveTint / DisabledTint / BarTint knobs this class used to carry are gone on purpose:
 * they were named for WHERE they appeared, so a re-skin meant hunting them down in every
 * widget. Six semantic slots in one asset is the whole of the answer to "is it easy to
 * change later" - see the game UI spec, section 2.1.
 *
 * TO RESTYLE IN THE DESIGNER: make a Widget Blueprint with this class as parent, give it
 * any layout you like, and name the panels you want filled TimeSection, ToolsSection,
 * EditSection, AircraftSection, SelectionSection, GameSection (any UPanelWidget; a HorizontalBox gets padded
 * slots) plus a TextBlock named ClockText. Set it as BuildBarClass on the
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

	/**
	 * The upper row: clock, the time controls, and the reserved ledger slot.
	 *
	 * A SECOND ROW IS THE WHOLE POINT. The six sections in EActionSection already existed
	 * and drew as one undifferentiated run of buttons, which is what made the bar read as a
	 * debug menu; splitting status from tools is what creates the hierarchy it lacked.
	 */
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UPanelWidget> StatusRow;

	/**
	 * A FLOOR, not the height.
	 *
	 * The bar sizes itself from UUIStyle - two rows, a heading and a ButtonSize hit target -
	 * so raising ButtonSize in the asset does not crop the buttons off the bottom of the
	 * screen. Colours and metrics being free to change is the reason the style asset exists;
	 * a hard-coded height here would have quietly taken half of that back.
	 */
	UPROPERTY(EditAnywhere, Category = "Bar|Style", meta = (ClampMin = "24.0")) double BarHeight = 56.0;

	/** Runs an action by registry index on the owning controller. Called by entries. */
	void RunAction(int32 ActionIndex);

	/**
	 * How tall the two rows need to be for this style, before BarHeight's floor is applied.
	 *
	 * PUBLIC AND STATIC because the bar is not the only thing that needs the answer: the
	 * toast stack floats above it and would sit behind it the moment ButtonSize changed, if
	 * it carried its own copy of this arithmetic. One formula, two consumers, no drift.
	 */
	static float BarHeightFor(const UUIStyle& Style);

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

};
