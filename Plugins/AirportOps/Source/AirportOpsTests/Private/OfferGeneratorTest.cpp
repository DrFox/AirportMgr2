#include "CoreMinimal.h"
#include "Build/AnchorLink.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/Flight.h"
#include "Model/LandingRun.h"
#include "Model/OfferGenerator.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FAirframe Needing(double FieldLength, double Wingspan)
	{
		FAirframe Out;
		Out.Requirements.LandingFieldLength = FieldLength;
		Out.Wingspan = Wingspan;
		return Out;
	}

	/**
	 * A usable little airport: one runway of the given WIDTH, split at an exit, a taxiway
	 * south from that exit, and a stand beside it.
	 *
	 * Modelled on ArrivalPlannerTest's BuildTwoExitAirport, because the offer filter now asks
	 * the same ArrivalPlanner::Plan those tests do - a thinner fixture (a bare runway and a
	 * stand with no taxiway) refuses everything with NoRouteToStand and would make this file
	 * pass for the wrong reason.
	 *
	 * The WIDTH is the parameter that matters here: the 2026-09-11 bug was A320s offered to a
	 * 15 m strip, which RunwayAdmission refuses on wingspan and the old filter never asked.
	 */
	URoadNetwork* FieldWith(double RunwayWidth, const FAirframe& For)
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());

		const double Needed = FLandingRun::RequiredLandingDistance(
			For.Ground, For.Climb, For.Approach) * FLandingRun::LandingMargin;
		const double Length = FMath::Max(Needed * 3.0, 60000.0);
		const FVector2D ExitAt(Length * 0.5, 0.0);

		URoadProfile* Runway = URoadProfile::MakeTransient(RunwayWidth, 1500.0, 450.0);
		Runway->bContinuousThroughJunctions = true;
		URoadProfile* Taxiway = URoadProfile::MakeTransient(2300.0, 1500.0, 230.0);

		// SPLIT AT THE EXIT: a T-junction is what puts a guideline node on the centreline for
		// RunwayExitNodes to find - see ArrivalDispatchTest.
		const FRoadNodeId ThresholdNode = Net->AddNode(FVector2D::ZeroVector);
		const FRoadNodeId ExitNode = Net->AddNode(ExitAt);
		const FRoadNodeId FarNode = Net->AddNode(FVector2D(Length, 0.0));
		Net->AddStraightSegment(ThresholdNode, ExitNode, Runway);
		Net->AddStraightSegment(ExitNode, FarNode, Runway);

		const FRoadNodeId TaxiEnd = Net->AddNode(ExitAt + FVector2D(0.0, -20000.0));
		Net->AddStraightSegment(ExitNode, TaxiEnd, Taxiway);

		const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Net);
		FRoadGuidelineBuilder::Build(*Net, Solved);

		// Facing east (heading 0), so its lead-in casts west onto the taxiway.
		UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
		Net->PlaceEntity(Stand, Stand->Anchors, ExitAt + FVector2D(9000.0, -10000.0), 0.0,
			6000.0, Stand->PoseRole, Stand->Trucks);

		FAnchorLink::Build(*Net);
		return Net;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOfferGeneratorWidthTest,
	"AirportOps.Model.OfferGenerator.ANarrowRunwayIsNotOfferedAirliners",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOfferGeneratorWidthTest::RunTest(const FString& Parameters)
{
	// THE BUG OF 2026-09-11, pinned. M_Starter's runway is 15 m wide and admits a 15 m
	// wingspan; every offer was an A320 at 35.8 m, so every Accept in the inbox was greyed
	// out and the player had no way to tell why. The old filter asked FAirsideCapability,
	// which knows the runway's LENGTH and the stands, and nothing about RunwayAdmission.
	const FAirframe Airliner = Needing(0.0, 3580.0);
	URoadNetwork* Narrow = FieldWith(1800.0, Airliner);   // an 18 m strip, as M_Starter has

	EArrivalRefusal Why = EArrivalRefusal::None;
	TestFalse(TEXT("an airliner is not offered a strip too narrow for its wingspan"),
		UOfferGenerator::CouldEverAdmit(*Narrow, FVector2D::ZeroVector, Airliner, Why));
	TestEqual(TEXT("and the reason is admission, not something vaguer"),
		Why, EArrivalRefusal::NotAdmitted);

	TestTrue(TEXT("a light aircraft that fits the same strip still is"),
		UOfferGenerator::CouldEverAdmit(*Narrow, FVector2D::ZeroVector, Needing(0.0, 1200.0), Why));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOfferGeneratorTransientRefusalTest,
	"AirportOps.Model.OfferGenerator.ATransientRefusalStillGetsOffered",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOfferGeneratorTransientRefusalTest::RunTest(const FString& Parameters)
{
	// The filter must reject only what NO amount of waiting fixes. A busy runway or a full
	// stand row clears on its own, and the offer is answered minutes before it lands - so
	// filtering those out would empty the inbox of perfectly good flights at every rush.
	TestTrue(TEXT("a busy runway is not a permanent refusal"),
		UOfferGenerator::IsPermanentRefusal(EArrivalRefusal::RunwayOccupied) == false);
	TestTrue(TEXT("nor is every stand being taken"),
		UOfferGenerator::IsPermanentRefusal(EArrivalRefusal::NoFreeStand) == false);

	TestTrue(TEXT("a runway too short IS permanent - the player must build"),
		UOfferGenerator::IsPermanentRefusal(EArrivalRefusal::RunwayTooShort));
	TestTrue(TEXT("so is a wingspan the pavement will not admit"),
		UOfferGenerator::IsPermanentRefusal(EArrivalRefusal::NotAdmitted));
	TestTrue(TEXT("so is no route to a stand"),
		UOfferGenerator::IsPermanentRefusal(EArrivalRefusal::NoRouteToStand));
	TestFalse(TEXT("and None is not a refusal at all"),
		UOfferGenerator::IsPermanentRefusal(EArrivalRefusal::None));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOfferGeneratorEtaTest,
	"AirportOps.Model.OfferGenerator.AnOfferCarriesAnEtaAndAnExpiry",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOfferGeneratorEtaTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Field = FieldWith(4500.0, Needing(0.0, 3000.0));
	UOfferGenerator* Generator = NewObject<UOfferGenerator>();

	FOfferCandidate Candidate;
	Candidate.Airframe = Needing(0.0, 3000.0);
	Candidate.Airframe.TurnaroundSeconds = 1800.0;
	Candidate.AirlineName = FText::FromString(TEXT("Meridian"));
	Candidate.TypeName = FText::FromString(TEXT("A320"));

	UFlight* Offer = Generator->MakeOffer(*Field, FVector2D::ZeroVector, {Candidate}, 1000.0, 1);
	TestNotNull(TEXT("an admissible fleet produces an offer"), Offer);
	if (Offer == nullptr) { return false; }

	TestEqual(TEXT("it is Offered, not Accepted"), Offer->Phase, EFlightPhase::Offered);
	TestEqual(TEXT("the ETA is the lead time out"),
		Offer->ArrivesAt, 1000.0 + Generator->LeadTimeSeconds, 1e-9);
	TestTrue(TEXT("the expiry is BEFORE the ETA, or an offer could lapse mid-approach"),
		Offer->ExpiresAt < Offer->ArrivesAt);
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
	URoadNetwork* Field = FieldWith(4500.0, Needing(0.0, 3000.0));
	UOfferGenerator* Generator = NewObject<UOfferGenerator>();
	Generator->LeadTimeSeconds = 300.0;
	Generator->OfferLifeSeconds = 9000.0;

	FOfferCandidate Candidate;
	Candidate.Airframe = Needing(0.0, 3000.0);

	UFlight* Offer = Generator->MakeOffer(*Field, FVector2D::ZeroVector, {Candidate}, 0.0, 1);
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
	// An empty inbox is the RIGHT answer for a field nothing can use - but it must be an
	// empty inbox with a log line, not silence: the two look identical on screen and the
	// fixes are opposite.
	URoadNetwork* Tiny = FieldWith(1800.0, Needing(0.0, 1200.0));
	UOfferGenerator* Generator = NewObject<UOfferGenerator>();

	FOfferCandidate TooBig;
	TooBig.Airframe = Needing(200000.0, 6500.0);

	TestNull(TEXT("nothing is offered rather than something unlandable"),
		Generator->MakeOffer(*Tiny, FVector2D::ZeroVector, {TooBig}, 0.0, 1));
	return true;
}

#endif
