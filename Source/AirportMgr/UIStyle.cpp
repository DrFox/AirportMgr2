#include "UIStyle.h"

#include "Engine/Texture2D.h"

DEFINE_LOG_CATEGORY_STATIC(LogUIStyle, Log, All);

UTexture2D* UUIStyle::IconFor(FName ActionId) const
{
	const TSoftObjectPtr<UTexture2D>* Found = IconsByActionId.Find(ActionId);
	if (Found == nullptr)
	{
		return nullptr;
	}
	// Synchronous because the bar builds once, at construction, and an icon that streams in
	// later would pop after the player has already looked at the button.
	return Found->LoadSynchronous();
}

const UUIStyle* UAirportMgrUISettings::ResolveStyle()
{
	const UAirportMgrUISettings* Settings = GetDefault<UAirportMgrUISettings>();
	if (Settings->Style.IsNull())
	{
		// Once, not per widget construction: an unconfigured style is a supported state.
		static bool bWarned = false;
		if (!bWarned)
		{
			bWarned = true;
			UE_LOG(LogUIStyle, Log, TEXT("No UI Style configured; using UUIStyle's built-in defaults"));
		}
		return GetDefault<UUIStyle>();
	}

	const UUIStyle* Loaded = Settings->Style.LoadSynchronous();
	if (Loaded == nullptr)
	{
		UE_LOG(LogUIStyle, Error,
			TEXT("UI Style '%s' is configured but failed to load; falling back to built-in defaults"),
			*Settings->Style.ToString());
		return GetDefault<UUIStyle>();
	}
	return Loaded;
}
