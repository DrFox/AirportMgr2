#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "AirsideSettings.generated.h"

class UAirsideContent;

/**
 * Where the plugin is told which content set to use. The ONE remaining path, and it is data.
 *
 * A UDeveloperSettings puts it in Config/DefaultAirside.ini as one readable line and gives it
 * an asset picker under Project Settings > Plugins > Airside. That matters for two reasons
 * this project cares about: it is plain text, so a change to it shows up in a diff the way
 * every other decision here does; and it is not C++, so the eight literals it replaces can
 * never again be a reference the editor cannot see.
 *
 * It does not solve the problem so much as reduce it from eight to one - a folder move still
 * leaves this line stale. The difference is that everything BEHIND it is a real asset
 * reference the editor maintains, so the one line only changes when the content set itself
 * moves, and a wrong value here fails loudly at first use rather than silently at CDO time.
 */
UCLASS(config = Airside, defaultconfig, meta = (DisplayName = "Airside"))
class AIRSIDE_API UAirsideSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UAirsideSettings();

	/** Defaults for materials, profiles and the stand. See UAirsideContent. */
	UPROPERTY(config, EditAnywhere, Category = "Content")
	TSoftObjectPtr<UAirsideContent> Content;

	/**
	 * The content set, loaded on first use, or null if none is configured.
	 *
	 * NOT callable from a constructor, and that is the point rather than a limitation. The
	 * paths this replaces were read at CDO construction, which is the one moment the asset
	 * registry is not reliably up, and ConstructorHelpers exists precisely to work around
	 * that. Every caller now asks at the moment it needs the asset - a rebuild, a spawn -
	 * by which time loading is ordinary.
	 *
	 * Logs once, loudly, when a content set is configured and cannot be loaded. A missing
	 * default is a supported state; a configured one that is not there is a mistake, and
	 * silence about it is what cost this project an hour of looking at empty roads.
	 */
	static const UAirsideContent* GetContent();
};
