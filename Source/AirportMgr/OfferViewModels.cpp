#include "OfferViewModels.h"

#include "Model/AirlineRoster.h"
#include "Model/ArrivalPlanner.h"
#include "Model/Flight.h"
#include "Model/FlightBoard.h"
#include "Model/GameTimeText.h"
#include "Model/GroundTraffic.h"
#include "Model/OfferGenerator.h"
#include "Model/Pricing.h"
#include "Model/RoadNetwork.h"
#include "Model/SimClock.h"

namespace
{
	/** The stamp each row change takes - see UOfferViewModel::GetRevision. Prefixed for the unity build. */
	int32 GOfferRowStamp = 0;
}

FText UOfferViewModel::DescribeContract(double LeadTimeSeconds, double ContractSeconds)
{
	// "ON STAND", NOT "AIRBORNE WITHIN" (#398): the contract is time on the stand, on-blocks to off-blocks. The old words promised
	// a deadline from the accept that the taxiways and the runway queue spent, and the player could not see where it went.
	return FText::Format(NSLOCTEXT("AirportMgr", "OfferContract", "lands in {0} \u00B7 {1} on stand"),
		GameTimeText::Duration(LeadTimeSeconds), GameTimeText::Duration(ContractSeconds));
}

FOfferMoodKey UOfferViewModel::MoodKeyOf(const FAirlineStanding* Standing)
{
	FOfferMoodKey Out;
	if (Standing == nullptr)
	{
		return Out;
	}
	Out.bStanding = true;
	Out.Percent = FMath::RoundToInt(Standing->Satisfaction * 100.0);
	Out.bRecent = Standing->Recent.Num() > 0;
	if (Out.bRecent)
	{
		// THE NEWEST CHANGE ONLY: the row has room for one reason, and the latest is the one the player
		// can still connect to something they just did.
		const FAirlineSatisfactionChange& Latest = Standing->Recent.Last();
		Out.bUp = Latest.Delta >= 0.0;
		Out.Cause = Latest.Cause;
	}
	return Out;
}

FText UOfferViewModel::DescribeMood(const FOfferMoodKey& Mood)
{
	if (!Mood.bStanding)
	{
		return FText::GetEmpty();
	}
	const FText Percent = FText::Format(NSLOCTEXT("AirportMgr", "OfferSatisfactionPct", "{0}%"), FText::AsNumber(Mood.Percent));
	if (!Mood.bRecent)
	{
		return Percent;
	}
	return FText::Format(NSLOCTEXT("AirportMgr", "OfferSatisfaction", "{0} {1} {2}"), Percent,
		FText::FromString(Mood.bUp ? TEXT("\u25B2") : TEXT("\u25BC")), FText::FromString(Mood.Cause));
}

FText UOfferViewModel::DescribeSatisfaction(const FAirlineStanding* Standing)
{
	return DescribeMood(MoodKeyOf(Standing));
}

bool UOfferViewModel::Refresh(const UFlightBoard& Board, const UGroundTraffic& Traffic,
	const URoadNetwork& Network, const USimClock& Clock, const UAirlineRoster* Airlines)
{
	const UFlight* Live = Flight.Get();
	if (Live == nullptr)
	{
		return false;
	}
	bool bChanged = false;

	// HOW THE AIRLINE FEELS, composed when the mood moved (a few times a game day), not every tick.
	const FOfferMoodKey Mood = MoodKeyOf(Airlines != nullptr ? Airlines->Find(Live->AirlineId) : nullptr);
	if (!bMoodValid || !(Mood == MoodKey))
	{
		Satisfaction = DescribeMood(Mood);
		MoodKey = Mood;
		bMoodValid = true;
		bChanged = true;
	}

	// WHAT THE FLIGHT FIXED AT THE OFFER - who, what it pays, what it wants, the contract: composed once, and again only if a figure or the
	// pricing that words the fee moved.
	FOfferFixedKey Fixed;
	Fixed.Flight = Live;
	Fixed.Pricing = Board.Pricing;
	Fixed.LeadTimeSeconds = Live->LeadTimeSeconds;
	Fixed.ContractSeconds = Live->ContractSeconds;
	Fixed.LandingFee = Live->LandingFee;
	Fixed.FuelLitres = Live->FuelLitres;
	if (!bFixedValid || !(Fixed == FixedKey))
	{
		Callsign = FText::FromString(Live->Callsign);
		Airline = Live->AirlineName;
		TypeName = Live->TypeName;
		Fee = Board.Pricing != nullptr ? Board.Pricing->Format(Live->LandingFee) : FText::GetEmpty();
		Contract = DescribeContract(Live->LeadTimeSeconds, Live->ContractSeconds);
		bNeedsTug = Live->Airframe.PushbackNeed == EPushbackNeed::VehicleTug;
		// THE SIZE OF THE JOB, before the accept (spec 2026-09-28-fuel-litres).
		FuelText = Live->FuelLitres > 0.0
			? FText::Format(NSLOCTEXT("AirportMgr", "OfferFuelLitres", "Fuel {0} L"), FText::AsNumber(FMath::RoundToInt(Live->FuelLitres)))
			: FText::GetEmpty();
		FixedKey = Fixed;
		bFixedValid = true;
		bChanged = true;
	}

	// ROUNDED UP: "0 s" while there is still time to click reads as a lie.
	SecondsLeft = FMath::CeilToInt(FMath::Max(Live->OfferSecondsLeft, 0.0));
	TimeLeftFraction = Live->OfferWindowSeconds > 0.0
		? static_cast<float>(FMath::Clamp(Live->OfferSecondsLeft / Live->OfferWindowSeconds, 0.0, 1.0))
		: 0.0f;

	// THE BOARD'S VERDICT, cached on the board (issue #169's revisions moved there 2026-09-28 -
	// see FOfferVerdict), so this row and the lapse classification read one answer. Cheap on
	// every frame nothing moved.
	const FOfferVerdict& Verdict = Board.VerdictFor(Traffic, Network, *Live);
	// THE QUOTE THE ACCEPT WILL ASK (#431): the cached verdict, then the airport's gate - so the button is lit exactly
	// when UFlightBoard::TryAccept would take the click, and a closed airport greys it with its reason.
	const FArrivalQuote Quote = Board.QuoteFor(Traffic, Network, *Live);
	const bool bNowAcceptable = Quote.IsAccepted();
	// THE PLAN'S OWN SENTENCE (#456 review), carried on the verdict - not the reason-only overload, which reads "not
	// admitted to that runway" for an arrivals-only field whose real reason is that nothing can take the departure.
	// The plan names the stand letter to build, the figures and the admission itself.
	const FString WantedSentence = bNowAcceptable ? FString() : Quote.Sentence;
	// THE VERDICT'S THREE OUTPUTS, rewritten when any of them moved: the text is built from the plan's sentence and the caption from the
	// fuel flag, so neither is rebuilt for a verdict that is the one it already holds.
	if (!bVerdictValid || bNowAcceptable != bAcceptable || Verdict.bFuelServable != bFuelServable || WantedSentence != RefusalSentence)
	{
		bAcceptable = bNowAcceptable;
		bFuelServable = Verdict.bFuelServable;
		RefusalSentence = WantedSentence;
		Refusal = bAcceptable ? FText::GetEmpty() : FText::FromString(Quote.Sentence);
		AcceptLabel = bFuelServable
			? NSLOCTEXT("AirportMgr", "OfferAccept", "Accept")
			: NSLOCTEXT("AirportMgr", "OfferAcceptNoFuel", "Accept (no fuel)");
		bVerdictValid = true;
		bChanged = true;
	}
	if (bChanged)
	{
		Revision = ++GOfferRowStamp;
	}
	return bChanged;
}

TArray<double> UOfferInboxViewModel::SampleDemand(TArrayView<const FAirlineOffers> Airlines,
	const USimClock& Clock, double DemandFactor, int32 Count,
	TFunctionRef<double(const UAirlineDefinition&)> AirlineFactorOf)
{
	TArray<double> Out;
	const int32 N = FMath::Max(Count, 0);
	Out.Reserve(N);
	for (int32 Index = 0; Index < N; ++Index)
	{
		const double Midpoint = (Index + 0.5) * USimClock::SecondsPerDay / N;
		Out.Add(UOfferGenerator::TotalRateAt(Airlines, Midpoint, Clock.IsDaylight(Midpoint), DemandFactor, AirlineFactorOf));
	}
	return Out;
}

void UOfferInboxViewModel::Refresh(UFlightBoard& InBoard, UGroundTraffic& InTraffic,
	const URoadNetwork& InNetwork, const USimClock& InClock, const UAirlineRoster* InAirlines)
{
	SyncRows(InBoard, InTraffic, InNetwork, InClock, InAirlines);
	RefreshRows(InBoard, InTraffic, InNetwork, InClock, InAirlines);
}

void UOfferInboxViewModel::SyncRows(UFlightBoard& InBoard, UGroundTraffic& InTraffic,
	const URoadNetwork& InNetwork, const USimClock& InClock, const UAirlineRoster* InAirlines)
{
	Airlines = InAirlines;
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

	// A PLAIN ASSIGNMENT (issue #191 dropped UE_MVVM_SET_PROPERTY_VALUE here): the count used
	// to be set through the macro so a Blueprint binding would hear about it, but nothing
	// ever bound this field - OfferInboxWidget::PaintRows reads GetPendingCount() directly
	// every refresh, which is what actually keeps the badge current.
	PendingCount = Rows.Num();
	Capacity = InBoard.Generator != nullptr ? InBoard.Generator->MaxPendingOffers : 0;
}

void UOfferInboxViewModel::RefreshRows(UFlightBoard& InBoard, UGroundTraffic& InTraffic,
	const URoadNetwork& InNetwork, const USimClock& InClock, const UAirlineRoster* InAirlines)
{
	for (TObjectPtr<UOfferViewModel>& Row : Rows)
	{
		if (Row != nullptr && Row->Refresh(InBoard, InTraffic, InNetwork, InClock, InAirlines))
		{
			++Composes;
		}
	}
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

	// THE ONE DOOR. The viewmodel asks the board; it never writes a phase or a stand itself. TryAccept (#431), the
	// door key 7 comes through too.
	const bool bAccepted = LiveBoard->TryAccept(*LiveTraffic, *LiveNetwork, *LiveClock, *Flight).IsAccepted();
	Refresh(*LiveBoard, *LiveTraffic, *LiveNetwork, *LiveClock, Airlines.Get());
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
			Refresh(*LiveBoard, *LiveTraffic, *LiveNetwork, *LiveClock, Airlines.Get());
		}
	}
}
