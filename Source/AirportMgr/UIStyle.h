#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Engine/DeveloperSettings.h"
#include "Fonts/SlateFontInfo.h"
#include "UIStyle.generated.h"

class UTexture2D;

/**
 * Every colour, font, size and icon the game UI draws with.
 *
 * SEMANTIC SLOTS, NOT PER-WIDGET COLOURS, and that is the whole reason this asset exists.
 * A slot is named for what it IS - Panel, Accent - never for where it appears
 * (BarBackground, SelectedToolTint). Three whole visual directions were mocked up during
 * design and each is reachable from these six values alone; name them for their location
 * and a re-skin becomes a hunt through widgets instead of six edits in a Details panel.
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

	UPROPERTY(EditAnywhere, Category = "Type") FSlateFontInfo TitleFont;
	UPROPERTY(EditAnywhere, Category = "Type") FSlateFontInfo LabelFont;

	/** Square edge of a tool button, uu. Today's bar is about 30 and is hard to hit. */
	UPROPERTY(EditAnywhere, Category = "Metrics", meta = (ClampMin = "32.0")) float ButtonSize = 56.0f;

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

	/** The icon for an action, or null when none is mapped. Loads on first use. */
	UTexture2D* IconFor(FName ActionId) const;
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
