#include "ArrivalViewModels.h"

#include "Model/ExhaustiveSwitch.h"
#include "Model/Flight.h"
#include "Model/FlightBoard.h"
#include "Model/GameTimeText.h"
#include "Model/SimClock.h"
#include "OfferViewModels.h"

bool UArrivalRowViewModel::IsHoldingPhase(const UFlight& Flight)
{
	// THE SAME TEST DescribeStatus WORDS "HOLDING" FROM - Inbound is the phase the arrival queue holds a flight in. One function names
	// the phase, so the row's accent and its word cannot part (the panel used to compare the word, #447).
	return Flight.GetPhase() == EFlightPhase::Inbound;
}

// EVERY PHASE BY NAME, NO default, and a missing one is a BUILD ERROR (#442 review): this had a `default: return empty`, so a new phase
// (Diverted) would have shown a blank status on the arrivals row with nothing to say so - the one per-phase table outside the board the
// predicates in Flight.h did not reach. The phases a live row never shows (an offer, anything finished) are named, and empty.
// ENFORCED BY: C4062 as an error, AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN; Check-Architecture rule 58 (a switch on EFlightPhase outside Flight.h is
// inside it and has no default); AirportMgr.Arrivals.StatusText (every phase the enum has: a live one says something, the rest are empty)
AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN
FText UArrivalRowViewModel::DescribeStatus(const UFlight& Flight, double Now)
{
	switch (Flight.GetPhase())
	{
	case EFlightPhase::Accepted:
		return FText::Format(NSLOCTEXT("AirportMgr", "ArrivalIn", "in {0}"),
			GameTimeText::Duration(FMath::Max(Flight.ArrivesAt - Now, 0.0)));
	case EFlightPhase::Inbound:     return NSLOCTEXT("AirportMgr", "ArrivalHolding", "HOLDING");   // IsHoldingPhase's phase
	case EFlightPhase::Landing:     return NSLOCTEXT("AirportMgr", "ArrivalLanding", "LANDING");
	case EFlightPhase::TaxiIn:      return NSLOCTEXT("AirportMgr", "ArrivalTaxiIn", "TAXI IN");
	case EFlightPhase::Turnaround:  return NSLOCTEXT("AirportMgr", "ArrivalOnStand", "ON STAND");
	case EFlightPhase::Manoeuvring: return NSLOCTEXT("AirportMgr", "ArrivalManoeuvring", "MANOEUVRING");
	case EFlightPhase::TaxiOut:     return NSLOCTEXT("AirportMgr", "ArrivalTaxiOut", "TAXI OUT");
	case EFlightPhase::Departing:   return NSLOCTEXT("AirportMgr", "ArrivalDeparting", "DEPARTING");
	case EFlightPhase::Offered:
	case EFlightPhase::Declined:
	case EFlightPhase::Expired:
	case EFlightPhase::Departed:
	case EFlightPhase::Cancelled:
	case EFlightPhase::Withdrawn:
		return FText::GetEmpty();
	}
	return FText::GetEmpty();
}
AIRSIDE_EXHAUSTIVE_SWITCH_END

FText UArrivalRowViewModel::DescribeDetail(const UFlight& Flight, double Now, bool& bOutLate)
{
	bOutLate = false;
	if (Flight.ContractSeconds <= 0.0)
	{
		return FText::GetEmpty();
	}
	const double Left = Flight.ContractSecondsLeft(Now);
	if (Flight.IsLate(Now))
	{
		// LATE, AND BY HOW MUCH - the turnaround contract C will score this flight against.
		bOutLate = true;
		return FText::Format(NSLOCTEXT("AirportMgr", "ArrivalLate", "{0} late"),
			GameTimeText::Duration(-Left));
	}
	const FText Remaining = FText::Format(NSLOCTEXT("AirportMgr", "ArrivalLeft", "{0} left"),
		GameTimeText::Duration(Left));
	if (Flight.GetPhase() == EFlightPhase::Inbound)
	{
		// THE WAIT, beside what it is costing: holding time comes out of the same contract.
		return FText::Format(NSLOCTEXT("AirportMgr", "ArrivalWaited", "waited {0} · {1}"),
			GameTimeText::Duration(FMath::Max(Now - Flight.HoldingSince, 0.0)), Remaining);
	}
	return Remaining;
}

FText UArrivalRowViewModel::DescribeTurnaround(const UFlight& Flight, double Now)
{
	if (Flight.ContractSeconds <= 0.0)
	{
		return FText::GetEmpty();
	}
	// THE SAME DURATION WORDS AND THE SAME LEFT/LATE RULE as the ARRIVALS row (DescribeDetail),
	// so the card and the list cannot tell the player two different things.
	const double Left = Flight.ContractSecondsLeft(Now);
	const FText Remaining = Flight.IsLate(Now)
		? FText::Format(NSLOCTEXT("AirportMgr", "ArrivalLate", "{0} late"), GameTimeText::Duration(-Left))
		: FText::Format(NSLOCTEXT("AirportMgr", "ArrivalLeft", "{0} left"), GameTimeText::Duration(Left));
	return FText::Format(NSLOCTEXT("AirportMgr", "CardTurnaround", "Turnaround {0} \u00B7 {1}"),
		GameTimeText::Duration(Flight.ContractSeconds), Remaining);
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
	bHolding = IsHoldingPhase(*Live);
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
			if (Each == nullptr || Each->GetPhase() == EFlightPhase::Inbound)
			{
				continue;
			}
			// AN ACCEPTED FLIGHT IS COMING; ANYTHING ELSE LIVE AND NOT HOLDING IS ON THE FIELD (IsOnGround, #442: the two
			// were an equality and its complement, each a place a new phase would have landed in the wrong list).
			(Each->IsOnGround() ? Ground : Inbound).Add(Each);
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
