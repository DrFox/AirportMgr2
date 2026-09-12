#include "Model/OfferGenerator.h"

#include "AirportOpsLog.h"
#include "Model/AirsideCapability.h"
#include "Model/Flight.h"
#include "Model/RoadNetwork.h"

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
		return false;

	// These need the player to BUILD something. NoRunway, RunwayTooShort, NotAdmitted,
	// NoExit, NoRouteToStand.
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

UFlight* UOfferGenerator::MakeOffer(const URoadNetwork& Network, const FVector2D& Focus,
	const TArray<FOfferCandidate>& Fleet, double Now, int32 NextId)
{
	TArray<const FOfferCandidate*> Admissible;
	EArrivalRefusal FirstRefusal = EArrivalRefusal::None;
	FAirframe FirstRefused;

	for (const FOfferCandidate& Candidate : Fleet)
	{
		EArrivalRefusal Why = EArrivalRefusal::None;
		if (CouldEverAdmit(Network, Focus, Candidate.Airframe, Why))
		{
			Admissible.Add(&Candidate);
		}
		else if (FirstRefusal == EArrivalRefusal::None)
		{
			FirstRefusal = Why;
			FirstRefused = Candidate.Airframe;
		}
	}

	if (Admissible.Num() == 0)
	{
		// SAYS WHY, and names the aeroplane. An inbox that is simply empty is
		// indistinguishable from a generator that is not running - which is exactly how the
		// 2026-09-11 width refusal presented, and it cost a PIE session to tell apart.
		FArrivalPlan Explain;
		Explain.Why = FirstRefusal;
		UE_LOG(LogAirportOps, Log,
			TEXT("Offers: nothing in any fleet can use this airport. %s (the first refused "
				"has a %.0f uu wingspan and wants %.0f uu of runway)"),
			*ArrivalPlanner::DescribeRefusal(Explain), FirstRefused.Wingspan,
			FirstRefused.Requirements.LandingFieldLength);
		return nullptr;
	}

	const FOfferCandidate& Chosen = *Admissible[FMath::RandHelper(Admissible.Num())];

	UFlight* Offer = NewObject<UFlight>(this);
	Offer->Id = NextId;
	Offer->Airframe = Chosen.Airframe;
	Offer->AirlineName = Chosen.AirlineName;
	Offer->TypeName = Chosen.TypeName;
	Offer->Phase = EFlightPhase::Offered;
	Offer->ArrivesAt = Now + LeadTimeSeconds;

	// CLAMPED, not trusted. An offer that outlived its own ETA would sit in the inbox while
	// the aeroplane it describes was already on the approach, and accepting it would hold a
	// stand for something landing in the past.
	Offer->ExpiresAt = Now + FMath::Min(OfferLifeSeconds, LeadTimeSeconds);

	// Fees are deliberately left at zero. Nothing banks them until the ledger exists, and a
	// number nothing reads is a number that will be wrong by the time something does.
	return Offer;
}
