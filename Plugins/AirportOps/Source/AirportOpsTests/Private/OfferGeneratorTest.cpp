#include "CoreMinimal.h"
#include "Content/AirsideSettings.h"
#include "Build/AnchorLink.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/AirlineDefinition.h"
#include "Model/Flight.h"
#include "Model/LandingRun.h"
#include "Model/OfferGenerator.h"
#include "Model/OpsSave.h"
#include "Model/Pricing.h"
#include "Model/RoadNetwork.h"
#include "Model/SimClock.h"
#include "Profiles/RoadProfile.h"
#include "Testing/AirsideTestGraph.h"

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
			For.Chassis.Ground, For.Climb, For.Approach) * FLandingRun::LandingMargin;
		const double Length = FMath::Max(Needed * 3.0, 60000.0);
		const FVector2D ExitAt(Length * 0.5, 0.0);

		URoadProfile* Runway = URoadProfile::MakeTransient(RunwayWidth, 1500.0, 450.0);
		Runway->bContinuousThroughJunctions = true;
		URoadProfile* Taxiway = TestProfiles::Taxiway();

		// SPLIT AT THE EXIT: a T-junction is what puts a guideline node on the centreline for
		// RunwayExitNodes to find - see ArrivalDispatchTest.
		const FRoadNodeId ThresholdNode = Net->AddNode(FVector2D::ZeroVector);
		const FRoadNodeId ExitNode = Net->AddNode(ExitAt);
		const FRoadNodeId FarNode = Net->AddNode(FVector2D(Length, 0.0));
		Net->AddStraightSegment(ThresholdNode, ExitNode, Runway);
		Net->AddStraightSegment(ExitNode, FarNode, Runway);

		const FRoadNodeId TaxiEnd = Net->AddNode(ExitAt + FVector2D(0.0, -20000.0));
		Net->AddStraightSegment(ExitNode, TaxiEnd, Taxiway);

		TestGraph::Derive(*Net);

		// Facing east (heading 0), so its lead-in casts west onto the taxiway.
		UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
		Net->PlaceEntity(Stand, Stand->Anchors, ExitAt + FVector2D(9000.0, -10000.0), 0.0,
			6000.0, Stand->PoseRole, Stand->Trucks);

		FAnchorLink::Build(*Net, UAirsideSettings::ResolveLargestServiceVehicle());
		return Net;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOfferGeneratorWidthTest,
	"AirportOps.Model.OfferGenerator.ANarrowRunwayIsNotOfferedAirliners",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOfferGeneratorWidthTest::RunTest(const FString& Parameters)
{
	// THE BUG OF 2026-09-11, pinned. M_Test's runway is 15 m wide and admits a 15 m
	// wingspan; every offer was an A320 at 35.8 m, so every Accept in the inbox was greyed
	// out and the player had no way to tell why. The old filter asked FAirsideCapability,
	// which knows the runway's LENGTH and the stands, and nothing about RunwayAdmission.
	const FAirframe Airliner = Needing(0.0, 3580.0);
	URoadNetwork* Narrow = FieldWith(1800.0, Airliner);   // an 18 m strip, as M_Test has

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
	// A SERVICE THE AIRPORT CANNOT GIVE IS SOFT (spec 2026-09-28 ruling 5): the offer is made,
	// the row says what is missing, and C scores it - the player's decision, not a filter's.
	TestFalse(TEXT("a stand whose service cannot work is not a permanent refusal"),
		UOfferGenerator::IsPermanentRefusal(EArrivalRefusal::NoStandServiceable));
	return true;
}

namespace
{
	/** An airline for these tests. Named uniquely: the generator keys its state by FName. */
	UAirlineDefinition* MakeAirline(double Peak, double Floor = 0.0, bool bIsFloor = false)
	{
		UAirlineDefinition* Airline = NewObject<UAirlineDefinition>(GetTransientPackage(),
			MakeUniqueObjectName(GetTransientPackage(), UAirlineDefinition::StaticClass(), TEXT("OfferTestAirline")));
		Airline->DisplayName = FText::FromString(TEXT("Test Air"));
		Airline->PeakOffersPerHour = Peak;
		Airline->FloorOffersPerHour = Floor;
		Airline->bIsFloor = bIsFloor;
		return Airline;
	}

	/** One game second per real second, starting at Hour - so a test's arithmetic reads plainly. */
	USimClock* ClockAt(double Hour)
	{
		USimClock* Clock = NewObject<USimClock>(GetTransientPackage());
		Clock->SetUniformDay(USimClock::SecondsPerDay);
		Clock->StartAtHour(Hour);
		return Clock;
	}

	FOfferCandidate Candidate(double Wingspan, const TCHAR* Type = TEXT("A320"))
	{
		FOfferCandidate Out;
		Out.Airframe = Needing(0.0, Wingspan);
		Out.Airframe.TurnaroundSeconds = 1800.0;
		Out.AirlineName = FText::FromString(TEXT("Test Air"));
		Out.TypeName = FText::FromString(Type);
		return Out;
	}

	FAirlineOffers Offering(UAirlineDefinition* Airline, TArray<FOfferCandidate> Fleet)
	{
		FAirlineOffers Out;
		Out.Airline = Airline;
		Out.Fleet = MoveTemp(Fleet);
		return Out;
	}

	/** Runs Minutes generator ticks, advancing the clock a minute after each. */
	TArray<UFlight*> RunMinutes(UOfferGenerator& Generator, const URoadNetwork& Net,
		TArrayView<const FAirlineOffers> Airlines, USimClock& Clock, int32 Minutes, int32 Pending = 0)
	{
		TArray<UFlight*> Out;
		int32 Id = 1;
		for (int32 Minute = 0; Minute < Minutes; ++Minute)
		{
			Out.Append(Generator.TickMinute(Net, FVector2D::ZeroVector, Airlines, Clock, Pending,
				[&Id]() { return Id++; }));
			Clock.Advance(UOfferGenerator::TickSeconds);
		}
		return Out;
	}

	UOfferGenerator* SeededGenerator(int32 Seed = 7)
	{
		UOfferGenerator* Generator = NewObject<UOfferGenerator>(GetTransientPackage());
		Generator->Stream.Initialize(Seed);
		return Generator;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOfferRateCurveTest, "AirportOps.Model.Offers.Rate.CurveInterpolates",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOfferRateCurveTest::RunTest(const FString& Parameters)
{
	UAirlineDefinition* Airline = MakeAirline(10.0);
	Airline->DemandCurve.Init(0.0, 24);
	Airline->DemandCurve[8] = 1.0;
	// BETWEEN HOURS, not stepped: a step on the hour would make 07:59 and 08:00 two different
	// airports, and the demand strip would draw a staircase.
	TestEqual(TEXT("halfway from 0 at 07:00 to 1 at 08:00 is half"), Airline->CurveAt(7.5 * 3600.0), 0.5, 1e-9);
	TestEqual(TEXT("so the rate is half the peak"),
		UOfferGenerator::RateAt(*Airline, 7.5 * 3600.0, true, 1.0), 5.0, 1e-9);
	Airline->DemandCurve[23] = 1.0;
	Airline->DemandCurve[8] = 0.0;
	Airline->DemandCurve[0] = 0.0;
	TestEqual(TEXT("23:30 interpolates toward midnight's weight, wrapping"), Airline->CurveAt(23.5 * 3600.0), 0.5, 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOfferRateFlatTest, "AirportOps.Model.Offers.Rate.FlatWhenEmpty",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOfferRateFlatTest::RunTest(const FString& Parameters)
{
	UAirlineDefinition* Airline = MakeAirline(6.0);
	TestEqual(TEXT("no curve authored is flat at the peak, not silent"),
		UOfferGenerator::RateAt(*Airline, 3.0 * 3600.0, false, 1.0), 6.0, 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOfferRateFeeTest, "AirportOps.Model.Offers.Rate.FeeScales",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOfferRateFeeTest::RunTest(const FString& Parameters)
{
	UAirlineDefinition* Airline = MakeAirline(6.0);
	TestEqual(TEXT("half the demand is half the rate - the lever's whole cost"),
		UOfferGenerator::RateAt(*Airline, 0.0, true, 0.5), 3.0, 1e-9);
	TestEqual(TEXT("a negative factor is none, not a negative rate"),
		UOfferGenerator::RateAt(*Airline, 0.0, true, -1.0), 0.0, 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOfferRateFloorTest, "AirportOps.Model.Offers.Rate.FloorHoldsInDaylightOnly",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOfferRateFloorTest::RunTest(const FString& Parameters)
{
	UAirlineDefinition* Club = MakeAirline(0.0, 1.0, true);
	TestEqual(TEXT("in daylight the floor holds even with no demand at all"),
		UOfferGenerator::RateAt(*Club, 10.0 * 3600.0, true, 0.1), 1.0, 1e-9);
	TestEqual(TEXT("at night the floor lets go - the lull is real"),
		UOfferGenerator::RateAt(*Club, 23.0 * 3600.0, false, 1.0), 0.0, 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOfferFlatRateTest, "AirportOps.Model.Offers.Generate.FlatRateOverTenHours",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOfferFlatRateTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Field = FieldWith(4500.0, Needing(0.0, 3000.0));
	UOfferGenerator* Generator = SeededGenerator();
	Generator->MaxPendingOffers = 100000;
	const TArray<FAirlineOffers> Airlines = { Offering(MakeAirline(6.0), { Candidate(3000.0) }) };
	USimClock* Clock = ClockAt(8.0);
	const int32 Count = RunMinutes(*Generator, *Field, Airlines, *Clock, 600).Num();
	TestTrue(FString::Printf(TEXT("six an hour for ten hours is about sixty (got %d)"), Count),
		Count >= 55 && Count <= 65);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOfferPeakTest, "AirportOps.Model.Offers.Generate.PeakBeatsTrough",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOfferPeakTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Field = FieldWith(4500.0, Needing(0.0, 3000.0));
	UAirlineDefinition* Airline = MakeAirline(60.0);
	Airline->DemandCurve.Init(0.1, 24);
	Airline->DemandCurve[8] = 1.0;
	const TArray<FAirlineOffers> Airlines = { Offering(Airline, { Candidate(3000.0) }) };

	UOfferGenerator* Generator = SeededGenerator();
	Generator->MaxPendingOffers = 100000;
	const int32 Peak = RunMinutes(*Generator, *Field, Airlines, *ClockAt(8.0), 60).Num();
	const int32 Trough = RunMinutes(*Generator, *Field, Airlines, *ClockAt(14.0), 60).Num();
	TestTrue(FString::Printf(TEXT("the morning peak is busier than the afternoon (%d vs %d)"), Peak, Trough),
		Peak > Trough * 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOfferFloorAtMaxFeeTest, "AirportOps.Model.Offers.Generate.FloorHoldsAtMaxFee",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOfferFloorAtMaxFeeTest::RunTest(const FString& Parameters)
{
	// "You should never be in a place where no one wants to use you" - not even at the
	// dearest landing fee the lever allows.
	URoadNetwork* Field = FieldWith(4500.0, Needing(0.0, 3000.0));
	UOfferGenerator* Generator = SeededGenerator();
	Generator->Pricing = NewObject<UPricing>(GetTransientPackage());
	for (int32 Step = 0; Step < 20; ++Step) { Generator->Pricing->StepLandingFee(+1); }
	const TArray<FAirlineOffers> Airlines = { Offering(MakeAirline(0.0, 1.0, true), { Candidate(3000.0) }) };
	const int32 Count = RunMinutes(*Generator, *Field, Airlines, *ClockAt(9.0), 180).Num();
	TestTrue(FString::Printf(TEXT("the club still comes, about once an hour (got %d in 3 h)"), Count), Count >= 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOfferClubQuietTest, "AirportOps.Model.Offers.Generate.ClubQuietAtNight",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOfferClubQuietTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Field = FieldWith(4500.0, Needing(0.0, 3000.0));
	UOfferGenerator* Generator = SeededGenerator();
	const TArray<FAirlineOffers> Airlines = { Offering(MakeAirline(0.0, 1.0, true), { Candidate(3000.0) }) };
	TestEqual(TEXT("nobody flies for fun at night"),
		RunMinutes(*Generator, *Field, Airlines, *ClockAt(21.0), 180).Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOfferNoBankedBurstTest, "AirportOps.Model.Offers.Generate.NoBankedBurst",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOfferNoBankedBurstTest::RunTest(const FString& Parameters)
{
	// SIX HOURS unable to come, then able: the demand those hours would have carried is gone,
	// not queued. A banked total would dump hundreds of offers into the inbox the minute the
	// player widened the runway.
	URoadNetwork* Narrow = FieldWith(1800.0, Needing(0.0, 3000.0));
	URoadNetwork* Wide = FieldWith(4500.0, Needing(0.0, 3000.0));
	UOfferGenerator* Generator = SeededGenerator();
	Generator->MaxPendingOffers = 100000;
	const TArray<FAirlineOffers> Airlines = { Offering(MakeAirline(60.0), { Candidate(3000.0) }) };
	USimClock* Clock = ClockAt(6.0);
	TestEqual(TEXT("nothing is offered while the strip is too narrow"),
		RunMinutes(*Generator, *Narrow, Airlines, *Clock, 360).Num(), 0);
	const int32 FirstMinute = RunMinutes(*Generator, *Wide, Airlines, *Clock, 1).Num();
	TestTrue(FString::Printf(TEXT("and the first minute after is one minute's worth (got %d)"), FirstMinute),
		FirstMinute <= 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOfferFullInboxTest, "AirportOps.Model.Offers.Generate.FullInboxDrops",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOfferFullInboxTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Field = FieldWith(4500.0, Needing(0.0, 3000.0));
	UOfferGenerator* Generator = SeededGenerator();
	const TArray<FAirlineOffers> Airlines = { Offering(MakeAirline(60.0), { Candidate(3000.0) }) };
	TestEqual(TEXT("a full inbox takes no more"),
		RunMinutes(*Generator, *Field, Airlines, *ClockAt(9.0), 10, Generator->MaxPendingOffers).Num(), 0);
	TestTrue(TEXT("and counts what it turned away, for C to read as unmet demand"), Generator->DroppedOffers > 0);

	UOfferGenerator* Partial = SeededGenerator();
	TestTrue(TEXT("one slot short of full takes at most one in a minute"),
		RunMinutes(*Partial, *Field, Airlines, *ClockAt(9.0), 1, Partial->MaxPendingOffers - 1).Num() <= 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOfferSameSeedTest, "AirportOps.Model.Offers.Generate.SameSeedSameSequence",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOfferSameSeedTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Field = FieldWith(4500.0, Needing(0.0, 3000.0));
	UAirlineDefinition* Airline = MakeAirline(30.0);
	Airline->CallsignPrefix = TEXT("G-????");
	const TArray<FAirlineOffers> Airlines = {
		Offering(Airline, { Candidate(3000.0, TEXT("A")), Candidate(2800.0, TEXT("B")) }) };

	auto Run = [&]()
	{
		UOfferGenerator* Generator = SeededGenerator(11);
		Generator->MaxPendingOffers = 100000;
		TArray<FString> Out;
		for (const UFlight* Each : RunMinutes(*Generator, *Field, Airlines, *ClockAt(8.0), 120))
		{
			Out.Add(Each->Callsign + TEXT("/") + Each->TypeName.ToString());
		}
		return Out;
	};
	const TArray<FString> First = Run();
	const TArray<FString> Second = Run();
	TestTrue(TEXT("there is something to compare"), First.Num() > 10);
	TestTrue(TEXT("same seed, same inputs, same offers - callsigns and types"), First == Second);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOfferFeeCadenceTest, "AirportOps.Model.Offers.Generate.FeeStepMovesCadence",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOfferFeeCadenceTest::RunTest(const FString& Parameters)
{
	// THE BUG THIS SPEC OPENED WITH: the demand factor was read once, at Attach, so the fee
	// lever changed the price of new offers and never their number. Read every tick, the
	// same airline at double the fee comes about half as often, straight away.
	URoadNetwork* Field = FieldWith(4500.0, Needing(0.0, 3000.0));
	UOfferGenerator* Generator = SeededGenerator();
	Generator->MaxPendingOffers = 100000;
	Generator->Pricing = NewObject<UPricing>(GetTransientPackage());
	const TArray<FAirlineOffers> Airlines = { Offering(MakeAirline(60.0), { Candidate(3000.0) }) };
	USimClock* Clock = ClockAt(8.0);
	const int32 AtPar = RunMinutes(*Generator, *Field, Airlines, *Clock, 60).Num();
	for (int32 Step = 0; Step < 10; ++Step) { Generator->Pricing->StepLandingFee(+1); }
	const int32 AtDouble = RunMinutes(*Generator, *Field, Airlines, *Clock, 60).Num();
	TestTrue(FString::Printf(TEXT("double the fee, far fewer offers the very next hour (%d then %d)"), AtPar, AtDouble),
		AtDouble < AtPar * 0.75);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOfferContractTest, "AirportOps.Model.Offers.Generate.ContractFormula",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOfferContractTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Field = FieldWith(4500.0, Needing(0.0, 3000.0));
	UOfferGenerator* Generator = SeededGenerator();
	Generator->TaxiAllowanceSeconds = 600.0;
	UAirlineDefinition* Airline = MakeAirline(1.0);
	Airline->LeadTimeSeconds = 900.0;
	Airline->TurnaroundSlack = 1.5;
	Airline->OfferWindowSeconds = 45.0;
	UFlight* Offer = Generator->MakeOffer(FVector2D::ZeroVector, *Airline, Candidate(3000.0), 1000.0, 3);
	if (!TestNotNull(TEXT("a candidate makes an offer"), Offer)) { return false; }
	TestEqual(TEXT("it is Offered"), Offer->Phase, EFlightPhase::Offered);
	TestEqual(TEXT("its id is the one it was given"), Offer->Id, 3);
	TestEqual(TEXT("the contract is lead + taxi + turnaround x slack"), Offer->ContractSeconds, 900.0 + 600.0 + 1800.0 * 1.5, 1e-9);
	TestEqual(TEXT("the window is the airline's, in real seconds"), Offer->OfferSecondsLeft, 45.0, 1e-9);
	TestEqual(TEXT("and remembered, so a row can draw how much is left"), Offer->OfferWindowSeconds, 45.0, 1e-9);
	TestEqual(TEXT("the lead time travels with the flight"), Offer->LeadTimeSeconds, 900.0, 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOfferCallsignTest, "AirportOps.Model.Offers.Generate.CallsignPattern",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOfferCallsignTest::RunTest(const FString& Parameters)
{
	FRandomStream Stream(3);
	const FString Tail = UOfferGenerator::MakeCallsign(TEXT("G-????"), Stream);
	TestEqual(TEXT("a tail pattern keeps its length"), Tail.Len(), 6);
	TestTrue(TEXT("and its prefix"), Tail.StartsWith(TEXT("G-")));
	bool bLetters = true;
	for (int32 I = 2; I < Tail.Len(); ++I) { bLetters &= FChar::IsUpper(Tail[I]); }
	TestTrue(TEXT("each ? became a capital letter"), bLetters);

	const FString Flight = UOfferGenerator::MakeCallsign(TEXT("CU"), Stream);
	TestTrue(FString::Printf(TEXT("a flight prefix gets a three-digit number (%s)"), *Flight),
		Flight.StartsWith(TEXT("CU ")) && Flight.Len() == 6 && Flight.Mid(3).IsNumeric());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOfferNothingFitsTest, "AirportOps.Model.Offers.Generate.AnAirportThatFitsNothingGetsNoOffers",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOfferNothingFitsTest::RunTest(const FString& Parameters)
{
	// An empty inbox is the RIGHT answer for a field nothing can use - the log line (on the
	// transition, see TickMinute) is what tells it from a generator that is not running.
	URoadNetwork* Tiny = FieldWith(1800.0, Needing(0.0, 1200.0));
	UOfferGenerator* Generator = SeededGenerator();
	FOfferCandidate TooBig;
	TooBig.Airframe = Needing(200000.0, 6500.0);
	const TArray<FAirlineOffers> Airlines = { Offering(MakeAirline(60.0), { TooBig }) };
	TestEqual(TEXT("nothing is offered rather than something unlandable"),
		RunMinutes(*Generator, *Tiny, Airlines, *ClockAt(9.0), 30).Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOfferGeneratorSaveTest, "AirportOps.Save.GeneratorSurvivesASave",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOfferGeneratorSaveTest::RunTest(const FString& Parameters)
{
	// A reload continues the SAME sequence: the stream, each airline's running total and its
	// next threshold all travel. Before snapshot v5 the generator was saved by nothing at all.
	URoadNetwork* Field = FieldWith(4500.0, Needing(0.0, 3000.0));
	UAirlineDefinition* Airline = MakeAirline(20.0);
	const TArray<FAirlineOffers> Airlines = { Offering(Airline, { Candidate(3000.0, TEXT("A")), Candidate(2800.0, TEXT("B")) }) };
	UOfferGenerator* Before = SeededGenerator(5);
	Before->MaxPendingOffers = 100000;
	USimClock* Clock = ClockAt(8.0);
	RunMinutes(*Before, *Field, Airlines, *Clock, 47);
	Before->DroppedOffers = 3;

	TArray<uint8> Bytes;
	OpsSave::SerializeObject(*Before, Bytes);
	UOfferGenerator* After = NewObject<UOfferGenerator>(GetTransientPackage());
	OpsSave::DeserializeObject(*After, Bytes);
	After->MaxPendingOffers = 100000;
	TestEqual(TEXT("the drop count travels"), After->DroppedOffers, 3);

	USimClock* ClockB = ClockAt(8.0);
	ClockB->Advance(Clock->Now() - ClockB->Now());
	TArray<FString> A, B;
	for (const UFlight* F : RunMinutes(*Before, *Field, Airlines, *Clock, 60)) { A.Add(F->Callsign + F->TypeName.ToString()); }
	for (const UFlight* F : RunMinutes(*After, *Field, Airlines, *ClockB, 60)) { B.Add(F->Callsign + F->TypeName.ToString()); }
	TestTrue(TEXT("there is a sequence to compare"), A.Num() > 5);
	TestTrue(TEXT("and the loaded generator continues it exactly"), A == B);
	return true;
}

#endif
