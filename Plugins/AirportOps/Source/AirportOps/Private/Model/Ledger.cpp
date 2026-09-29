#include "Model/Ledger.h"
#include "Model/OpsEventBus.h"

#include "AirportOpsLog.h"
#include "Model/Pricing.h"
#include "Model/SimClock.h"

void ULedger::Open(double InStartingBalance)
{
	StartingBalance = InStartingBalance;
	Rows.Reset();
	NextId = 1;
	Recache();
	++RevisionCount;
	UE_LOG(LogAirportOps, Log, TEXT("Ledger opened at %.0f"), StartingBalance);
}

int32 ULedger::Post(double At, ELedgerCategory Category, double Amount, FText What)
{
	FLedgerEntry& Entry = Rows.AddDefaulted_GetRef();
	Entry.At = At;
	Entry.Category = Category;
	Entry.Amount = Amount;
	Entry.What = MoveTemp(What);
	Entry.Id = NextId++;

	// Added rather than recomputed: Post is the hot path, and the fold that would verify it is
	// what FoldBalanceForTest and its test are for.
	const double Before = CachedBalance;
	CachedBalance += Amount;
	++RevisionCount;

	// ANNOUNCED HERE, THE ONE FUNNEL FOR PLAY: every fee, charge, credit, reversal and upkeep comes through
	// Post. Three changes of the balance do NOT, on purpose, and each is covered elsewhere: Open (a new game
	// - the attach's first network event dirties the alerts pass), OpsSave's restore (Recache - a load runs
	// every pass via MarkAllDirty), and RollUp (the fold leaves the balance unchanged).
	// ENFORCED BY: AirportOps.Model.Money.PostIsAnnounced, AirportOps.Model.Money.CrossingZeroIsAnnouncedOnce
	if (Bus != nullptr)
	{
		Bus->Publish(FMoneyPostedEvent{ Entry.Id, Category, Amount, CachedBalance });
		if ((Before < 0.0) != (CachedBalance < 0.0))
		{
			Bus->Publish(FBalanceSignChangedEvent{ CachedBalance < 0.0 });
		}
	}
	return Entry.Id;
}

bool ULedger::Reverse(double At, int32 ChargeId)
{
	if (ChargeId == INDEX_NONE)
	{
		return false;
	}

	const FLedgerEntry* Charge = Rows.FindByPredicate(
		[ChargeId](const FLedgerEntry& Row) { return Row.Id == ChargeId; });
	if (Charge == nullptr)
	{
		return false;
	}

	// ALREADY REVERSED is a refusal, not a second reversal. Redo pushes a FRESH charge with a
	// fresh id, so a second reversal of the same id can only be a double-undo bug - and paying
	// the player twice for it would stay invisible until the balance was inexplicable.
	const bool bAlready = Rows.ContainsByPredicate(
		[ChargeId](const FLedgerEntry& Row) { return Row.Reverses == ChargeId; });
	if (bAlready)
	{
		return false;
	}

	// COPIED BEFORE Post, which appends to Rows and may reallocate it - Charge would dangle.
	const double Amount = -Charge->Amount;
	FText What = Charge->What;

	const int32 Id = Post(At, ELedgerCategory::Refund, Amount, MoveTemp(What));
	Rows.Last().Reverses = ChargeId;
	UE_LOG(LogAirportOps, Log, TEXT("Ledger reversed charge %d (%.0f) as entry %d"),
		ChargeId, Amount, Id);
	return true;
}

void ULedger::RollUp(double Now)
{
	const double Cutoff = Now - (MaxDays * USimClock::SecondsPerDay);

	double Folded = 0.0;
	int32 Count = 0;
	for (const FLedgerEntry& Row : Rows)
	{
		if (Row.At < Cutoff)
		{
			Folded += Row.Amount;
			++Count;
		}
	}
	if (Count == 0)
	{
		return;
	}

	Rows.RemoveAll([Cutoff](const FLedgerEntry& Row) { return Row.At < Cutoff; });

	FLedgerEntry Summary;
	Summary.At = Cutoff;
	Summary.Category = ELedgerCategory::BroughtForward;
	Summary.Amount = Folded;
	Summary.What = NSLOCTEXT("Ledger", "BroughtForward", "Brought forward");
	Summary.Id = NextId++;
	Rows.Insert(Summary, 0);

	// The balance must not move: this summarises history, it does not rewrite it. Recomputed
	// rather than reasoned about, because the arithmetic that "obviously" cancels is exactly
	// where a rounding or an off-by-one would hide.
	Recache();
	++RevisionCount;
	UE_LOG(LogAirportOps, Log, TEXT("Ledger rolled up %d entries into %.0f; balance %.0f"),
		Count, Folded, CachedBalance);
}

void ULedger::PostDailyUpkeep(double Base, double Now)
{
	if (Base > 0.0)
	{
		Post(Now, ELedgerCategory::Upkeep, -Base, NSLOCTEXT("Ledger", "DailyUpkeep", "Upkeep"));
	}

	// UNCONDITIONAL - see this method's own header comment for why a Base of zero used to
	// (wrongly) skip this too.
	RollUp(Now);
}

double ULedger::FoldBalanceForTest() const
{
	double Sum = StartingBalance;
	for (const FLedgerEntry& Row : Rows)
	{
		Sum += Row.Amount;
	}
	return Sum;
}

void ULedger::Recache()
{
	CachedBalance = FoldBalanceForTest();
}

// --- IBuildPurse --------------------------------------------------------------------------
//
// Airside asks these five; none of them is reached from AirportOps itself. See IBuildPurse.

double ULedger::NowOrZero() const
{
	// A LEDGER THAT CANNOT DATE ITS OWN ENTRIES IS WORSE THAN ONE THAT REFUSES, but refusing
	// would mean a test that only checks arithmetic could not use the purse at all. Zero is
	// honest and visible: every entry piled at time zero is obvious the moment anyone looks.
	return Clock != nullptr ? Clock->Now() : 0.0;
}

double ULedger::PriceOf(const FBuildQuote& Quote) const
{
	// PER LINE, so a discount keyed on one line's source never discounts the ground beside
	// it - see FBuildLine. UPricing is unchanged: it still prices an amount for a source.
	double Price = 0.0;
	for (const FBuildLine& Line : Quote.Lines)
	{
		Price += Pricing != nullptr ? Pricing->PriceOfBuild(Line.Amount(), Line.Source.Get()) : Line.Amount();
	}
	return Price;
}

bool ULedger::CanAfford(const FBuildQuote& Quote) const
{
	return CanPay(PriceOf(Quote));
}

bool ULedger::CanPay(double Price) const
{
	// A FREE EDIT IS ALWAYS ALLOWED, even under water, and the check has to come first: with a
	// balance of -1000, "0 <= -1000" is false, so a plain comparison would refuse to split a
	// segment or name a runway for a player who cannot pay their upkeep. Locking PLACEMENT is
	// not locking the editor, and a refusal with nothing to buy would be unexplainable on
	// screen.
	if (Price <= 0.0)
	{
		return true;
	}

	// THE GDD'S "A NEGATIVE BALANCE LOCKS PLACEMENT" FALLS OUT OF THIS, rather than being a
	// second rule somewhere that could disagree with it: nothing that costs anything can be
	// afforded while the balance is under water. And because a build is never authorised past
	// the balance, building can never PUT the player under - only upkeep can.
	return Price <= Balance();
}

int32 ULedger::Charge(const FBuildQuote& Quote)
{
	const double Price = PriceOf(Quote);
	if (Price <= 0.0)
	{
		return INDEX_NONE;
	}

	const int32 Id = Post(NowOrZero(), ELedgerCategory::Placement, -Price, Quote.What);
	UE_LOG(LogAirportOps, Log, TEXT("Built %s for %.0f; balance %.0f"),
		*Quote.What.ToString(), Price, Balance());
	return Id;
}

void ULedger::Reverse(int32 ChargeId)
{
	Reverse(NowOrZero(), ChargeId);
}

void ULedger::Credit(const FBuildQuote& Quote)
{
	// SCRAP VALUE AT TODAY'S PRICE, not what was paid - see IBuildPurse::Credit for why the
	// ledger is never asked to remember what each segment cost. PER LINE, same reason as
	// PriceOf: the pad's line (null source) is scrapped too, or a demolished stand would
	// refund only its equipment.
	double Scrap = 0.0;
	for (const FBuildLine& Line : Quote.Lines)
	{
		Scrap += Pricing != nullptr ? Pricing->ScrapValue(Line.Amount(), Line.Source.Get()) : 0.0;
	}
	if (Scrap <= 0.0)
	{
		return;
	}

	Post(NowOrZero(), ELedgerCategory::Refund, Scrap, Quote.What);
	UE_LOG(LogAirportOps, Log, TEXT("Demolished %s for %.0f; balance %.0f"),
		*Quote.What.ToString(), Scrap, Balance());
}

FText ULedger::Describe(const FBuildQuote& Quote) const
{
	const double Price = PriceOf(Quote);
	if (Pricing == nullptr)
	{
		return FText::AsNumber(Price);
	}
	return FText::Format(NSLOCTEXT("Ledger", "QuoteAndPrice", "{0}  {1}"),
		Quote.What, Pricing->Format(Price));
}
