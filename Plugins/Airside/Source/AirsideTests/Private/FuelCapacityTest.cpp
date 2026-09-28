#include "CoreMinimal.h"
#include "Entities/AircraftType.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Every aircraft type's tank, as the ASSETS say it (spec 2026-09-28-fuel-litres section 1).
 *
 * THE CHECK THAT SURVIVES THE AUTHORING SCRIPT, for the reason Airside.Content.
 * PushbackNeedsAuthored gives: both headless save APIs report success while writing nothing, so
 * build_fuel_capacities.py's MARKER lines say it ran, not that the value landed. The figures are
 * published tank capacities in litres; the load a flight asks for is drawn from them at the offer.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFuelCapacitiesAuthoredTest,
	"Airside.Content.FuelCapacitiesAuthored",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFuelCapacitiesAuthoredTest::RunTest(const FString& Parameters)
{
	struct FExpected { const TCHAR* Asset; double Litres; };
	const FExpected Expected[] = {
		{ TEXT("DA_Aircraft_Plane1"), 212.0 },     // Cessna 172 Skyhawk
		{ TEXT("DA_Aircraft_Plane2"), 1466.0 },    // DHC-6 Twin Otter
		{ TEXT("DA_Aircraft_Plane3"), 6526.0 },    // Dash 8-Q400
		{ TEXT("DA_Aircraft_Plane4"), 26020.0 },   // 737-800
		{ TEXT("DA_Aircraft_Plane5"), 2040.0 },    // King Air 350i
		{ TEXT("DA_Aircraft_Plane6"), 181283.0 },  // 777-300ER
		{ TEXT("DA_Aircraft_Plane7"), 454.0 },     // PA-46-500TP Meridian
		{ TEXT("DA_Aircraft_Plane8"), 320000.0 },  // A380-800
		{ TEXT("DA_Aircraft_Plane9"), 24210.0 },   // A320-200
		{ TEXT("DA_Aircraft_Plane10"), 1268.0 },   // Cessna 208B Grand Caravan
		{ TEXT("DA_Aircraft_Plane11"), 158987.0 }, // A350-1000
		{ TEXT("DA_Aircraft_Plane12"), 189.0 },    // Piper PA-28-180 Cherokee
		{ TEXT("DA_Aircraft_Plane13"), 43490.0 },  // 757-300
		{ TEXT("DA_Aircraft_Plane14"), 3020.0 },   // Phenom 300
		{ TEXT("DA_Aircraft_Plane15"), 348.0 },    // Cirrus SR22
		{ TEXT("DA_Aircraft_Plane16"), 734.0 },    // Beechcraft Baron 58
		{ TEXT("DA_Aircraft_Plane17"), 466.0 },    // Piper PA-34 Seneca
		{ TEXT("DA_Aircraft_Plane18"), 3220.0 },   // Saab 340B
		{ TEXT("DA_Aircraft_A320"), 24210.0 },
		{ TEXT("DA_Aircraft_B738"), 26020.0 },
	};
	for (const FExpected& Each : Expected)
	{
		const FString Path = FString::Printf(TEXT("/Game/Entities/%s.%s"), Each.Asset, Each.Asset);
		const UAircraftType* Type = LoadObject<UAircraftType>(nullptr, *Path);
		if (!TestNotNull(*FString::Printf(TEXT("%s loads"), Each.Asset), Type))
		{
			continue;
		}
		TestEqual(*FString::Printf(TEXT("%s's tank"), Each.Asset), Type->FuelCapacityLitres, Each.Litres, 0.5);
		TestEqual(*FString::Printf(TEXT("%s's tank travels in its airframe"), Each.Asset),
			Type->Airframe().FuelCapacityLitres, Each.Litres, 0.5);
	}
	return true;
}

#endif
