#include "SunDriver.h"

#include "Components/DirectionalLightComponent.h"
#include "Engine/DirectionalLight.h"
#include "Model/GameTimeText.h"
#include "Model/SimClock.h"
#include "Present/OpsRuntime.h"
#include "OpsRuntimeResolver.h"

DEFINE_LOG_CATEGORY_STATIC(LogSunDriver, Log, All);

ASunDriver::ASunDriver()
{
	PrimaryActorTick.bCanEverTick = true;
}

double ASunDriver::ResolveDayFraction(const USimClock* Clock)
{
	if (Clock == nullptr)
	{
		return 0.5;
	}

	// TimeOfDay() is the clock's OWN answer to "where in the day are we", and it already
	// floors correctly across a day boundary (SimClock.cpp:9-12). Re-deriving it here with
	// an Fmod would be a second implementation of the day wrap, and the two would drift.
	return Clock->TimeOfDay() / USimClock::SecondsPerDay;
}

// Copy the tunables above onto a path, so details-panel edits take effect live - and the clock's daylight, so night starts when the clock says.
FSunPath ASunDriver::MakePath(const USimClock* Clock) const
{
	FSunPath Path;
	if (Clock != nullptr)
	{
		// THE CLOCK'S DAY. With none, Path keeps FSunPath's own hours - the scenario's defaults (DefaultsAreTheScenarios) - which is the one path
		// to those figures: this used to read GetDefault<UScenario>() as well, a second route to the same numbers (#447 review).
		Path.DawnHour = Clock->DawnHour;
		Path.DuskHour = Clock->DuskHour;
	}
	Path.MaxElevationDegrees = MaxElevationDegrees;
	Path.MinElevationDegrees = MinElevationDegrees;
	Path.NoonAzimuthDegrees = NoonAzimuthDegrees;
	Path.NoonTemperatureKelvin = NoonTemperatureKelvin;
	Path.DuskTemperatureKelvin = DuskTemperatureKelvin;
	Path.NoonIntensity = NoonIntensity;
	Path.DuskIntensityFraction = DuskIntensityFraction;
	return Path;
}

void ASunDriver::ApplyToSun()
{
	if (Sun == nullptr)
	{
		if (!bWarnedAboutMissingSun)
		{
			bWarnedAboutMissingSun = true;
			UE_LOG(LogSunDriver, Warning,
				TEXT("ASunDriver '%s' has no Sun set, so the day cycle does nothing. "
				     "Point it at the level's DirectionalLight in the Details panel."),
				*GetName());
		}
		return;
	}

	UDirectionalLightComponent* Component = Sun->FindComponentByClass<UDirectionalLightComponent>();
	if (Component == nullptr)
	{
		return;
	}

	// THE RESOLVER is built for exactly this: editor worlds have no game instance, and it
	// returns null there rather than making every caller unpick the chain. ResolveDayFraction
	// turns that null into noon.
	const UOpsRuntime* Runtime = OpsRuntimeResolver::Resolve(GetWorld());
	const USimClock* Clock = Runtime != nullptr ? Runtime->GetClock() : nullptr;

	const double DayFraction = ResolveDayFraction(Clock);
	const FSunLighting Lighting = MakePath(Clock).At(DayFraction);
	Sun->SetActorRotation(Lighting.Rotation);
	Component->SetTemperature(Lighting.TemperatureKelvin);
	Component->SetIntensity(Lighting.Intensity);

	// A TRAIL, NOT A SPAM. Logging every tick would bury the log; logging only at BeginPlay
	// would prove the driver ran once and nothing about whether it keeps up with the clock.
	// Two degrees of elevation is roughly eight minutes of game time at this curve's
	// steepest, so the trail is a handful of lines per game day and each one is a real
	// movement. Without it "the sun is not moving" is a guess about a clock, a subsystem,
	// a tick and a curve at once.
	const double Elevation = -Lighting.Rotation.Pitch;
	if (FMath::Abs(Elevation - LastLoggedElevation) >= 2.0)
	{
		LastLoggedElevation = Elevation;
		UE_LOG(LogSunDriver, Log,
			TEXT("Sun at day fraction %.4f (%s): elevation %.1f, azimuth %.1f, "
			     "intensity %.2f, %.0fK%s"),
			DayFraction, *GameTimeText::TimeOfDay(DayFraction * USimClock::SecondsPerDay),
			Elevation, Lighting.Rotation.Yaw, Lighting.Intensity, Lighting.TemperatureKelvin,
			Clock == nullptr ? TEXT(" (NO CLOCK - parked at noon)") : TEXT(""));
	}
}

void ASunDriver::BeginPlay()
{
	Super::BeginPlay();

	// Once up front so the first frame is already at the right time of day, rather than
	// snapping to it after a tick.
	ApplyToSun();
}

void ASunDriver::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	ApplyToSun();
}
