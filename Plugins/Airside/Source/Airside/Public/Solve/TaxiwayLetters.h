#pragma once

#include "CoreMinimal.h"

/**
 * The letters a taxiway may be issued (ICAO Annex 14; spec 2026-10-02): A..Z without I, O and X - they read as 1, 0 and
 * a closed runway - then AA, AB, ... (bijective base 23). Solve/: no engine type beyond CoreMinimal.
 */
namespace TaxiwayLetters
{
	inline constexpr int32 Count = 23;

	/** The Index'th letter: 0 -> "A", 22 -> "Z", 23 -> "AA", 46 -> "BA". */
	AIRSIDE_API FString LetterAt(int32 Index);

	/** I, O or X, either case - never issued, and refused in a rename (URoadNetwork::WhyTaxiwayNameRefused). */
	AIRSIDE_API bool IsAvoided(TCHAR Character);
}
