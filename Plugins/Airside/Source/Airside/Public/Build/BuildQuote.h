#pragma once

#include "CoreMinimal.h"

/**
 * What one build or demolition is worth, at the AUTHORED rate.
 *
 * BASE, NOT PRICE. Airside owns the geometry and the profile, so Airside is the only layer
 * that can answer "how much of it is there"; what it COSTS is AirportOps' answer, because only
 * that side knows about research discounts, contracts and the player's own levers. The split
 * is the same one BuildCost and UPricing are named for.
 *
 * A PLAIN STRUCT, not a USTRUCT: nothing saves a quote, nothing reflects one, and no
 * Blueprint sees one. A quote lives inside a single mutator call and is gone.
 */
struct FBuildQuote
{
	double BaseAmount = 0.0;

	/**
	 * The URoadProfile or UEntityDefinition being placed - NOT a parallel enum of build kinds.
	 *
	 * A research discount aimed at taxiways keys on the asset ITSELF, so there is no second
	 * list to keep in agreement with EPlaceableEntity - the "lists that must agree" failure
	 * this codebase has shipped three times. Weak rather than raw because a quote crosses a
	 * plugin boundary to be priced, and an asset unloaded in between would leave a raw pointer
	 * pointing at nothing with no way to tell.
	 */
	TWeakObjectPtr<const UObject> Source;

	/** "Taxiway, 500 m". What the ghost prints beside the price. */
	FText What;

	/**
	 * A quote for nothing: an edit that moves no pavement.
	 *
	 * Splitting a segment, naming a runway, linking a guideline - real edits that create no new
	 * surface and destroy none. They go through the FREE door rather than being charged zero,
	 * so "this costs nothing" is a decision someone made rather than an arithmetic accident.
	 */
	bool IsFree() const { return BaseAmount <= 0.0; }
};
