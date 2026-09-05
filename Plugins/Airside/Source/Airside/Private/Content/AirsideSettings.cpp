#include "Content/AirsideSettings.h"

#include "Content/AirsideContent.h"
#include "Entities/AircraftType.h"

// File-local, matching every other category in this module.
DEFINE_LOG_CATEGORY_STATIC(LogAirsideContent, Log, All);

UAirsideSettings::UAirsideSettings()
{
	CategoryName = TEXT("Plugins");
	SectionName = TEXT("Airside");
}

const UAirsideContent* UAirsideSettings::GetContent()
{
	const UAirsideSettings* Settings = GetDefault<UAirsideSettings>();
	if (Settings == nullptr || Settings->Content.IsNull())
	{
		// Nothing configured. A supported state: every consumer of this already has to cope
		// with a null default, because each of these assets was optional before it moved
		// here. Deliberately silent - a project that assigns its own materials on the actor
		// should not be nagged about a set it does not want.
		return nullptr;
	}

	const UAirsideContent* Loaded = Settings->Content.LoadSynchronous();
	if (Loaded == nullptr)
	{
		// CONFIGURED AND MISSING is the case worth shouting about, and the reason this
		// function exists rather than a bare LoadSynchronous at each call site. The paths
		// this replaced failed exactly here and said so only in a CDO-time line that
		// scrolled past at startup, hours before anyone noticed the roads were gone.
		//
		// Warned once per load attempt rather than once ever: the useful moment to see it
		// is when the thing that needed it ran, not when the module happened to start.
		UE_LOG(LogAirsideContent, Warning,
			TEXT("Airside content set '%s' is configured but could not be loaded. "
				 "Materials, the default profile and the stand definition will all be absent. "
				 "Check Project Settings > Plugins > Airside."),
			*Settings->Content.ToString());
	}

	return Loaded;
}

FAirframe UAirsideSettings::ResolveDefaultAirframe()
{
	if (const UAirsideContent* Content = GetContent(); Content != nullptr)
	{
		if (const UAircraftType* Default = Content->DefaultAircraft.LoadSynchronous())
		{
			return Default->Airframe();
		}
	}

	// TODO(#30): author DA_PiperMeridian and set UAirsideContent::DefaultAircraft. Until an
	// asset exists to point it at, this is the fallback every project runs on - including
	// every automation test, which configures no content set at all.
	FAirframe Piper;
	Piper.Ground = UAircraftType::PiperMeridianGround();
	Piper.Climb = UAircraftType::PiperMeridianClimb();
	Piper.Approach = UAircraftType::PiperMeridianApproach();
	Piper.Engine = UAircraftType::PiperMeridianEngine();
	return Piper;
}
