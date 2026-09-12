#include "OfferViewModels.h"

#include "Model/ArrivalPlanner.h"
#include "Model/Flight.h"
#include "Model/FlightBoard.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadNetwork.h"
#include "Model/SimClock.h"

namespace
{
	/** "in 12 min", or "now". Minutes, because the compressed clock makes seconds meaningless. */
	FText DescribeEta(double ArrivesAt, double Now)
	{
		const double Remaining = ArrivesAt - Now;
		if (Remaining <= 60.0)
		{
			return NSLOCTEXT("AirportMgr", "EtaNow", "now");
		}
		const int32 Minutes = FMath::RoundToInt(Remaining / 60.0);
		return FText::Format(NSLOCTEXT("AirportMgr", "EtaMinutes", "in {0} min"),
			FText::AsNumber(Minutes));
	}
}

void UOfferViewModel::Refresh(const UFlightBoard& Board, const UGroundTraffic& Traffic,
	const URoadNetwork& Network, const USimClock& Clock)
{
	const UFlight* Live = Flight.Get();
	if (Live == nullptr)
	{
		return;
	}

	UE_MVVM_SET_PROPERTY_VALUE(Airline, Live->AirlineName);
	UE_MVVM_SET_PROPERTY_VALUE(TypeName, Live->TypeName);
	UE_MVVM_SET_PROPERTY_VALUE(Eta, DescribeEta(Live->ArrivesAt, Clock.Now()));

	// THE REAL PLAN, with the live occupancy. The greyed-out reason is the sentence the
	// arrival itself would print, because it is the same refusal.
	const EArrivalRefusal Why = Board.WhyNotAcceptable(Traffic, Network, *Live);
	UE_MVVM_SET_PROPERTY_VALUE(bAcceptable, Why == EArrivalRefusal::None);

	FArrivalPlan Plan;
	Plan.Why = Why;
	UE_MVVM_SET_PROPERTY_VALUE(Refusal, Why == EArrivalRefusal::None
		? FText::GetEmpty()
		: FText::FromString(ArrivalPlanner::DescribeRefusal(Plan)));
}

void UOfferInboxViewModel::Refresh(UFlightBoard& InBoard, UGroundTraffic& InTraffic,
	const URoadNetwork& InNetwork, const USimClock& InClock)
{
	Board = &InBoard;
	Traffic = &InTraffic;
	Network = const_cast<URoadNetwork*>(&InNetwork);
	Clock = const_cast<USimClock*>(&InClock);

	const TArray<UFlight*> Pending = InBoard.Offers();

	// Rows are rebuilt only when the SET of offers changed; otherwise the existing rows are
	// refreshed in place. A row object replaced every tick would drop the list view's
	// selection and re-run every binding for no reason.
	bool bSameFlights = Rows.Num() == Pending.Num();
	if (bSameFlights)
	{
		for (int32 Index = 0; Index < Rows.Num(); ++Index)
		{
			if (Rows[Index] == nullptr || Rows[Index]->Flight.Get() != Pending[Index])
			{
				bSameFlights = false;
				break;
			}
		}
	}

	if (!bSameFlights)
	{
		Rows.Reset();
		Offers.Reset();
		for (UFlight* Each : Pending)
		{
			UOfferViewModel* Row = NewObject<UOfferViewModel>(this);
			Row->Flight = Each;
			Rows.Add(Row);
			Offers.Add(Row);
		}
	}

	for (TObjectPtr<UOfferViewModel>& Row : Rows)
	{
		if (Row != nullptr)
		{
			Row->Refresh(InBoard, InTraffic, InNetwork, InClock);
		}
	}

	// THROUGH THE MACRO, never a plain assignment. Assigning the member compiles, draws
	// correctly the first time, and then never updates the badge again - and nothing in a
	// binding says why.
	UE_MVVM_SET_PROPERTY_VALUE(PendingCount, Rows.Num());
}

bool UOfferInboxViewModel::Accept(UOfferViewModel* Row)
{
	UFlightBoard* LiveBoard = Board.Get();
	UGroundTraffic* LiveTraffic = Traffic.Get();
	URoadNetwork* LiveNetwork = Network.Get();
	USimClock* LiveClock = Clock.Get();
	if (Row == nullptr || LiveBoard == nullptr || LiveTraffic == nullptr
		|| LiveNetwork == nullptr || LiveClock == nullptr)
	{
		return false;
	}

	UFlight* Flight = Row->Flight.Get();
	if (Flight == nullptr)
	{
		return false;
	}

	// THE ONE DOOR. The viewmodel asks the board; it never writes a phase or a stand itself.
	const bool bAccepted = LiveBoard->Accept(*LiveTraffic, *LiveNetwork, *LiveClock, *Flight);
	Refresh(*LiveBoard, *LiveTraffic, *LiveNetwork, *LiveClock);
	return bAccepted;
}

void UOfferInboxViewModel::Decline(UOfferViewModel* Row)
{
	UFlightBoard* LiveBoard = Board.Get();
	if (Row == nullptr || LiveBoard == nullptr)
	{
		return;
	}
	if (UFlight* Flight = Row->Flight.Get())
	{
		LiveBoard->Decline(*Flight);
	}
	if (UGroundTraffic* LiveTraffic = Traffic.Get())
	{
		if (URoadNetwork* LiveNetwork = Network.Get())
		{
			if (USimClock* LiveClock = Clock.Get())
			{
				Refresh(*LiveBoard, *LiveTraffic, *LiveNetwork, *LiveClock);
			}
		}
	}
}
