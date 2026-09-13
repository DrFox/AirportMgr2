#include "Model/Ledger.h"

#include "AirportOpsLog.h"
#include "Model/Pricing.h"
#include "Model/SimClock.h"

void ULedger::Open(double InStartingBalance)
{
	StartingBalance = InStartingBalance;
	Rows.Reset();
	NextId = 1;
	Recache();
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
	CachedBalance += Amount;
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
	UE_LOG(LogAirportOps, Log, TEXT("Ledger rolled up %d entries into %.0f; balance %.0f"),
		Count, Folded, CachedBalance);
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
	return Pricing != nullptr
		? Pricing->PriceOfBuild(Quote.BaseAmount, Quote.Source.Get())
		: Quote.BaseAmount;
}

bool ULedger::CanAfford(const FBuildQuote& Quote) const
{
	const double Price = PriceOf(Quote);

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
	// ledger is never asked to remember what each segment cost.
	const double Scrap = Pricing != nullptr
		? Pricing->ScrapValue(Quote.BaseAmount, Quote.Source.Get())
		: 0.0;
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
