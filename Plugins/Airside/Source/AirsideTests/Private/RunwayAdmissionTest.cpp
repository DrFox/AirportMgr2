#include "CoreMinimal.h"
#include "Content/AirsideSettings.h"
#include "Misc/AutomationTest.h"
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
	// A 1000 m, 23 m tarmac visual runway and the Piper: grass-capable, visual, 800 m
	// field lengths, 13 m span. Every refusal below is one fact moved past what it needs.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FRoadSegmentId RW = MakeRunway(*Net, 100000.0, 2300.0);
	const FAirframe Piper = UAirsideSettings::ResolveDefaultAirframe();
	TestEqual(TEXT("the fixture's Piper publishes a landing field length"), Piper.Requirements.LandingFieldLength, 80000.0);

	TestEqual(TEXT("the Piper is admitted to a tarmac visual runway"),
		RunwayAdmission::Check(*Net, RW, Piper, true).Why, ERunwayRefusal::None);

	FRunwayFacts Grass;
	Grass.Surface = ERunwaySurface::Grass;
	Net->SetRunwayFacts(RW, Grass);
	TestEqual(TEXT("and to grass, because it needs only grass"),
		RunwayAdmission::Check(*Net, RW, Piper, true).Why, ERunwayRefusal::None);

	FAirframe NeedsTarmac = Piper;
	NeedsTarmac.Requirements.MinimumSurface = ERunwaySurface::Tarmac;
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
	URoadProfile* Taxiway = URoadProfile::MakeTransient(2300.0, 1500.0, 230.0);
	const FRoadNodeId P = Net->AddNode(FVector2D(0.0, 50000.0));
	const FRoadNodeId Q = Net->AddNode(FVector2D(10000.0, 50000.0));
	const FRoadSegmentId Tx = Net->AddStraightSegment(P, Q, Taxiway);
	TestEqual(TEXT("a taxiway is not refused - it is not a runway, which is a different answer"),
		RunwayAdmission::Check(*Net, Tx, NeedsTarmac, true).Why, ERunwayRefusal::None);
	return true;
}

#endif
