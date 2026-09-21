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

	Airline = Live->AirlineName;
	TypeName = Live->TypeName;
	Eta = DescribeEta(Live->ArrivesAt, Clock.Now());

	// THE REAL PLAN IS EXPENSIVE (issue #169): a full ArrivalPlanner::Plan is a route search
	// over every stand, then every runway exit, so it is only re-run when one of the three
	// things it could possibly depend on has moved since the last time - see this class's own
	// comment on the three revisions below. Everything else on this row (Eta above) stays
	// O(1) and keeps updating every call regardless.
	const uint32 BoardNow = Board.Revision();
	const uint32 GuidelineNow = Network.GetGuidelineRevision();
	const uint32 OccupancyNow = Traffic.OccupancyRevision();
	if (!bWhyComputed || BoardNow != BoardRevisionAt || GuidelineNow != GuidelineRevisionAt
		|| OccupancyNow != OccupancyRevisionAt)
	{
		// THE REAL PLAN, with the live occupancy. The greyed-out reason is the sentence the
		// arrival itself would print, because it is the same refusal.
		const EArrivalRefusal Why = Board.WhyNotAcceptable(Traffic, Network, *Live);
		bAcceptable = Why == EArrivalRefusal::None;

		// THE REASON-ONLY OVERLOAD, not a plan built by hand just to carry Why - ToastStackWidget
		// already reads it this way, and a plan with every other field default-constructed is not
		// a plan, it is Why wearing a bigger struct.
		Refusal = Why == EArrivalRefusal::None
			? FText::GetEmpty()
			: FText::FromString(ArrivalPlanner::DescribeRefusal(Why));

		BoardRevisionAt = BoardNow;
		GuidelineRevisionAt = GuidelineNow;
		OccupancyRevisionAt = OccupancyNow;
		bWhyComputed = true;
	}
}

void UOfferInboxViewModel::Refresh(UFlightBoard& InBoard, UGroundTraffic& InTraffic,
	const URoadNetwork& InNetwork, const USimClock& InClock)
{
	Board = &InBoard;
	Traffic = &InTraffic;
	Network = const_cast<URoadNetwork*>(&InNetwork);
	Clock = const_cast<USimClock*>(&InClock);

	// THE ROW SET is rebuilt only when the BOARD's own revision has moved - added, accepted,
	// declined or expired (issue #169). InBoard.Offers() allocates a fresh TArray on every
	// call, and the diff against Rows below was the same cost again; gating both on
	// Board.Revision() means a quiet inbox does neither, every frame, for as long as nothing
	// has happened - which is most frames a player is simply looking at the panel.
	const uint32 BoardNow = InBoard.Revision();
	if (!bRowsValid || BoardNow != BoardRevisionAt)
	{
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
			for (UFlight* Each : Pending)
			{
				UOfferViewModel* Row = NewObject<UOfferViewModel>(this);
				Row->Flight = Each;
				Rows.Add(Row);
			}
		}

		BoardRevisionAt = BoardNow;
		bRowsValid = true;
	}

	for (TObjectPtr<UOfferViewModel>& Row : Rows)
	{
		if (Row != nullptr)
		{
			Row->Refresh(InBoard, InTraffic, InNetwork, InClock);
		}
	}

	// A PLAIN ASSIGNMENT (issue #191 dropped UE_MVVM_SET_PROPERTY_VALUE here): the count used
	// to be set through the macro so a Blueprint binding would hear about it, but nothing
	// ever bound this field - OfferInboxWidget::PaintRows reads GetPendingCount() directly
	// every refresh, which is what actually keeps the badge current.
	PendingCount = Rows.Num();
}

TArray<UOfferViewModel*> UOfferInboxViewModel::GetOffers() const
{
	// BUILT HERE, NOT STORED: see the header comment on why this used to be a second member
	// (Offers) kept in step with Rows by hand, and why that was the "two lists" bug.
	TArray<UOfferViewModel*> Result;
	Result.Reserve(Rows.Num());
	for (const TObjectPtr<UOfferViewModel>& Row : Rows)
	{
		Result.Add(Row);
	}
	return Result;
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
	USimClock* LiveClock = Clock.Get();
	if (Row == nullptr || LiveBoard == nullptr || LiveClock == nullptr)
	{
		return;
	}
	if (UFlight* Flight = Row->Flight.Get())
	{
		LiveBoard->Decline(*LiveClock, *Flight);
	}
	if (UGroundTraffic* LiveTraffic = Traffic.Get())
	{
		if (URoadNetwork* LiveNetwork = Network.Get())
		{
			Refresh(*LiveBoard, *LiveTraffic, *LiveNetwork, *LiveClock);
		}
	}
}
