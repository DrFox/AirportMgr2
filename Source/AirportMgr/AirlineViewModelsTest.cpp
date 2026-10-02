#include "CoreMinimal.h"
#include "AirlineViewModels.h"
#include "ArrivalViewModels.h"
#include "Misc/AutomationTest.h"
#include "Model/AirlineDefinition.h"
#include "Model/AirlineHistory.h"
#include "Model/AirlineRoster.h"
#include "Model/Flight.h"
#include "Model/FlightBoard.h"
#include "Model/OfferGenerator.h"
#include "Model/OpsEventBus.h"
#include "Model/SimClock.h"
#include "OfferViewModels.h"
#include "Present/OpsRuntime.h"
#include "Testing/AirsideTestGraph.h"

#if WITH_DEV_AUTOMATION_TESTS

// THE AIRLINES PANEL'S VIEW MODELS (spec 2026-10-02-airlines-panel section 2), world-free: a roster wired to a history the way
// UOpsRuntime::Attach wires them, driven through the roster's own handlers so every figure is one the game would produce.
// Prefixed names: the module is a unity build.

namespace
{
	/** An airline definition; its object name is the roster id, as UOpsRuntime::SeedAirlines keys it. */
	UAirlineDefinition* AirlinesVmAirline(const TCHAR* Display, bool bFloor = false)
	{
		UAirlineDefinition* Airline = NewObject<UAirlineDefinition>(GetTransientPackage(),
			MakeUniqueObjectName(GetTransientPackage(), UAirlineDefinition::StaticClass(), TEXT("AirlinesVmAirline")));
		Airline->DisplayName = FText::FromString(Display);
		Airline->PeakOffersPerHour = 6.0;
		Airline->bIsFloor = bFloor;
		return Airline;
	}

	FOfferCandidate AirlinesVmCandidate(double Wingspan, const TCHAR* Type)
	{
		FOfferCandidate Out;
		Out.Airframe.Wingspan = Wingspan;
		Out.Airframe.TurnaroundSeconds = 1800.0;
		Out.AirlineName = FText::FromString(TEXT("Test Air"));
		Out.TypeName = FText::FromString(Type);
		return Out;
	}

	/** One game second per real second from Hour - the generator tests' clock. */
	USimClock* AirlinesVmClock(double Hour)
	{
		USimClock* Clock = NewObject<USimClock>(GetTransientPackage());
		Clock->SetUniformDay(USimClock::SecondsPerDay);
		Clock->StartAtHour(Hour);
		return Clock;
	}

	/** A roster and its history, wired as UOpsRuntime wires them (no bus: Apply checks), and the catalog list beside them. */
	struct FAirlinesVmFixture
	{
		UAirlineRoster* Roster = nullptr;
		UAirlineHistory* History = nullptr;
		TArray<FAirlineOffers> Airlines;

		FAirlinesVmFixture()
		{
			Roster = NewObject<UAirlineRoster>(GetTransientPackage());
			History = NewObject<UAirlineHistory>(GetTransientPackage());
			Roster->History = History;
		}

		FName Add(UAirlineDefinition* Airline, TArray<FOfferCandidate> Fleet = TArray<FOfferCandidate>())
		{
			FAirlineOffers Entry;
			Entry.Airline = Airline;
			Entry.Fleet = MoveTemp(Fleet);
			Airlines.Add(MoveTemp(Entry));
			Roster->Ensure(Airline->GetFName());
			return Airline->GetFName();
		}

		/** Off blocks LateBy seconds late (<= 0: on time). */
		void OffBlocks(FName Id, double LateBy) { Roster->OnFlightOffBlocks(FFlightOffBlocksEvent{ 1, Id, LateBy }); }
		void Ignored(FName Id) { Roster->OnOfferExpired(FOfferExpiredEvent{ 1, Id, ELapseReason::Ignored, false }); }
		void EndDay() { Roster->OnDayEnded(FDayEndedEvent{ 1 }); }

		FAirlinePanelSources Sources() const
		{
			FAirlinePanelSources Out;
			Out.Roster = Roster;
			Out.History = History;
			Out.Airlines = Airlines;
			return Out;
		}
	};

	const FAirlineListRow* AirlinesVmRow(const TArray<FAirlineListRow>& Rows, FName Id)
	{
		return Rows.FindByPredicate([Id](const FAirlineListRow& Row) { return Row.AirlineId == Id; });
	}

	UFlight* AirlinesVmFlight(const TCHAR* Callsign, FName AirlineId, EFlightPhase Phase)
	{
		UFlight* Out = NewObject<UFlight>(GetTransientPackage());
		Out->Callsign = Callsign;
		Out->AirlineId = AirlineId;
		Out->TypeName = FText::FromString(TEXT("Saab 340B"));
		Out->SetPhaseForTest(Phase);
		return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirlinesListSortAndTrendTest, "AirportMgr.Airlines.List.SortAndTrend",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAirlinesListSortAndTrendTest::RunTest(const FString&)
{
	FAirlinesVmFixture F;
	const FName Meridian = F.Add(AirlinesVmAirline(TEXT("Meridian")));
	const FName Alpha = F.Add(AirlinesVmAirline(TEXT("Alpha Air")));
	const FName Club = F.Add(AirlinesVmAirline(TEXT("Wings Club"), /*bFloor=*/true));
	const FName Bravo = F.Add(AirlinesVmAirline(TEXT("Bravo")));

	// NO YESTERDAY, NO DIRECTION: a move on the first day is not a trend - there is no close to compare it with.
	F.OffBlocks(Meridian, 0.0);
	{
		const TArray<FAirlineListRow> Rows = UAirlineListViewModel::BuildRows(F.Sources());
		const FAirlineListRow* Row = AirlinesVmRow(Rows, Meridian);
		if (!TestNotNull(TEXT("a row per standing"), Row)) { return false; }
		TestEqual(TEXT("first day: Flat, whatever moved"), Row->Trend, EAirlineTrend::Flat);
		TestEqual(TEXT("the percentage is the standing's"), Row->SatisfactionPct, 53);
	}

	F.EndDay();
	F.OffBlocks(Alpha, 0.0);              // +3 points: Up
	F.OffBlocks(Meridian, 3600.0);        // -10 points (the cap) against last night's close: Down
	F.Roster->Tuning.OnTimeBonus = 0.004;
	F.OffBlocks(Bravo, 0.0);              // +0.4 points: moved, but prints as the same percent - Flat

	const TArray<FAirlineListRow> Rows = UAirlineListViewModel::BuildRows(F.Sources());
	if (!TestEqual(TEXT("one row per standing"), Rows.Num(), 4)) { return false; }
	TestEqual(TEXT("the floor airline first - it is what keeps the airport alive"), Rows[0].AirlineId, Club);
	TestEqual(TEXT("then by name: Alpha"), Rows[1].AirlineId, Alpha);
	TestEqual(TEXT("then by name: Bravo"), Rows[2].AirlineId, Bravo);
	TestEqual(TEXT("then by name: Meridian"), Rows[3].AirlineId, Meridian);
	TestTrue(TEXT("the floor row says so"), Rows[0].bFloor);
	TestFalse(TEXT("and no other does"), Rows[1].bFloor || Rows[2].bFloor || Rows[3].bFloor);
	TestEqual(TEXT("the name is the definition's display name"), Rows[1].Name.ToString(), FString(TEXT("Alpha Air")));

	TestEqual(TEXT("Alpha gained today: Up"), Rows[1].Trend, EAirlineTrend::Up);
	TestEqual(TEXT("Meridian lost today: Down"), Rows[3].Trend, EAirlineTrend::Down);
	TestEqual(TEXT("Bravo moved under half a point: Flat"), Rows[2].Trend, EAirlineTrend::Flat);
	TestEqual(TEXT("the club did nothing: Flat"), Rows[0].Trend, EAirlineTrend::Flat);
	TestEqual(TEXT("Alpha at 53%"), Rows[1].SatisfactionPct, 53);
	// 0.53, a fifth of the way home overnight (0.524), then the capped -0.10.
	TestEqual(TEXT("Meridian at 42%"), Rows[3].SatisfactionPct, 42);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirlinesDetailNoHistoryYetTest, "AirportMgr.Airlines.Detail.NoHistoryYet",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAirlinesDetailNoHistoryYetTest::RunTest(const FString&)
{
	// REVIEW FOCUS 1: a fresh game's first minute - seeded, nothing happened, nothing judged. The pane must say "no history yet" and
	// "not judged yet", not draw a 0% line or a fleet of crosses.
	FAirlinesVmFixture F;
	const FName Alpha = F.Add(AirlinesVmAirline(TEXT("Alpha Air")));
	FAirlinePanelSources Sources = F.Sources();
	Sources.Generator = NewObject<UOfferGenerator>(GetTransientPackage());
	Sources.Board = NewObject<UFlightBoard>(GetTransientPackage());
	Sources.Clock = AirlinesVmClock(9.0);

	const FAirlineDetail Fresh = UAirlineDetailViewModel::Build(Sources, Alpha, Sources.Clock->Now());
	TestEqual(TEXT("the name"), Fresh.Name.ToString(), FString(TEXT("Alpha Air")));
	TestEqual(TEXT("the percentage is the start, not 0"), Fresh.SatisfactionPct, static_cast<int32>(FMath::RoundToInt(F.Roster->Tuning.Start * 100.0)));
	TestFalse(TEXT("no history yet"), Fresh.bHasHistory);
	TestEqual(TEXT("so no trend points"), Fresh.Trend.Num(), 0);
	TestEqual(TEXT("and no causes"), Fresh.Tallies.Num(), 0);
	TestFalse(TEXT("not judged yet"), Fresh.bJudged);
	TestEqual(TEXT("so no fleet rows - never a fleet of crosses"), Fresh.Fleet.Num(), 0);
	TestTrue(TEXT("the factor line says the fleet is unjudged"), Fresh.FactorLine.ToString().Contains(TEXT("not judged yet")));
	TestEqual(TEXT("no offers"), Fresh.Offers.Num(), 0);
	TestEqual(TEXT("no flights"), Fresh.Flights.Num(), 0);

	// TODAY'S PARTIAL TALLY before the first close (spec section 2's empty state): still no history, but the cause shows.
	F.OffBlocks(Alpha, 0.0);
	const FAirlineDetail Partial = UAirlineDetailViewModel::Build(Sources, Alpha, Sources.Clock->Now());
	TestFalse(TEXT("one open day is not history"), Partial.bHasHistory);
	TestEqual(TEXT("and draws no trend"), Partial.Trend.Num(), 0);
	if (TestEqual(TEXT("today's cause is listed"), Partial.Tallies.Num(), 1))
	{
		TestEqual(TEXT("with its sign"), Partial.Tallies[0].DeltaText.ToString(), FString(TEXT("+3%")));
	}

	// AN AIRLINE NOTHING KNOWS, from empty sources: no crash, nothing claimed.
	const FAirlineDetail Ghost = UAirlineDetailViewModel::Build(FAirlinePanelSources(), TEXT("Ghost"), 0.0);
	TestFalse(TEXT("a ghost has no history"), Ghost.bHasHistory);
	TestFalse(TEXT("and is not judged"), Ghost.bJudged);
	TestEqual(TEXT("and has no causes"), Ghost.Tallies.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirlinesDetailTallyOrderTest, "AirportMgr.Airlines.Detail.TallyOrderAndText",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAirlinesDetailTallyOrderTest::RunTest(const FString&)
{
	FAirlinesVmFixture F;
	const FName Alpha = F.Add(AirlinesVmAirline(TEXT("Alpha Air")));
	F.Roster->Tuning.DailyDriftFraction = 1.0;   // the whole way home: a drift (+4) larger than the ignored offer (-2)

	F.OffBlocks(Alpha, 0.0);                     // +3
	F.OffBlocks(Alpha, 0.0);                     // +3
	F.OffBlocks(Alpha, 3600.0);                  // -10 (the cap): 0.46
	F.EndDay();                                  // drift +4: closes at 0.50
	F.Ignored(Alpha);                            // -2 today: 0.48

	const FAirlineDetail D = UAirlineDetailViewModel::Build(F.Sources(), Alpha, 0.0);
	TestTrue(TEXT("a closed day is history"), D.bHasHistory);
	if (TestEqual(TEXT("two days, oldest first, today's running value last"), D.Trend.Num(), 2))
	{
		TestEqual(TEXT("yesterday closed at 50%"), D.Trend[0], 0.5, 1e-9);
		TestEqual(TEXT("today runs at 48%"), D.Trend[1], 0.48, 1e-9);
	}
	if (!TestEqual(TEXT("one row per cause seen in the week"), D.Tallies.Num(), 4)) { return false; }
	const EAirlineSatisfactionCause Expected[] = { EAirlineSatisfactionCause::LateOffStand, EAirlineSatisfactionCause::OnTime,
		EAirlineSatisfactionCause::OfferIgnored, EAirlineSatisfactionCause::DailyDrift };
	const TCHAR* ExpectedText[] = { TEXT("-10%"), TEXT("+6%"), TEXT("-2%"), TEXT("+4%") };
	const int32 ExpectedCount[] = { 1, 2, 1, 1 };
	for (int32 Index = 0; Index < 4; ++Index)
	{
		const FAirlineTallyRow& Row = D.Tallies[Index];
		const FString Why = FString::Printf(TEXT("row %d: %s"), Index, *UAirlineDetailViewModel::CauseLabel(Expected[Index]).ToString());
		TestEqual(*(Why + TEXT(" - largest |delta| first, drift LAST though it outweighs the ignored offer")),
			Row.Label.ToString(), UAirlineDetailViewModel::CauseLabel(Expected[Index]).ToString());
		TestEqual(*(Why + TEXT(" - signed whole percent")), Row.DeltaText.ToString(), FString(ExpectedText[Index]));
		TestEqual(*(Why + TEXT(" - count")), Row.Count, ExpectedCount[Index]);
	}
	TestEqual(TEXT("the sum is the clamped figure, kept unrounded"), D.Tallies[1].SumDelta, 0.06, 1e-9);

	TestEqual(TEXT("+18%"), UAirlineDetailViewModel::DescribeDelta(0.18).ToString(), FString(TEXT("+18%")));
	TestEqual(TEXT("-6%"), UAirlineDetailViewModel::DescribeDelta(-0.06).ToString(), FString(TEXT("-6%")));
	TestEqual(TEXT("nothing: 0%, unsigned"), UAirlineDetailViewModel::DescribeDelta(0.0).ToString(), FString(TEXT("0%")));
	TestEqual(TEXT("a loss that rounds to nothing is no -0%"), UAirlineDetailViewModel::DescribeDelta(-0.004).ToString(), FString(TEXT("0%")));

	// EVERY CAUSE HAS ITS OWN LABEL - the switch is exhaustive at build time; this catches an empty or a copied one.
	const UEnum* Enum = StaticEnum<EAirlineSatisfactionCause>();
	TSet<FString> Seen;
	for (int32 Index = 0; Index < Enum->NumEnums() - 1; ++Index)
	{
		const FString Label = UAirlineDetailViewModel::CauseLabel(static_cast<EAirlineSatisfactionCause>(Enum->GetValueByIndex(Index))).ToString();
		TestFalse(*FString::Printf(TEXT("%s has a label"), *Enum->GetNameStringByIndex(Index)), Label.IsEmpty());
		TestFalse(*FString::Printf(TEXT("%s's label is its own"), *Enum->GetNameStringByIndex(Index)), Seen.Contains(Label));
		Seen.Add(Label);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirlinesDetailFleetRowsTest, "AirportMgr.Airlines.Detail.FleetRows",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAirlinesDetailFleetRowsTest::RunTest(const FString&)
{
	// THE GENERATOR'S OWN VERDICTS, judged on a real field: one type that fits, one far too wide for anything here.
	FAirframe Fits;
	Fits.Wingspan = 3400.0;
	const FTestAirport Field = FTestAirport::Build(Fits);
	FAirlinesVmFixture F;
	UAirlineDefinition* Airline = AirlinesVmAirline(TEXT("Alpha Air"));
	const FName Alpha = F.Add(Airline, { AirlinesVmCandidate(3400.0, TEXT("Fits")), AirlinesVmCandidate(60000.0, TEXT("TooWide")) });
	UOfferGenerator* Generator = NewObject<UOfferGenerator>(GetTransientPackage());
	Generator->Stream.Initialize(7);
	USimClock* Clock = AirlinesVmClock(9.0);
	int32 Id = 1;
	Generator->TickMinute(*Field.Net, Field.Threshold, F.Airlines, *Clock, 0, [&Id]() { return Id++; });
	const TArray<FFleetAdmission> Verdicts = Generator->GetFleetAdmission(Alpha);
	if (!TestEqual(TEXT("fixture: two verdicts"), Verdicts.Num(), 2)
		|| !TestTrue(TEXT("fixture: the fitting type is admitted"), Verdicts[0].bAdmitted)
		|| !TestFalse(TEXT("fixture: the wide one is not"), Verdicts[1].bAdmitted))
	{
		return false;
	}

	FAirlinePanelSources Sources = F.Sources();
	Sources.Generator = Generator;
	Sources.Clock = Clock;
	const FAirlineDetail D = UAirlineDetailViewModel::Build(Sources, Alpha, Clock->Now());
	TestTrue(TEXT("judged"), D.bJudged);
	if (!TestEqual(TEXT("one row per fleet type"), D.Fleet.Num(), 2)) { return false; }
	TestEqual(TEXT("in fleet order"), D.Fleet[0].TypeName.ToString(), FString(TEXT("Fits")));
	TestTrue(TEXT("a tick"), D.Fleet[0].bAdmitted);
	TestTrue(TEXT("with no reason"), D.Fleet[0].Reason.IsEmpty());
	TestEqual(TEXT("then the wide one"), D.Fleet[1].TypeName.ToString(), FString(TEXT("TooWide")));
	TestFalse(TEXT("a cross"), D.Fleet[1].bAdmitted);
	TestFalse(TEXT("which says why"), Verdicts[1].Sentence.IsEmpty());
	TestEqual(TEXT("in the plan's own sentence"), D.Fleet[1].Reason.ToString(), Verdicts[1].Sentence);
	TestEqual(TEXT("the factor line: mood and the admitted share"), D.FactorLine.ToString(),
		FString(TEXT("mood x1.0 · 1 of 2 types can come")));
	TestEqual(TEXT("the rate line is the generator's CurrentRate - the number actually accruing"), D.RateLine.ToString(),
		FString::Printf(TEXT("~%.1f offers/h now"), Generator->CurrentRate(*Airline, *Clock)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirlinesDetailFilteredTest, "AirportMgr.Airlines.Detail.OffersAndFlightsFilteredByAirline",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAirlinesDetailFilteredTest::RunTest(const FString&)
{
	FAirlinesVmFixture F;
	const FName Alpha = F.Add(AirlinesVmAirline(TEXT("Alpha Air")));
	const FName Bravo = F.Add(AirlinesVmAirline(TEXT("Bravo")));
	USimClock* Clock = NewObject<USimClock>(GetTransientPackage());
	UFlightBoard* Board = NewObject<UFlightBoard>(GetTransientPackage());

	UFlight* AlphaOffer = AirlinesVmFlight(TEXT("AA 1"), Alpha, EFlightPhase::Offered);
	AlphaOffer->OfferWindowSeconds = 60.0;
	AlphaOffer->OfferSecondsLeft = 42.3;
	UFlight* BravoOffer = AirlinesVmFlight(TEXT("BB 1"), Bravo, EFlightPhase::Offered);
	UFlight* OnStand = AirlinesVmFlight(TEXT("AA 2"), Alpha, EFlightPhase::Turnaround);
	OnStand->ContractSeconds = 2400.0;
	OnStand->OnBlocksAt = 1.0;               // on blocks at 1 s: at Now = 3000 it is ten minutes late
	UFlight* Coming = AirlinesVmFlight(TEXT("AA 3"), Alpha, EFlightPhase::Accepted);
	Coming->ArrivesAt = 3600.0;
	Coming->ContractSeconds = 1800.0;
	UFlight* Gone = AirlinesVmFlight(TEXT("AA 4"), Alpha, EFlightPhase::Departed);
	UFlight* Cancelled = AirlinesVmFlight(TEXT("AA 5"), Alpha, EFlightPhase::Cancelled);
	UFlight* BravoTaxi = AirlinesVmFlight(TEXT("BB 2"), Bravo, EFlightPhase::TaxiIn);
	for (UFlight* Each : { AlphaOffer, BravoOffer, OnStand, Coming, Gone, Cancelled, BravoTaxi }) { Board->AddOffer(*Clock, Each); }

	FAirlinePanelSources Sources = F.Sources();
	Sources.Board = Board;
	const double Now = 3000.0;
	const FAirlineDetail D = UAirlineDetailViewModel::Build(Sources, Alpha, Now);

	if (TestEqual(TEXT("only Alpha's offer"), D.Offers.Num(), 1))
	{
		TestEqual(TEXT("its callsign"), D.Offers[0].Callsign.ToString(), FString(TEXT("AA 1")));
		TestEqual(TEXT("its type"), D.Offers[0].TypeName.ToString(), FString(TEXT("Saab 340B")));
		TestEqual(TEXT("the inbox's own countdown words"), D.Offers[0].Countdown.ToString(),
			UOfferViewModel::DescribeSecondsLeft(UOfferViewModel::SecondsLeftOf(*AlphaOffer)).ToString());
		TestEqual(TEXT("rounded up, as the inbox rounds"), D.Offers[0].Countdown.ToString(), FString(TEXT("43 s")));
	}

	if (!TestEqual(TEXT("Alpha's two flights in progress - departed, cancelled and Bravo's left out"), D.Flights.Num(), 2)) { return false; }
	for (const FAirlineFlightRow& Row : D.Flights)
	{
		const UFlight* Source = Row.Callsign.ToString() == TEXT("AA 2") ? OnStand : Row.Callsign.ToString() == TEXT("AA 3") ? Coming : nullptr;
		if (!TestNotNull(*FString::Printf(TEXT("%s is one of Alpha's live flights"), *Row.Callsign.ToString()), Source)) { continue; }
		bool bLate = false;
		const FText Contract = UArrivalRowViewModel::DescribeDetail(*Source, Now, bLate);
		TestEqual(*FString::Printf(TEXT("%s: the arrivals row's contract text"), *Source->Callsign), Row.Contract.ToString(), Contract.ToString());
		TestEqual(*FString::Printf(TEXT("%s: and its lateness"), *Source->Callsign), Row.bLate, bLate);
		TestEqual(*FString::Printf(TEXT("%s: the arrivals row's phase word"), *Source->Callsign), Row.Phase.ToString(),
			UArrivalRowViewModel::DescribeStatus(*Source, Now).ToString());
		TestEqual(*FString::Printf(TEXT("%s: its type"), *Source->Callsign), Row.TypeName.ToString(), FString(TEXT("Saab 340B")));
	}
	const FAirlineFlightRow* Late = D.Flights.FindByPredicate([](const FAirlineFlightRow& Row) { return Row.Callsign.ToString() == TEXT("AA 2"); });
	TestTrue(TEXT("the flight past its contract is late"), Late != nullptr && Late->bLate);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirlinesSourcesFromRuntimeTest, "AirportMgr.Airlines.Sources.FromRuntime",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAirlinesSourcesFromRuntimeTest::RunTest(const FString&)
{
	// THE SEAM THE RUNTIME OVERLOADS FORWARD THROUGH: every field from the runtime's own getter, so a panel never builds from an
	// empty source that a test's fixture would have filled.
	const UOpsRuntime* Runtime = NewObject<UOpsRuntime>(GetTransientPackage());
	const FAirlinePanelSources S = FAirlinePanelSources::From(*Runtime);
	TestTrue(TEXT("the roster"), S.Roster == Runtime->GetAirlines() && S.Roster != nullptr);
	TestTrue(TEXT("the history"), S.History == Runtime->GetAirlineHistory() && S.History != nullptr);
	TestTrue(TEXT("the generator"), S.Generator == Runtime->GetOfferGenerator() && S.Generator != nullptr);
	TestTrue(TEXT("the board"), S.Board == Runtime->GetFlightBoard() && S.Board != nullptr);
	TestTrue(TEXT("the clock"), S.Clock == Runtime->GetClock() && S.Clock != nullptr);
	TestTrue(TEXT("the catalog's airlines - the generator's own list"),
		S.Airlines.GetData() == Runtime->GetAirlineOffers().GetData() && S.Airlines.Num() == Runtime->GetAirlineOffers().Num());
	return true;
}

#endif
