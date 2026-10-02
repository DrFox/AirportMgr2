#include "CoreMinimal.h"
#include "Content/AirsideSettings.h"
#include "Build/AnchorLink.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/AirlineDefinition.h"
#include "Model/Airport.h"
#include "Model/Flight.h"
#include "Model/LandingRun.h"
#include "Model/OfferGenerator.h"
#include "Model/OpsEventBus.h"
#include "Model/OpsSave.h"
#include "Model/Pricing.h"
#include "Model/RoadNetwork.h"
#include "Model/RunwayFacts.h"
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
	URoadNetwork* FieldWith(double RunwayWidth, const FAirframe& For, FRoadSegmentId* OutRunway = nullptr)
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
		const FRoadSegmentId Seed = Net->AddStraightSegment(ThresholdNode, ExitNode, Runway);
		Net->AddStraightSegment(ExitNode, FarNode, Runway);
		if (OutRunway != nullptr)
		{
			*OutRunway = Seed;
		}

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
	FString Sentence;
	TestFalse(TEXT("an airliner is not offered a strip too narrow for its wingspan"),
		UOfferGenerator::CouldEverAdmit(*Narrow, FVector2D::ZeroVector, Airliner, Why, Sentence));
	TestEqual(TEXT("and the reason is admission, not something vaguer"),
		Why, EArrivalRefusal::NotAdmitted);
	TestFalse(TEXT("and it comes with the plan's own sentence, figures and all (issue #396)"), Sentence.IsEmpty());

	TestTrue(TEXT("a light aircraft that fits the same strip still is"),
		UOfferGenerator::CouldEverAdmit(*Narrow, FVector2D::ZeroVector, Needing(0.0, 1200.0), Why, Sentence));
	TestTrue(TEXT("with no sentence, because nothing was refused"), Sentence.IsEmpty());
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
		ArrivalPlanner::IsPermanentRefusal(EArrivalRefusal::RunwayOccupied) == false);
	TestTrue(TEXT("nor is every stand being taken"),
		ArrivalPlanner::IsPermanentRefusal(EArrivalRefusal::NoFreeStand) == false);

	TestTrue(TEXT("a runway too short IS permanent - the player must build"),
		ArrivalPlanner::IsPermanentRefusal(EArrivalRefusal::RunwayTooShort));
	TestTrue(TEXT("so is a wingspan the pavement will not admit"),
		ArrivalPlanner::IsPermanentRefusal(EArrivalRefusal::NotAdmitted));
	TestTrue(TEXT("so is no route to a stand"),
		ArrivalPlanner::IsPermanentRefusal(EArrivalRefusal::NoRouteToStand));
	TestFalse(TEXT("and None is not a refusal at all"),
		ArrivalPlanner::IsPermanentRefusal(EArrivalRefusal::None));
	// A SERVICE THE AIRPORT CANNOT GIVE IS SOFT (spec 2026-09-28 ruling 5): the offer is made,
	// the row says what is missing, and C scores it - the player's decision, not a filter's.
	TestFalse(TEXT("a stand whose service cannot work is not a permanent refusal"),
		ArrivalPlanner::IsPermanentRefusal(EArrivalRefusal::NoStandServiceable));
	// A STAND IN A TAXIWAY'S CLEARANCE STRIP needs redrawing - building, not waiting - so no
	// airline is offered a flight it would refuse (strip spec 2026-09-28). It reaches the
	// generator's default branch; this pins that it stays there.
	TestTrue(TEXT("every fitting stand inside a strip is a permanent refusal"),
		ArrivalPlanner::IsPermanentRefusal(EArrivalRefusal::NoStandClearOfStrip));

	// EVERY REASON THE ENUM HAS, NAMED (#442): the question moved beside the enum and lost its `default: return true`, so a
	// reason added to EArrivalRefusal is a build error at the switch AND a count mismatch here - classified by a person, not
	// by falling into a default. The table is the rule: what needs the player to build or change something is permanent.
	struct FClass
	{
		EArrivalRefusal Why;
		bool bPermanent;
	};
	const FClass Classes[] = {
		{ EArrivalRefusal::None, false },
		{ EArrivalRefusal::NoRunway, true },
		{ EArrivalRefusal::RunwayTooShort, true },
		{ EArrivalRefusal::NoExit, true },
		{ EArrivalRefusal::NoRouteToStand, true },
		{ EArrivalRefusal::RunwayOccupied, false },
		{ EArrivalRefusal::NotAdmitted, true },
		{ EArrivalRefusal::NoFreeStand, false },
		{ EArrivalRefusal::NoStandBigEnough, true },
		{ EArrivalRefusal::NoStandPavedEnough, true },
		{ EArrivalRefusal::NoStandServiceable, false },
		{ EArrivalRefusal::GraphBeingEdited, false },
		{ EArrivalRefusal::NoStandClearOfStrip, true },
		{ EArrivalRefusal::TaxiwayTooNarrow, true },
		{ EArrivalRefusal::NoArrivalRunway, true },
	};
	const UEnum* Enum = StaticEnum<EArrivalRefusal>();
	if (TestNotNull(TEXT("the refusal enum reflects"), Enum))
	{
		TestEqual(TEXT("every refusal is classified above - a new one must be placed"),
			Enum->NumEnums() - 1 /* the _MAX entry */, static_cast<int32>(UE_ARRAY_COUNT(Classes)));
	}
	for (const FClass& Each : Classes)
	{
		TestEqual(*FString::Printf(TEXT("%s: is it one only the player can clear"), *UEnum::GetValueAsString(Each.Why)),
			ArrivalPlanner::IsPermanentRefusal(Each.Why), Each.bPermanent);
	}
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOfferRateAirlineFactorTest, "AirportOps.Model.Offers.Rate.AirlineFactorSparesTheFloor",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOfferRateAirlineFactorTest::RunTest(const FString& Parameters)
{
	UAirlineDefinition* Airline = MakeAirline(10.0);
	Airline->DemandCurve.Init(0.0, 24);
	Airline->DemandCurve[8] = 1.0;
	TestEqual(TEXT("a delighted airline (1.5x) offers half as often again"),
		UOfferGenerator::RateAt(*Airline, 7.5 * 3600.0, true, 1.0, 1.5), 7.5, 1e-9);
	TestEqual(TEXT("and the factor multiplies with the fee's, not instead of it"),
		UOfferGenerator::RateAt(*Airline, 7.5 * 3600.0, true, 0.5, 1.5), 3.75, 1e-9);

	UAirlineDefinition* Club = MakeAirline(4.0, 1.0, true);
	TestEqual(TEXT("an unhappy airline's demand falls - but the floor is not scaled by its mood, nor by the fee"),
		UOfferGenerator::RateAt(*Club, 10.0 * 3600.0, true, 0.1, 0.5), 1.0, 1e-9);
	TestEqual(TEXT("a negative factor is none, not a negative rate"),
		UOfferGenerator::RateAt(*Airline, 7.5 * 3600.0, true, 1.0, -1.0), 0.0, 1e-9);

	UOfferGenerator* Generator = NewObject<UOfferGenerator>(GetTransientPackage());
	TestEqual(TEXT("a generator nobody wired scales nothing"), Generator->AirlineFactor(*Airline), 1.0, 1e-9);
	Generator->AirlineFactorOf = [](const UAirlineDefinition&) { return 0.7; };
	TestEqual(TEXT("a wired one answers its reader"), Generator->AirlineFactor(*Airline), 0.7, 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOfferRateFleetShareTest, "AirportOps.Model.Offers.Rate.ScalesWithAdmissibleShare",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOfferRateFleetShareTest::RunTest(const FString& Parameters)
{
	// DEMAND SCALES WITH THE AIRPORT (spec 2026-10-02 §6): an airport that can take half an airline's
	// fleet gets half its flights, not all of them drawn from the half it can take. Two candidates on
	// a 45 m strip - one fits, one is twice too wide - so the share is exactly a half.
	URoadNetwork* Field = FieldWith(4500.0, Needing(0.0, 3000.0));
	const TArray<FAirlineOffers> Half = { Offering(MakeAirline(6.0),
		{ Candidate(3000.0, TEXT("Fits")), Candidate(9000.0, TEXT("TooWide")) }) };

	UOfferGenerator* Generator = SeededGenerator();
	Generator->MaxPendingOffers = 100000;
	TestEqual(TEXT("before any admission check the share is whole, so a fresh strip draws the full rate"),
		Generator->FleetShare(*Half[0].Airline), 1.0, 1e-9);
	const int32 Count = RunMinutes(*Generator, *Field, Half, *ClockAt(8.0), 600).Num();
	TestTrue(FString::Printf(TEXT("six an hour at half the fleet is about thirty in ten hours (got %d)"), Count),
		Count >= 24 && Count <= 36);
	TestEqual(TEXT("the share is the admissible half"), Generator->FleetShare(*Half[0].Airline), 0.5, 1e-9);
	TestEqual(TEXT("and every reader of the rate - strip, banner - sees it through AirlineFactor"),
		Generator->AirlineFactor(*Half[0].Airline), 0.5, 1e-9);

	// THE FLOOR IS NOT SCALED, for RateAt's reason: the club must never go quiet because the strip
	// cannot take one of its types.
	UAirlineDefinition* Club = MakeAirline(0.5, 1.0, true);
	const TArray<FAirlineOffers> HalfClub = { Offering(Club,
		{ Candidate(3000.0, TEXT("Fits")), Candidate(9000.0, TEXT("TooWide")) }) };
	UOfferGenerator* ClubGenerator = SeededGenerator();
	ClubGenerator->MaxPendingOffers = 100000;
	const int32 Floor = RunMinutes(*ClubGenerator, *Field, HalfClub, *ClockAt(8.0), 600).Num();
	TestTrue(FString::Printf(TEXT("the floor of one an hour holds at half the fleet: about ten (got %d)"), Floor),
		Floor >= 7 && Floor <= 13);
	return true;
}

namespace
{
	/** A candidate whose published landing field no 600 m test runway covers - refused by admission, whatever its wingspan. */
	FOfferCandidate NeedingARunwayOf(double FieldLength, double Wingspan, const TCHAR* Type)
	{
		FOfferCandidate Out = Candidate(Wingspan, Type);
		Out.Airframe.Requirements.LandingFieldLength = FieldLength;
		return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOfferFleetAdmissionMixedTest, "AirportOps.Model.Offers.FleetAdmission.EveryTypeHasAVerdict",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOfferFleetAdmissionMixedTest::RunTest(const FString& Parameters)
{
	// ONE LIST (the airlines panel, spec 2026-10-02 1.3): the pick, FleetShare, DescribeWhyNot and the panel's tick/cross rows all read
	// these verdicts, so a row can never disagree with what the generator actually does.
	URoadNetwork* Field = FieldWith(4500.0, Needing(0.0, 3000.0));
	UAirlineDefinition* Airline = MakeAirline(6.0);
	const TArray<FAirlineOffers> Airlines = { Offering(Airline, {
		Candidate(3000.0, TEXT("Fits")),
		NeedingARunwayOf(100000000.0, 3000.0, TEXT("TooLong")),
		Candidate(9000.0, TEXT("TooWide")) }) };
	UOfferGenerator* Generator = SeededGenerator();
	TestEqual(TEXT("nothing judged yet: no rows"), Generator->GetFleetAdmission(Airline->GetFName()).Num(), 0);

	RunMinutes(*Generator, *Field, Airlines, *ClockAt(9.0), 1);
	const TArray<FFleetAdmission> Rows = Generator->GetFleetAdmission(Airline->GetFName());
	if (!TestEqual(TEXT("one row per fleet type"), Rows.Num(), 3)) { return false; }
	TestEqual(TEXT("and no extra route search bought them"), Generator->AdmissionChecksForTest(), 3);
	TestEqual(TEXT("in fleet order"), Rows[0].TypeName.ToString(), FString(TEXT("Fits")));
	TestEqual(TEXT("in fleet order, second"), Rows[1].TypeName.ToString(), FString(TEXT("TooLong")));
	TestEqual(TEXT("in fleet order, third"), Rows[2].TypeName.ToString(), FString(TEXT("TooWide")));
	TestTrue(TEXT("the fitting type is admitted"), Rows[0].bAdmitted);
	TestEqual(TEXT("with no reason"), Rows[0].Why, EArrivalRefusal::None);
	TestTrue(TEXT("and no sentence"), Rows[0].Sentence.IsEmpty());
	for (int32 Index = 1; Index < 3; ++Index)
	{
		TestFalse(*FString::Printf(TEXT("row %d is a cross"), Index), Rows[Index].bAdmitted);
		TestTrue(*FString::Printf(TEXT("row %d has a permanent reason"), Index), ArrivalPlanner::IsPermanentRefusal(Rows[Index].Why));
		TestFalse(*FString::Printf(TEXT("row %d says why"), Index), Rows[Index].Sentence.IsEmpty());
	}
	TestEqual(TEXT("too long a landing field: refused by admission"), Rows[1].Why, EArrivalRefusal::NotAdmitted);
	TestEqual(TEXT("too wide a wingspan: refused by admission"), Rows[2].Why, EArrivalRefusal::NotAdmitted);
	TestTrue(TEXT("each cross has ITS OWN sentence"), Rows[1].Sentence != Rows[2].Sentence);
	TestEqual(TEXT("FleetShare reads the same list: one of three"), Generator->FleetShare(*Airline), 1.0 / 3.0, 1e-9);
	TestEqual(TEXT("DescribeWhyNot is the FIRST cross's sentence"), Generator->DescribeWhyNot(Airline->GetFName()), Rows[1].Sentence);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOfferFleetAdmissionAllRefusedTest, "AirportOps.Model.Offers.FleetAdmission.AllRefusedEachHasItsOwnReason",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOfferFleetAdmissionAllRefusedTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Field = FieldWith(4500.0, Needing(0.0, 3000.0));
	UAirlineDefinition* Airline = MakeAirline(6.0);
	const TArray<FAirlineOffers> Airlines = { Offering(Airline, {
		NeedingARunwayOf(100000000.0, 3000.0, TEXT("TooLong")),
		Candidate(9000.0, TEXT("TooWide")) }) };
	UOfferGenerator* Generator = SeededGenerator();
	TestEqual(TEXT("an airline nothing of whose fleet can come: no offers"),
		RunMinutes(*Generator, *Field, Airlines, *ClockAt(9.0), 30).Num(), 0);
	const TArray<FFleetAdmission> Rows = Generator->GetFleetAdmission(Airline->GetFName());
	if (!TestEqual(TEXT("both rows present"), Rows.Num(), 2)) { return false; }
	for (const FFleetAdmission& Row : Rows)
	{
		TestFalse(*FString::Printf(TEXT("%s is crossed"), *Row.TypeName.ToString()), Row.bAdmitted);
		TestFalse(*FString::Printf(TEXT("%s has its own sentence"), *Row.TypeName.ToString()), Row.Sentence.IsEmpty());
	}
	TestEqual(TEXT("too long: its reason"), Rows[0].Why, EArrivalRefusal::NotAdmitted);
	TestEqual(TEXT("too wide: its reason"), Rows[1].Why, EArrivalRefusal::NotAdmitted);
	TestTrue(TEXT("two crosses, two sentences"), Rows[0].Sentence != Rows[1].Sentence);
	TestEqual(TEXT("the share is zero"), Generator->FleetShare(*Airline), 0.0, 1e-9);
	TestEqual(TEXT("DescribeWhyNot is unchanged: the first row's sentence"),
		Generator->DescribeWhyNot(Airline->GetFName()), Rows[0].Sentence);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOfferFleetAdmissionEmptyTest, "AirportOps.Model.Offers.FleetAdmission.EmptyBeforeJudgement",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOfferFleetAdmissionEmptyTest::RunTest(const FString& Parameters)
{
	UOfferGenerator* Generator = SeededGenerator();
	TestEqual(TEXT("an airline never judged has no rows - the panel reads that as 'not judged yet', not as 'all crossed'"),
		Generator->GetFleetAdmission(TEXT("Nobody")).Num(), 0);
	TestEqual(TEXT("and no whyNot"), Generator->DescribeWhyNot(TEXT("Nobody")), FString());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOfferFleetAdmissionRebuildTest, "AirportOps.Model.Offers.FleetAdmission.CacheRebuildRefreshesVerdicts",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOfferFleetAdmissionRebuildTest::RunTest(const FString& Parameters)
{
	// A better airport: a different network, which is what the cache key sees first (a guideline bump re-keys it the same way -
	// AdmissionCachedOnGuidelineRevision). The type an 18 m strip refuses is taken by a 45 m one.
	URoadNetwork* Narrow = FieldWith(1800.0, Needing(0.0, 3000.0));
	URoadNetwork* Wide = FieldWith(4500.0, Needing(0.0, 3000.0));
	UAirlineDefinition* Airline = MakeAirline(6.0);
	const TArray<FAirlineOffers> Airlines = { Offering(Airline, { Candidate(1200.0, TEXT("Fits")), Candidate(3000.0, TEXT("Big")) }) };
	UOfferGenerator* Generator = SeededGenerator();
	USimClock* Clock = ClockAt(9.0);
	RunMinutes(*Generator, *Narrow, Airlines, *Clock, 1);
	TArray<FFleetAdmission> Rows = Generator->GetFleetAdmission(Airline->GetFName());
	if (!TestEqual(TEXT("two rows"), Rows.Num(), 2)) { return false; }
	TestFalse(TEXT("the big type is crossed on the narrow strip"), Rows[1].bAdmitted);

	RunMinutes(*Generator, *Wide, Airlines, *Clock, 1);
	Rows = Generator->GetFleetAdmission(Airline->GetFName());
	if (!TestEqual(TEXT("still two rows"), Rows.Num(), 2)) { return false; }
	TestTrue(TEXT("the big type is admitted once the airport can take it"), Rows[1].bAdmitted);
	TestTrue(TEXT("and its sentence is gone"), Rows[1].Sentence.IsEmpty());
	TestEqual(TEXT("and an admitted row carries no reason (Why == None), so the panel never reads one beside a tick"),
		Rows[1].Why, EArrivalRefusal::None);
	TestEqual(TEXT("and the share follows the same list"), Generator->FleetShare(*Airline), 1.0, 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOfferCurrentRateTest, "AirportOps.Model.Offers.Rate.CurrentRateIsWhatAccrues",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOfferCurrentRateTest::RunTest(const FString& Parameters)
{
	// THE PANEL'S "~N offers/h now" must be the number TickMinute accrues, not a second formula that drifts from it.
	URoadNetwork* Field = FieldWith(4500.0, Needing(0.0, 3000.0));
	UAirlineDefinition* Airline = MakeAirline(6.0);
	const TArray<FAirlineOffers> Airlines = { Offering(Airline,
		{ Candidate(3000.0, TEXT("Fits")), Candidate(9000.0, TEXT("TooWide")) }) };
	UOfferGenerator* Generator = SeededGenerator();
	Generator->AirlineFactorOf = [](const UAirlineDefinition&) { return 0.9; };
	TestEqual(TEXT("MoodFactor is the wired reader's answer"), Generator->MoodFactor(*Airline), 0.9, 1e-9);
	UOfferGenerator* Bare = SeededGenerator();
	TestEqual(TEXT("and 1.0 for a generator nobody wired"), Bare->MoodFactor(*Airline), 1.0, 1e-9);

	USimClock* Clock = ClockAt(9.0);
	// A threshold nothing reaches, so the accumulator holds the increment and no offer is drawn.
	Generator->States.FindOrAdd(Airline->GetFName()).Threshold = 1.0e9;
	Generator->TickMinute(*Field, FVector2D::ZeroVector, Airlines, *Clock, 0, []() { return 1; });
	TestEqual(TEXT("judged: half the fleet"), Generator->FleetShare(*Airline), 0.5, 1e-9);

	const double Expected = UOfferGenerator::RateAt(*Airline, Clock->TimeOfDay(), Clock->IsDaylight(),
		Generator->DemandFactor(), Generator->AirlineFactor(*Airline));
	const double Now = Generator->CurrentRate(*Airline, *Clock);
	TestEqual(TEXT("CurrentRate is RateAt at the clock's time, with demand and the airline's factor"), Now, Expected, 1e-9);
	TestEqual(TEXT("6 an hour x 0.9 mood x 0.5 share"), Now, 2.7, 1e-9);

	Generator->States.FindOrAdd(Airline->GetFName()).Accumulated = 0.0;
	Generator->TickMinute(*Field, FVector2D::ZeroVector, Airlines, *Clock, 0, []() { return 1; });
	TestEqual(TEXT("and one minute's accrual x 60 is that rate"),
		Generator->States.FindOrAdd(Airline->GetFName()).Accumulated * 60.0, Now, 1e-9);
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

// (AirportOps.Model.Offers.Generate.ClubQuietAtNight is AirportOps.Model.Offers.Generate.ZeroRateSkipsAdmission's second assertion now,
// #462 M16: the same airline, the same 21:00 clock and the same 180 minutes.)

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

	// 600 AN HOUR, NOT 60 (#463): at 60 an hour (one a minute) a single minute can never draw two offers - the draw needs the rate to cover two
	// thresholds, and the thresholds run 0.6-1.4 - so "at most one" held with the cap deleted, or with the check ignoring the offers already
	// made THIS minute (`PendingNow >= Max` for `PendingNow + Made.Num() >= Max`). At ten a minute the cap is the only thing between the
	// minute and ten offers, and EXACTLY one fits.
	UOfferGenerator* Partial = SeededGenerator();
	const TArray<FAirlineOffers> Busy = { Offering(MakeAirline(600.0), { Candidate(3000.0) }) };
	TestEqual(TEXT("one slot short of full takes exactly one in a minute that would have made ten"),
		RunMinutes(*Partial, *Field, Busy, *ClockAt(9.0), 1, Partial->MaxPendingOffers - 1).Num(), 1);
	TestTrue(TEXT("and turned the other nine away"), Partial->DroppedOffers > 0);
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOfferContractTest, "AirportOps.Model.Offers.Generate.ContractIsTheAirlines",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOfferContractTest::RunTest(const FString& Parameters)
{
	// ONE FIGURE, THE AIRLINE'S (2026-09-28, from play): the old lead + 10 min taxi +
	// turnaround x slack gave an SR22 40 game minutes, and it reached its stand with one left -
	// aircraft MOVE in real seconds while the clock runs ~21x, so landing and taxiing alone ate
	// the contract. How demanding an airline is, is now simply how long it gives.
	UOfferGenerator* Generator = SeededGenerator();
	UAirlineDefinition* Airline = MakeAirline(1.0);
	Airline->LeadTimeSeconds = 900.0;
	Airline->ContractSeconds = 7200.0;
	Airline->OfferWindowSeconds = 45.0;
	UFlight* Offer = Generator->MakeOffer(FVector2D::ZeroVector, *Airline, Candidate(3000.0), 1000.0, 3);
	if (!TestNotNull(TEXT("a candidate makes an offer"), Offer)) { return false; }
	TestEqual(TEXT("it is Offered"), Offer->GetPhase(), EFlightPhase::Offered);
	TestEqual(TEXT("its id is the one it was given"), Offer->Id, 3);
	TestEqual(TEXT("the contract is the airline's, whatever the airframe"), Offer->ContractSeconds, 7200.0, 1e-9);
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

// (AirportOps.Model.Offers.Generate.AnAirportThatFitsNothingGetsNoOffers is gone, #462 #35: the same empty-Admissible branch as
// Generate.NoBankedBurst's "nothing is offered while the strip is too narrow", reached by an aeroplane too big for a tiny field instead
// of a narrow strip - and it never asserted the log line its comment said tells an empty inbox from a generator that is not running.
// That the verdict IS announced when it moves is Offers.AdmissionChangeIsAnnounced's.)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOfferAdmissionAnnouncedTest, "AirportOps.Model.Offers.AdmissionChangeIsAnnounced",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOfferAdmissionAnnouncedTest::RunTest(const FString& Parameters)
{
	// #446/#445: an airline's "can it come?" was re-judged inside TickMinute and announced nothing, so the alerts pass looked again every offer
	// minute - about every frame at x32 - whether or not anything had moved. The generator announces the CHANGE: could and cannot, cannot and
	// can, and nothing in between.
	URoadNetwork* Narrow = FieldWith(1800.0, Needing(0.0, 3000.0));
	URoadNetwork* Wide = FieldWith(4500.0, Needing(0.0, 3000.0));
	UOfferGenerator* Generator = SeededGenerator();
	FOpsEventBus Bus;
	Generator->Bus = &Bus;
	TArray<FAirlineAdmissionChangedEvent> Seen;
	Bus.BeginWiring();
	Bus.Subscribe<FAirlineAdmissionChangedEvent>(EOpsTier::Presentation, TEXT("test"),
		[&Seen](const FAirlineAdmissionChangedEvent& E) { Seen.Add(E); });
	Bus.EndWiring();
	UAirlineDefinition* Airline = MakeAirline(6.0);
	const TArray<FAirlineOffers> Airlines = { Offering(Airline, { Candidate(3000.0, TEXT("A")) }) };
	USimClock* Clock = ClockAt(9.0);

	RunMinutes(*Generator, *Narrow, Airlines, *Clock, 3);
	Bus.Drain();
	if (!TestEqual(TEXT("an airline that cannot use the airport: announced ONCE over three minutes"), Seen.Num(), 1)) { return false; }
	TestTrue(TEXT("naming the airline, saying it cannot come, and why"),
		Seen[0].AirlineId == Airline->GetFName() && !Seen[0].bCouldCome && !Seen[0].Reason.IsEmpty());

	RunMinutes(*Generator, *Narrow, Airlines, *Clock, 3);
	Bus.Drain();
	TestEqual(TEXT("three more minutes of the same verdict announce nothing - the alerts pass is not woken for a repeat"), Seen.Num(), 1);

	RunMinutes(*Generator, *Wide, Airlines, *Clock, 1);
	Bus.Drain();
	if (TestEqual(TEXT("a field it can use: announced"), Seen.Num(), 2))
	{
		TestTrue(TEXT("as can-come, with no reason"), Seen[1].bCouldCome && Seen[1].Reason.IsEmpty());
	}
	RunMinutes(*Generator, *Narrow, Airlines, *Clock, 1);
	Bus.Drain();
	TestEqual(TEXT("and back to one it cannot: announced again"), Seen.Num(), 3);
	Generator->Bus = nullptr;
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

/**
 * REVIEW I2 (2026-09-28): the admissibility check is a full ArrivalPlanner::Plan per fleet
 * type, and it ran every GAME minute - ~40 times a real second at x32 at night, for a rate of
 * zero. Occupancy plays no part in it, so its answer can only move with the graph.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOfferAdmissionCachedTest, "AirportOps.Model.Offers.Generate.AdmissionCachedOnGuidelineRevision",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOfferAdmissionCachedTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Field = FieldWith(4500.0, Needing(0.0, 3000.0));
	UOfferGenerator* Generator = SeededGenerator();
	Generator->MaxPendingOffers = 100000;
	const TArray<FAirlineOffers> Airlines = {
		Offering(MakeAirline(6.0), { Candidate(3000.0, TEXT("A")), Candidate(2800.0, TEXT("B")) }) };
	RunMinutes(*Generator, *Field, Airlines, *ClockAt(9.0), 60);
	TestEqual(TEXT("an hour on an unchanged graph asks each type once"), Generator->AdmissionChecksForTest(), 2);

	Field->AddGuidelineNode(FVector2D(900000.0, 900000.0), /*bDerived*/ false);
	RunMinutes(*Generator, *Field, Airlines, *ClockAt(10.0), 1);
	TestEqual(TEXT("an edit to the graph asks again"), Generator->AdmissionChecksForTest(), 4);
	return true;
}

/**
 * #446's TRAP, PINNED: "optimising a runway flip to Markings silently stales admission". A runway's facts (its surface,
 * its approach, the end in use) are what RunwayAdmission judges an airframe by, and the admission cache keys on
 * GuidelineRevision. Until #446 URoadNetwork::SetRunwayFacts moved no clock, and the cache saw a flip only because the
 * facade escalated it to a Topology rebuild that re-made the graph. The facade notifies Facts now - no rebuild re-makes
 * anything - so the flip is written here STRAIGHT INTO THE MODEL, with no rebuild at all: the model's own bump
 * (NoteFactChanged) is the whole of what the cache hears. Delete that bump and this goes red.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOfferRunwayFlipAsksAgainTest, "AirportOps.Model.Offers.Generate.RunwayFlipAsksAgain",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOfferRunwayFlipAsksAgainTest::RunTest(const FString& Parameters)
{
	FRoadSegmentId Runway;
	URoadNetwork* Field = FieldWith(4500.0, Needing(0.0, 3000.0), &Runway);
	UOfferGenerator* Generator = SeededGenerator();
	Generator->MaxPendingOffers = 100000;
	const TArray<FAirlineOffers> Airlines = {
		Offering(MakeAirline(6.0), { Candidate(3000.0, TEXT("A")), Candidate(2800.0, TEXT("B")) }) };
	RunMinutes(*Generator, *Field, Airlines, *ClockAt(9.0), 60);
	TestEqual(TEXT("control: an hour on an unchanged airport asks each type once"), Generator->AdmissionChecksForTest(), 2);

	FRunwayFacts Concrete;
	Concrete.Surface = EPavement::Concrete;
	if (!TestTrue(TEXT("setup: the runway is reclassified"), Field->SetRunwayFacts(Runway, Concrete))) { return false; }
	RunMinutes(*Generator, *Field, Airlines, *ClockAt(10.0), 1);
	TestEqual(TEXT("a runway flip - a fact, with no graph re-made - asks every type again (#446)"),
		Generator->AdmissionChecksForTest(), 4);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOfferZeroRateSkipsTest, "AirportOps.Model.Offers.Generate.ZeroRateSkipsAdmission",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOfferZeroRateSkipsTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Field = FieldWith(4500.0, Needing(0.0, 3000.0));
	UOfferGenerator* Generator = SeededGenerator();
	const TArray<FAirlineOffers> Airlines = { Offering(MakeAirline(0.0, 1.0, true), { Candidate(3000.0) }) };
	// A CLUB: no demand, a daylight floor - and at 21:00 the floor lets go (ClubQuietAtNight's sentence, merged here, #462 M16).
	TestEqual(TEXT("nobody flies for fun at night"),
		RunMinutes(*Generator, *Field, Airlines, *ClockAt(21.0), 180).Num(), 0);
	TestEqual(TEXT("an airline with nothing to offer this minute costs no route search"),
		Generator->AdmissionChecksForTest(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOfferNothingUnlessOpenTest, "AirportOps.Model.Offers.Generate.NothingUnlessOpen",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOfferNothingUnlessOpenTest::RunTest(const FString& Parameters)
{
	// SPEC 2026-09-29-ops-batch3 §3: offers only while Open, and while not, NO airline is judged - so a runway-less
	// field raises no AirlineCannotCome alert and no toast per airline. Each case a fresh generator on the same field.
	URoadNetwork* Field = FieldWith(4500.0, Needing(0.0, 3000.0));
	const TArray<FAirlineOffers> Airlines = { Offering(MakeAirline(6.0), { Candidate(3000.0) }) };

	auto Run = [&](UAirport* Airport, int32& OutChecks, bool& OutJudged)
	{
		UOfferGenerator* Generator = SeededGenerator();
		Generator->MaxPendingOffers = 100000;
		Generator->Airport = Airport;
		const int32 Made = RunMinutes(*Generator, *Field, Airlines, *ClockAt(9.0), 120).Num();
		OutChecks = Generator->AdmissionChecksForTest();
		OutJudged = Generator->States.Num() > 0;
		return Made;
	};

	int32 Checks = 0;
	bool bJudged = false;
	UAirport* Open = NewObject<UAirport>(GetTransientPackage());
	Open->Reseat(*Field);
	TestTrue(TEXT("CONTROL: open, two hours at six an hour makes offers"), Run(Open, Checks, bJudged) > 0);
	TestTrue(TEXT("CONTROL: and judges the airline"), Checks > 0 && bJudged);

	UAirport* Closed = NewObject<UAirport>(GetTransientPackage());
	Closed->SetClosedByPlayer(true, *Field);
	TestEqual(TEXT("closed by the player: no offers"), Run(Closed, Checks, bJudged), 0);
	TestEqual(TEXT("and no route search"), Checks, 0);
	TestFalse(TEXT("and no airline's state touched"), bJudged);

	UAirport* Runwayless = NewObject<UAirport>(GetTransientPackage());
	Runwayless->Reseat(*NewObject<URoadNetwork>(GetTransientPackage()));
	TestEqual(TEXT("no runway: no offers"), Run(Runwayless, Checks, bJudged), 0);
	TestEqual(TEXT("and no route search"), Checks, 0);
	TestFalse(TEXT("and no airline's state touched"), bJudged);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOfferArrivalsOnlyTest, "AirportOps.Model.Offers.Generate.ArrivalsOnlyFieldGetsNoneUntilMixed",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOfferArrivalsOnlyTest::RunTest(const FString& Parameters)
{
	// #456 REVIEW: ONE RUNWAY, ARRIVALS ONLY. Anything that lands could never leave, so the planner refuses every type
	// (NotAdmitted, worded as the departure) and no airline is offered a flight the player could only strand. Flipped to
	// mixed, the offers resume: the refusal was the runway's use, which the admission cache must see change.
	FRoadSegmentId Runway;
	URoadNetwork* Field = FieldWith(4500.0, Needing(0.0, 3000.0), &Runway);
	FRunwayFacts Facts = Field->RunwayFactsFor(Runway);
	Facts.Use = ERunwayUse::ArrivalsOnly;
	if (!TestTrue(TEXT("the runway takes its use"), Field->SetRunwayFacts(Runway, Facts))) { return false; }
	// THE FACADE'S TOPOLOGY NOTIFY re-derives the graph on a facts change in play, stand links included; this
	// model-level field does both by hand (a derive alone drops the anchor link FieldWith built).
	TestGraph::Derive(*Field);
	FAnchorLink::Build(*Field, UAirsideSettings::ResolveLargestServiceVehicle());

	UOfferGenerator* Generator = SeededGenerator();
	Generator->MaxPendingOffers = 100000;
	const TArray<FAirlineOffers> Airlines = { Offering(MakeAirline(6.0), { Candidate(3000.0) }) };
	TestEqual(TEXT("arrivals only: two hours at six an hour make no offer"),
		RunMinutes(*Generator, *Field, Airlines, *ClockAt(9.0), 120).Num(), 0);
	EArrivalRefusal Why = EArrivalRefusal::None;
	FString Sentence;   // not read here: the reason is what this case is about
	TestFalse(TEXT("because the type could never be admitted"),
		UOfferGenerator::CouldEverAdmit(*Field, FVector2D::ZeroVector, Candidate(3000.0).Airframe, Why, Sentence));
	TestEqual(TEXT("for the runway's use, not something vaguer"), Why, EArrivalRefusal::NotAdmitted);

	const uint32 Before = Field->GetGuidelineRevision();
	Facts.Use = ERunwayUse::Mixed;
	if (!TestTrue(TEXT("the runway takes its use back"), Field->SetRunwayFacts(Runway, Facts))) { return false; }
	TestGraph::Derive(*Field);
	FAnchorLink::Build(*Field, UAirsideSettings::ResolveLargestServiceVehicle());
	TestNotEqual(TEXT("(the flip moves the guideline revision the admission cache is keyed on)"), Field->GetGuidelineRevision(), Before);
	TestTrue(TEXT("mixed: the offers resume"), RunMinutes(*Generator, *Field, Airlines, *ClockAt(11.0), 120).Num() > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOfferFuelLoadTest, "AirportOps.Model.Offers.Generate.FuelLoadIsHalfToNineTenths",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOfferFuelLoadTest::RunTest(const FString& Parameters)
{
	// THE LOAD IS FIXED AT THE OFFER (spec 2026-09-28-fuel-litres section 2), from the seeded
	// stream: 50-90% of the tank, so the row can show the size of the job before the accept.
	UOfferGenerator* Generator = SeededGenerator();
	UAirlineDefinition* Airline = MakeAirline(1.0);
	FOfferCandidate Tanked = Candidate(3000.0);
	Tanked.Airframe.FuelCapacityLitres = 1000.0;
	double Lowest = 1.0e9, Highest = 0.0;
	for (int32 Index = 0; Index < 200; ++Index)
	{
		const UFlight* Offer = Generator->MakeOffer(FVector2D::ZeroVector, *Airline, Tanked, 0.0, Index + 1);
		Lowest = FMath::Min(Lowest, Offer->FuelLitres);
		Highest = FMath::Max(Highest, Offer->FuelLitres);
	}
	TestTrue(FString::Printf(TEXT("never under half the tank (lowest %.0f)"), Lowest), Lowest >= 500.0);
	TestTrue(FString::Printf(TEXT("never over nine tenths (highest %.0f)"), Highest), Highest <= 900.0);
	TestTrue(TEXT("and it varies"), Highest - Lowest > 200.0);
	FOfferCandidate NoTank = Candidate(3000.0);
	TestEqual(TEXT("a type with no tank asks for nothing"),
		Generator->MakeOffer(FVector2D::ZeroVector, *Airline, NoTank, 0.0, 999)->FuelLitres, 0.0, 1e-9);
	return true;
}

#endif
