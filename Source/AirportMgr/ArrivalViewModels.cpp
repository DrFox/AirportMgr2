#include "ArrivalViewModels.h"

#include "Model/Flight.h"
#include "Model/FlightBoard.h"
#include "Model/SimClock.h"
#include "OfferViewModels.h"

FText UArrivalRowViewModel::DescribeStatus(const UFlight& Flight, double Now)
{
	switch (Flight.Phase)
	{
	case EFlightPhase::Accepted:
		return FText::Format(NSLOCTEXT("AirportMgr", "ArrivalIn", "in {0}"),
			UOfferViewModel::DescribeDuration(FMath::Max(Flight.ArrivesAt - Now, 0.0)));
	case EFlightPhase::Inbound:     return NSLOCTEXT("AirportMgr", "ArrivalHolding", "HOLDING");
	case EFlightPhase::Landing:     return NSLOCTEXT("AirportMgr", "ArrivalLanding", "LANDING");
	case EFlightPhase::TaxiIn:      return NSLOCTEXT("AirportMgr", "ArrivalTaxiIn", "TAXI IN");
	case EFlightPhase::Turnaround:  return NSLOCTEXT("AirportMgr", "ArrivalOnStand", "ON STAND");
	case EFlightPhase::Manoeuvring: return NSLOCTEXT("AirportMgr", "ArrivalManoeuvring", "MANOEUVRING");
	case EFlightPhase::TaxiOut:     return NSLOCTEXT("AirportMgr", "ArrivalTaxiOut", "TAXI OUT");
	case EFlightPhase::Departing:   return NSLOCTEXT("AirportMgr", "ArrivalDeparting", "DEPARTING");
	default:                        return FText::GetEmpty();
	}
}

FText UArrivalRowViewModel::DescribeDetail(const UFlight& Flight, double Now, bool& bOutLate)
{
	bOutLate = false;
	if (Flight.ContractSeconds <= 0.0)
	{
		return FText::GetEmpty();
	}
	const double Left = Flight.AirborneBy() - Now;
	if (Left < 0.0)
	{
		// LATE, AND BY HOW MUCH - the turnaround contract C will score this flight against.
		bOutLate = true;
		return FText::Format(NSLOCTEXT("AirportMgr", "ArrivalLate", "{0} late"),
			UOfferViewModel::DescribeDuration(-Left));
	}
	const FText Remaining = FText::Format(NSLOCTEXT("AirportMgr", "ArrivalLeft", "{0} left"),
		UOfferViewModel::DescribeDuration(Left));
	if (Flight.Phase == EFlightPhase::Inbound)
	{
		// THE WAIT, beside what it is costing: holding time comes out of the same contract.
		return FText::Format(NSLOCTEXT("AirportMgr", "ArrivalWaited", "waited {0} · {1}"),
			UOfferViewModel::DescribeDuration(FMath::Max(Now - Flight.HoldingSince, 0.0)), Remaining);
	}
	return Remaining;
}

void UArrivalRowViewModel::Refresh(const USimClock& Clock)
{
	const UFlight* Live = Flight.Get();
	if (Live == nullptr)
	{
		return;
	}
	const FString Name = FString::Printf(TEXT("%s  %s"), *Live->Callsign, *Live->TypeName.ToString()).TrimStartAndEnd();
	Title = QueuePosition > 0
		? FText::FromString(FString::Printf(TEXT("#%d %s"), QueuePosition, *Name))
		: FText::FromString(Name);
	Status = DescribeStatus(*Live, Clock.Now());
	Detail = DescribeDetail(*Live, Clock.Now(), bLate);
}

void UArrivalsViewModel::Refresh(const UFlightBoard& Board, const USimClock& Clock)
{
	// THE ROW SET is rebuilt only when the board's revision moved - an accept, a phase change, a
	// clearance - exactly as UOfferInboxViewModel gates its own; the text refreshes every call,
	// because "in 6 min" and "47 min left" count down between revisions.
	const uint32 BoardNow = Board.Revision();
	if (!bRowsValid || BoardNow != BoardRevisionAt)
	{
		Rows.Reset();
		const TArray<UFlight*> Holding = Board.Queue();
		for (int32 Index = 0; Index < Holding.Num(); ++Index)
		{
			UArrivalRowViewModel* Row = NewObject<UArrivalRowViewModel>(this);
			Row->Flight = Holding[Index];
			Row->QueuePosition = Index + 1;
			Rows.Add(Row);
		}

		TArray<UFlight*> Inbound;
		TArray<UFlight*> Ground;
		for (UFlight* Each : Board.Live())
		{
			if (Each == nullptr || Each->Phase == EFlightPhase::Inbound)
			{
				continue;
			}
			(Each->Phase == EFlightPhase::Accepted ? Inbound : Ground).Add(Each);
		}
		Algo::StableSortBy(Inbound, [](const UFlight* F) { return F->ArrivesAt; });
		for (UFlight* Each : Inbound) { UArrivalRowViewModel* Row = NewObject<UArrivalRowViewModel>(this); Row->Flight = Each; Rows.Add(Row); }
		for (UFlight* Each : Ground) { UArrivalRowViewModel* Row = NewObject<UArrivalRowViewModel>(this); Row->Flight = Each; Rows.Add(Row); }

		BoardRevisionAt = BoardNow;
		bRowsValid = true;
	}
	for (const TObjectPtr<UArrivalRowViewModel>& Row : Rows)
	{
		if (Row != nullptr)
		{
			Row->Refresh(Clock);
		}
	}
}

TArray<UArrivalRowViewModel*> UArrivalsViewModel::GetRows() const
{
	TArray<UArrivalRowViewModel*> Out;
	Out.Reserve(Rows.Num());
	for (const TObjectPtr<UArrivalRowViewModel>& Row : Rows) { Out.Add(Row); }
	return Out;
}
