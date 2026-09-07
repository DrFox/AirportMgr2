#include "CoreMinimal.h"
#include "Content/AirsideContent.h"
#include "Content/AirsideSettings.h"
#include "Entities/AircraftType.h"
#include "Misc/AutomationTest.h"
#include "Model/LandingRun.h"
#include "Model/TakeoffRun.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFieldLengthsCoverTheRollTest,
	"Airside.Model.FieldLengthsCoverTheRoll",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFieldLengthsCoverTheRollTest::RunTest(const FString& Parameters)
{
	// Two numbers for one thing, by design (spec 2026-09-07 §3.2): the published field
	// length admits an aircraft to a runway, the derived roll moves it down one. This is
	// the relation that makes the pair safe - a published figure may be generous, but one
	// SHORTER than the roll would admit an aircraft to a strip it then runs off the end of.
	auto Check = [this](const TCHAR* Name, const FAirframe& Airframe)
	{
		const double Roll = FTakeoffRun::RequiredRoll(Airframe.Ground, Airframe.Climb);
		const double Landing = FLandingRun::RequiredLandingDistance(Airframe.Ground, Airframe.Climb, Airframe.Approach)
			* FLandingRun::LandingMargin;
		TestTrue(FString::Printf(TEXT("%s publishes a take-off field length"), Name), Airframe.Requirements.TakeoffFieldLength > 0.0);
		TestTrue(FString::Printf(TEXT("%s publishes a landing field length"), Name), Airframe.Requirements.LandingFieldLength > 0.0);
		TestTrue(FString::Printf(TEXT("%s: take-off roll %.0f <= published %.0f"), Name, Roll, Airframe.Requirements.TakeoffFieldLength),
			Roll <= Airframe.Requirements.TakeoffFieldLength);
		TestTrue(FString::Printf(TEXT("%s: landing distance with margin %.0f <= published %.0f"), Name, Landing, Airframe.Requirements.LandingFieldLength),
			Landing <= Airframe.Requirements.LandingFieldLength);
	};

	Check(TEXT("the default airframe"), UAirsideSettings::ResolveDefaultAirframe());

	// Every type the content set names, when one is configured. Tests run without one, so
	// this branch is exercised only in a project that has authored DA_PiperMeridian (#30).
	if (const UAirsideContent* Content = UAirsideSettings::GetContent())
	{
		if (const UAircraftType* Type = Content->DefaultAircraft.LoadSynchronous())
		{
			Check(*Type->GetName(), Type->Airframe());
		}
	}

	// The authored Piper, independently of whether content resolved it: BuildPiperMeridian
	// and the settings fallback must both carry figures that cover the same rolls.
	UAircraftType* Piper = NewObject<UAircraftType>(GetTransientPackage());
	UAircraftType::BuildPiperMeridian(Piper);
	Check(TEXT("BuildPiperMeridian"), Piper->Airframe());
	return true;
}

#endif
