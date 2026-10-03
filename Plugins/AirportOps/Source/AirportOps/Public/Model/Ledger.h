#pragma once

#include "CoreMinimal.h"
#include "Model/OpsSave.h"
#include "Model/BuildPurse.h"
#include "UObject/Object.h"

#include "Ledger.generated.h"

class UPricing;
class USimClock;

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
	BroughtForward,
	/** A vehicle bought (negative) or sold (positive) - UFacilityPurchases. Appended, not inserted. */
	Fleet,
	/** Fuel bought - contract days, spot orders and contract cancellations (spec 2026-10-02 §7). Appended. */
	FuelPurchase
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

/** One described upkeep entry. A plain struct: it lives for one PostDailyUpkeep call. */
struct FUpkeepLine
{
	/** Positive: what the day costs. <= 0 posts nothing. */
	double Amount = 0.0;
	FText What;
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
class AIRPORTOPS_API ULedger : public UObject, public IOpsPersistent, public IBuildPurse
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

	// --- IBuildPurse ------------------------------------------------------------------
	//
	// IMPLEMENTED DIRECTLY, with no adapter class, exactly as USimClock implements
	// IOpsPersistent. The ledger IS the purse: it knows the balance, it knows the prices
	// through UPricing, and it is the thing that has to record the movement anyway.

	virtual bool CanAfford(const FBuildQuote& Quote) const override;
	virtual int32 Charge(const FBuildQuote& Quote) override;
	virtual void Reverse(int32 ChargeId) override;
	virtual void Credit(const FBuildQuote& Quote) override;
	virtual FText Describe(const FBuildQuote& Quote) const override;

	/**
	 * The ONE affordability rule, for a price already known: free is always allowed, else Price <= Balance.
	 * CanAfford prices a quote and asks this; UFacilityPurchases asks it directly for a catalogue price -
	 * so a shed and a taxiway are refused under water by the same line.
	 */
	bool CanPay(double Price) const;

	/**
	 * What things cost, and what dates an entry. Both set by the ops runtime (see below).
	 *
	 * THE CLOCK IS NOT OPTIONAL FOR A PURSE. IBuildPurse hands no time down - Airside has no
	 * notion of game time - so a ledger that could not date its own entries would write every
	 * build at time zero, and the roll-up and the determinism test would both quietly stop
	 * meaning anything. Null is tolerated (a test that only checks arithmetic) and dates to 0.
	 *
	 * SET BY UOpsRuntime's CONSTRUCTOR, AND TRANSIENT (#425): wiring, not state. Saved, each was a path to the runtime's
	 * subobject, which a later session's load resolved to null - so every entry after it dated to 0.
	 */
	UPROPERTY(Transient) TObjectPtr<UPricing> Pricing = nullptr;
	UPROPERTY(Transient) TObjectPtr<USimClock> Clock = nullptr;

	/** Where Post announces money moving (ops alerts spec 2026-09-29 §2). Set by UOpsRuntime::Attach;
	 *  null in a bare NewObject, and Post checks. Raw: the runtime owns both. */
	class FOpsEventBus* Bus = nullptr;

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

	/** The cached fold of the entries - see IBuildPurse::Balance, which this is: the figure CanAfford
	 *  compares against, and what a cache of its answers keys on. */
	virtual double Balance() const override { return CachedBalance; }

	/**
	 * Below zero: the state that locks every paid placement (CanAfford) and raises the Overdrawn alert. THE ONE DEFINITION of "overdrawn"
	 * (#447) - the bar's red balance, the ledger panel's flag and the alert each wrote `Balance() < 0.0` themselves, so a change to what
	 * overdrawn means (an overdraft limit, a grace) would have had to find every copy and left the lock and the colour disagreeing.
	 * ENFORCED BY: Check-Architecture rule 4's 'overdrawn is the ledger's' row (no other file compares the balance to zero),
	 * AirportOps.Model.Ledger.IsOverdrawnIsBelowZero
	 */
	bool IsOverdrawn() const { return CachedBalance < 0.0; }

	/**
	 * Bumped by every Post and every RollUp. A view rebuilds only when this changes.
	 *
	 * A COUNTER AND NOT A DELEGATE. The HUD polls - UBuildBarWidget::RefreshClock already
	 * does, every tick - and a panel that re-derived two hundred rows per tick to discover
	 * nothing had happened would be the expensive kind of correct. The same idiom
	 * URoadNetwork::GetGuidelineRevision uses, and for the same reason: the cheapest question
	 * a poller can ask is "has anything changed since the number I remember".
	 *
	 * AND BY EVERY LOAD (Serialize, issue #426): a load restores the rows and the balance with no Post, so the bar and
	 * the ledger panel - both gated on this - showed the pre-load money until the next fee.
	 */
	int32 Revision() const { return RevisionCount; }

	/**
	 * A RESTORE IS A CHANGE: the tagged pass, then RevisionCount bumped on a load - UJobBoard::Serialize's idiom, and
	 * for its reason: OpsSave::DeserializeObject restores this object without any other hook a view could notice.
	 * ENFORCED BY: AirportMgr.UI.LedgerPanelGate (straight through OpsSave::DeserializeObject, so no OnBeforeRestore
	 * bump can stand in for this one), Check-Architecture rule 41 (persistent-revision-bumps-on-load)
	 */
	virtual void Serialize(FArchive& Ar) override;

	/**
	 * BEFORE ANY RESTORE, blob or none (#426, UFlightBoard::OnBeforeRestore's shape): the rows go and the balance is
	 * re-folded from StartingBalance - this session's opening money - so a snapshot with NO "Ledger" blob (from before
	 * the ledger) does not keep the replaced session's money. With a blob, Serialize overwrites all of it next.
	 * ENFORCED BY: AirportOps.Model.Save.NoLedgerBlobResetsTheMoney
	 */
	virtual void OnBeforeRestore() override;

	const TArray<FLedgerEntry>& Entries() const { return Rows; }

	/** Fold entries older than Now minus MaxDays days into one BroughtForward entry. */
	void RollUp(double Now);

	/**
	 * One day's upkeep for everything standing, as DESCRIBED lines - the airport's base upkeep,
	 * "Facility upkeep", "Fleet upkeep" (facility-upgrades spec §3) - one Upkeep entry per
	 * positive line, plus this ledger's own RollUp on the same beat.
	 *
	 * MOVED FROM UOpsRuntime (issue #191): computing the figure needs BuildCost::DailyUpkeep,
	 * which lives in Build/ and which Model/ may not include (Check-Architecture rule 1), so
	 * the runtime still resolves each line's amount itself - this is just where POSTING it
	 * belongs, the same split UOpsRuntime::AirlineOffersFromCatalog draws for Entities/.
	 *
	 * A line <= 0 skips its CHARGE - an airport with nothing standing on it costs nothing to
	 * own, and a zero entry every day would be noise in the one place the player goes to find
	 * out where the money went - but RollUp always runs. The previous shape (still in
	 * UOpsRuntime before this move) skipped RollUp too whenever the base was zero, which PR #213's
	 * own "Not done" flagged and nobody then decided on: a quiet day has nothing to do with
	 * whether entries older than MaxDays are due to be folded, and skipping it only meant the
	 * ledger stayed unbounded for exactly the games with the least happening in them.
	 *
	 * ONE OVERLOAD, since #462 (M18): a scalar (Base, Now) one forwarded here with a single line, and no
	 * production code called it - UOpsRuntime::PostDailyUpkeep hands the lines - so it was a second door
	 * only its own test used.
	 * ENFORCED BY: AirportOps.Model.LedgerUpkeepLines (the skip-if-zero rule and the unconditional RollUp)
	 */
	void PostDailyUpkeep(TConstArrayView<FUpkeepLine> Lines, double Now);

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

	/** See Revision(). Not saved: a view's idea of "since when" is a session's, and a restored
	 *  counter that happened to match would leave a panel showing the previous game's rows. */
	int32 RevisionCount = 0;

	void Recache();

	/** Clock.Now(), or zero when there is no clock. See the Clock member. */
	double NowOrZero() const;

	/** The quote's base amount run through UPricing, or the base amount when there is none. */
	double PriceOf(const FBuildQuote& Quote) const;
};
