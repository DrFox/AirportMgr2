#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Content/AirsideSettings.h"
#include "Build/AnchorLink.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/ArrivalPlanner.h"
#include "Model/DeparturePlanner.h"
#include "Model/GroundTraffic.h"
#include "Model/LandingRun.h"
#include "Model/RoadNetwork.h"
#include "Model/RunwayAdmission.h"
#include "Model/RunwayQuery.h"
#include "Model/TrafficOccupancy.h"
#include "Profiles/RoadProfile.h"
#include "Solve/RunwayDesignator.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * TWO RUNWAYS, BOTH USED (samples/2runways.png, 2026-09-29): every arrival went to the runway
 * nearest the longest strip's threshold and every departure to the shortest taxi, so the second
 * runway sat empty. Now each planner asks every runway its ERunwayUse allows and prefers a free
 * one, then one the player dedicated, then the shorter taxi.
 *
 * THE FIELD MOVED to Testing/AirsideTestGraph.h as FTestTwoRunways on 2026-09-30 (ops batch 3 PR D), when
 * AirportOpsTests needed it too; its own comment there carries the geometry's reasons.
 */
using FTwoRunways = FTestTwoRunways;

/**
 * AN ARRIVAL TAKES THE FREE RUNWAY. Nothing held: A, the shorter taxi. A held: B - it used to be
 * refused, waiting on A with B empty. Both held: refused as busy, and IsRunwayBusy - what the
 * queue asks - agrees at every step, or a queued flight would wait with a runway free.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunwayUseArrivalFreeTest, "Airside.Model.RunwayUse.ArrivalsTakeTheFreeRunway",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayUseArrivalFreeTest::RunTest(const FString& Parameters)
{
	const FAirframe Airframe = TestAirframes::Piper();
	const FTwoRunways F = FTwoRunways::Build(Airframe);
	const FVector2D Focus(-1000.0, 0.0);   // A's threshold - the old rule's only runway
	FTrafficOccupancy Occupancy;

	const FArrivalPlan Free = ArrivalPlanner::Plan(*F.Net, Focus, Airframe, &Occupancy);
	if (!TestTrue(FString::Printf(TEXT("fixture: an arrival plans (%s)"), *ArrivalPlanner::DescribeRefusal(Free)), Free.IsValid())) { return false; }
	TestTrue(TEXT("nothing held: A, the shorter taxi"), FTwoRunways::IsA(Free.End));
	TestFalse(TEXT("and the queue sees a free runway"), ArrivalPlanner::IsRunwayBusy(*F.Net, Focus, &Occupancy));

	F.Hold(Occupancy, F.A, 90);
	const FArrivalPlan OnB = ArrivalPlanner::Plan(*F.Net, Focus, Airframe, &Occupancy);
	TestTrue(FString::Printf(TEXT("A held: it still plans (%s)"), *ArrivalPlanner::DescribeRefusal(OnB)), OnB.IsValid());
	TestTrue(TEXT("on B"), FTwoRunways::IsB(OnB.End));
	TestFalse(TEXT("and the queue does not wait for A"), ArrivalPlanner::IsRunwayBusy(*F.Net, Focus, &Occupancy));

	F.Hold(Occupancy, F.B, 91);
	const FArrivalPlan Neither = ArrivalPlanner::Plan(*F.Net, Focus, Airframe, &Occupancy);
	TestEqual(TEXT("both held: refused as busy - the refusal that clears"), Neither.Why, EArrivalRefusal::RunwayOccupied);
	TestTrue(TEXT("and the queue agrees both are busy"), ArrivalPlanner::IsRunwayBusy(*F.Net, Focus, &Occupancy));
	return true;
}

/**
 * A DEPARTURE TAKES THE FREE RUNWAY. Nothing held: A, the shorter taxi. A held: B - every
 * departure used to go to A whatever was on it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunwayUseDepartureFreeTest, "Airside.Model.RunwayUse.DeparturesTakeTheFreeRunway",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayUseDepartureFreeTest::RunTest(const FString& Parameters)
{
	const FAirframe Airframe = TestAirframes::Piper();
	const FTwoRunways F = FTwoRunways::Build(Airframe);
	FTrafficOccupancy Occupancy;

	const FDeparturePlan Free = DeparturePlanner::PlanAny(*F.Net, F.Pose(0), Airframe, ETraversalClass::Aircraft, &Occupancy);
	if (!TestTrue(FString::Printf(TEXT("fixture: a departure plans (%s)"), *DeparturePlanner::Describe(Free)), Free.IsValid())) { return false; }
	TestTrue(TEXT("nothing held: A, the shorter taxi"), FTwoRunways::IsA(Free.End));

	F.Hold(Occupancy, F.A, 90);
	const FDeparturePlan OnB = DeparturePlanner::PlanAny(*F.Net, F.Pose(0), Airframe, ETraversalClass::Aircraft, &Occupancy);
	TestTrue(FString::Printf(TEXT("A held: it plans (%s)"), *DeparturePlanner::Describe(OnB)), OnB.IsValid());
	TestTrue(TEXT("from B"), FTwoRunways::IsB(OnB.End));
	return true;
}

/**
 * THE PLAYER'S MODES SEGREGATE, against geometry: A is always the shorter taxi, so every B here
 * is the setting speaking. Then the modes swapped, so neither answer is a fixture accident.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunwayUseModesTest, "Airside.Model.RunwayUse.ModesSegregate",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayUseModesTest::RunTest(const FString& Parameters)
{
	const FAirframe Airframe = TestAirframes::Piper();
	const FTwoRunways F = FTwoRunways::Build(Airframe);
	const FVector2D Focus(-1000.0, 0.0);

	F.SetUse(F.A, ERunwayUse::DeparturesOnly);
	F.SetUse(F.B, ERunwayUse::ArrivalsOnly);
	const FArrivalPlan Land = ArrivalPlanner::Plan(*F.Net, Focus, Airframe);
	TestTrue(FString::Printf(TEXT("A departures, B arrivals: an arrival plans (%s)"), *ArrivalPlanner::DescribeRefusal(Land)), Land.IsValid());
	TestTrue(TEXT("and lands on B, though A is the shorter taxi"), FTwoRunways::IsB(Land.End));
	const FDeparturePlan Leave = DeparturePlanner::PlanAny(*F.Net, F.Pose(0), Airframe, ETraversalClass::Aircraft);
	TestTrue(TEXT("a departure leaves from A"), Leave.IsValid() && FTwoRunways::IsA(Leave.End));

	F.SetUse(F.A, ERunwayUse::ArrivalsOnly);
	F.SetUse(F.B, ERunwayUse::DeparturesOnly);
	TestTrue(TEXT("swapped: lands on A"), FTwoRunways::IsA(ArrivalPlanner::Plan(*F.Net, Focus, Airframe).End));
	const FDeparturePlan Swapped = DeparturePlanner::PlanAny(*F.Net, F.Pose(0), Airframe, ETraversalClass::Aircraft);
	TestTrue(TEXT("and leaves from B, though A is the shorter taxi"), Swapped.IsValid() && FTwoRunways::IsB(Swapped.End));

	// A DEDICATED RUNWAY BEATS A MIXED ONE, free against free: A mixed and nearer, B set to arrivals.
	F.SetUse(F.A, ERunwayUse::Mixed);
	F.SetUse(F.B, ERunwayUse::ArrivalsOnly);
	TestTrue(TEXT("A mixed, B arrivals: lands on B, the runway the player dedicated"),
		FTwoRunways::IsB(ArrivalPlanner::Plan(*F.Net, Focus, Airframe).End));
	return true;
}

/**
 * A FIELD WITH NO RUNWAY FOR ONE KIND says it is the SETTING, not a missing runway - the fix is
 * the card, not the build tool.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunwayUseNoneTest, "Airside.Model.RunwayUse.OneWayFieldNamesTheSetting",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayUseNoneTest::RunTest(const FString& Parameters)
{
	const FAirframe Airframe = TestAirframes::Piper();
	const FTwoRunways F = FTwoRunways::Build(Airframe);

	F.SetUse(F.A, ERunwayUse::DeparturesOnly);
	F.SetUse(F.B, ERunwayUse::DeparturesOnly);
	const FArrivalPlan Land = ArrivalPlanner::Plan(*F.Net, FVector2D(-1000.0, 0.0), Airframe);
	TestEqual(TEXT("every runway departures only: no arrival runway"), Land.Why, EArrivalRefusal::NoArrivalRunway);
	TestTrue(TEXT("and the sentence names the setting"), ArrivalPlanner::DescribeRefusal(Land).Contains(TEXT("departures only")));

	F.SetUse(F.A, ERunwayUse::ArrivalsOnly);
	F.SetUse(F.B, ERunwayUse::ArrivalsOnly);
	const FDeparturePlan Leave = DeparturePlanner::PlanAny(*F.Net, F.Pose(0), Airframe, ETraversalClass::Aircraft);
	TestEqual(TEXT("every runway arrivals only: no departure runway"), Leave.Why, EDepartureRefusal::NoDepartureRunway);
	TestTrue(TEXT("and the sentence names the setting"), DeparturePlanner::Describe(Leave).Contains(TEXT("arrivals only")));

	// AND THE ARRIVAL IS REFUSED TOO (#433): it could land, and could never leave its stand. CheckArrival used to
	// walk every runway, the arrivals-only ones included, and admitted it; the offers behind CouldEverAdmit kept coming.
	const FArrivalPlan Stranded = ArrivalPlanner::Plan(*F.Net, FVector2D(-1000.0, 0.0), Airframe);
	TestFalse(TEXT("every runway arrivals only: an arrival is not admitted - it could land and never leave"), Stranded.IsValid());
	TestEqual(TEXT("refused as not admitted"), Stranded.Why, EArrivalRefusal::NotAdmitted);
	TestEqual(TEXT("for want of a departure runway"), Stranded.Admission.Why, ERunwayRefusal::NoDepartureRunway);
	TestTrue(TEXT("about leaving"), Stranded.Admission.bForDeparture);
	TestTrue(FString::Printf(TEXT("and the sentence names the setting: \"%s\""), *ArrivalPlanner::DescribeRefusal(Stranded)),
		ArrivalPlanner::DescribeRefusal(Stranded).Contains(TEXT("arrivals only")));
	return true;
}

/**
 * A RECLASSIFY KEEPS THE MODE: the runway tool writes a fresh FRunwayFacts (Use Unset) to change
 * a surface, and a player's "arrivals only" must survive it - InUse's rule. And a runway never
 * set reads Mixed.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunwayUseKeptTest, "Airside.Model.RunwayUse.ReclassifyKeepsTheMode",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayUseKeptTest::RunTest(const FString& Parameters)
{
	const FTwoRunways F = FTwoRunways::Build(TestAirframes::Piper());
	TestEqual(TEXT("a runway never set reads mixed"),
		RunwayUse::Resolve(F.Net->RunwayFactsFor(F.A).Use), ERunwayUse::Mixed);

	F.SetUse(F.A, ERunwayUse::ArrivalsOnly);
	FRunwayFacts Grass;
	Grass.Surface = EPavement::Grass;
	TestTrue(TEXT("a surface-only reclassify is accepted"), F.Net->SetRunwayFacts(F.A, Grass));
	TestEqual(TEXT("the surface changed"), F.Net->RunwayFactsFor(F.A).Surface, EPavement::Grass);
	TestEqual(TEXT("and the mode did not"), F.Net->RunwayFactsFor(F.A).Use, ERunwayUse::ArrivalsOnly);
	return true;
}

/**
 * THE COMPOSITION: two arrivals dispatched back to back through UGroundTraffic land on TWO
 * runways - the first's claim (raised at dispatch) is what sends the second to B. The shape of
 * samples/2runways.png, where the second waited for the first's runway.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunwayUseTwoArrivalsTest, "Airside.Model.RunwayUse.TwoArrivalsLandOnTwoRunways",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayUseTwoArrivalsTest::RunTest(const FString& Parameters)
{
	const FAirframe Airframe = TestAirframes::Piper();
	const FTwoRunways F = FTwoRunways::Build(Airframe);
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const FVector2D Focus(-1000.0, 0.0);

	const int32 First = Traffic->DispatchArrival(*F.Net, Focus, Airframe, 1.0);
	const int32 Second = Traffic->DispatchArrival(*F.Net, Focus, Airframe, 1.0);
	if (!TestTrue(TEXT("the first is dispatched"), First > 0) || !TestTrue(TEXT("and the second - not refused as busy"), Second > 0))
	{
		return false;
	}
	const FRoadAgent* One = Traffic->FindAgent(First);
	const FRoadAgent* Two = Traffic->FindAgent(Second);
	if (!TestNotNull(TEXT("both exist"), One) || !TestNotNull(TEXT("both exist"), Two)) { return false; }
	TestTrue(TEXT("the first lands on A"), One->RunwayHeld.Contains(F.A));
	TestTrue(TEXT("the second on B"), Two->RunwayHeld.Contains(F.B));
	return true;
}

/**
 * A LANDING IS ADMITTED ONLY WHERE THE DEPARTURE IT NEEDS WOULD PLAN (#433). Every mode pair, on the one field:
 * whenever an arrival plans, PlanAny must have a runway that takes its departure. CheckArrival's "can it leave
 * again" half used to walk EVERY runway with no use filter while PlanAny skipped the ones set to arrivals only, so
 * a field of arrivals-only strips admitted jets that could land and never depart - the stand blocked for ever.
 * The property, not a case: a fourth rule (curfew, wind) that one side learns and the other does not turns a
 * pair here red.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunwayUseLandsOnlyWhatCanLeaveTest, "Airside.Model.RunwayUse.EveryModePairLandsOnlyWhatCanLeave",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayUseLandsOnlyWhatCanLeaveTest::RunTest(const FString& Parameters)
{
	const FAirframe Airframe = TestAirframes::Piper();
	const FTwoRunways F = FTwoRunways::Build(Airframe);
	const FVector2D Focus(-1000.0, 0.0);
	const ERunwayUse Modes[] = { ERunwayUse::Mixed, ERunwayUse::ArrivalsOnly, ERunwayUse::DeparturesOnly };
	int32 Lands = 0;
	for (const ERunwayUse UseA : Modes)
	{
		for (const ERunwayUse UseB : Modes)
		{
			F.SetUse(F.A, UseA);
			F.SetUse(F.B, UseB);
			const FString Pair = FString::Printf(TEXT("A %s, B %s"), RunwayUse::Name(UseA), RunwayUse::Name(UseB));
			const FArrivalPlan Land = ArrivalPlanner::Plan(*F.Net, Focus, Airframe);
			const FDeparturePlan Leave = DeparturePlanner::PlanAny(*F.Net, F.Pose(0), Airframe, ETraversalClass::Aircraft);
			const bool bAnyLands = RunwayUse::Lands(UseA) || RunwayUse::Lands(UseB);
			const bool bAnyDeparts = RunwayUse::Departs(UseA) || RunwayUse::Departs(UseB);
			// BOTH DIRECTIONS: an arrival that plans must be able to leave (the stranding bug), and one that
			// can land AND leave must plan - a one-way check passes a planner that refuses every arrival. The
			// Piper on this fixture plans whenever a runway takes each kind, so the pair's settings alone
			// decide it.
			TestEqual(FString::Printf(TEXT("%s: an arrival plans exactly when some runway lands it and some takes it out again (%s)"),
				*Pair, *ArrivalPlanner::DescribeRefusal(Land)), Land.IsValid(), bAnyLands && bAnyDeparts);
			if (Land.IsValid())
			{
				++Lands;
				TestTrue(FString::Printf(TEXT("%s: it lands, so a runway must take it out again (%s)"), *Pair,
					*DeparturePlanner::Describe(Leave)), Leave.IsValid());
			}
			else
			{
				TestEqual(FString::Printf(TEXT("%s: refused with the admission's reason, not a route (%s)"), *Pair,
					*ArrivalPlanner::DescribeRefusal(Land)), Land.Why,
					bAnyLands ? EArrivalRefusal::NotAdmitted : EArrivalRefusal::NoArrivalRunway);
			}
		}
	}
	// FIXTURE CONTROL: the sweep must not be all refusals, or "lands implies leaves" is vacuous.
	TestTrue(TEXT("fixture: some pair does land"), Lands > 0);
	return true;
}

namespace
{
	/** One straight runway strip along +X at Y, of Length and Use, returning its seed. */
	FRoadSegmentId MakeUseStrip(URoadNetwork& Net, double Y, double Length, ERunwayUse Use)
	{
		URoadProfile* Runway = TestProfiles::Runway();
		const FRoadNodeId A = Net.AddNode(FVector2D(0.0, Y));
		const FRoadNodeId B = Net.AddNode(FVector2D(Length, Y));
		const FRoadSegmentId Seed = Net.AddStraightSegment(A, B, Runway);
		FRunwayFacts Facts = Net.RunwayFactsFor(Seed);
		Facts.Use = Use;
		Net.SetRunwayFacts(Seed, Facts);
		return Seed;
	}
}

/**
 * THE LEAVING HALF ASKS ONLY THE RUNWAYS THAT TAKE DEPARTURES (#433). A long strip set to arrivals only can hold
 * the take-off figure but will never be asked to; the short strip set to departures only is the one PlanAny would
 * use, and the aircraft cannot roll from it. The arrival is refused - naming the take-off, on the SHORT strip's
 * length - rather than admitted and stranded on its stand. Flipping the long strip to mixed is the control: the
 * same airframe on the same field is admitted, so the setting is what refused it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunwayUseShortDepartureStripTest, "Airside.Model.RunwayUse.ArrivalsOnlyLongStripDoesNotAdmitWhatTheShortDepartureStripCannotLaunch",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayUseShortDepartureStripTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FRoadSegmentId Long = MakeUseStrip(*Net, 0.0, 100000.0, ERunwayUse::ArrivalsOnly);
	MakeUseStrip(*Net, 300000.0, 40366.0, ERunwayUse::DeparturesOnly);
	// The SR22's figures (ArrivalAdmitsOnlyWhatCanLeave): 390 m to land, 430 m to leave.
	FAirframe Sr22 = TestAirframes::Piper();
	Sr22.Requirements.LandingFieldLength = 39000.0;
	Sr22.Requirements.TakeoffFieldLength = 43000.0;

	// FIXTURE: the long strip takes this airframe both ways as a strip - the setting, not the length, is what is under test.
	TestTrue(TEXT("fixture: it lands on the long strip"), RunwayAdmission::Check(*Net, Long, Sr22, true).IsAdmitted());
	TestTrue(TEXT("fixture: the long strip could launch it, if it were allowed to"),
		RunwayAdmission::Check(*Net, Long, Sr22, false).IsAdmitted());

	const FRunwayAdmission Arrival = RunwayAdmission::CheckArrival(*Net, Long, Sr22);
	TestEqual(TEXT("the arrival is refused: the only departure runway is the short one"), Arrival.Why, ERunwayRefusal::TooShort);
	TestTrue(TEXT("about leaving"), Arrival.bForDeparture);
	TestEqual(TEXT("judged against the take-off figure"), Arrival.FieldLength, 43000.0);
	TestEqual(TEXT("on the SHORT strip's length, the departure runway - not the long arrivals-only one"), Arrival.RunwayLength, 40366.0);

	const FArrivalPlan Plan = ArrivalPlanner::Plan(*Net, FVector2D::ZeroVector, Sr22, nullptr);
	TestEqual(TEXT("through the planner, offers and the Land key included: not admitted"), Plan.Why, EArrivalRefusal::NotAdmitted);

	// CONTROL: the long strip mixed takes the departure as well.
	FRunwayFacts Mixed = Net->RunwayFactsFor(Long);
	Mixed.Use = ERunwayUse::Mixed;
	Net->SetRunwayFacts(Long, Mixed);
	const FRunwayAdmission Admitted = RunwayAdmission::CheckArrival(*Net, Long, Sr22);
	TestTrue(FString::Printf(TEXT("control: the long strip set to mixed admits it (%s)"), *RunwayAdmission::Describe(Admitted)), Admitted.IsAdmitted());
	return true;
}

/**
 * A DEDICATED DEPARTURE RUNWAY BEATS A MIXED ONE, free against free, and a HELD one loses to a free one whatever
 * it is set to - the departure half of the ranking ModesSegregate pins for arrivals. A is mixed and the shorter
 * taxi, so every B here is the setting speaking. Guards the seam #433 introduced: PlanAny reads its dedicated
 * setting through RankRunway(Departure), and a planner ranking with the ARRIVAL kind would read ArrivalsOnly and
 * prefer nothing.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunwayUseDedicatedDepartureTest, "Airside.Model.RunwayUse.DedicatedDepartureBeatsMixed",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayUseDedicatedDepartureTest::RunTest(const FString& Parameters)
{
	const FAirframe Airframe = TestAirframes::Piper();
	const FTwoRunways F = FTwoRunways::Build(Airframe);
	FTrafficOccupancy Occupancy;

	F.SetUse(F.A, ERunwayUse::Mixed);
	F.SetUse(F.B, ERunwayUse::DeparturesOnly);
	const FDeparturePlan Dedicated = DeparturePlanner::PlanAny(*F.Net, F.Pose(0), Airframe, ETraversalClass::Aircraft, &Occupancy);
	TestTrue(FString::Printf(TEXT("A mixed, B departures: it plans (%s)"), *DeparturePlanner::Describe(Dedicated)), Dedicated.IsValid());
	TestTrue(TEXT("and leaves from B, the runway the player dedicated, though A is the shorter taxi"), FTwoRunways::IsB(Dedicated.End));

	F.Hold(Occupancy, F.B, 90);
	const FDeparturePlan Free = DeparturePlanner::PlanAny(*F.Net, F.Pose(0), Airframe, ETraversalClass::Aircraft, &Occupancy);
	TestTrue(FString::Printf(TEXT("B held: it plans (%s)"), *DeparturePlanner::Describe(Free)), Free.IsValid());
	TestTrue(TEXT("from A: a free runway beats a dedicated one that is held"), FTwoRunways::IsA(Free.End));
	return true;
}

/**
 * THE ENUMERATORS FILTER BY KIND, and say how many runways there are AT ALL (#433). Every consumer of "which runways
 * may this traffic use" - both planners and CheckArrival's departure half - asks these two, so this is where a use
 * setting is read. The count is the difference between "no runway" (draw one) and "every runway refuses this kind"
 * (the card's setting), and it is written even when the answer is empty. The end is the END IN USE.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunwayUseEnumeratorsTest, "Airside.Model.RunwayUse.EnumeratorsFilterByKind",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayUseEnumeratorsTest::RunTest(const FString& Parameters)
{
	const FTwoRunways F = FTwoRunways::Build(TestAirframes::Piper());
	int32 Count = -1;

	TestEqual(TEXT("mixed both: two arrival runways"), RunwayQuery::ArrivalRunways(*F.Net, &Count).Num(), 2);
	TestEqual(TEXT("and the count is every runway"), Count, 2);
	TestEqual(TEXT("mixed both: two departure runways"), RunwayQuery::DepartureRunways(*F.Net, &Count).Num(), 2);

	F.SetUse(F.A, ERunwayUse::ArrivalsOnly);
	F.SetUse(F.B, ERunwayUse::DeparturesOnly);
	const TArray<FRunwayEnd> Landing = RunwayQuery::ArrivalRunways(*F.Net, &Count);
	TestTrue(TEXT("A arrivals, B departures: arrivals get only A"), Landing.Num() == 1 && FTwoRunways::IsA(Landing[0]));
	TestEqual(TEXT("still two runways on the field"), Count, 2);
	const TArray<FRunwayEnd> Leaving = RunwayQuery::DepartureRunways(*F.Net, &Count);
	TestTrue(TEXT("and departures get only B"), Leaving.Num() == 1 && FTwoRunways::IsB(Leaving[0]));

	Count = -1;
	F.SetUse(F.B, ERunwayUse::ArrivalsOnly);
	TestEqual(TEXT("both arrivals only: no departure runway"), RunwayQuery::DepartureRunways(*F.Net, &Count).Num(), 0);
	TestEqual(TEXT("yet two runways exist - the setting, not a missing runway"), Count, 2);

	Count = -1;
	F.SetUse(F.A, ERunwayUse::DeparturesOnly);
	F.SetUse(F.B, ERunwayUse::DeparturesOnly);
	TestEqual(TEXT("both departures only: no arrival runway"), RunwayQuery::ArrivalRunways(*F.Net, &Count).Num(), 0);
	TestEqual(TEXT("yet two runways exist"), Count, 2);

	Count = -1;
	const URoadNetwork* Empty = NewObject<URoadNetwork>(GetTransientPackage());
	TestEqual(TEXT("no network content: no arrival runway"), RunwayQuery::ArrivalRunways(*Empty, &Count).Num(), 0);
	TestEqual(TEXT("and the count is written as 0, not left as it was"), Count, 0);

	// THE END IN USE: the answer follows FRunwayFacts::InUse, whichever end the summary found the strip from.
	F.SetUse(F.A, ERunwayUse::Mixed);
	for (const int32 InUse : { 36, 18 })
	{
		FRunwayFacts Facts = F.Net->RunwayFactsFor(F.A);
		Facts.InUse = InUse;
		F.Net->SetRunwayFacts(F.A, Facts);
		bool bFoundA = false;
		for (const FRunwayEnd& End : RunwayQuery::ArrivalRunways(*F.Net))
		{
			if (FTwoRunways::IsA(End))
			{
				bFoundA = true;
				TestEqual(FString::Printf(TEXT("in use %d: the arrival end points %d"), InUse, InUse),
					RunwayDesignator::Designate(End.Direction), InUse);
			}
		}
		TestTrue(TEXT("and A is enumerated"), bFoundA);
	}
	return true;
}

/**
 * THE RANK ORDERS FREE, THEN DEDICATED, THEN SHORTEST - and RankRunway fills it. The comparison ArrivalPlanner::Plan
 * and DeparturePlanner::PlanAny each typed until #433, so the table is asserted on the value type itself, the
 * three inputs on the two kinds of traffic, and a tie as not-a-win (the first candidate found keeps its place).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunwayUseRankTest, "Airside.Model.RunwayUse.RankOrdersFreeDedicatedShortest",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayUseRankTest::RunTest(const FString& Parameters)
{
	auto Rank = [](bool bHeld, bool bDedicated, double Taxi) { FRunwayRank R; R.bHeld = bHeld; R.bDedicated = bDedicated; R.Taxi = Taxi; return R; };
	TestTrue(TEXT("free beats held, whatever the setting and the taxi"), Rank(false, false, 900.0).Beats(Rank(true, true, 1.0)));
	TestFalse(TEXT("and held never beats free"), Rank(true, true, 1.0).Beats(Rank(false, false, 900.0)));
	TestTrue(TEXT("dedicated beats mixed, free against free, whatever the taxi"), Rank(false, true, 900.0).Beats(Rank(false, false, 1.0)));
	TestTrue(TEXT("dedicated beats mixed, held against held"), Rank(true, true, 900.0).Beats(Rank(true, false, 1.0)));
	TestFalse(TEXT("mixed never beats dedicated"), Rank(false, false, 1.0).Beats(Rank(false, true, 900.0)));
	TestTrue(TEXT("the shorter taxi wins the rest"), Rank(false, true, 100.0).Beats(Rank(false, true, 200.0)));
	TestFalse(TEXT("the longer never does"), Rank(false, true, 200.0).Beats(Rank(false, true, 100.0)));
	TestFalse(TEXT("a tie is not a win"), Rank(false, true, 100.0).Beats(Rank(false, true, 100.0)));

	const FTwoRunways F = FTwoRunways::Build(TestAirframes::Piper());
	auto EndOf = [&](bool bA)
	{
		for (const FRunwayEnd& End : RunwayQuery::ArrivalRunways(*F.Net))
		{
			if (FTwoRunways::IsA(End) == bA) { return End; }
		}
		return FRunwayEnd();
	};
	const FRunwayEnd EndA = EndOf(true);
	const FRunwayEnd EndB = EndOf(false);
	FTrafficOccupancy Occupancy;

	F.SetUse(F.A, ERunwayUse::ArrivalsOnly);
	F.SetUse(F.B, ERunwayUse::Mixed);
	TestTrue(TEXT("arrivals only is dedicated to an ARRIVAL"), RunwayQuery::RankRunway(*F.Net, EndA, ERunwayTraffic::Arrival, &Occupancy, 5.0).bDedicated);
	TestFalse(TEXT("and not to a departure"), RunwayQuery::RankRunway(*F.Net, EndA, ERunwayTraffic::Departure, &Occupancy, 5.0).bDedicated);
	TestFalse(TEXT("a mixed runway is dedicated to neither"), RunwayQuery::RankRunway(*F.Net, EndB, ERunwayTraffic::Arrival, &Occupancy, 5.0).bDedicated
		|| RunwayQuery::RankRunway(*F.Net, EndB, ERunwayTraffic::Departure, &Occupancy, 5.0).bDedicated);
	F.SetUse(F.B, ERunwayUse::DeparturesOnly);
	TestTrue(TEXT("departures only is dedicated to a DEPARTURE"), RunwayQuery::RankRunway(*F.Net, EndB, ERunwayTraffic::Departure, &Occupancy, 5.0).bDedicated);
	TestFalse(TEXT("and not to an arrival"), RunwayQuery::RankRunway(*F.Net, EndB, ERunwayTraffic::Arrival, &Occupancy, 5.0).bDedicated);
	TestEqual(TEXT("the taxi is the caller's figure"), RunwayQuery::RankRunway(*F.Net, EndA, ERunwayTraffic::Arrival, &Occupancy, 5.0).Taxi, 5.0);

	TestFalse(TEXT("nothing held: free"), RunwayQuery::RankRunway(*F.Net, EndA, ERunwayTraffic::Arrival, &Occupancy, 5.0).bHeld);
	F.Hold(Occupancy, F.A, 90);
	TestTrue(TEXT("A held: A ranks held"), RunwayQuery::RankRunway(*F.Net, EndA, ERunwayTraffic::Arrival, &Occupancy, 5.0).bHeld);
	TestFalse(TEXT("and B does not"), RunwayQuery::RankRunway(*F.Net, EndB, ERunwayTraffic::Arrival, &Occupancy, 5.0).bHeld);
	TestFalse(TEXT("with no occupancy nothing is held"), RunwayQuery::RankRunway(*F.Net, EndA, ERunwayTraffic::Arrival, nullptr, 5.0).bHeld);
	TestTrue(TEXT("and ArrivalPlanner::IsChainHeld is the same answer, by its old name"),
		ArrivalPlanner::IsChainHeld(*F.Net, F.A, &Occupancy) == RunwayQuery::IsChainHeld(*F.Net, F.A, &Occupancy));
	return true;
}

#endif
