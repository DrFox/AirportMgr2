#include "Model/OfferGenerator.h"

#include "AirportOpsLog.h"
#include "Model/AirlineDefinition.h"
#include "Model/Airport.h"
#include "Model/AirsideCapability.h"
#include "Model/Flight.h"
#include "Model/OpsEventBus.h"
#include "Model/Pricing.h"
#include "Model/RoadNetwork.h"
#include "Model/SimClock.h"

// IsPermanentRefusal MOVED BESIDE EArrivalRefusal (ArrivalPlanner::IsPermanentRefusal, #442): the same question is now asked of
// a holding flight (UFlightBoard::UnlandableWhy), and its reasons - what clears on its own, what the player must build -
// travelled with it.

bool UOfferGenerator::CouldEverAdmit(const URoadNetwork& Network, const FVector2D& Focus,
	const FAirframe& Airframe, EArrivalRefusal& OutWhy, FString& OutSentence)
{
	// No occupancy: the question is what this FIELD can take, not what is free this second.
	const FArrivalPlan Plan = ArrivalPlanner::Plan(Network, Focus, Airframe, nullptr);
	OutWhy = Plan.Why;
	OutSentence = Plan.Why == EArrivalRefusal::None ? FString() : ArrivalPlanner::DescribeRefusal(Plan);
	return !ArrivalPlanner::IsPermanentRefusal(Plan.Why);
}

double UOfferGenerator::RateAt(const UAirlineDefinition& Airline, double TimeOfDaySeconds,
	bool bDaylight, double DemandFactor, double AirlineFactor)
{
	const double Demand = Airline.PeakOffersPerHour * Airline.CurveAt(TimeOfDaySeconds)
		* FMath::Max(DemandFactor, 0.0) * FMath::Max(AirlineFactor, 0.0);
	// THE FLOOR IS NOT SCALED BY THE FEE, which is the whole of what makes it a floor - see
	// UAirlineDefinition::FloorOffersPerHour. Daylight only, so the night is a real lull.
	const double Floor = bDaylight ? Airline.FloorOffersPerHour : 0.0;
	return FMath::Max(Demand, Floor);
}

double UOfferGenerator::TotalRateAt(TArrayView<const FAirlineOffers> Airlines,
	double TimeOfDaySeconds, bool bDaylight, double DemandFactor,
	TFunctionRef<double(const UAirlineDefinition&)> AirlineFactorOf)
{
	double Total = 0.0;
	for (const FAirlineOffers& Each : Airlines)
	{
		if (Each.Airline != nullptr)
		{
			Total += RateAt(*Each.Airline, TimeOfDaySeconds, bDaylight, DemandFactor, AirlineFactorOf(*Each.Airline));
		}
	}
	return Total;
}

double UOfferGenerator::DemandFactor() const
{
	return Pricing != nullptr ? Pricing->DemandFactor() : 1.0;
}

double UOfferGenerator::AirlineFactor(const UAirlineDefinition& Airline) const
{
	return (AirlineFactorOf ? AirlineFactorOf(Airline) : 1.0) * FleetShare(Airline);
}

double UOfferGenerator::FleetShare(const UAirlineDefinition& Airline) const
{
	const FAdmissionCache* Cache = AdmissionCache.Find(Airline.GetFName());
	if (Cache == nullptr || Cache->FleetSize <= 0)
	{
		return 1.0;
	}
	return static_cast<double>(Cache->AdmittedCount()) / Cache->FleetSize;
}

TArray<FFleetAdmission> UOfferGenerator::GetFleetAdmission(FName AirlineId) const
{
	const FAdmissionCache* Cache = AdmissionCache.Find(AirlineId);
	return Cache != nullptr ? Cache->Verdicts : TArray<FFleetAdmission>();
}

double UOfferGenerator::MoodFactor(const UAirlineDefinition& Airline) const
{
	return AirlineFactorOf ? AirlineFactorOf(Airline) : 1.0;
}

double UOfferGenerator::CurrentRate(const UAirlineDefinition& Airline, const USimClock& Clock) const
{
	// THE ONE EXPRESSION: TickMinute accrues exactly this each minute, and the panel shows it - see the declaration.
	// ENFORCED BY: AirportOps.Model.Offers.Rate.CurrentRateIsWhatAccrues
	return RateAt(Airline, Clock.TimeOfDay(), Clock.IsDaylight(), DemandFactor(), AirlineFactor(Airline));
}

TArray<UFlight*> UOfferGenerator::TickMinute(const URoadNetwork& Network, const FVector2D& Focus,
	TArrayView<const FAirlineOffers> Airlines, const USimClock& Clock, int32 PendingNow,
	TFunctionRef<int32()> NextId)
{
	TArray<UFlight*> Made;
	// NOT OPEN, NOTHING ASKED (spec 2026-09-29-ops-batch3 §3): before any airline's threshold, rate or admission,
	// so a runway-less field runs no route search and judges no airline unable to come - no AirlineCannotCome
	// alert, no "cannot use this airport" line per airline, for a condition the NoRunway alert already names.
	// ENFORCED BY: AirportOps.Model.Offers.Generate.NothingUnlessOpen
	if (Airport != nullptr && !Airport->AdmitsArrivals())
	{
		return Made;
	}
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
		// WITHOUT THE FLEET SHARE, deliberately: the share comes from the admission check below, and
		// an airline whose last share was 0 would read a zero rate here, skip the check, and never
		// be judged again - an airport that paved its runway would wait for ever for the King Air.
		if (RateAt(Airline, TimeOfDay, bDaylight, Factor, MoodFactor(Airline)) <= 0.0)
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
			// ONE SEARCH PER TYPE, ONE VERDICT PER SEARCH: this loop is the only place a verdict is made, and everything
			// below (the pick, the share, the log line, the event text) reads Verdicts - see GetFleetAdmission.
			// ENFORCED BY: AirportOps.Model.Offers.FleetAdmission.EveryTypeHasAVerdict
			Cache.Verdicts.Reset();
			for (int32 Index = 0; Index < Each.Fleet.Num(); ++Index)
			{
				++AdmissionChecks;
				FFleetAdmission Verdict;
				Verdict.TypeName = Each.Fleet[Index].TypeName;
				FString Sentence;
				Verdict.bAdmitted = CouldEverAdmit(Network, Focus, Each.Fleet[Index].Airframe, Verdict.Why, Sentence);
				if (Verdict.bAdmitted)
				{
					// CouldEverAdmit passes TEMPORARY refusals (RunwayOccupied...) through OutWhy with true; a tick must not
					// carry one, or the panel would read a reason beside it.
					Verdict.Why = EArrivalRefusal::None;
				}
				else
				{
					// THE PLAN'S SENTENCE, with its figures, when there is one (#396); the reason's own wording
					// otherwise.
					Verdict.Sentence = Sentence.IsEmpty() ? ArrivalPlanner::DescribeRefusal(Verdict.Why) : MoveTemp(Sentence);
				}
				Cache.Verdicts.Add(MoveTemp(Verdict));
			}
		}
		TArray<const FOfferCandidate*> Admissible;
		for (int32 Index = 0; Index < Cache.Verdicts.Num(); ++Index)
		{
			if (Cache.Verdicts[Index].bAdmitted)
			{
				Admissible.Add(&Each.Fleet[Index]);
			}
		}
		const int32 FirstRefusedIndex = Cache.FirstRefusedIndex();
		const FFleetAdmission* FirstVerdict = FirstRefusedIndex != INDEX_NONE ? &Cache.Verdicts[FirstRefusedIndex] : nullptr;
		const FString FirstRefusalText = FirstVerdict != nullptr ? FirstVerdict->Sentence
			: ArrivalPlanner::DescribeRefusal(EArrivalRefusal::None);
		const FOfferCandidate* FirstRefused = FirstRefusedIndex != INDEX_NONE ? &Each.Fleet[FirstRefusedIndex] : nullptr;

		if (Admissible.Num() == 0)
		{
			// NO BANKING - see TickMinute's header.
			State.Accumulated = 0.0;
			const bool bWasComing = State.bCouldCome;
			if (State.bCouldCome)
			{
				// SAYS WHY, and names the aeroplane - ON THE TRANSITION, not every minute. An
				// inbox that is simply empty is indistinguishable from a generator that is not
				// running, which is exactly how the 2026-09-11 width refusal presented and it
				// cost a PIE session to tell apart; a line every game minute would bury it.
				UE_LOG(LogAirportOps, Log,
					TEXT("Offers: %s cannot use this airport. %s (the first refused, %s, has a %.0f uu "
						"wingspan and wants %.0f uu of runway)"),
					*Airline.DisplayName.ToString(), *FirstRefusalText,
					FirstRefused != nullptr ? *FirstRefused->TypeName.ToString() : TEXT("none"),
					FirstRefused != nullptr ? FirstRefused->Airframe.Wingspan : 0.0,
					FirstRefused != nullptr ? FirstRefused->Airframe.Requirements.LandingFieldLength : 0.0);
				State.bCouldCome = false;
			}
			// ANNOUNCED WHEN THE VERDICT MOVES - it could come and cannot, or still cannot for another reason (#446, #445). The alerts
			// pass looks again on this and on nothing else about airlines: it used to look every offer minute, ~40 times a real
			// second at x32. The transition's own log line above stays the record; this is the wake-up.
			// ENFORCED BY: AirportOps.Model.Offers.AdmissionChangeIsAnnounced
			if (Cache.AnnouncedReason != FirstRefusalText || bWasComing)
			{
				Cache.AnnouncedReason = FirstRefusalText;
				if (Bus != nullptr)
				{
					Bus->Publish(FAirlineAdmissionChangedEvent{ Airline.GetFName(), false, FirstRefusalText });
				}
			}
			continue;
		}
		if (!State.bCouldCome)
		{
			UE_LOG(LogAirportOps, Log, TEXT("Offers: %s can use this airport again (%d type(s))"),
				*Airline.DisplayName.ToString(), Admissible.Num());
			State.bCouldCome = true;
			Cache.AnnouncedReason.Reset();
			if (Bus != nullptr)
			{
				Bus->Publish(FAirlineAdmissionChangedEvent{ Airline.GetFName(), true, FString() });
			}
		}

		// THE SHARE NOW, from the check just made - see FleetShare. CurrentRate IS this expression: the panel shows what accrues.
		const double Rate = CurrentRate(Airline, Clock);
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
	Offer->AirlineId = Airline.GetFName();
	Offer->TypeName = Chosen.TypeName;
	Offer->Callsign = MakeCallsign(Airline.CallsignPrefix, Stream);
	Offer->bFloorAirline = Airline.bIsFloor;

	// REAL SECONDS, drained by UFlightBoard::TickOffers - see UAirlineDefinition::OfferWindowSeconds.
	Offer->OfferWindowSeconds = Airline.OfferWindowSeconds;
	Offer->OfferSecondsLeft = Airline.OfferWindowSeconds;

	// THE CONTRACT, fixed now so the row can show it before the player decides: the airline's
	// own figure - see UAirlineDefinition::ContractSeconds for why it is not a sum any more.
	Offer->LeadTimeSeconds = Airline.LeadTimeSeconds;
	Offer->ContractSeconds = Airline.ContractSeconds;

	// THE FUEL LOAD, fixed now so the row shows the size of the job before the accept - 50-90% of
	// the tank, from this generator's own stream so a seed repeats (spec 2026-09-28-fuel-litres).
	const double Tank = Chosen.Airframe.FuelCapacityLitres;
	Offer->FuelLitres = Tank > 0.0
		? FMath::RoundToDouble(Tank * Stream.FRandRange(OpsDesignDefaults::FuelLoadDrawMin, OpsDesignDefaults::FuelLoadDrawMax)) : 0.0;

	// CARRIED WITH THE FLIGHT, not left for the board's own field to answer later - see
	// UFlight::RunwayPreference. (NO PHASE WRITTEN HERE: a flight is born Offered, UFlight::Phase's default, and this line
	// used to write the value the field already held - the thirteenth writer of #442, and a no-op.)
	Offer->RunwayPreference = Focus;

	// PRICED AT THE OFFER, not at touchdown, so the inbox row shows what accepting it is worth
	// and the player's fee lever moves NEW offers only. A fee computed on landing would let
	// them accept cheaply and put the price up afterwards, and the number they decided on
	// would have been a lie. ParkingFee stays zero: nobody knows how long it will stay - but the RATE it will be billed at
	// is known now, and is fixed here for the same reason (#442): PostParkingFee used to ask for it at departure, through the
	// lever as it then stood, which is the trade this comment rules out for the landing fee, open for parking.
	if (Pricing != nullptr)
	{
		Offer->LandingFee = Pricing->LandingFee(Offer->Airframe);
		Offer->ParkingRatePerHour = Pricing->ParkingFeePerHour(Offer->Airframe);
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

void UOfferGenerator::ForgetAirlineVerdicts()
{
	for (TPair<FName, FAirlineOfferState>& Each : States)
	{
		Each.Value.bCouldCome = true;
	}
	UE_LOG(LogAirportOps, Log, TEXT("Offers: %d airline verdict(s) forgotten - judged again next minute"), States.Num());
}

FString UOfferGenerator::DescribeWhyNot(FName AirlineId) const
{
	const FAdmissionCache* Cache = AdmissionCache.Find(AirlineId);
	const int32 First = Cache != nullptr ? Cache->FirstRefusedIndex() : INDEX_NONE;
	if (First == INDEX_NONE)
	{
		return FString();
	}
	// THE SAME VERDICT TickMinute's log line prints: the plan's sentence with its figures, else the reason (resolved when the verdict was made).
	return Cache->Verdicts[First].Sentence;
}
