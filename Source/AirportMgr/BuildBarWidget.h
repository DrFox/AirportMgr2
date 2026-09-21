#pragma once

#include "CoreMinimal.h"
#include "AirportMgrPanelWidget.h"
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
class AIRPORTMGR_API UBuildBarWidget : public UAirportMgrPanelWidget
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
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UPanelWidget> SnapSection;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UPanelWidget> SnapToSection;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> ClockText;

	/**
	 * The money, on the right of the status strip.
	 *
	 * REPLACES THE RESERVED SPACER that stood here holding the space open - see the slot's own
	 * comment, which said a readout would wait until something consumed
	 * UScenario::StartingBalance. The ledger does now.
	 */
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> BalanceText;

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

	/** Runs NativeTick with a throwaway geometry - the same precedent as
	 *  ARoadBuildController::PlayerTickForTest - so a headless test can prove the per-tick
	 *  refresh is cheap without a viewport ticking it for real. */
	void NativeTickForTest(float DeltaTime) { FGeometry G; NativeTick(G, DeltaTime); }

	/** How many times RefreshClock/RefreshBalance actually called SetText, as opposed to how
	 *  many times they were asked - issue #187's gate measured directly, the same seam
	 *  UInspectorWidget's own SetTextCallCountForTest uses. */
	int32 SetTextCallCountForTest() const { return SetTextCalls; }

	/**
	 * The size the section row needs when it is only allowed to be AvailableWidth wide.
	 *
	 * A TEST SEAM, because the thing that goes wrong here cannot be seen any other way: the
	 * buttons are all present in the widget tree whether or not they FIT, so counting them
	 * says nothing - see AirportMgr.Actions.BarBuildsFromRegistry passing throughout the
	 * stage-3 regression where the whole Snap section was off-screen.
	 *
	 * In the game the row takes its wrap width from the geometry it is given, which arrives
	 * on Tick; there is no geometry and no tick headless, so the width is stated here. The
	 * arithmetic under test - where a line breaks - is the same either way.
	 */
	FVector2D SectionRowSizeForTest(float AvailableWidth) const;

	/**
	 * The height the bar actually gets on the canvas, at this width.
	 *
	 * SEPARATE FROM SectionRowSizeForTest because wrapping the row is only half the job: a
	 * row that wraps inside a bar pinned to one line's height has swapped clipping at the
	 * right-hand edge for clipping at the bottom, and the row's own measurement cannot see
	 * that. This reads what the CANVAS will give it.
	 */
	float BarReservedHeightForTest(float AvailableWidth) const;

protected:
	/** Builds the bar's chrome and buttons. See UAirportMgrPanelWidget::Initialize for why
	 *  this runs from Initialize rather than NativeOnInitialized. */
	virtual void BuildOnce(const UUIStyle& Style) override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

private:
	UPROPERTY() TArray<TObjectPtr<UBuildBarEntry>> Entries;

	/**
	 * The row the section frames sit on.
	 *
	 * A UWrapBox, so a section that will not fit spills onto another line rather than off the
	 * right-hand edge. HELD rather than local to EnsureSlots, which is where it used to live:
	 * a row nothing can reach is a row nothing can measure, and whether the sections fit is
	 * the one thing about this bar that has actually broken.
	 */
	UPROPERTY() TObjectPtr<UPanelWidget> SectionRow;

	UPanelWidget* SectionPanel(EActionSection Section) const;
	void EnsureSlots(const UUIStyle* Style);
	void BuildButtons(const UUIStyle* Style);
	void RefreshState();
	void RefreshClock();

	/** The balance, and the landing-fee multiplier beside it. Called from the same tick. */
	void RefreshBalance();

	/**
	 * The clock's last composed sentence, so a still tick - 59 of every 60 real seconds at
	 * x1, every tick while paused - sets no text at all (issue #187: SetText has no early-out
	 * of its own). Compared as the COMPOSED string rather than a bare minute key: the
	 * sentence also carries speed and the PAUSED suffix, and a minute-only key would leave a
	 * just-paused game reading its old speed for up to a minute.
	 */
	FString LastClockText;

	/**
	 * What the balance line was last built from. Ledger::Revision() is the ledger's own
	 * cheapest question (see ULedgerPanelWidget::Refresh for the identical idiom); the fee
	 * multiplier joins it because StepLandingFee changes this line's text without posting to
	 * the ledger, and Revision alone would leave the fee stale until the next post.
	 */
	int32 LastLedgerRevision = INDEX_NONE;
	double LastFeeMultiplier = -1.0;
	/** Whether the last balance paint was the "no ledger" fallback, so a real ledger appearing
	 *  is never mistaken for "nothing changed" by the two keys above. */
	bool bLastBalanceWasFallback = true;

	/** See SetTextCallCountForTest. */
	int32 SetTextCalls = 0;
};
