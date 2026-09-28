#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameUserSettings.h"
#include "UI/UiLayoutStore.h"
#include "AirportMgrUserSettings.generated.h"

/**
 * The player's own settings (UI library step 3, spec section 3): the engine's per-player object,
 * named as GameUserSettingsClassName in DefaultEngine.ini, so it saves to the player's
 * GameUserSettings.ini beside the scalability it already owns - where a shipped game keeps them.
 *
 * EVERY FIELD HAS A READER: WindowLayout the window host's store, the rest FGamePlayerSettingsSink.
 * A config field no code reads is the declared-never-consumed bug this codebase keeps shipping
 * (CLAUDE.md, "Check where a list is CONSUMED") - which is why these arrived with their reader in
 * step 4b, not with WindowLayout in step 3.
 */
UCLASS(config = GameUserSettings)
class AIRPORTMGR_API UAirportMgrUserSettings : public UGameUserSettings
{
	GENERATED_BODY()

public:
	/** The one accessor. Null (logged once) if the engine was not told to make this class. */
	static UAirportMgrUserSettings* Get();

	/** Where each window was left, by window id - see FUserSettingsLayoutStore. */
	UPROPERTY(config) TMap<FName, FUiWindowPlacement> WindowLayout;

	// THE SETTINGS WINDOW'S FOUR (step 4b). Each is read by FGamePlayerSettingsSink, which pushes it
	// to its consumer; the drive side and graphics are not here - the road actor and the engine's
	// own scalability already hold them.

	/** Multiplies the engine's DPI curve (UUserInterfaceSettings::ApplicationScale). 0.75-1.5. */
	UPROPERTY(config) float UIScale = 1.0f;
	/** Multiply UBuildCameraComponent's PanRate / ZoomStep (SetPlayerSpeedScales). 0.5-2.0. */
	UPROPERTY(config) float PanSpeedScale = 1.0f;
	UPROPERTY(config) float ZoomSpeedScale = 1.0f;
	/** A fresh start turns the world grid on (PlayerSettings::ApplyStartGrid). */
	UPROPERTY(config) bool bGridSnapOnStart = false;
};
