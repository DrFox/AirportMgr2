#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Engine/DeveloperSettings.h"
#include "Fonts/SlateFontInfo.h"
#include "UIStyle.generated.h"

class UTexture2D;
class UTextBlock;

/**
 * Where a text block sits in the type hierarchy - ROLE, not a retyped size.
 *
 * ApplyText is the one function that turns a role into a fallback font, a size, a
 * letter-spacing and a colour. Eleven call sites across four widgets used to do this by hand,
 * with thirteen literal sizes between them (9/10/11/12/13) - see issue #89. Title and Clock
 * stay two roles even though they share a default size: the clock is a specific readout that
 * may later want tabular figures or its own size without dragging every other title with it.
 */
UENUM(BlueprintType)
enum class EUITextRole : uint8
{
	Heading,   // ALL-CAPS section captions, widely letter-spaced - a section name, "OFFERS".
	Label,     // A short caption - a button label, a badge count, a countdown.
	Body,      // A descriptive sentence that may wrap - facts, a refusal, a toast message.
	Title,     // A prominent single-line identifier - an airline name, a panel title.
	Clock,     // The bar's clock readout. See the enum comment for why it is not Title.
};

/**
 * Every colour, font, size and icon the game UI draws with.
 *
 * SEMANTIC SLOTS, NOT PER-WIDGET COLOURS, and that is the whole reason this asset exists.
 * A slot is named for what it IS - Panel, Accent - never for where it appears
 * (BarBackground, SelectedToolTint). Three whole visual directions were mocked up during
 * design and each is reachable from these few values alone; name them for their location
 * and a re-skin becomes a hunt through widgets instead of a handful of edits in a Details
 * panel. Six were specified; Warning and Positive were added when the toasts needed to say
 * "this went badly" without spending Accent - see their own comment.
 *
 * The defaults below are the concept sheet's BUILDINGS row, so the UI is literally the
 * colour the hangars and terminal will be - see the art direction spec, section 1.1.
 *
 * NOT a Widget Blueprint, which would give WYSIWYG layout but cannot be authored from
 * Python on this engine build (UWidgetBlueprint::WidgetTree is not a scriptable property -
 * see UBuildBarWidget's class comment) and would bring back the stale-Blueprint failure
 * mode. The cost of that choice, stated plainly: colours and fonts are free forever;
 * layout is C++.
 */
UCLASS(BlueprintType)
class AIRPORTMGR_API UUIStyle : public UDataAsset
{
	GENERATED_BODY()

public:
	/** Section panels. Sheet: buildings, slate blue. */
	UPROPERTY(EditAnywhere, Category = "Colours") FLinearColor Panel = FLinearColor::FromSRGBColor(FColor(0x4F, 0x5E, 0x6A));

	/** The status strip, and any surface that must sit behind Panel. */
	UPROPERTY(EditAnywhere, Category = "Colours") FLinearColor PanelDark = FLinearColor::FromSRGBColor(FColor(0x3E, 0x4A, 0x54));

	/** An unselected button. */
	UPROPERTY(EditAnywhere, Category = "Colours") FLinearColor Button = FLinearColor::FromSRGBColor(FColor(0x5D, 0x6D, 0x7A));

	/** The selected tool, AND NOTHING ELSE. Sheet: vehicles, yellow. If a second thing
	 *  takes this colour, the player stops being able to see at a glance what is armed. */
	UPROPERTY(EditAnywhere, Category = "Colours") FLinearColor Accent = FLinearColor::FromSRGBColor(FColor(0xF4, 0xBA, 0x38));

	/** Sheet: buildings, cream. */
	UPROPERTY(EditAnywhere, Category = "Colours") FLinearColor Text = FLinearColor::FromSRGBColor(FColor(0xE4, 0xE0, 0xD9));

	/** Section headings and disabled labels. */
	UPROPERTY(EditAnywhere, Category = "Colours") FLinearColor TextMuted = FLinearColor::FromSRGBColor(FColor(0x9F, 0xB0, 0xBD));

	/**
	 * Something went wrong, or will. A brick that sits with the slate rather than a signal
	 * red, because it appears on a toast the player reads, not on a klaxon.
	 *
	 * A SEVENTH AND EIGHTH SLOT, added deliberately. Six could not express "this went badly"
	 * without spending Accent, and Accent means the armed tool and nothing else - the moment
	 * a warning shared it, the one glance that says which tool is live would be gone. These
	 * two are still SEMANTIC (named for what they mean, not where they appear), so the
	 * re-skin promise in section 2.1 of the spec holds.
	 */
	UPROPERTY(EditAnywhere, Category = "Colours") FLinearColor Warning = FLinearColor::FromSRGBColor(FColor(0xC4, 0x5D, 0x45));

	/** It worked. A sage that belongs to the same field as the grass, not a UI green. */
	UPROPERTY(EditAnywhere, Category = "Colours") FLinearColor Positive = FLinearColor::FromSRGBColor(FColor(0x7E, 0x9C, 0x6B));

	/**
	 * The plot-panel backdrop ARoadBuildHUD::DrawPlotPanel draws behind its readout text - the
	 * one label in the game that can land on grass, concrete or the ghost's own white, where
	 * coloured text alone is unreadable. NOT a PreviewPalette entry (issue #192, following
	 * PR #224's move of that table into Present/): PreviewPalette is the shared table the HUD
	 * and the editor viewport's IToolPreviewSink both read so a style's LOOK cannot drift
	 * between the two; this backdrop has exactly one reader (the game HUD's own text panel) and
	 * belongs with the rest of the game UI's colours instead. Default equals the old literal
	 * `FLinearColor(0.02f, 0.03f, 0.04f, 0.72f)`.
	 */
	UPROPERTY(EditAnywhere, Category = "Colours") FLinearColor HudGround = FLinearColor(0.02f, 0.03f, 0.04f, 0.72f);

	UPROPERTY(EditAnywhere, Category = "Type") FSlateFontInfo TitleFont;
	UPROPERTY(EditAnywhere, Category = "Type") FSlateFontInfo LabelFont;

	// Per-role sizes, uu. ONE UPROPERTY PER EUITextRole, so a size lives in exactly one place
	// instead of at every call site that used to retype it - the whole point of issue #89.
	//
	// LabelSize IS 11, NOT 9, ON PURPOSE: the six Label call sites this collapses came in at
	// 9 (bar button captions, the inbox badge) AND 11 (the inbox's ETA countdown, its Accept/
	// Decline verbs). One role can only pick one size, and Accept/Decline are the two buttons
	// an offer actually lives or dies on - shrinking the affirmative verb the player must read
	// and click is the wrong two sites to save, against a badge count and tool captions that
	// only gain legibility from the same +2. See the PR body's size table for every site.
	UPROPERTY(EditAnywhere, Category = "Type", meta = (ClampMin = "6.0")) float HeadingSize = 9.0f;
	UPROPERTY(EditAnywhere, Category = "Type", meta = (ClampMin = "6.0")) float LabelSize = 11.0f;
	UPROPERTY(EditAnywhere, Category = "Type", meta = (ClampMin = "6.0")) float BodySize = 11.0f;
	UPROPERTY(EditAnywhere, Category = "Type", meta = (ClampMin = "6.0")) float TitleSize = 13.0f;
	UPROPERTY(EditAnywhere, Category = "Type", meta = (ClampMin = "6.0")) float ClockSize = 13.0f;

	/** Square edge of a tool button, uu. Today's bar is about 30 and is hard to hit. */
	UPROPERTY(EditAnywhere, Category = "Metrics", meta = (ClampMin = "32.0")) float ButtonSize = 56.0f;

	/**
	 * Corner rounding, uu. CONSUMED by the toast cards through FSlateRoundedBoxBrush.
	 *
	 * It sat here unread for a while and the toasts drew as flat square slabs because of it -
	 * the declared-but-never-consumed bug CLAUDE.md names three times, in a new place.
	 * AirportMgr.UI.ToastCardUsesTheStyleCornerRadius reads it back off the brush so it
	 * cannot quietly stop being used again.
	 */
	UPROPERTY(EditAnywhere, Category = "Metrics") float CornerRadius = 5.0f;
	UPROPERTY(EditAnywhere, Category = "Metrics") float SectionPadding = 14.0f;

	/**
	 * Padding inside the one bordered card EnsureCardRoot builds for every panel that calls it
	 * (the inspector, the offer inbox, the ledger). NAMED by issue #192: it was a bare
	 * `FMargin(12.0f, 10.0f)` inside UAirportMgrPanelWidget::EnsureCardRoot with nothing else
	 * in the codebase reading the same value, the "UI literals remaining after #89-#91" finding.
	 * Default equals that literal, so this is a rename, not a re-tune - see UIStyleTest.
	 */
	UPROPERTY(EditAnywhere, Category = "Metrics") FMargin CardPadding = FMargin(12.0f, 10.0f);

	/**
	 * Gap between stacked rows in a list panel. NAMED by issue #192 for the ledger panel's own
	 * row spacing (`ULedgerPanelWidget::EnsureSlots`'s `FMargin(0.0f, 6.0f, 0.0f, 0.0f)`), which
	 * had no name at all. UOfferInboxWidget::RowGap is a SEPARATE, already-named per-widget
	 * knob (its own EditAnywhere property, predating this issue) and is deliberately left alone
	 * - the two happening to share a value today is not a reason to delete a knob a Blueprint
	 * restyle may already be relying on. Default equals the ledger's old literal.
	 */
	UPROPERTY(EditAnywhere, Category = "Metrics") float RowGap = 6.0f;

	/**
	 * Padding inside an offer's Accept/Decline button, both states. NAMED by issue #192: two
	 * identical `FMargin(12.0f, 5.0f)` literals in UOfferInboxWidget::MakeAnswerButton
	 * (SetNormalPadding and SetPressedPadding both took the same value, unnamed, twice).
	 * Default equals that literal.
	 */
	UPROPERTY(EditAnywhere, Category = "Metrics") FMargin ButtonPadding = FMargin(12.0f, 5.0f);

	/**
	 * Alpha of a toast card's severity-coloured outline (UToastStackWidget::BuildCard). NAMED
	 * by issue #192: a bare `0.85f` built inline into the FSlateRoundedBoxBrush's outline
	 * colour, with no other reader. Default equals that literal.
	 */
	UPROPERTY(EditAnywhere, Category = "Metrics", meta = (ClampMin = "0.0", ClampMax = "1.0")) float OutlineAlpha = 0.85f;

	/**
	 * The two constants of UToastStackWidget::OpacityFor's fade curve - see that function's own
	 * comment for the reasoning, unchanged by this rename. NAMED by issue #192: both were bare
	 * literals (`2.0` and `0.15`) in a single FMath::Clamp call with no other reader. Defaults
	 * equal those literals.
	 */
	UPROPERTY(EditAnywhere, Category = "Metrics", meta = (ClampMin = "0.0")) float ToastFadeDuration = 2.0f;
	UPROPERTY(EditAnywhere, Category = "Metrics", meta = (ClampMin = "0.0", ClampMax = "1.0")) float ToastFadeFloor = 0.15f;

	/**
	 * Keyed by FBuildAction::Id, NOT held as a field on the action.
	 *
	 * The registry's Tools section is generated from Airside's ToolRegistry(), and Airside
	 * must not learn about game-module UI textures - Check-Architecture enforces that
	 * direction and has failed on a COMMENT that merely named a forbidden module. Keying by
	 * Id keeps the dependency pointing one way.
	 */
	UPROPERTY(EditAnywhere, Category = "Icons") TMap<FName, TSoftObjectPtr<UTexture2D>> IconsByActionId;

	/**
	 * The notification icons, one per severity.
	 *
	 * NAMED FIELDS, not a map keyed by severity. A map can be missing a key and then draws
	 * nothing, which is a blank chip nobody notices; three fields cannot. The action icons
	 * above are a map for the opposite reason - their keys come from a registry that grows.
	 */
	UPROPERTY(EditAnywhere, Category = "Icons") TSoftObjectPtr<UTexture2D> IconInfo;
	UPROPERTY(EditAnywhere, Category = "Icons") TSoftObjectPtr<UTexture2D> IconSuccess;
	UPROPERTY(EditAnywhere, Category = "Icons") TSoftObjectPtr<UTexture2D> IconWarning;

	/** The icon for an action, or null when none is mapped. Loads on first use. */
	UTexture2D* IconFor(FName ActionId) const;

	/**
	 * Fallback font, size, letter-spacing and colour for one role, applied once.
	 *
	 * Replaces the pattern `FSlateFontInfo F = Style->LabelFont.HasValidFont() ? Style->
	 * LabelFont : X->GetFont(); F.Size = 9; ...` that was copy-pasted at eleven call sites
	 * across UBuildBarWidget, UOfferInboxWidget and UToastStackWidget (issue #89). Colour is a
	 * PARAMETER, not part of the role, because the same role draws in different slots
	 * depending on state (a label is Text when enabled, TextMuted when not) - baking one
	 * colour into the role would just move that second source of truth rather than remove it.
	 * Layout (wrap width, alignment, visibility) stays at the call site; this only owns type.
	 */
	void ApplyText(UTextBlock& TextBlock, EUITextRole Role, FLinearColor Colour) const;
};

/**
 * Where the game is told which style asset to use - one line in Config/DefaultGame.ini and
 * an asset picker under Project Settings. Mirrors UAirsideSettings for the same reasons.
 */
UCLASS(config = Game, defaultconfig, meta = (DisplayName = "AirportMgr UI"))
class AIRPORTMGR_API UAirportMgrUISettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UPROPERTY(config, EditAnywhere, Category = "UI")
	TSoftObjectPtr<UUIStyle> Style;

	/**
	 * The configured style, or UUIStyle's CDO when none is configured or one fails to load.
	 * NEVER NULL, deliberately.
	 *
	 * ResolveDefaultScenario returned null in exactly this situation, every caller guarded
	 * with `if (...)`, and the defaults its own log promised were therefore applied to
	 * nothing - the clock silently started at midnight for weeks. A UI that falls back to
	 * the CDO renders in the declared palette instead of not rendering.
	 */
	static const UUIStyle* ResolveStyle();

	/**
	 * How many times ResolveStyle has actually run, since process start - not how many times a
	 * caller ASKED, since that is the whole point of caching it.
	 *
	 * issue #187: UBuildBarWidget called this (a TSoftObjectPtr::LoadSynchronous) twice a
	 * tick before it had anywhere to cache the result; this is the seam a headless test reads
	 * to measure the fix directly, as a DELTA across N ticks, rather than trust a trace of the
	 * call sites by eye - the same reason FBuildSession::MakeContextCallCountForTest exists.
	 * A global counter, so a test must read it before and after and compare the difference:
	 * other tests in the same run call ResolveStyle too, and an absolute count would be
	 * whatever order the automation runner happened to execute them in.
	 */
	static int32 ResolveCallCountForTest() { return CallCountForTest; }

private:
	static int32 CallCountForTest;
};
