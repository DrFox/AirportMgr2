#include "Model/Ledger.h"

#include "AirportOpsLog.h"
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
