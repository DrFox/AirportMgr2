#pragma once

#include "CoreMinimal.h"
#include "Pavement.generated.h"

/**
 * What ground is paved with - a runway, a road or taxiway, a stand's pad. ORDERED: strength
 * as well as look, so an aircraft names the weakest pavement it may use and admission
 * compares with >=.
 *
 * Pavement strength is the scale itself rather than a separate PCN figure (spec 2026-09-07
 * §1): four steps is what a player can read off the ground, and a classification number
 * would be a second axis nothing in the game varies independently.
 *
 * ONE SCALE FOR EVERY BUILDABLE since 2026-09-27 (was ERunwaySurface; spec
 * 2026-09-27-shared-pavement). A second enum per kind is how roads came to have a surface
 * scale runway admission could not read.
 * ENFORCED BY: Check-Architecture rule 23 (one pavement enum)
 */
UENUM(BlueprintType)
enum class EPavement : uint8
{
	Grass,
	Tarmac,
	Concrete,
	Reinforced,
	/** Sentinel, never a real pavement - sizes tables and % cycling instead of retyping 4. */
	Count UMETA(Hidden),
};

/** How many pavement material slots there are - Grass/Tarmac/Concrete, Reinforced aliased in
 *  by Pavement::MaterialSlot below. The one figure UAirsideContent::RunwayMaterials (a TArray,
 *  sized at runtime) and FSurfaceSettings::RunwayMaterials (a fixed C array, sized at compile
 *  time) both size against - see the static_assert at the latter's declaration. */
inline constexpr int32 PavementMaterialSlotCount = 3;

namespace Pavement
{
	/** Lower case, for a refusal sentence or a tool label. One spelling each. */
	AIRSIDE_API const TCHAR* Name(EPavement P);

	/**
	 * Which of the THREE pavement material slots (Grass/Tarmac/Concrete) a pavement draws
	 * with. Reinforced has no slot of its own - it is concrete with a stronger rating, and
	 * the difference shows in the details panel and in what may land there, not on the
	 * ground (spec 2026-09-07 §8) - so this is the ONE place that alias happens. Everything
	 * downstream (UAirsideContent::RunwayMaterials, FSurfaceSettings::RunwayMaterials) indexes
	 * by this, never by EPavement directly.
	 */
	AIRSIDE_API int32 MaterialSlot(EPavement P);
}
