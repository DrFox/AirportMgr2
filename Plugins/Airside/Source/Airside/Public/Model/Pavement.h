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
	 * Allowed IN SCALE ORDER (grass first), or all four when empty - the one reading of "empty
	 * means all" (URoadProfile::AllowedPavements). SORTED since 2026-09-28: every road and
	 * taxiway asset lists tarmac before grass, so its row put grass second while the runway's
	 * and the stand's put it first; the row is the scale, so a profile's authoring order must
	 * not reorder it. HERE IN Model/, not beside Pavement::AppendAxis in
	 * Tool/PavementAxis.h: URoadNetwork::SetSegmentSurface refuses by it, and Model/ may not
	 * see Tool/. The row and the refusal read one list, so a pick the row offered is never
	 * one the network then refuses.
	 * ENFORCED BY: Airside.Present.GrassRoadLaid (grass picked on the row is laid, through the
	 * actor), Airside.Model.RoutePavementGate (concrete, which the row omits, is refused)
	 */
	AIRSIDE_API TArray<EPavement> Offered(TConstArrayView<EPavement> Allowed);

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

/**
 * One pavement comparison with the two figures it was made from, so the sentence can be
 * written from the decision - FRunwayAdmission's rule for Describe.
 *
 * A VALUE OBJECT, the one piece runway and stand admission share (spec 2026-09-27 §1). A
 * Specification chain of rule objects was rejected: each refusal carries different figures
 * for its sentence, USTRUCT plans cannot hold polymorphic rules, and two facilities with six
 * rules did not pay for it. Revisit at a third facility.
 */
USTRUCT()
struct AIRSIDE_API FPavementCheck
{
	GENERATED_BODY()

	/** The ground's. */
	UPROPERTY() EPavement Have = EPavement::Tarmac;

	/** The weakest the aircraft may use. */
	UPROPERTY() EPavement Need = EPavement::Grass;

	bool Passes() const { return Have >= Need; }
};

namespace Pavement
{
	AIRSIDE_API FPavementCheck Judge(EPavement Have, EPavement Need);

	/** "the surface is grass; this aircraft needs tarmac". Empty when the check passes. */
	AIRSIDE_API FString Describe(const FPavementCheck& Check);

	/**
	 * What building or owning a thing on P costs, as a multiple of its authored rate - build AND
	 * upkeep, for every buildable. THE ONE TABLE; FBuildLine::Amount and BuildCost's upkeep are
	 * its only readers.
	 * ENFORCED BY: Check-Architecture rule 4 row 'Pavement::RateFactor'
	 * First guesses 2026-09-27: grass is levelled ground and seed, no base course; concrete
	 * and reinforced carry heavier slabs.
	 *
	 * A FACTOR ON THE PROFILE, not a second set of rates: the profile is the cross-section and
	 * is shared by grass and tarmac roads of one width (see FRoadSegment::Surface), so rates
	 * per surface would mean a rate per profile per surface authored by hand. Tune it here, in
	 * the one place both the quote and the upkeep read. (Moved from BuildCost::GrassRateFactor,
	 * #356's, when ERoadSurface folded into EPavement.)
	 * ENFORCED BY: Airside.Build.GrassRoadCost (quote and upkeep both at the factor)
	 */
	AIRSIDE_API double RateFactor(EPavement P);
}
