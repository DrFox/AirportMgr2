#include "LedgerViewModels.h"

#include "Model/Ledger.h"
#include "Model/Pricing.h"
#include "Model/SimClock.h"

#define LOCTEXT_NAMESPACE "Ledger"

namespace
{
	/**
	 * The player's word for a category.
	 *
	 * A SWITCH WITH NO DEFAULT, on purpose: adding an enumerator to ELedgerCategory should be
	 * a compiler warning here rather than a row that silently reads "?" in the one screen
	 * whose whole job is explaining where the money went. The research, contract and fine
	 * categories arrive in M4 and will land in this switch when they do.
	 */
	FText WordFor(ELedgerCategory Category)
	{
		switch (Category)
		{
		case ELedgerCategory::LandingFee:     return LOCTEXT("CatLanding", "Landing");
		case ELedgerCategory::ParkingFee:     return LOCTEXT("CatParking", "Parking");
		case ELedgerCategory::ServiceFee:     return LOCTEXT("CatService", "Service");
		case ELedgerCategory::Placement:      return LOCTEXT("CatPlacement", "Built");
		case ELedgerCategory::Refund:         return LOCTEXT("CatRefund", "Refund");
		case ELedgerCategory::Upkeep:         return LOCTEXT("CatUpkeep", "Upkeep");
		case ELedgerCategory::BroughtForward: return LOCTEXT("CatBrought", "Brought fwd");
		}
		return FText::GetEmpty();
	}
}

void ULedgerRowViewModel::Refresh(const FLedgerEntry& Entry, const USimClock& Clock,
	const UPricing& Pricing)
{
	// DERIVED FROM THE ENTRY'S OWN TIME, not from the clock's current one: a row describes when
	// something happened, and reading "now" here would relabel the whole panel every tick.
	const double Seconds = Entry.At;
	const int32 Day = static_cast<int32>(Seconds / USimClock::SecondsPerDay) + 1;
	const double TimeOfDay = FMath::Fmod(Seconds, USimClock::SecondsPerDay);
	const int32 Hour = static_cast<int32>(TimeOfDay / 3600.0);
	const int32 Minute = static_cast<int32>(FMath::Fmod(TimeOfDay, 3600.0) / 60.0);

	UE_MVVM_SET_PROPERTY_VALUE(When, FText::FromString(
		FString::Printf(TEXT("Day %d  %02d:%02d"), Day, Hour, Minute)));
	UE_MVVM_SET_PROPERTY_VALUE(Category, WordFor(Entry.Category));
	UE_MVVM_SET_PROPERTY_VALUE(What, Entry.What);
	UE_MVVM_SET_PROPERTY_VALUE(Amount, Pricing.Format(Entry.Amount));
	UE_MVVM_SET_PROPERTY_VALUE(bOutgoing, Entry.Amount < 0.0);
}

bool ULedgerPanelViewModel::Refresh(const ULedger& Ledger, const USimClock& Clock,
	const UPricing& Pricing)
{
	// THE BALANCE IS REFRESHED EVEN WHEN THE ROWS ARE NOT. It is one string, and the gate
	// exists to avoid rebuilding forty row objects - not to avoid setting a label.
	UE_MVVM_SET_PROPERTY_VALUE(Balance, Pricing.Format(Ledger.Balance()));
	UE_MVVM_SET_PROPERTY_VALUE(bOverdrawn, Ledger.Balance() < 0.0);

	if (Ledger.Revision() == BuiltAtRevision)
	{
		return false;
	}
	BuiltAtRevision = Ledger.Revision();

	const TArray<FLedgerEntry>& Entries = Ledger.Entries();
	const int32 Wanted = FMath::Min(FMath::Max(MaxRows, 1), Entries.Num());

	// REUSED RATHER THAN REALLOCATED. The row objects are UObjects and this runs whenever the
	// money moves; churning forty of them per post would hand the collector work for nothing.
	while (RowModels.Num() > Wanted)
	{
		RowModels.Pop();
	}
	while (RowModels.Num() < Wanted)
	{
		RowModels.Add(NewObject<ULedgerRowViewModel>(this));
	}

	// NEWEST FIRST. The entries are appended in order, so this walks back from the end - the
	// question the panel answers is "what just happened", and that is the bottom of the array.
	for (int32 Row = 0; Row < Wanted; ++Row)
	{
		RowModels[Row]->Refresh(Entries[Entries.Num() - 1 - Row], Clock, Pricing);
	}
	return true;
}

#undef LOCTEXT_NAMESPACE
