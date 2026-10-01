#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Content/AirsideSettings.h"
#include "Misc/AutomationTest.h"
#include "Model/ArrivalPlanner.h"
#include "Model/RoadNetwork.h"
#include "Model/RunwayAdmission.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** A straight runway W -> E of Length and TotalWidth, returning its one segment. */
	FRoadSegmentId MakeRunway(URoadNetwork& Net, double Length, double TotalWidth)
	{
		URoadProfile* Runway = URoadProfile::MakeTransient(TotalWidth, 1500.0, TotalWidth * 0.1);
		Runway->bContinuousThroughJunctions = true;
		const FRoadNodeId A = Net.AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId B = Net.AddNode(FVector2D(Length, 0.0));
		return Net.AddStraightSegment(A, B, Runway);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRunwayAdmissionTest,
	"Airside.Model.RunwayAdmission",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayAdmissionTest::RunTest(const FString& Parameters)
{
	// A 1000 m, 23 m tarmac visual runway and the Piper: grass-capable, visual, 510 m /
	// 470 m field lengths, 13 m span. Every refusal below is one fact moved past what it needs.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	constexpr double StripLength = 100000.0;
	const FRoadSegmentId RW = MakeRunway(*Net, StripLength, 2300.0);
	const FAirframe Piper = UAirsideSettings::ResolveDefaultAirframe();

	// READ FROM THE ONE SOURCE, NOT TYPED. This pinned 40000 as a literal until 2026-09-27,
	// and went red when the Meridian's landing figure moved to 47000 for a reason that had
	// nothing to do with admission - a second copy of the figure, not a test of it.
	// PiperType, not Piper(): the bare FAirframe fixture carries no Requirements at all.
	const FRunwayRequirements Authored = TestAirframes::PiperType()->Airframe().Requirements;
	TestEqual(TEXT("the fixture's Piper publishes BuildPiperMeridian's landing field length"),
		Piper.Requirements.LandingFieldLength, Authored.LandingFieldLength);
	TestEqual(TEXT("and its take-off field length"),
		Piper.Requirements.TakeoffFieldLength, Authored.TakeoffFieldLength);
	// What the admissions below actually rely on: both figures fit this strip, so a refusal
	// is the one fact each case moves, never the length by accident.
	TestTrue(FString::Printf(TEXT("both field lengths (%.0f / %.0f) fit the %.0f uu strip"),
		Authored.TakeoffFieldLength, Authored.LandingFieldLength, StripLength),
		Authored.TakeoffFieldLength > 0.0 && Authored.LandingFieldLength > 0.0
			&& Authored.TakeoffFieldLength <= StripLength && Authored.LandingFieldLength <= StripLength);

	TestEqual(TEXT("the Piper is admitted to a tarmac visual runway"),
		RunwayAdmission::Check(*Net, RW, Piper, true).Why, ERunwayRefusal::None);

	FRunwayFacts Grass;
	Grass.Surface = EPavement::Grass;
	Net->SetRunwayFacts(RW, Grass);
	TestEqual(TEXT("and to grass, because it needs only grass"),
		RunwayAdmission::Check(*Net, RW, Piper, true).Why, ERunwayRefusal::None);

	FAirframe NeedsTarmac = Piper;
	NeedsTarmac.MinimumPavement = EPavement::Tarmac;
	const FRunwayAdmission BySurface = RunwayAdmission::Check(*Net, RW, NeedsTarmac, true);
	TestEqual(TEXT("an aircraft needing tarmac is refused grass by SURFACE"), BySurface.Why, ERunwayRefusal::Surface);
	TestTrue(TEXT("and the sentence names the surface it found"), RunwayAdmission::Describe(BySurface).Contains(TEXT("grass")));
	TestTrue(TEXT("and the one it needed"), RunwayAdmission::Describe(BySurface).Contains(TEXT("tarmac")));

	Net->SetRunwayFacts(RW, FRunwayFacts());
	FAirframe NeedsPrecision = Piper;
	NeedsPrecision.Requirements.ApproachNeeded = ERunwayApproach::Precision;
	const FRunwayAdmission ByApproach = RunwayAdmission::Check(*Net, RW, NeedsPrecision, true);
	TestEqual(TEXT("an aircraft needing precision is refused a visual runway by APPROACH"), ByApproach.Why, ERunwayRefusal::Approach);
	TestTrue(TEXT("and the sentence says visual"), RunwayAdmission::Describe(ByApproach).Contains(TEXT("visual")));

	// Field length is per OPERATION: bLanding picks the landing figure, else take-off,
	// so an aircraft that lands short but needs a long roll is refused only for departure.
	URoadNetwork* ShortNet = NewObject<URoadNetwork>(GetTransientPackage());
	const FRoadSegmentId Short = MakeRunway(*ShortNet, 80000.0, 2300.0);
	FAirframe LandsLong = Piper;
	LandsLong.Requirements.LandingFieldLength = 90000.0;
	LandsLong.Requirements.TakeoffFieldLength = 70000.0;
	const FRunwayAdmission Landing = RunwayAdmission::Check(*ShortNet, Short, LandsLong, true);
	TestEqual(TEXT("a 900 m landing field length is refused 800 m of runway as TOO SHORT"), Landing.Why, ERunwayRefusal::TooShort);
	TestEqual(TEXT("with the strip's length in the decision"), Landing.RunwayLength, 80000.0, 1.0);
	TestEqual(TEXT("and the figure it was judged against"), Landing.FieldLength, 90000.0);
	// #471: SAID IN METRES - the Land panel's rows and the toasts show this sentence since #470, and "80000 uu" is not a
	// figure a player choosing an aeroplane can use. Exact, so a unit dropped from either figure is red.
	TestEqual(TEXT("and the sentence says it in metres, both figures"), RunwayAdmission::Describe(Landing),
		FString(TEXT("the runway is 800 m; this aircraft's field length is 900 m")));
	TestEqual(TEXT("the same aircraft taking off needs 700 m and is admitted"),
		RunwayAdmission::Check(*ShortNet, Short, LandsLong, false).Why, ERunwayRefusal::None);
	FAirframe RollsLong = Piper;
	RollsLong.Requirements.TakeoffFieldLength = 90000.0;
	TestEqual(TEXT("and a 900 m take-off field length is refused for departure"),
		RunwayAdmission::Check(*ShortNet, Short, RollsLong, false).Why, ERunwayRefusal::TooShort);

	// Width by ICAO code: 23 m is code B, 24 m of span. The profile declares no
	// MaxWingspan (MakeTransient leaves it 0), so the table answers.
	FAirframe WideWing = Piper;
	WideWing.Wingspan = 3600.0;
	const FRunwayAdmission ByWidth = RunwayAdmission::Check(*Net, RW, WideWing, true);
	TestEqual(TEXT("a 36 m wingspan is refused a 23 m runway as TOO NARROW"), ByWidth.Why, ERunwayRefusal::TooNarrow);
	TestEqual(TEXT("with the code's limit in the decision"), ByWidth.MaxWingspan, 2400.0);
	TestEqual(TEXT("and the sentence says both spans in metres"), RunwayAdmission::Describe(ByWidth),
		FString(TEXT("the runway admits a 24.0 m wingspan; this aircraft's is 36.0 m")));
	TestEqual(TEXT("code A admits 15 m"), RunwayAdmission::MaxWingspanForWidth(1800.0), 1500.0);
	TestEqual(TEXT("code F admits 80 m"), RunwayAdmission::MaxWingspanForWidth(6000.0), 8000.0);
	TestEqual(TEXT("an odd width takes the nearest code"), RunwayAdmission::MaxWingspanForWidth(4000.0), 6500.0);

	// A refusal is the FIRST failing fact in scale order: grass AND too short reports grass.
	ShortNet->SetRunwayFacts(Short, Grass);
	FAirframe Both = NeedsTarmac;
	Both.Requirements.LandingFieldLength = 90000.0;
	TestEqual(TEXT("grass and too short is refused for the grass, the fact drawing longer cannot fix"),
		RunwayAdmission::Check(*ShortNet, Short, Both, true).Why, ERunwayRefusal::Surface);

	// Not a runway: admitted, because "no runway" is the planners' word and not this one's.
	URoadProfile* Taxiway = TestProfiles::Taxiway();
	const FRoadNodeId P = Net->AddNode(FVector2D(0.0, 50000.0));
	const FRoadNodeId Q = Net->AddNode(FVector2D(10000.0, 50000.0));
	const FRoadSegmentId Tx = Net->AddStraightSegment(P, Q, Taxiway);
	TestEqual(TEXT("a taxiway is not refused - it is not a runway, which is a different answer"),
		RunwayAdmission::Check(*Net, Tx, NeedsTarmac, true).Why, ERunwayRefusal::None);
	return true;
}

/**
 * AN ARRIVAL IS ADMITTED ONLY WHERE IT CAN LEAVE.
 *
 * REPORTED FROM PLAY, 2026-09-27: an SR22 landed on a 404 m strip and then refused to depart,
 * logging "the runway is 40366 uu; this aircraft's field length is 43000" every frame. Once
 * field lengths became the model's roll x 1.1, landing asked less than take-off for most
 * types (the SR22: 390 m in, 430 m out), and the landing check judged only the landing.
 *
 * THE DEPARTURE MAY USE ANY RUNWAY, because DeparturePlanner::PlanAny skips a refused one
 * (Airside.Model.DeparturePlanner.PlanAny.SkipsRefusedRunway) - so a short landing strip
 * beside a long departure one is still an airport the type can use.
 *
 * NOT NAMED Airside.Model.RunwayAdmission.Something - the suite above has that bare name, and
 * a dotted child would silently drop it from the automation tree.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FArrivalAdmitsOnlyWhatCanLeaveTest,
	"Airside.Model.ArrivalAdmitsOnlyWhatCanLeave",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FArrivalAdmitsOnlyWhatCanLeaveTest::RunTest(const FString& Parameters)
{
	// The reported strip and the SR22's figures (plane15.py).
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FRoadSegmentId Strip = MakeRunway(*Net, 40366.0, 2300.0);
	FAirframe Sr22 = TestAirframes::Piper();
	Sr22.Requirements.LandingFieldLength = 39000.0;
	Sr22.Requirements.TakeoffFieldLength = 43000.0;

	TestTrue(TEXT("the landing alone fits - 390 m on 404 m, which is how it got in"),
		RunwayAdmission::Check(*Net, Strip, Sr22, true).IsAdmitted());

	const FRunwayAdmission Arrival = RunwayAdmission::CheckArrival(*Net, Strip, Sr22);
	TestEqual(TEXT("but the arrival is refused, because no runway takes it out again"),
		Arrival.Why, ERunwayRefusal::TooShort);
	TestTrue(TEXT("and the verdict says it is about leaving"), Arrival.bForDeparture);
	TestEqual(TEXT("judged against the take-off figure"), Arrival.FieldLength, 43000.0);
	TestTrue(FString::Printf(TEXT("in words that say so: \"%s\""), *RunwayAdmission::Describe(Arrival)),
		RunwayAdmission::Describe(Arrival).Contains(TEXT("take off")));

	// THROUGH THE PLANNER, the seam offers, the board and the Land key all share: a plan that
	// still asked the landing-only check would pass everything above.
	const FArrivalPlan Plan = ArrivalPlanner::Plan(*Net, FVector2D::ZeroVector, Sr22, nullptr);
	TestEqual(TEXT("the arrival planner refuses it as not admitted"), Plan.Why, EArrivalRefusal::NotAdmitted);
	TestTrue(FString::Printf(TEXT("and its sentence names the departure: \"%s\""), *ArrivalPlanner::DescribeRefusal(Plan)),
		ArrivalPlanner::DescribeRefusal(Plan).Contains(TEXT("take off")));

	// A LONG DEPARTURE RUNWAY ELSEWHERE admits it - land short, leave long.
	const FRoadNodeId FarA = Net->AddNode(FVector2D(0.0, 300000.0));
	const FRoadNodeId FarB = Net->AddNode(FVector2D(60000.0, 300000.0));
	Net->AddStraightSegment(FarA, FarB, TestProfiles::NarrowRunway());
	const FRunwayAdmission WithLong = RunwayAdmission::CheckArrival(*Net, Strip, Sr22);
	TestTrue(FString::Printf(TEXT("with a 600 m runway to leave from, the 404 m landing is admitted (%s)"),
		*RunwayAdmission::Describe(WithLong)), WithLong.IsAdmitted());
	TestFalse(TEXT("and carries no departure verdict"), WithLong.bForDeparture);

	// A LANDING REFUSAL IS STILL THE LANDING'S: the departure is asked only once the landing fits.
	FAirframe TooLongIn = Sr22;
	TooLongIn.Requirements.LandingFieldLength = 50000.0;
	const FRunwayAdmission In = RunwayAdmission::CheckArrival(*Net, Strip, TooLongIn);
	TestEqual(TEXT("a landing that does not fit is refused for the landing"), In.Why, ERunwayRefusal::TooShort);
	TestFalse(TEXT("not for the departure"), In.bForDeparture);
	TestEqual(TEXT("against the landing figure"), In.FieldLength, 50000.0);
	return true;
}

#endif
