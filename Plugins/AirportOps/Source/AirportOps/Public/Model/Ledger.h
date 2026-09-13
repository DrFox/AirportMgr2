#pragma once

#include "CoreMinimal.h"
#include "Model/OpsSave.h"
#include "UObject/Object.h"

#include "Ledger.generated.h"

/**
 * What a ledger entry was for.
 *
 * ONLY CATEGORIES WITH A PUBLISHER IN THIS SLICE. Research, contract and fine categories
 * arrive with the systems that raise them - an enumerator nothing can ever produce is the
 * "list nothing consumes" bug CLAUDE.md names three times, and a finance screen written
 * against it would read the gap as a promise rather than as an absence.
 */
UENUM()
enum class ELedgerCategory : uint8
{
	LandingFee,
	ParkingFee,
	/** Fuelling, today. Every per-service fee the job board adds lands here too. */
	ServiceFee,
	Placement,
	/** An undo reversing a placement, or a demolition's scrap value. */
	Refund,
	Upkeep,
	/** RollUp's summary of everything older than MaxDays. Never posted directly. */
	BroughtForward
};

/** One movement of money. Append-only: entries are never edited, only followed by more. */
USTRUCT()
struct AIRPORTOPS_API FLedgerEntry
{
	GENERATED_BODY()

	/** USimClock::Now when it happened. GAME time - the ledger never sees wall time. */
	UPROPERTY() double At = 0.0;

	UPROPERTY() ELedgerCategory Category = ELedgerCategory::Placement;

	/**
	 * SIGNED. Income positive, spending negative, so Balance is a plain sum and no call site
	 * has to remember which categories subtract - the kind of knowledge that ends up encoded
	 * differently in two places and disagrees.
	 */
	UPROPERTY() double Amount = 0.0;

	UPROPERTY() FText What;

	/** Ids start at 1, so INDEX_NONE means "not a real charge" everywhere. */
	UPROPERTY() int32 Id = 0;

	/** The id this entry reverses, or INDEX_NONE. What stops a charge being reversed twice. */
	UPROPERTY() int32 Reverses = INDEX_NONE;
};

/**
 * The money. Append-only entries; the balance is their sum.
 *
 * THE BALANCE IS CACHED AND THE CACHE IS TESTED. Folding a long game's entries on every HUD
 * frame is waste, but a running total that can silently disagree with the entries is worse
 * than either - so FoldBalanceForTest exists and one test asserts the two agree. If they ever
 * diverge the entries win: they are the record, the total is a convenience.
 *
 * World-free, like every other AirportOps Model/ class: NewObject, Post, and no world.
 */
UCLASS()
class AIRPORTOPS_API ULedger : public UObject, public IOpsPersistent
{
	GENERATED_BODY()

public:
	// --- IOpsPersistent ---------------------------------------------------------------
	virtual FName SaveBlobName() const override { return TEXT("Ledger"); }
	virtual UObject& AsPersistentObject() override { return *this; }

	/**
	 * How many game days of entries are kept in full before RollUp folds them into one.
	 *
	 * Append-only does not mean unbounded: every entry is saved, and a long game would
	 * otherwise carry tens of thousands of rows through every write to disk.
	 */
	UPROPERTY() int32 MaxDays = 30;

	/**
	 * The balance a new game opens at, from UScenario::StartingBalance.
	 *
	 * A FIELD AND NOT AN OPENING ENTRY, because an opening entry would be rolled up like any
	 * other and RollUp would then need a special case to avoid folding away the float the
	 * whole balance rests on. Balance is this plus the fold.
	 */
	UPROPERTY() double StartingBalance = 0.0;

	/** Start a NEW GAME at this balance. Not for a load - Restore brings back the entries. */
	void Open(double InStartingBalance);

	/** Append one entry. Returns its id, for Reverse. */
	int32 Post(double At, ELedgerCategory Category, double Amount, FText What);

	/**
	 * Post the exact opposite of entry ChargeId. False if it is unknown or already reversed.
	 *
	 * REVERSES BY ID RATHER THAN BY AMOUNT, so an undo cannot put back a number that was never
	 * taken: the amount comes from the entry itself, not from a caller recomputing it from
	 * geometry that may since have changed.
	 */
	bool Reverse(double At, int32 ChargeId);

	double Balance() const { return CachedBalance; }

	const TArray<FLedgerEntry>& Entries() const { return Rows; }

	/** Fold entries older than Now minus MaxDays days into one BroughtForward entry. */
	void RollUp(double Now);

	/**
	 * The balance computed from the entries.
	 *
	 * The test's half of the cached-total invariant described in the class comment. Never used
	 * in production - if it ever is, the cache has stopped being trustworthy and the fix is
	 * the cache, not the call site.
	 */
	double FoldBalanceForTest() const;

private:
	UPROPERTY() TArray<FLedgerEntry> Rows;
	UPROPERTY() int32 NextId = 1;
	UPROPERTY() double CachedBalance = 0.0;

	void Recache();
};
