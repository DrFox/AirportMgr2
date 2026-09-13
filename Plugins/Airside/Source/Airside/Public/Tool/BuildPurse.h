#pragma once

#include "CoreMinimal.h"
#include "Build/BuildQuote.h"

/**
 * Where the money for a build comes from, as Airside sees it.
 *
 * A PLAIN ABSTRACT CLASS, not a UINTERFACE, for the same reason IRoadEditTarget is one: nothing
 * in Blueprint needs to see this seam. ULedger implements it directly - there is no adapter
 * class - exactly as USimClock implements IOpsPersistent.
 *
 * WHY NOT A TFunction, which is what UFlightBoard::Dispatcher is. That one is a single call in
 * a single direction, and its own comment argues against an interface for one call site. This
 * is five operations that must ALL be bound together: five independently-bindable TFunction
 * members would let a build charge while undo silently stopped refunding, and nothing anywhere
 * would say so.
 *
 * A NULL PURSE MEANS FREE. URoadBuildEdMode, every existing tool test and a bare PIE session
 * build at no cost and needed no change when this arrived - and one test asserts exactly that,
 * because a default that silently began charging would break design-time building.
 */
class AIRSIDE_API IBuildPurse
{
public:
	virtual ~IBuildPurse() = default;

	/** Can this be paid for right now? No side effect - the ghost asks it every frame. */
	virtual bool CanAfford(const FBuildQuote& Quote) const = 0;

	/** Take the money. Returns an id to reverse it by, or INDEX_NONE if nothing was charged. */
	virtual int32 Charge(const FBuildQuote& Quote) = 0;

	/**
	 * Undo: put back exactly what charge Id took.
	 *
	 * BY ID, because undo reverses the transaction that happened rather than re-pricing the
	 * geometry - see Credit for the other half of that decision.
	 */
	virtual void Reverse(int32 ChargeId) = 0;

	/**
	 * Demolish: a NEW transaction valuing this geometry at today's price.
	 *
	 * TAKES NO FRACTION AND NO ID. Airside says what was torn out; the purse decides what that
	 * is worth back. Quoting fresh rather than remembering what each segment cost is what keeps
	 * a BuiltFor field out of FRoadSegment and out of every save, and it makes scrap value
	 * track current prices rather than what was paid an hour ago.
	 */
	virtual void Credit(const FBuildQuote& Quote) = 0;

	/** The quote as money, for the ghost's label - so no currency symbol ever enters Airside. */
	virtual FText Describe(const FBuildQuote& Quote) const = 0;
};
