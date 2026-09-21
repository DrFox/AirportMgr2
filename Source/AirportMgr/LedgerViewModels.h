#pragma once

#include "CoreMinimal.h"

#include "LedgerViewModels.generated.h"

struct FLedgerEntry;
class ULedger;
class UPricing;
class USimClock;
enum class ELedgerCategory : uint8;

/**
 * One row of the ledger panel.
 *
 * A VIEWMODEL, NOT THE MODEL, for the reason UOfferViewModel's header gives: FLedgerEntry
 * lives in AirportOps Model/ and must not learn about the UI, or it could not be tested
 * without one. This flattens an entry into the four strings a row draws.
 *
 * PLAIN UObject, NOT UMVVMViewModelBase (issue #191 dropped the base) - see
 * UOfferViewModel's header for why: nothing ever bound a field on either class.
 *
 * THE AMOUNT IS ALREADY FORMATTED, currency symbol and all, because UPricing::Format is the
 * one place money becomes text - a row doing its own formatting would be a second answer to
 * what a number looks like, and the two would drift the first time the symbol changed.
 */
UCLASS(BlueprintType)
class AIRPORTMGR_API ULedgerRowViewModel : public UObject
{
	GENERATED_BODY()

public:
	/** Fill every field from one entry. Takes the clock to turn game seconds into a day and
	 *  a time, and the pricing to format the amount. */
	void Refresh(const FLedgerEntry& Entry, const USimClock& Clock, const UPricing& Pricing);

	FText GetWhen() const { return When; }
	FText GetCategory() const { return Category; }
	FText GetWhat() const { return What; }
	FText GetAmount() const { return Amount; }
	bool IsOutgoing() const { return bOutgoing; }

private:
	/** "Day 3  14:20". The player has no feel for a game-second count - the same choice
	 *  UOfferViewModel::Eta makes. */
	UPROPERTY(BlueprintReadOnly, Transient, Getter = "GetWhen",
		Category = "Ledger", meta = (AllowPrivateAccess))
	FText When;

	UPROPERTY(BlueprintReadOnly, Transient, Getter = "GetCategory",
		Category = "Ledger", meta = (AllowPrivateAccess))
	FText Category;

	UPROPERTY(BlueprintReadOnly, Transient, Getter = "GetWhat",
		Category = "Ledger", meta = (AllowPrivateAccess))
	FText What;

	UPROPERTY(BlueprintReadOnly, Transient, Getter = "GetAmount",
		Category = "Ledger", meta = (AllowPrivateAccess))
	FText Amount;

	/**
	 * Whether this took money rather than earned it.
	 *
	 * A BOOL RATHER THAN THE ROW READING THE SIGN OFF THE TEXT. The amount is a formatted
	 * string by the time a row sees it, and parsing a minus back out of it to pick a colour
	 * is how a localised minus sign becomes a green outgoing.
	 */
	UPROPERTY(BlueprintReadOnly, Transient, Getter = "IsOutgoing",
		Category = "Ledger", meta = (AllowPrivateAccess))
	bool bOutgoing = false;
};

/**
 * The ledger panel's rows, newest first, and the balance above them.
 *
 * GATED ON ULedger::Revision. The panel polls on tick like every other HUD surface here, and
 * re-deriving the rows each time to discover nothing had happened would be the expensive kind
 * of correct. Refresh returns whether it actually rebuilt, so a test can assert the gate
 * rather than trusting it.
 *
 * CAPPED AT MaxRows. The code-built row path is a VerticalBox with no virtualisation - see
 * UOfferInboxWidget's header on why a UListView cannot be built usefully in code here - so
 * the panel shows the most recent few dozen movements rather than every one a long game
 * holds. That is also what the panel is FOR: "where did my money just go".
 *
 * PLAIN UObject, NOT UMVVMViewModelBase (issue #191 dropped the base) - see
 * UOfferViewModel's header for why.
 */
UCLASS(BlueprintType)
class AIRPORTMGR_API ULedgerPanelViewModel : public UObject
{
	GENERATED_BODY()

public:
	/** How many of the most recent entries are shown. See the class comment. */
	UPROPERTY(EditAnywhere, Category = "Ledger", meta = (ClampMin = "1")) int32 MaxRows = 40;

	/**
	 * Re-read the ledger if it has moved. Returns true when the rows were actually rebuilt.
	 *
	 * The return is the seam the gate is tested through: a test ticks twice with nothing
	 * posted in between and asserts the second call did no work.
	 */
	bool Refresh(const ULedger& Ledger, const USimClock& Clock, const UPricing& Pricing);

	const TArray<TObjectPtr<ULedgerRowViewModel>>& Rows() const { return RowModels; }

	FText GetBalance() const { return Balance; }
	bool IsOverdrawn() const { return bOverdrawn; }

private:
	UPROPERTY(Transient) TArray<TObjectPtr<ULedgerRowViewModel>> RowModels;

	UPROPERTY(BlueprintReadOnly, Transient, Getter = "GetBalance",
		Category = "Ledger", meta = (AllowPrivateAccess))
	FText Balance;

	UPROPERTY(BlueprintReadOnly, Transient, Getter = "IsOverdrawn",
		Category = "Ledger", meta = (AllowPrivateAccess))
	bool bOverdrawn = false;

	/** The ledger revision these rows were built from. INDEX_NONE until the first build, so
	 *  an untouched ledger at revision 0 still paints once. */
	int32 BuiltAtRevision = INDEX_NONE;
};
