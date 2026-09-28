#pragma once

#include "CoreMinimal.h"

/**
 * Where a build tool's last choice is kept between launches - today, each tool's Surface row.
 *
 * AN INTERFACE, not GConfig called from FBuildSession, so a test's session never reads or
 * writes the player's own GameUserSettings.ini: a player who last picked concrete would
 * otherwise change what every default-constructed session in the suite lays. A session starts
 * with none (every tool on grass, nothing written); the two drivers - ARoadBuildController
 * and URoadBuildEdMode - hand it an FConfigToolPreferences. See FBuildSession::SetToolPreferences.
 */
class AIRSIDE_API IToolPreferences
{
public:
	virtual ~IToolPreferences() = default;

	/** What was stored under Key, or unset when nothing ever was. */
	virtual TOptional<FString> Read(const FString& Key) const = 0;

	virtual void Write(const FString& Key, const FString& Value) = 0;
};

/** Held in memory and lost with the object - what a test hands two sessions in turn. */
class AIRSIDE_API FMemoryToolPreferences : public IToolPreferences
{
public:
	virtual TOptional<FString> Read(const FString& Key) const override;
	virtual void Write(const FString& Key, const FString& Value) override;

	/** How many writes reached the store - a remembered choice that did not change must not. */
	int32 GetWriteCount() const { return WriteCount; }

private:
	TMap<FString, FString> Values;
	int32 WriteCount = 0;
};

/**
 * The player's per-user config: section [Airside.ToolPreferences] of GameUserSettings.ini.
 *
 * PER USER, NOT PER AIRPORT (ruled 2026-09-28): the surface is a habit of the player's, like a
 * keybinding, so it follows them to every level. GameUserSettings rather than
 * EditorPerProjectUserSettings because the game driver has no editor ini to write to, and the
 * two drivers should remember the same thing.
 *
 * FLUSHED ON EVERY WRITE. Writes happen only when a pick actually changes (FBuildSession
 * compares against what it last wrote), so this is a handful per session - and a flush left
 * for shutdown is lost on a crash or a Stop-Process, the way this project's editor usually ends.
 */
class AIRSIDE_API FConfigToolPreferences : public IToolPreferences
{
public:
	virtual TOptional<FString> Read(const FString& Key) const override;
	virtual void Write(const FString& Key, const FString& Value) override;

	/** Takes Key out of the file again - for a test that wrote one of its own, never a tool. */
	void Remove(const FString& Key);

	static const TCHAR* Section;
};
