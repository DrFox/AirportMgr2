#pragma once

#include "CoreMinimal.h"
#include "Model/Pavement.h"

/** What a build line's Quantity counts: metres of profile, square metres of ground, or one thing. */
enum class EBuildUnit : uint8
{
	Metre,
	SquareMetre,
	Each,
};

/**
 * One priced quantity: so many metres of a profile, square metres of ground, or one placed
 * thing, at its authored rate, on its pavement.
 *
 * WHY LINES (spec 2026-09-27 §4): a pavement factor applied per buildable kind is a factor
 * that the next kind forgets. Every buildable is lines; Amount is the ONE place the factor
 * meets a rate, so a grass road, runway and stand pad are cheaper with no kind-specific code.
 * ENFORCED BY: Check-Architecture rule 4 row 'Pavement::RateFactor'
 */
struct FBuildLine
{
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

	/** What Quantity counts - see EBuildUnit. */
	EBuildUnit Unit = EBuildUnit::Each;

	/** How much of Unit this line is: metres, square metres, or 1.0 for a single placed thing. */
	double Quantity = 0.0;

	/** The authored rate per Unit, before any pavement factor. */
	double RatePerUnit = 0.0;

	/** Set when this line lays pavement; unset for a placed thing with no ground of its own. */
	TOptional<EPavement> Pavement;

	/**
	 * Quantity * RatePerUnit, at Pavement's rate factor if this line carries one - the ONE
	 * place a pavement factor meets a rate, so a grass line and a tarmac line differ only in
	 * Pavement, never in code.
	 *
	 * CLAMPED AT ZERO. A NEGATIVE CHARGE PAYS THE PLAYER TO BUILD - BuildCost::ForApron's
	 * signed shoelace sum is negative for one of the two windings a real outline can be drawn
	 * in, and any other quantity that ever comes back negative would print money the same way.
	 * The clamp lives here once, so every kind is covered without each caller remembering its
	 * own guard.
	 */
	AIRSIDE_API double Amount() const;
};

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
	/** Every priced part of this build - a stand is its equipment plus its pad, as two lines. */
	TArray<FBuildLine> Lines;

	/** "Taxiway, 500 m". What the ghost prints beside the price. */
	FText What;

	/** The sum of every line's Amount() - see FBuildLine. */
	AIRSIDE_API double BaseAmount() const;

	/**
	 * A quote for nothing: an edit that moves no pavement.
	 *
	 * Splitting a segment, naming a runway, linking a guideline - real edits that create no new
	 * surface and destroy none. They go through the FREE door rather than being charged zero,
	 * so "this costs nothing" is a decision someone made rather than an arithmetic accident.
	 */
	bool IsFree() const { return BaseAmount() <= 0.0; }
};

/**
 * Why a build was refused AT COMMIT, for URoadEditFacade::OnRefused. One value today: every other
 * refusal is explained before the click by the tool's readout and preview colour (the Why* evaluators);
 * "cannot afford" is the one a player could only learn from the log (ops alerts spec 2026-09-29 §2).
 * An enum rather than a bool so the next silent refusal has somewhere to go.
 */
enum class EBuildRefusal : uint8
{
	CannotAfford
};

/**
 * Where the money for a build comes from, as Airside sees it.
 *
 * A PLAIN ABSTRACT CLASS, not a UINTERFACE, for the same reason IRoadEditTarget is one: nothing
 * in Blueprint needs to see this seam. ULedger implements it directly - there is no adapter
 * class - exactly as USimClock implements IOpsPersistent.
 *
 * WHY NOT A TFunction, which is what UFlightBoard::Dispatcher is. That one is a single call in
 * a single direction, and its own comment argues against an interface for one call site. This
 * is six operations that must ALL be bound together: six independently-bindable TFunction
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

	/**
	 * The funds CanAfford compares a quote against - the one figure that decides it, exposed so a
	 * cache of anything CanAfford influenced can KEY on it (ARoadBuildController's readout cache
	 * does; issue #439). The balance moves on its own - landing fees, upkeep, a load - through no
	 * edit of the model, so nothing else a cache could watch sees it, and a stamp that only some
	 * mutations bump (ULedger::Revision, which a load leaves alone - issue #426) would miss the rest.
	 *
	 * A VALUE, NOT A COUNTER: for one quote, equal balances give equal CanAfford answers whichever
	 * route they were reached by, and a restored save that lands on a different balance differs by
	 * construction. THAT HOLDS ONLY WHILE THE PRICE OF A QUOTE DEPENDS ON THE QUOTE ALONE, which is
	 * true today because UPricing::PriceOfBuild is the identity; the M4 modifier seam (research,
	 * contracts) will end it, and the day it does this key must grow to name what the price also
	 * reads. PURE, so an implementer answers it rather than inheriting a constant that would freeze
	 * every cache keyed on it.
	 * ENFORCED BY: AirportMgr.Actions.ReadoutCacheSeesThePurse (the key sees the balance),
	 * AirportOps.Model.PriceOfBuildIsTheIdentityWhileBalanceKeysCaches (the tripwire on the price)
	 */
	virtual double Balance() const = 0;

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
