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

	UPROPERTY(EditAnywhere, Category = "Type") FSlateFontInfo TitleFont;
	UPROPERTY(EditAnywhere, Category = "Type") FSlateFontInfo LabelFont;

	// Per-role sizes, uu. ONE UPROPERTY PER EUITextRole, so a size lives in exactly one place
	// instead of at every call site that used to retype it - the whole point of issue #89.
	UPROPERTY(EditAnywhere, Category = "Type", meta = (ClampMin = "6.0")) float HeadingSize = 9.0f;
	UPROPERTY(EditAnywhere, Category = "Type", meta = (ClampMin = "6.0")) float LabelSize = 9.0f;
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
};
