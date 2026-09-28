#pragma once

#include "CoreMinimal.h"
#include "Components/Button.h"
#include "UiButton.generated.h"

class UImage;
class UTextBlock;
class UTexture2D;
class UUIStyle;

/** What a button is FOR - picks its resting fill. See UUiButton::LookFor for the whole rule. */
UENUM()
enum class EUiButtonKind : uint8
{
	Primary,    // the affirmative verb: Accept, Save. Accent.
	Secondary,  // everything else, and every bar tool. Control; Accent while Selected.
	Danger,     // a destructive verb. Warning.
	Ghost,      // no fill until hovered: a window's close, an icon-only affordance.
};

UENUM()
enum class EUiButtonLayout : uint8
{
	Inline,   // [icon] label  detail - rows, verbs
	Stacked,  // icon over label over detail - the bar's tool buttons
};

struct FUiButtonLook
{
	FLinearColor Fill;
	FLinearColor Ink;
};

/**
 * The game's one button (UI library step 1, spec 2026-09-28).
 *
 * A UButton SUBCLASS, not a UUserWidget wrapping one: it is placed wherever a UButton was, keeps
 * UButton's OnClicked and BindWidgetOptional compatibility, and costs no extra widget tree. What
 * it adds is the ONE place the enabled / selected / kind -> colour rule lives - four widgets each
 * carried a copy (the bar, the variant bar, the inspector, the inbox), and one copy drifted.
 *
 * Inner widgets are NewObject'd with this button as outer rather than through the owning user
 * widget's WidgetTree: a UWidget has no tree of its own, and SetContent's slot is what holds them.
 */
UCLASS()
class AIRPORTMGR_API UUiButton : public UButton
{
	GENERATED_BODY()

public:
	/**
	 * Styles the button and builds its content from whatever SetLabel/SetDetail/SetIcon recorded.
	 * Call ONCE, after those. bStylePadding false keeps UButton's own padding (the bar's square
	 * tool buttons are sized by their content, not by ButtonPadding).
	 */
	void Build(const UUIStyle& Style, EUiButtonKind InKind, EUiButtonLayout InLayout = EUiButtonLayout::Inline,
		bool bStylePadding = true);

	/** Before Build: records the caption. After: SetText on the existing block, nothing rebuilt. */
	void SetLabel(const FText& Text);
	/** A second, muted piece of text - beside the label (Inline) or under it (Stacked). */
	void SetDetail(const FText& Text);
	void SetIcon(UTexture2D* InIcon, float Size);
	void SetLabelMinWidth(float Width);

	/** Enabled + selected -> fill, ink, IsEnabled. A no-op when neither changed. */
	void SetState(bool bInEnabled, bool bInSelected);

	UTextBlock* GetLabel() const { return Label; }
	UImage* GetIcon() const { return Icon; }
	EUiButtonKind GetKind() const { return Kind; }

	/** The whole colour rule. Pure, so the table test reads it without a widget. */
	static FUiButtonLook LookFor(const UUIStyle& Style, EUiButtonKind Kind, bool bEnabled, bool bSelected);

	/** How many times SetState actually painted - see UnchangedStatePaintsNothing. */
	int32 PaintCountForTest() const { return PaintCount; }

private:
	void BuildContent();
	void Paint();

	UPROPERTY() TObjectPtr<const UUIStyle> Style;
	UPROPERTY() TObjectPtr<UTextBlock> Label;
	UPROPERTY() TObjectPtr<UTextBlock> Detail;
	UPROPERTY() TObjectPtr<UImage> Icon;
	UPROPERTY() TObjectPtr<UTexture2D> IconTexture;

	FText PendingLabel;
	FText PendingDetail;
	float IconSize = 0.0f;
	float LabelMinWidth = 0.0f;
	EUiButtonKind Kind = EUiButtonKind::Secondary;
	EUiButtonLayout Layout = EUiButtonLayout::Inline;
	bool bEnabled = true;
	bool bSelected = false;
	bool bPainted = false;
	int32 PaintCount = 0;
};
