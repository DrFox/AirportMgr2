#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"
#include "Model/TaxiwayStrip.h"
#include "Solve/IcaoCode.h"

class URoadNetwork;

/**
 * A taxiway whose strip has grown over something operates at the largest letter whose strip is
 * clear (taxiway clearance strip spec, "When a taxiway is upgraded"): roads, depots and other
 * taxiways' pavement RESTRICT it rather than being destroyed - real taxiways carry published
 * span limits for exactly this. Stands inside the strip are NOT an obstruction here; they close
 * to new arrivals instead (StandAdmission, stage 6 plan ruling 3).
 *
 * PER SEGMENT (plan ruling 4): the letter of the tightest obstruction along that piece; a turn
 * between two pieces takes the Min of both, the guideline builder's existing turn rule.
 *
 * World-free, tested with a bare URoadNetwork, like TaxiwayStrip whose machinery it asks.
 */
namespace TaxiwayRestriction
{
	/** FRoadSegment::RestrictedLetter's "operates at its pavement's own letter". */
	inline constexpr uint8 Unrestricted = 0xFF;

	/**
	 * What restricts a taxiway - TaxiwayStrip's own kind-and-index. The plan's struct carried a
	 * Depth too; nothing reads one (the log and the inspector name the thing), and the overlap
	 * test it comes from is a yes/no, so a depth would be a second, approximate measure.
	 */
	using FObstruction = TaxiwayStrip::FSwallowed;

	/**
	 * The largest letter at or below the pavement's whose strip is clear of roads, depots and
	 * other taxiways' pavement - unset when that is the pavement's own letter (unrestricted), or
	 * for anything TaxiwayStrip::HasStrip refuses. Tries each letter down from the pavement's;
	 * the first clear one wins, and A when none is. OutWorst, when given, is what blocked the
	 * letter above the answer - the obstruction to move to lift it one step.
	 * ENFORCED BY: Airside.Model.TaxiwayRestriction
	 */
	AIRSIDE_API TOptional<EIcaoCode> RestrictionOf(const URoadNetwork& Network, FRoadSegmentId Taxiway,
		FObstruction* OutWorst = nullptr);

	/** The letter a taxiway operates at: the pavement's, lowered by its stored RestrictedLetter.
	 *  The one reader of that field for routing and the inspector. Unset for no strip. */
	AIRSIDE_API TOptional<EIcaoCode> EffectiveLetterOf(const URoadNetwork& Network, FRoadSegmentId Taxiway);

	/**
	 * Writes RestrictedLetter on every live segment (Unrestricted on anything without a strip) -
	 * THE ONE WRITER of that field. Runs in the Topology rebuild after the solve and BEFORE the
	 * guideline builder, which reads it. Logs a `Restriction:` line only when a segment's letter
	 * CHANGES (compared with the stored value), not every rebuild. Returns how many are restricted.
	 * ENFORCED BY: Airside.Model.TaxiwayRestriction
	 */
	AIRSIDE_API int32 Apply(URoadNetwork& Network);

	/** "a service road", "a fuel depot", "a taxiway", "stand 3" - the obstruction as the log,
	 *  the arrival refusal and the inspector all say it. */
	AIRSIDE_API FString Describe(const FObstruction& Obstruction);
}
