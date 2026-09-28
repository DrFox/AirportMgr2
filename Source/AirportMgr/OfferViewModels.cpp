#include "OfferViewModels.h"

#include "Model/ArrivalPlanner.h"
#include "Model/Flight.h"
#include "Model/FlightBoard.h"
#include "Model/GroundTraffic.h"
#include "Model/OfferGenerator.h"
#include "Model/Pricing.h"
#include "Model/RoadNetwork.h"
#include "Model/SimClock.h"

FText UOfferViewModel::DescribeDuration(double Seconds)
{
	const int32 Minutes = FMath::Max(0, FMath::RoundToInt(Seconds / 60.0));
	if (Minutes < 60)
	{
		return FText::Format(NSLOCTEXT("AirportMgr", "DurationMin", "{0} min"), FText::AsNumber(Minutes));
	}
	const int32 Hours = Minutes / 60;
	const int32 Rest = Minutes % 60;
	return Rest == 0
		? FText::Format(NSLOCTEXT("AirportMgr", "DurationH", "{0} h"), FText::AsNumber(Hours))
		: FText::Format(NSLOCTEXT("AirportMgr", "DurationHMin", "{0} h {1} min"),
			FText::AsNumber(Hours), FText::AsNumber(Rest));
}

FText UOfferViewModel::DescribeContract(double LeadTimeSeconds, double ContractSeconds)
{
	return FText::Format(NSLOCTEXT("AirportMgr", "OfferContract", "lands in {0} \u00B7 airborne within {1}"),
		DescribeDuration(LeadTimeSeconds), DescribeDuration(ContractSeconds));
}

void UOfferViewModel::Refresh(const UFlightBoard& Board, const UGroundTraffic& Traffic,
	const URoadNetwork& Network, const USimClock& Clock)
{
	const UFlight* Live = Flight.Get();
	if (Live == nullptr)
	{
		return;
	}

	Callsign = FText::FromString(Live->Callsign);
	Airline = Live->AirlineName;
	TypeName = Live->TypeName;
	Fee = Board.Pricing != nullptr ? Board.Pricing->Format(Live->LandingFee) : FText::GetEmpty();
	Contract = DescribeContract(Live->LeadTimeSeconds, Live->ContractSeconds);
	bNeedsTug = Live->Airframe.PushbackNeed == EPushbackNeed::VehicleTug;

	// ROUNDED UP: "0 s" while there is still time to click reads as a lie.
	SecondsLeft = FMath::CeilToInt(FMath::Max(Live->OfferSecondsLeft, 0.0));
	TimeLeftFraction = Live->OfferWindowSeconds > 0.0
		? static_cast<float>(FMath::Clamp(Live->OfferSecondsLeft / Live->OfferWindowSeconds, 0.0, 1.0))
		: 0.0f;

	// THE BOARD'S VERDICT, cached on the board (issue #169's revisions moved there 2026-09-28 -
	// see FOfferVerdict), so this row and the lapse classification read one answer. Cheap on
	// every frame nothing moved.
	const FOfferVerdict& Verdict = Board.VerdictFor(Traffic, Network, *Live);
	bAcceptable = Verdict.Why == EArrivalRefusal::None;
	bFuelServable = Verdict.bFuelServable;
	// THE REASON-ONLY OVERLOAD, not a plan built by hand just to carry Why - ToastStackWidget
	// already reads it this way, and a plan with every other field default-constructed is not
	// a plan, it is Why wearing a bigger struct.
	// THE FLIGHT'S OWN SPAN rides along so NoStandBigEnough can name the letter to build
	// ("needs a Code F stand") - the row has the airframe, which the toast does not.
	Refusal = bAcceptable
		? FText::GetEmpty()
		: FText::FromString(ArrivalPlanner::DescribeRefusal(Verdict.Why, Live->Airframe.Wingspan));
	AcceptLabel = bFuelServable
		? NSLOCTEXT("AirportMgr", "OfferAccept", "Accept")
		: NSLOCTEXT("AirportMgr", "OfferAcceptNoFuel", "Accept (no fuel)");
}

TArray<double> UOfferInboxViewModel::SampleDemand(TArrayView<const FAirlineOffers> Airlines,
	const USimClock& Clock, double DemandFactor, int32 Count)
{
	TArray<double> Out;
	const int32 N = FMath::Max(Count, 0);
	Out.Reserve(N);
	for (int32 Index = 0; Index < N; ++Index)
	{
		const double Midpoint = (Index + 0.5) * USimClock::SecondsPerDay / N;
		Out.Add(UOfferGenerator::TotalRateAt(Airlines, Midpoint, Clock.IsDaylight(Midpoint), DemandFactor));
	}
	return Out;
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
	Capacity = InBoard.Generator != nullptr ? InBoard.Generator->MaxPendingOffers : 0;
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
