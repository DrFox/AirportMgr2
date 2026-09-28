#pragma once

#include "CoreMinimal.h"
#include "Model/RoadTraffic.h"

struct FSnapGuideSettings;

/**
 * Everything the Settings window edits, as ONE value (UI library step 4b, spec section 3) - so the
 * snapshot Cancel restores is a copy, and "every field restored" is one comparison, not six.
 *
 * GraphicsQuality is the engine's overall scalability level, 0..3 (Low..Epic), or -1 when the
 * engine reports a CUSTOM mix (UGameUserSettings::GetOverallScalabilityLevel). DriveSide is the
 * AIRPORT's, not the player's: it lives on the road actor with its undo step, and this carries it
 * so the dialog edits the one value the bar's "Drive left" button already lights from.
 */
struct FPlayerSettings
{
	float UIScale = 1.0f;
	float PanSpeedScale = 1.0f;
	float ZoomSpeedScale = 1.0f;
	bool bGridSnapOnStart = false;
	int32 GraphicsQuality = -1;
	EDriveSide DriveSide = EDriveSide::Right;

	bool operator==(const FPlayerSettings& Other) const = default;
};

/**
 * Where FPlayerSettings come from and go to - the IUiLayoutStore pattern: the Settings window
 * knows only this, a test hands it FMemoryPlayerSettingsSink, and the game FGamePlayerSettingsSink,
 * which writes UAirportMgrUserSettings and pushes each value to the code that reads it.
 */
class IPlayerSettingsSink
{
public:
	virtual ~IPlayerSettingsSink() = default;
	/** The values in force now. */
	virtual FPlayerSettings Read() const = 0;
	/** Put these in force now - live, unsaved. */
	virtual void Apply(const FPlayerSettings& Values) = 0;
	/** Persist what is in force. */
	virtual void Save() = 0;
};

/** A sink that remembers, for tests: what is in force, what was last saved, and how often. */
class FMemoryPlayerSettingsSink : public IPlayerSettingsSink
{
public:
	virtual FPlayerSettings Read() const override { return Values; }
	virtual void Apply(const FPlayerSettings& InValues) override { Values = InValues; ++Applies; }
	virtual void Save() override { Saved = Values; ++Saves; }

	FPlayerSettings Values;
	FPlayerSettings Saved;
	int32 Applies = 0;
	int32 Saves = 0;
};

class ARoadBuildController;

/**
 * The game's sink: UAirportMgrUserSettings for the player's own values (saved to their
 * GameUserSettings.ini), and each value pushed to the code that reads it.
 */
class AIRPORTMGR_API FGamePlayerSettingsSink : public IPlayerSettingsSink
{
public:
	explicit FGamePlayerSettingsSink(ARoadBuildController& InController);
	virtual FPlayerSettings Read() const override;
	virtual void Apply(const FPlayerSettings& Values) override;
	virtual void Save() override;

	/** Puts the engine's UI scale back as it was: the settings CDO Apply writes outlives a PIE session. */
	void RestoreEngineScale();

private:
	TWeakObjectPtr<ARoadBuildController> Controller;
	float EngineScaleAtStart = 1.0f;
};

namespace PlayerSettings
{
	/** Grid snap on start: turns a grid ON (the Grid button's first step) when the level has none;
	 *  never turns a level's own grid off. ENFORCED BY: AirportMgr.Settings.StartGrid. */
	AIRPORTMGR_API void ApplyStartGrid(FSnapGuideSettings& Guides, bool bOn);
}
