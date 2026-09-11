#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/AirsideCapability.h"
#include "Model/Flight.h"
#include "Model/OfferGenerator.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FAirsideCapability AirportWith(double RunwayLength, double StandWingspan)
	{
		FAirsideCapability Out;
		FRunwaySummary Runway;
		Runway.Length = RunwayLength;
		Out.Runways.Add(Runway);

		FStandSummary Stand;
		Stand.DesignWingspan = StandWingspan;
		Out.Stands.Add(Stand);
		return Out;
	}

	FAirframe Needing(double FieldLength, double Wingspan)
	{
		FAirframe Out;
		Out.Requirements.LandingFieldLength = FieldLength;
		Out.Wingspan = Wingspan;
		return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOfferGeneratorAdmissionTest,
	"AirportOps.Model.OfferGenerator.OffersOnlyWhatTheAirfieldCanTake",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOfferGeneratorAdmissionTest::RunTest(const FString& Parameters)
{
	const FAirsideCapability Airport = AirportWith(90000.0, 3600.0);

	TestFalse(TEXT("a type needing more runway than exists is not offered"),
		UOfferGenerator::AirportAdmits(Airport, Needing(200000.0, 3000.0)));
	TestTrue(TEXT("a type that fits the runway and a stand is offered"),
		UOfferGenerator::AirportAdmits(Airport, Needing(60000.0, 3000.0)));

	// A runway it can land on is no use without somewhere to park. This is the half that is
	// easy to forget, and the symptom would be an arrival that taxis and is then refused.
	TestFalse(TEXT("a type wider than every stand is not offered, however long the runway"),
		UOfferGenerator::AirportAdmits(Airport, Needing(60000.0, 6500.0)));

	// 0 is "no published claim" in FRunwayRequirements, not "needs nothing measured".
	TestTrue(TEXT("a type with no published field length is not refused on length"),
		UOfferGenerator::AirportAdmits(AirportWith(1000.0, 3600.0), Needing(0.0, 3000.0)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOfferGeneratorEtaTest,
	"AirportOps.Model.OfferGenerator.AnOfferCarriesAnEtaAndAnExpiry",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOfferGeneratorEtaTest::RunTest(const FString& Parameters)
{
	UOfferGenerator* Generator = NewObject<UOfferGenerator>();

	FOfferCandidate Candidate;
	Candidate.Airframe = Needing(60000.0, 3000.0);
	Candidate.Airframe.TurnaroundSeconds = 1800.0;
	Candidate.AirlineName = FText::FromString(TEXT("Meridian"));
	Candidate.TypeName = FText::FromString(TEXT("A320"));

	UFlight* Offer = Generator->MakeOffer(AirportWith(90000.0, 3600.0), {Candidate}, 1000.0, 1);
	TestNotNull(TEXT("an admissible fleet produces an offer"), Offer);
	if (Offer == nullptr) { return false; }

	TestEqual(TEXT("it is Offered, not Accepted"), Offer->Phase, EFlightPhase::Offered);
	TestEqual(TEXT("the ETA is the lead time out"),
		Offer->ArrivesAt, 1000.0 + Generator->LeadTimeSeconds, 1e-9);
	TestTrue(TEXT("the expiry is BEFORE the ETA, or an offer could lapse mid-approach"),
		Offer->ExpiresAt < Offer->ArrivesAt);
	TestEqual(TEXT("off-block is a turnaround after it lands, not after it was offered"),
		Offer->OffBlockAt, Offer->ArrivesAt + 1800.0, 1e-9);
	TestEqual(TEXT("the airframe travelled onto the flight"), Offer->Airframe.Wingspan, 3000.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOfferGeneratorClampsExpiryTest,
	"AirportOps.Model.OfferGenerator.AnOfferCannotOutliveItsOwnEta",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOfferGeneratorClampsExpiryTest::RunTest(const FString& Parameters)
{
	// Authored the wrong way round on purpose: a designer can type these two numbers into a
	// data asset in either order, and one order is nonsense.
	UOfferGenerator* Generator = NewObject<UOfferGenerator>();
	Generator->LeadTimeSeconds = 300.0;
	Generator->OfferLifeSeconds = 9000.0;

	FOfferCandidate Candidate;
	Candidate.Airframe = Needing(0.0, 3000.0);

	UFlight* Offer = Generator->MakeOffer(AirportWith(90000.0, 3600.0), {Candidate}, 0.0, 1);
	TestNotNull(TEXT("still an offer"), Offer);
	if (Offer == nullptr) { return false; }
	TestTrue(TEXT("the expiry is clamped to the ETA rather than trusted"),
		Offer->ExpiresAt <= Offer->ArrivesAt);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOfferGeneratorNothingFitsTest,
	"AirportOps.Model.OfferGenerator.AnAirportThatFitsNothingGetsNoOffers",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOfferGeneratorNothingFitsTest::RunTest(const FString& Parameters)
{
	UOfferGenerator* Generator = NewObject<UOfferGenerator>();
	FOfferCandidate TooBig;
	TooBig.Airframe = Needing(200000.0, 6500.0);

	TestNull(TEXT("nothing is offered rather than something unlandable"),
		Generator->MakeOffer(AirportWith(30000.0, 2000.0), {TooBig}, 0.0, 1));
	return true;
}

#endif
