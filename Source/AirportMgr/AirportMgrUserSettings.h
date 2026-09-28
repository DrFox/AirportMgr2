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
 * ONE FIELD FOR NOW - WindowLayout. The spec's other five (UI scale, camera speeds, drive side,
 * grid snap default) arrive in step 4 with the Settings control that edits each and the code that
 * reads it: a config field no code reads is the declared-never-consumed bug this codebase keeps
 * shipping (CLAUDE.md, "Check where a list is CONSUMED").
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
};
