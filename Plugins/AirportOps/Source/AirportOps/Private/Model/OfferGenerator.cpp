#include "Model/OfferGenerator.h"

#include "AirportOpsLog.h"
#include "Model/AirlineDefinition.h"
#include "Model/AirsideCapability.h"
#include "Model/Flight.h"
#include "Model/Pricing.h"
#include "Model/RoadNetwork.h"
#include "Model/SimClock.h"

bool UOfferGenerator::IsPermanentRefusal(EArrivalRefusal Why)
{
	switch (Why)
	{
	case EArrivalRefusal::None:
		return false;

	// These clear on their own: the runway empties, an aeroplane leaves a stand. An offer
	// refused for one of them is still worth making - the player answers it minutes before
	// it lands, and the row shows the live reason meanwhile.
	case EArrivalRefusal::RunwayOccupied:
	case EArrivalRefusal::NoFreeStand:
	case EArrivalRefusal::GraphBeingEdited:  // clears when the player lets go of the node
		return false;

	// A SERVICE THE AIRPORT CANNOT GIVE is the player's to accept badly (spec 2026-09-28
	// ruling 5): the offer is made, the row says what is missing, and C scores the flight
	// down. Filtering it out hid WHY an airline was not offering.
	case EArrivalRefusal::NoStandServiceable:
		return false;

	// These need the player to BUILD something. NoRunway, RunwayTooShort, NotAdmitted,
	// NoExit, NoRouteToStand, NoStandBigEnough (a bigger stand - so no airline is offered an
	// A380 until an F stand exists, which is the drawn-stands spec's own promise),
	// and NoStandPavedEnough (pave a stand) - one of shared-pavement Task 9's two new
	// refusals; its sibling NoStandServiceable is soft since 2026-09-28, above.
	default:
		return true;
	}
}

bool UOfferGenerator::CouldEverAdmit(const URoadNetwork& Network, const FVector2D& Focus,
	const FAirframe& Airframe, EArrivalRefusal& OutWhy)
{
	// No occupancy: the question is what this FIELD can take, not what is free this second.
	const FArrivalPlan Plan = ArrivalPlanner::Plan(Network, Focus, Airframe, nullptr);
	OutWhy = Plan.Why;
	return !IsPermanentRefusal(Plan.Why);
}

double UOfferGenerator::RateAt(const UAirlineDefinition& Airline, double TimeOfDaySeconds,
	bool bDaylight, double DemandFactor)
{
	const double Demand = Airline.PeakOffersPerHour * Airline.CurveAt(TimeOfDaySeconds)
		* FMath::Max(DemandFactor, 0.0);
	// THE FLOOR IS NOT SCALED BY THE FEE, which is the whole of what makes it a floor - see
	// UAirlineDefinition::FloorOffersPerHour. Daylight only, so the night is a real lull.
	const double Floor = bDaylight ? Airline.FloorOffersPerHour : 0.0;
	return FMath::Max(Demand, Floor);
}

double UOfferGenerator::TotalRateAt(TArrayView<const FAirlineOffers> Airlines,
	double TimeOfDaySeconds, bool bDaylight, double DemandFactor)
{
	double Total = 0.0;
	for (const FAirlineOffers& Each : Airlines)
	{
		if (Each.Airline != nullptr)
		{
			Total += RateAt(*Each.Airline, TimeOfDaySeconds, bDaylight, DemandFactor);
		}
	}
	return Total;
}

double UOfferGenerator::DemandFactor() const
{
	return Pricing != nullptr ? Pricing->DemandFactor() : 1.0;
}

TArray<UFlight*> UOfferGenerator::TickMinute(const URoadNetwork& Network, const FVector2D& Focus,
	TArrayView<const FAirlineOffers> Airlines, const USimClock& Clock, int32 PendingNow,
	TFunctionRef<int32()> NextId)
{
	TArray<UFlight*> Made;
	const double Now = Clock.Now();
	const double TimeOfDay = Clock.TimeOfDay();
	const bool bDaylight = Clock.IsDaylight();
	// READ NOW, every minute - see RateAt's ENFORCED BY.
	const double Factor = DemandFactor();

	for (const FAirlineOffers& Each : Airlines)
	{
		if (Each.Airline == nullptr)
		{
			continue;
		}
		const UAirlineDefinition& Airline = *Each.Airline;
		FAirlineOfferState& State = States.FindOrAdd(Airline.GetFName());
		if (State.Threshold <= 0.0)
		{
			// FROM THE STREAM, not 1.0: every airline starting on exactly 1.0 would put all
			// their first offers on the same minute of a flat curve.
			State.Threshold = Stream.FRandRange(0.6, 1.4);
		}

		// NOTHING TO OFFER THIS MINUTE, NOTHING TO ASK (review I2): a night-quiet club at x32
		// would otherwise buy a route search per type per game minute for a rate of zero.
		const double Rate = RateAt(Airline, TimeOfDay, bDaylight, Factor);
		if (Rate <= 0.0)
		{
			continue;
		}

		FAdmissionCache& Cache = AdmissionCache.FindOrAdd(Airline.GetFName());
		const uint32 Revision = Network.GetGuidelineRevision();
		if (Cache.Network.Get() != &Network || Cache.GuidelineRevision != Revision
			|| Cache.Focus != Focus || Cache.FleetSize != Each.Fleet.Num())
		{
			Cache.Network = &Network;
			Cache.GuidelineRevision = Revision;
			Cache.Focus = Focus;
			Cache.FleetSize = Each.Fleet.Num();
			Cache.Admissible.Reset();
			Cache.FirstRefusal = EArrivalRefusal::None;
			Cache.FirstRefused = INDEX_NONE;
			for (int32 Index = 0; Index < Each.Fleet.Num(); ++Index)
			{
				++AdmissionChecks;
				EArrivalRefusal Why = EArrivalRefusal::None;
				if (CouldEverAdmit(Network, Focus, Each.Fleet[Index].Airframe, Why))
				{
					Cache.Admissible.Add(Index);
				}
				else if (Cache.FirstRefused == INDEX_NONE)
				{
					Cache.FirstRefusal = Why;
					Cache.FirstRefused = Index;
				}
			}
		}
		TArray<const FOfferCandidate*> Admissible;
		for (const int32 Index : Cache.Admissible)
		{
			Admissible.Add(&Each.Fleet[Index]);
		}
		const EArrivalRefusal FirstRefusal = Cache.FirstRefusal;
		const FOfferCandidate* FirstRefused = Cache.FirstRefused != INDEX_NONE ? &Each.Fleet[Cache.FirstRefused] : nullptr;

		if (Admissible.Num() == 0)
		{
			// NO BANKING - see TickMinute's header.
			State.Accumulated = 0.0;
			if (State.bCouldCome)
			{
				// SAYS WHY, and names the aeroplane - ON THE TRANSITION, not every minute. An
				// inbox that is simply empty is indistinguishable from a generator that is not
				// running, which is exactly how the 2026-09-11 width refusal presented and it
				// cost a PIE session to tell apart; a line every game minute would bury it.
				UE_LOG(LogAirportOps, Log,
					TEXT("Offers: %s cannot use this airport. %s (the first refused, %s, has a %.0f uu "
						"wingspan and wants %.0f uu of runway)"),
					*Airline.DisplayName.ToString(), *ArrivalPlanner::DescribeRefusal(FirstRefusal),
					FirstRefused != nullptr ? *FirstRefused->TypeName.ToString() : TEXT("none"),
					FirstRefused != nullptr ? FirstRefused->Airframe.Wingspan : 0.0,
					FirstRefused != nullptr ? FirstRefused->Airframe.Requirements.LandingFieldLength : 0.0);
				State.bCouldCome = false;
			}
			continue;
		}
		if (!State.bCouldCome)
		{
			UE_LOG(LogAirportOps, Log, TEXT("Offers: %s can use this airport again (%d type(s))"),
				*Airline.DisplayName.ToString(), Admissible.Num());
			State.bCouldCome = true;
		}

		State.Accumulated += Rate * (TickSeconds / 3600.0);
		while (State.Accumulated >= State.Threshold)
		{
			State.Accumulated -= State.Threshold;
			State.Threshold = Stream.FRandRange(0.6, 1.4);

			// FROM THIS GENERATOR'S OWN STREAM, never the global RNG - see Stream. Drawn even
			// when the inbox is full, so a dropped offer consumes the same draws a taken one
			// does and a full inbox cannot shift the sequence of every later offer.
			const FOfferCandidate& Chosen = *Admissible[Stream.RandHelper(Admissible.Num())];
			if (PendingNow + Made.Num() >= MaxPendingOffers)
			{
				++DroppedOffers;
				UE_LOG(LogAirportOps, Log, TEXT("Offers: inbox full (%d), dropped %s %s"),
					MaxPendingOffers, *Airline.DisplayName.ToString(), *Chosen.TypeName.ToString());
				continue;
			}
			Made.Add(MakeOffer(Focus, Airline, Chosen, Now, NextId()));
		}
	}
	return Made;
}

UFlight* UOfferGenerator::MakeOffer(const FVector2D& Focus, const UAirlineDefinition& Airline,
	const FOfferCandidate& Chosen, double Now, int32 Id)
{
	UFlight* Offer = NewObject<UFlight>(this);
	Offer->Id = Id;
	Offer->Airframe = Chosen.Airframe;
	Offer->AirlineName = Chosen.AirlineName;
	Offer->TypeName = Chosen.TypeName;
	Offer->Callsign = MakeCallsign(Airline.CallsignPrefix, Stream);
	Offer->Phase = EFlightPhase::Offered;
	Offer->bFloorAirline = Airline.bIsFloor;

	// REAL SECONDS, drained by UFlightBoard::TickOffers - see UAirlineDefinition::OfferWindowSeconds.
	Offer->OfferWindowSeconds = Airline.OfferWindowSeconds;
	Offer->OfferSecondsLeft = Airline.OfferWindowSeconds;

	// THE CONTRACT, fixed now so the row can show it before the player decides: the airline's
	// own figure - see UAirlineDefinition::ContractSeconds for why it is not a sum any more.
	Offer->LeadTimeSeconds = Airline.LeadTimeSeconds;
	Offer->ContractSeconds = Airline.ContractSeconds;

	// CARRIED WITH THE FLIGHT, not left for the board's own field to answer later - see
	// UFlight::ApproachFocus.
	Offer->ApproachFocus = Focus;

	// PRICED AT THE OFFER, not at touchdown, so the inbox row shows what accepting it is worth
	// and the player's fee lever moves NEW offers only. A fee computed on landing would let
	// them accept cheaply and put the price up afterwards, and the number they decided on
	// would have been a lie. ParkingFee stays zero: nobody knows how long it will stay.
	if (Pricing != nullptr)
	{
		Offer->LandingFee = Pricing->LandingFee(Offer->Airframe);
	}
	return Offer;
}

FString UOfferGenerator::MakeCallsign(const FString& Prefix, FRandomStream& Stream)
{
	if (Prefix.Contains(TEXT("?")))
	{
		FString Out = Prefix;
		for (TCHAR& Char : Out.GetCharArray())
		{
			if (Char == TEXT('?'))
			{
				Char = static_cast<TCHAR>(TEXT('A') + Stream.RandHelper(26));
			}
		}
		return Out;
	}
	return FString::Printf(TEXT("%s %d"), *Prefix, 100 + Stream.RandHelper(900));
}
