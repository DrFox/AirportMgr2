#include "Model/OfferGenerator.h"

#include "Model/AirsideCapability.h"
#include "Model/Flight.h"

bool UOfferGenerator::AirportAdmits(const FAirsideCapability& Airport, const FAirframe& Airframe)
{
	// 0 means "no published claim" in FRunwayRequirements, and no length refusal with it -
	// asking otherwise would ground every light aircraft whose figures nobody typed in.
	const double Needed = Airframe.Requirements.LandingFieldLength;
	if (Needed > 0.0 && Airport.LongestRunway() < Needed)
	{
		return false;
	}

	// A runway it can land on is no use without somewhere to park: the arrival would plan,
	// taxi, and then be refused a stand - which the player would read as the game breaking.
	for (const FStandSummary& Stand : Airport.Stands)
	{
		if (Stand.DesignWingspan >= Airframe.Wingspan)
		{
			return true;
		}
	}
	return false;
}

UFlight* UOfferGenerator::MakeOffer(const FAirsideCapability& Airport,
	const TArray<FOfferCandidate>& Fleet, double Now, int32 NextId)
{
	TArray<const FOfferCandidate*> Admissible;
	for (const FOfferCandidate& Candidate : Fleet)
	{
		if (AirportAdmits(Airport, Candidate.Airframe))
		{
			Admissible.Add(&Candidate);
		}
	}
	if (Admissible.Num() == 0)
	{
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

	// Off-block is measured from the ETA, not from now: a turnaround starts when it parks.
	Offer->OffBlockAt = Offer->ArrivesAt + Chosen.Airframe.TurnaroundSeconds;

	// Fees are deliberately left at zero. Nothing banks them until the ledger exists, and a
	// number nothing reads is a number that will be wrong by the time something does.
	return Offer;
}
