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
	double MinElevationDegrees = 8.0;

	UPROPERTY(EditAnywhere, Category = "Airside|Sky")
	double NoonAzimuthDegrees = 150.0;

	UPROPERTY(EditAnywhere, Category = "Airside|Sky")
	float NoonTemperatureKelvin = 5800.0f;

	UPROPERTY(EditAnywhere, Category = "Airside|Sky")
	float DuskTemperatureKelvin = 3200.0f;

	UPROPERTY(EditAnywhere, Category = "Airside|Sky")
	float NoonIntensity = 10.0f;

	/** Floor brightness as a fraction of noon. Decides whether dusk is playable. */
	UPROPERTY(EditAnywhere, Category = "Airside|Sky", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float DuskIntensityFraction = 0.35f;

	/**
	 * Where in the day this clock is, as a fraction in [0, 1). Noon when Clock is null.
	 *
	 * Static and clock-in-hand so the fallback is testable without a world: the editor
	 * viewport and the first PIE frame both have no clock, and noon is the angle the art
	 * direction was judged against.
	 */
	static double ResolveDayFraction(const USimClock* Clock);

	virtual void Tick(float DeltaSeconds) override;

protected:
	virtual void BeginPlay() override;

	/** Copy the tunables above onto a path, so details-panel edits take effect live. */
	FSunPath MakePath() const;

	/** Read the clock, evaluate the path, set the light. */
	void ApplyToSun();

private:
	/** So a missing Sun warns once rather than every frame. */
	bool bWarnedAboutMissingSun = false;
};
