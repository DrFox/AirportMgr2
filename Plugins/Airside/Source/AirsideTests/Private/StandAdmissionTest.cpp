#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Content/AirsideSettings.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Model/StandAdmission.h"
#include "Profiles/RoadProfile.h"
#include "Solve/IcaoCode.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** An empty network: no taxiway, so no strip - these cases are not about the strip, and
	 *  passing one with none says so. */
	const URoadNetwork& NoStrips()
	{
		return *NewObject<URoadNetwork>(GetTransientPackage());
	}

	FEntityInstance StandOf(EIcaoCode Letter, EPavement P)
	{
		FEntityInstance Stand;
		Stand.bAlive = true;
		Stand.DesignWingspan = IcaoCode::DesignSpanForLetter(Letter);
		Stand.Pavement = P;
		return Stand;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStandAdmissionGrassRefusesTarmacAircraftTest,
	"Airside.Model.StandAdmission.GrassStandRefusesTarmacAircraft",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FStandAdmissionGrassRefusesTarmacAircraftTest::RunTest(const FString&)
{
	// THE USER'S CASE: an F stand an A380 fits on, laid on grass, is not usable by it. Size
	// alone - the rule this replaces - admitted it.
	FAirframe Heavy = UAirsideSettings::ResolveDefaultAirframe();
	Heavy.Wingspan = 7980.0;
	Heavy.MinimumPavement = EPavement::Concrete;
	const FStandAdmission A = StandAdmission::Judge(NoStrips(), StandOf(EIcaoCode::F, EPavement::Grass), Heavy);
	TestEqual(TEXT("refused"), A.Why, EStandRefusal::Surface);
	TestEqual(TEXT("with the runway's own sentence"), StandAdmission::Describe(A),
		FString(TEXT("the surface is grass; this aircraft needs concrete")));
	TestTrue(TEXT("and admitted once paved"), StandAdmission::Judge(NoStrips(), StandOf(EIcaoCode::F, EPavement::Concrete), Heavy).IsAdmitted());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStandAdmissionSurfaceBeatsSizeTest, "Airside.Model.StandAdmission.SurfaceBeatsSize",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FStandAdmissionSurfaceBeatsSizeTest::RunTest(const FString&)
{
	// FIRST REFUSAL WINS, surface first - runway admission's own order and reason: drawing the
	// stand bigger does not fix its pavement.
	FAirframe Heavy = UAirsideSettings::ResolveDefaultAirframe();
	Heavy.Wingspan = 7980.0;
	Heavy.MinimumPavement = EPavement::Concrete;
	const FStandAdmission TooSmallAndSoft = StandAdmission::Judge(NoStrips(), StandOf(EIcaoCode::B, EPavement::Grass), Heavy);
	TestEqual(TEXT("too small AND too soft reports surface"), TooSmallAndSoft.Why, EStandRefusal::Surface);
	TestFalse(TEXT("and bPassesSize says it would still be too small if paved"), TooSmallAndSoft.bPassesSize);
	TestEqual(TEXT("too small on the right pavement reports size"),
		StandAdmission::Judge(NoStrips(), StandOf(EIcaoCode::B, EPavement::Concrete), Heavy).Why, EStandRefusal::TooSmall);

	// BIG ENOUGH BUT SOFT: bPassesSize is what lets ArrivalPlanner::WhyEveryStandRefused tell
	// "pave one" (this case) from "every stand is too small" (the case above) without asking
	// IcaoCode::StandAdmits a second time itself.
	const FStandAdmission BigEnoughButSoft = StandAdmission::Judge(NoStrips(), StandOf(EIcaoCode::F, EPavement::Grass), Heavy);
	TestEqual(TEXT("big enough but soft still reports surface"), BigEnoughButSoft.Why, EStandRefusal::Surface);
	TestTrue(TEXT("and bPassesSize says paving would fix it"), BigEnoughButSoft.bPassesSize);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStandAdmissionEveryRoleTest, "Airside.Model.StandAdmission.EveryRoleWorksOnEveryPavement",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FStandAdmissionEveryRoleTest::RunTest(const FString&)
{
	// PINS TODAY'S RULING (user, 2026-09-27): every service works on every pavement. This goes
	// red BY DESIGN the day grass restricts a role - update it with that ruling, do not delete it.
	for (uint8 P = 0; P < static_cast<uint8>(EPavement::Count); ++P)
	{
		const UEnum* RoleEnum = StaticEnum<EServiceRole>();
		for (int32 RoleIndex = 0; RoleIndex < RoleEnum->NumEnums() - 1; ++RoleIndex)
		{
			const EServiceRole Role = static_cast<EServiceRole>(RoleEnum->GetValueByIndex(RoleIndex));
			TestTrue(FString::Printf(TEXT("%s works on %s"), *UEnum::GetValueAsString(Role), Pavement::Name(static_cast<EPavement>(P))),
				StandAdmission::PavementAdmitsRole(static_cast<EPavement>(P), Role));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStandAdmissionInsideStripTest, "Airside.Model.StandAdmission.InsideStrip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FStandAdmissionInsideStripTest::RunTest(const FString&)
{
	// A 24 m (Code E) taxiway along Y = 0: kerb at 12 m, strip edge at 12 + 28 = 40 m.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FRoadNodeId W = Net->AddNode(FVector2D(-10000.0, 0.0));
	const FRoadNodeId E = Net->AddNode(FVector2D(10000.0, 0.0));
	Net->AddSegment(W, E, FVector2D::ZeroVector, URoadProfile::MakeTransient(2400.0, 1600.0));

	FEntityInstance Stand = StandOf(EIcaoCode::C, EPavement::Concrete);
	FAirframe Airframe = UAirsideSettings::ResolveDefaultAirframe();
	Airframe.Wingspan = 3400.0;
	Airframe.MinimumPavement = EPavement::Tarmac;
	const double Wd = IcaoCode::StandWidthForLetter(EIcaoCode::C);
	const double Dp = IcaoCode::StandDepthForLetter(EIcaoCode::C);
	auto BoxFrom = [&](double NearY)
	{
		return TArray<FVector2D>{ { 0.0, NearY }, { Wd, NearY }, { Wd, NearY + Dp }, { 0.0, NearY + Dp } };
	};

	// A STAND DRAWN BEFORE THE STRIP EXISTED - flush to the kerb - is closed to new arrivals.
	// Placement already refuses these; this is the one the player built last week.
	Stand.Outline = BoxFrom(1200.0);
	const FStandAdmission Closed = StandAdmission::Judge(*Net, Stand, Airframe);
	TestEqual(TEXT("flush to the kerb: refused for the strip"), Closed.Why, EStandRefusal::InsideStrip);
	TestFalse(TEXT("and not admitted"), Closed.IsAdmitted());
	TestTrue(TEXT("the sentence names the strip"), StandAdmission::Describe(Closed).Contains(TEXT("strip")));
	TestTrue(TEXT("size is still reported - it is a fact about the stand, not the verdict"), Closed.bPassesSize);

	Stand.Outline = BoxFrom(1200.0 + 2800.0);
	TestTrue(TEXT("behind the strip: admitted"), StandAdmission::Judge(*Net, Stand, Airframe).IsAdmitted());
	return true;
}

#endif
