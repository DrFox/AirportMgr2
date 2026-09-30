#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SunPath.h"
#include "SunDriver.generated.h"

class ADirectionalLight;
class USimClock;

/**
 * Points the level's sun at wherever the game clock says the sun should be.
 *
 * A FORWARDER, deliberately. All the reasoning lives in FSunPath, which is world-free and
 * tested; this class reads a clock, calls it, and sets three values on a light. Putting
 * the curve here instead would make it reachable only by spawning an actor in a level.
 *
 * THE LIGHT IS AN EXPLICIT LEVEL-AUTHORED POINTER, not a TActorIterator<ADirectionalLight>
 * search. A search silently picks one of two lights, and the wrong choice looks exactly
 * like the feature not working; this project also puts knobs in properties on purpose.
 *
 * The tunables are UPROPERTYs copied onto a plain FSunPath rather than a USTRUCT member,
 * which is what ARoadBuildController does with FBuildCameraRig through ApplyViewLimits:
 * details-panel edits take effect live, and the struct stays clear of UHT.
 */
UCLASS()
class AIRPORTMGR_API ASunDriver : public AActor
{
	GENERATED_BODY()

public:
	ASunDriver();

	/** The level's sun. Nothing happens if this is empty, and a warning says so once. */
	UPROPERTY(EditAnywhere, Category = "Airside|Sky")
	TObjectPtr<ADirectionalLight> Sun;

	/** Elevation at noon, degrees above the horizon. */
	UPROPERTY(EditAnywhere, Category = "Airside|Sky", meta = (ClampMin = "1.0", ClampMax = "89.0"))
	double MaxElevationDegrees = 42.0;

	/** The dusk floor. The sun never drops below this - see FSunPath for why. */
	UPROPERTY(EditAnywhere, Category = "Airside|Sky", meta = (ClampMin = "0.0", ClampMax = "89.0"))
	double MinElevationDegrees = 15.0;

	UPROPERTY(EditAnywhere, Category = "Airside|Sky")
	double NoonAzimuthDegrees = 150.0;

	UPROPERTY(EditAnywhere, Category = "Airside|Sky")
	float NoonTemperatureKelvin = 5800.0f;

	UPROPERTY(EditAnywhere, Category = "Airside|Sky")
	float DuskTemperatureKelvin = 4300.0f;

	UPROPERTY(EditAnywhere, Category = "Airside|Sky")
	float NoonIntensity = 10.0f;

	/** Floor brightness as a fraction of noon. Decides whether dusk is playable. */
	UPROPERTY(EditAnywhere, Category = "Airside|Sky", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float DuskIntensityFraction = 0.45f;

	/**
	 * Where in the day this clock is, as a fraction in [0, 1). Noon when Clock is null.
	 *
	 * Static and clock-in-hand so the fallback is testable without a world: the editor
	 * viewport and the first PIE frame both have no clock, and noon is the angle the art
	 * direction was judged against.
	 */
	static double ResolveDayFraction(const USimClock* Clock);

	/**
	 * The path this driver evaluates: its tunables, and the clock's daylight - the game clock's DawnHour/DuskHour (the scenario's, applied by
	 * UOpsRuntime::ApplyScenarioFigures), the hours the day's time compression, the demand curve and the inbox's night shading already follow, so
	 * there is ONE definition of night (#447). WITH NO CLOCK (the editor viewport, the first PIE frame) the path keeps FSunPath's own hours, which
	 * are the scenario's defaults - ONE path to those figures, pinned by AirportMgr.Sky.SunPath.DefaultsAreTheScenarios - rather than a second read
	 * of the scenario here.
	 * Public so a test can ask the path a driver WOULD use: the wiring between the clock's dusk and the sky, which a test of FSunPath alone cannot see.
	 * ENFORCED BY: AirportMgr.Sky.SunDriver.DuskIsTheClocks, Check-Architecture rule 67 (MakePath hands both hours on)
	 */
	FSunPath MakePath(const USimClock* Clock) const;

	virtual void Tick(float DeltaSeconds) override;

protected:
	virtual void BeginPlay() override;

	/** Read the clock, evaluate the path, set the light. */
	void ApplyToSun();

private:
	/** So a missing Sun warns once rather than every frame. */
	bool bWarnedAboutMissingSun = false;

	/** Last elevation written to the log, so the trail is movements and not every tick. */
	double LastLoggedElevation = -1000.0;
};
